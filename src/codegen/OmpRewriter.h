// OmpRewriter — the Days 14-16 pass: turn a GPU_OFFLOAD verdict into text.
//
// Everything upstream of this file *decides*; nothing upstream *acts*. This is
// the first unit in src/codegen/, and its job is narrow on purpose: take a
// ProfitabilityVerdict this project's own analysis already computed, and
// render it as the exact OpenMP 4.5+ target-offload construct that verdict
// implies, via clang::Rewriter. It does not re-derive anything — the map
// clauses, the guard condition, the op counts, all come from
// ProfitabilityVerdict, because Days 11-13 built that struct specifically so
// codegen would not have to walk the loop body a second time to rediscover
// facts already established (see Profitability.h's comment on ArrayRegion).
//
// ---------------------------------------------------------------------------
// Scope: GPU_OFFLOAD only
// ---------------------------------------------------------------------------
//
// CPU_PARALLEL loops (`#pragma omp parallel for`) are Days 17-18's work, not
// today's — see DAY_BY_DAY.md. rewriteLoop is a no-op for anything other than
// an Evaluated GpuOffload verdict; SEQUENTIAL and CPU_PARALLEL loops, and
// anything the safety gate blocked, pass through untouched and are recorded
// in RewriteSummary::Skipped so the report says so rather than staying silent
// about it.
//
// ---------------------------------------------------------------------------
// The pragma
// ---------------------------------------------------------------------------
//
//   #pragma omp target teams distribute parallel for \
//       [if(target: <GuardExpr>)] map(<kind>: <base>[0:<extent>]) ...
//
// The map clauses are ProfitabilityVerdict::Regions rendered directly:
// mapKind() gives to/from/tofrom, ExtentText gives the section length. The
// `target:` directive-name modifier on `if` is required, not decorative: on a
// combined construct several constituent directives (target, parallel for)
// accept an `if` clause, and an unqualified `if()` is ambiguous about which
// one it gates. When GuardExpr is set and evaluates false at run time, this
// same combined construct still runs — on the host, as a threaded loop — so
// the guard is not just "skip the loop small", it *is* the CPU-parallel
// fallback for the guarded case, without a second pragma.
//
// ---------------------------------------------------------------------------
// declare target
// ---------------------------------------------------------------------------
//
// A target region can only call functions with a device-side compilation, so
// every function transitively reachable from an offloaded loop's callees
// (CallResolver::getReachable — the same walk ProfitabilityAnalyzer::getOpCount
// already makes) gets wrapped in `declare target` / `end declare target`.
// Without this, Day 15's build attempt fails immediately on the first call
// inside a target region, for a reason that has nothing to do with whether
// the offload decision itself was correct.
//
// ---------------------------------------------------------------------------
// What this pass declines, and why (see RewriteSummary::Skipped)
// ---------------------------------------------------------------------------
//
//   - Verdict isn't GpuOffload, or wasn't Evaluated: not an error, just not
//     this pass's loop.
//   - The loop isn't written in the main file (e.g. reached through a header):
//     LoopCollector has no main-file filter, so this can happen even though no
//     current benchmark triggers it.
//   - The loop's location is a macro expansion: rewriting inside macro-
//     generated text is not well-defined with Rewriter's source-location
//     model.
//   - The loop is lexically nested inside a loop this pass already annotated:
//     nested `target teams` is invalid OpenMP. RecursiveASTVisitor visits
//     outer-before-inner, so the outer loop is always offered first; this
//     guard exists for soundness even though nothing in benchmarks/ or
//     tests/ currently nests offloaded loops.
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
//   - This pass does not attempt to compile or run the output. Day 15's job.

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
};

/// What the rewrite pass did, and — equally important for the report — what
/// it declined to touch and why.
struct RewriteSummary {
  std::vector<RewriteSite> Sites;

  /// Functions wrapped in declare target, deduplicated across every loop this
  /// pass annotated.
  std::vector<const clang::FunctionDecl *> DeclareTargets;

  /// One human-readable line per loop this pass looked at but did not
  /// annotate, and why. Does not include loops that were never GpuOffload —
  /// only cases that reached this pass eligible and were still declined.
  std::vector<std::string> Skipped;
};

/// Applies ProfitabilityVerdicts to a Rewriter. One instance per translation
/// unit: offer it every loop via rewriteLoop, then call finalize() once.
class OmpRewriter {
public:
  OmpRewriter(clang::Rewriter &Rewrite, CallResolver &Resolver);

  /// No-op unless V.Evaluated && V.Target == OffloadTarget::GpuOffload.
  /// Safe to call once per loop, in any order relative to other loops.
  void rewriteLoop(const LoopInfo &LI, const ProfitabilityVerdict &V);

  /// Emits declare target/end declare target around every callee collected
  /// across all rewriteLoop calls so far. Call exactly once, after every loop
  /// in the translation unit has been offered.
  void finalize();

  const RewriteSummary &summary() const { return Summary; }

private:
  clang::Rewriter &Rewrite;
  CallResolver &Resolver;
  RewriteSummary Summary;

  /// Source ranges of loops already annotated, so a lexically nested loop can
  /// be detected and declined rather than producing invalid nested
  /// `target teams`.
  std::vector<clang::SourceRange> AnnotatedRanges;

  /// Callees pending declare target wrapping, collected across all
  /// rewriteLoop calls and applied once in finalize(). Canonical decls,
  /// deduplicated.
  llvm::DenseSet<const clang::FunctionDecl *> PendingDeclareTargets;

  void collectDeclareTargets(const LoopInfo &LI);
};

/// Human-readable report of what the rewrite pass did. Lives here rather than
/// the driver, matching printSafetyReport / printProfitabilityReport.
void printRewriteReport(llvm::raw_ostream &OS, const RewriteSummary &S);

} // namespace p05

#endif // P05_CODEGEN_OMPREWRITER_H
