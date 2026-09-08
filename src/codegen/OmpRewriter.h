// OmpRewriter — the Days 14-18 pass: turn a ProfitabilityVerdict into text.
//
// Everything upstream of this file *decides*; nothing upstream *acts*. This is
// the first unit in src/codegen/, and its job is narrow on purpose: take a
// ProfitabilityVerdict this project's own analysis already computed, and
// render it as the exact OpenMP 4.5+ construct that verdict implies, via
// clang::Rewriter. It does not re-derive anything — the map clauses, the
// guard conditions, the op counts, all come from ProfitabilityVerdict,
// because Days 11-13 built that struct specifically so codegen would not have
// to walk the loop body a second time to rediscover facts already established
// (see Profitability.h's comment on ArrayRegion).
//
// ---------------------------------------------------------------------------
// Scope: GPU_OFFLOAD and CPU_PARALLEL
// ---------------------------------------------------------------------------
//
// Day 14 covered GPU_OFFLOAD only. Days 17-18 added the CPU-threaded path:
// rewriteLoop is a no-op for anything other than an Evaluated GpuOffload or
// CpuParallel verdict; SEQUENTIAL loops, and anything the safety gate
// blocked, pass through silently, the same way they were never offered to
// this pass at all — RewriteSummary::Skipped is reserved for a loop that
// *was* eligible and was still declined, for one of the reasons below. That
// distinction matters for the report: every Skipped line names an actual
// gap, not routine scope.
//
// ---------------------------------------------------------------------------
// The two pragmas
// ---------------------------------------------------------------------------
//
//   #pragma omp target teams distribute parallel for \
//       [if(target: <GuardExpr>)] map(<kind>: <base>[0:<extent>]) ...
//
//   #pragma omp parallel for [if(parallel: <CpuGuardExpr>)]
//
// The GPU map clauses are ProfitabilityVerdict::Regions rendered directly:
// mapKind() gives to/from/tofrom, ExtentText gives the section length. The
// `target:` / `parallel:` directive-name modifiers on `if` are required, not
// decorative: on a combined construct several constituent directives accept
// an `if` clause (target + parallel for; parallel + for), and an unqualified
// `if()` is ambiguous about which one it gates. When a guard is set and
// evaluates false at run time, the construct still runs — as a 1-team/
// 1-thread loop for the GPU path, as a single-threaded loop for the CPU path
// — so the guard is not just "skip the loop small", it *is* the fallback for
// the guarded case, without a second pragma.
//
// The CPU path carries no map() clauses (shared memory, nothing to move) and
// no private()/reduction() clauses: SafetyAnalyzer already requires that any
// scalar written in the loop body be declared fresh inside it, and OpenMP
// predetermines the loop control variable private on its own — so there is
// no shared-scalar hazard left for a clause to guard against, and a real
// reduction (`sum += a[i]`) is already UNSAFE and never reaches this pass at
// all. Nothing is missing here; there is nothing left for those clauses to
// say.
//
// ---------------------------------------------------------------------------
// declare target — GPU path only
// ---------------------------------------------------------------------------
//
// A target region can only call functions with a device-side compilation, so
// every function transitively reachable from an offloaded loop's callees
// (CallResolver::getReachable — the same walk ProfitabilityAnalyzer::getOpCount
// already makes) gets wrapped in `declare target` / `end declare target`.
// Without this, a build attempt fails immediately on the first call inside a
// target region, for a reason that has nothing to do with whether the offload
// decision itself was correct. `parallel for` runs on the host, calling
// ordinary host-compiled functions, so the CPU path never collects anything
// here.
//
// ---------------------------------------------------------------------------
// Descending loops: GPU declines, CPU degrades to
// ---------------------------------------------------------------------------
//
// The mapped-region model (ArrayRegion, ExtentText — see Profitability.h)
// assumes a loop counts up from a low index to the printed bound: extent is
// derived from the bound text alone, on the documented assumption that it is
// also the element count. A descending loop's "bound" is its *lower* limit,
// which is not an element count at all — offloading one under this model
// would map a zero- or near-zero-length region while the kernel still
// touches every element, out of bounds on the device. `parallel for` has no
// map() clause at all, so that objection does not apply: a descending
// CPU-threaded loop is plainly correct OpenMP, and this pass degrades a
// descending GpuOffload verdict to the CPU pragma instead of emitting
// nothing, provided the verdict's own CpuBeatsSeq says host threading is a
// target the cost model actually endorses. This is a stated substitution
// (see RewriteSummary::Skipped), never a silent one, and it is the *only*
// GPU decline that degrades — the macro-expansion and nested-loop declines
// below are about the rewrite itself, not about map() extents, and apply
// identically to both pragmas.
//
// ---------------------------------------------------------------------------
// What this pass declines, and why (see RewriteSummary::Skipped)
// ---------------------------------------------------------------------------
//
//   - Verdict isn't GpuOffload/CpuParallel, or wasn't Evaluated: not an
//     error, just not this pass's loop.
//   - The loop isn't written in the main file (e.g. reached through a header):
//     LoopCollector has no main-file filter, so this can happen even though no
//     current benchmark triggers it.
//   - The loop's location is a macro expansion: rewriting inside macro-
//     generated text is not well-defined with Rewriter's source-location
//     model.
//   - The loop is lexically nested inside a loop this pass already annotated:
//     nested target/parallel constructs are invalid or actively harmful
//     OpenMP in every combination (GPU-in-GPU, CPU-in-GPU, GPU-in-CPU,
//     CPU-in-CPU) — this guard is unified across both kinds.
//     RecursiveASTVisitor visits outer-before-inner, so the outer loop is
//     always offered first; the guard exists for soundness even though
//     nothing in benchmarks/ or tests/ currently nests annotated loops.
//   - A CpuParallel verdict with !CpuBeatsSeq (an internally inconsistent
//     verdict the analysis is not expected to produce, but declined by name
//     rather than trusted blindly): emitting `parallel for` here would be a
//     pragma the cost model itself contradicts.
//   - A descending GpuOffload verdict whose CpuBeatsSeq is also false: the
//     GPU pragma is unsound for this shape (see above) and the CPU fallback
//     isn't a target the model endorses either, so nothing is emitted.
//
// ---------------------------------------------------------------------------
// Known limitations (documented, not papered over — see NOTES.md)
// ---------------------------------------------------------------------------
//
//   - Output goes to a sibling `<name>.omp.c` file, never in place, so the
//     sequential baseline stays diffable (CLAUDE.md's repo-layout convention
//     for benchmarks/). There is no in-place mode.
//   - declare target is applied to whole function definitions found in the
//     main file; a callee defined in another translation unit or a system
//     header cannot be wrapped here and is reported as such, not silently
//     skipped.
//   - This pass does not attempt to compile or run the output — that's
//     scripts/build_and_run.sh's job.

#ifndef P05_CODEGEN_OMPREWRITER_H
#define P05_CODEGEN_OMPREWRITER_H

#include "analysis/CallResolver.h"
#include "analysis/LoopInfo.h"
#include "analysis/Profitability.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace p05 {

/// One loop that received a pragma.
struct RewriteSite {
  const clang::ForStmt *Loop = nullptr;

  /// The exact directive text emitted, for the report to echo back.
  std::string Pragma;

  unsigned Line = 0;

  /// GpuOffload or CpuParallel — never Sequential, a Sequential loop never
  /// reaches a RewriteSite at all. Lets the report, and finalize()'s
  /// declare-target nesting check, tell the two pragma kinds apart without
  /// re-parsing Pragma's text.
  OffloadTarget Kind = OffloadTarget::Sequential;
};

/// What the rewrite pass did, and — equally important for the report — what
/// it declined to touch and why.
struct RewriteSummary {
  std::vector<RewriteSite> Sites;

  /// Functions wrapped in declare target, deduplicated across every
  /// GPU-offload loop this pass annotated. CPU-threaded loops never
  /// contribute here — see the header comment.
  std::vector<const clang::FunctionDecl *> DeclareTargets;

  /// One human-readable line per loop this pass looked at but did not
  /// annotate, and why. Does not include loops that were never eligible
  /// (GpuOffload/CpuParallel, Evaluated) — only cases that reached this pass
  /// eligible and were still declined or degraded.
  std::vector<std::string> Skipped;
};

/// Applies ProfitabilityVerdicts to a Rewriter. One instance per translation
/// unit: offer it every loop via rewriteLoop, then call finalize() once.
class OmpRewriter {
public:
  OmpRewriter(clang::Rewriter &Rewrite, CallResolver &Resolver);

  /// No-op unless V.Evaluated && (V.Target == GpuOffload || CpuParallel).
  /// Safe to call once per loop, in any order relative to other loops.
  void rewriteLoop(const LoopInfo &LI, const ProfitabilityVerdict &V);

  /// Emits declare target/end declare target around every callee collected
  /// across all rewriteLoop calls so far (GPU-offload sites only). Call
  /// exactly once, after every loop in the translation unit has been
  /// offered.
  void finalize();

  const RewriteSummary &summary() const { return Summary; }

private:
  clang::Rewriter &Rewrite;
  CallResolver &Resolver;
  RewriteSummary Summary;

  /// Source ranges of loops already annotated (either kind), so a lexically
  /// nested loop can be detected and declined rather than producing invalid
  /// or harmful nested constructs.
  std::vector<clang::SourceRange> AnnotatedRanges;

  /// Callees pending declare target wrapping, collected across all
  /// GPU-offload rewriteLoop calls and applied once in finalize(). Canonical
  /// decls, deduplicated.
  llvm::DenseSet<const clang::FunctionDecl *> PendingDeclareTargets;

  /// Inserts the pragma text (already fully built) before LI's loop, sharing
  /// every location gate (main-file, macro, rewritable, lexical nesting) and
  /// the indentation/own-line logic between the GPU and CPU paths. Returns
  /// true on success; on failure appends a named reason to Summary.Skipped
  /// and returns false. Does not touch AnnotatedRanges or Summary.Sites —
  /// the caller does that on success, since the GPU path has more to do
  /// afterward (collectDeclareTargets) before the site is fully recorded.
  bool insertPragma(const LoopInfo &LI, const std::string &Pragma);

  void collectDeclareTargets(const LoopInfo &LI);
};

/// Human-readable report of what the rewrite pass did. Lives here rather than
/// the driver, matching printSafetyReport / printProfitabilityReport.
void printRewriteReport(llvm::raw_ostream &OS, const RewriteSummary &S);

} // namespace p05

#endif // P05_CODEGEN_OMPREWRITER_H
