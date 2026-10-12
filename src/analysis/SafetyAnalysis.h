// SafetyAnalysis — the Week 2 safety pass: turns CallResolver's reachability
// facts (Days 5-6) into a per-loop verdict on whether it is safe to
// parallelize at all.
//
// Two independent hazard sources are checked, both coarse and conservative by
// design (ROADMAP.md's Week 1 note: full points-to analysis is a
// research-grade problem, out of scope for a hackathon build):
//
//   1. Interprocedural: does any function reachable from the loop's callees
//      write through a pointer parameter or touch a global? (FunctionEffects,
//      SafetyAnalyzer::getEffects)
//   2. Intraprocedural, scalars: does the loop body reassign its own
//      induction variable? A scalar written by name is classified by
//      ScalarAnalysis.h (Phase 1.4) as private, lastprivate, a reduction, or
//      a genuine loop-carried value (UNSAFE); writing *part* of an outer
//      variable (`s.x = ...`) stays UNSAFE here.
//   3. Intraprocedural, arrays (Phase 1.3): can two different iterations
//      touch the same array element, at least one writing it? Decided by the
//      dependence tests in DependenceTest.h over AffineAccess.h's affine
//      subscripts: proven independent, proven dependent (UNSAFE), or not
//      disproved (UNKNOWN).
//
// (1) and (2) were open TODOs left in LoopInfo.h since Day 2; (3) replaced
// the hackathon rule "every array/pointer access must be indexed by exactly
// the induction variable", which rejected every 2-D nest, every shifted read
// (`a[i] = b[i + 1]`) and `*(p + i)`.
//
// (1) is call-site aware since Phase 1.5 (Summaries.h): each call's callee
// accesses are substituted with that call's arguments and join the loop's own
// accesses in (3). What stays here is what has no element to test: a callee
// with no visible body, recursion, writes to globals by name, atomics and
// asm. The pre-1.5 rule (a function that writes through *some* pointer
// parameter is flagged regardless of which argument any call passed) is kept
// behind -ipa=coarse as the ablation baseline.
//
// IMPORTANT — (3) does NOT cover aliasing *between distinct* pointer
// parameters. `void f(double *a, double *b, int n) { for (i) a[i] = b[i]
// + 1.0; }` verifies SAFE, but if the caller passes b == a + 1 (overlapping,
// shifted by one), the loop is really `a[i] = a[i+1] + 1.0` in disguise: a
// genuine cross-iteration hazard, invisible because accesses through two
// differently named pointers are never compared. Distinct pointer/array
// parameters are assumed not to alias (the promise C's `restrict` makes
// explicit, here made implicitly and unverified). Phase 1.6 makes it explicit
// (prove disjointness, or emit a runtime overlap check).
//
// Known, documented over-approximations (real safe patterns not marked SAFE;
// see NOTES.md):
//   - Calls are only as precise as Summaries.h's limits (globals in a
//     callee's subscripts, reassigned pointer parameters, arguments that
//     are not a named pointer plus an affine offset).
//   - Floating-point reductions are UNSAFE unless -fp-reductions is given
//     (they change the last bits of the result); globals, statics and
//     address-taken scalars are never privatized.
//   - Subscripts AffineAccess cannot express (indirect `a[idx[i]]`, products
//     of variables, triangular rows `a[i * n + j]` with `j < i`) and symbolic
//     distances that can't be decided make a loop UNKNOWN.

#ifndef P05_ANALYSIS_SAFETYANALYSIS_H
#define P05_ANALYSIS_SAFETYANALYSIS_H

#include "analysis/AffineAccess.h"
#include "analysis/CallResolver.h"
#include "analysis/LoopInfo.h"
#include "analysis/ScalarAnalysis.h"
#include "analysis/Summaries.h"

#include "clang/AST/Decl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace p05 {

/// Tri-state rather than a bool: UNKNOWN ("the analysis cannot see this far")
/// is kept distinct from UNSAFE ("a real hazard was found"), even though both
/// block parallelization identically downstream. Losing that distinction
/// would conflate "no visible body" with "no effects" — exactly the
/// conflation LoopInfo::HasIndirectCall and CallChainSummary::HasOpaqueCallee
/// were already careful never to make.
enum class SafetyVerdict {
  Safe,
  Unknown,
  Unsafe,
};

llvm::StringRef toString(SafetyVerdict V);

/// Side effects attributable to one function's own body (not its callees).
struct FunctionEffects {
  bool WritesGlobal = false;
  bool WritesThroughPointerParam = false;

  /// A write whose target could not be resolved to a root declaration (e.g.
  /// through a call's return value). Kept apart from the two flags above:
  /// this means "cannot see", not "found a hazard" — feeds into UNKNOWN, not
  /// UNSAFE, same distinction the verdict itself makes.
  bool HasUnknownWrite = false;

  /// File-scope (or extern) variables the function reads or writes by name.
  /// A loop that writes one of them while calling this function has a
  /// dependence the loop body alone does not show.
  llvm::SmallVector<const clang::VarDecl *, 4> GlobalsUsed;

  /// Reads or writes memory through a pointer it was handed or computed
  /// (a parameter, or a local pointer): what it touches depends on the
  /// arguments, so a pointer argument into an array the loop writes is a
  /// possible dependence.
  bool AccessesThroughPointer = false;

  /// One entry per flagged write site, in source order, e.g. "writes through
  /// pointer parameter 'out'" or "writes global 'call_count'". Empty when
  /// none of the flags above are set.
  std::vector<std::string> Reasons;

  bool hasHazard() const {
    return WritesGlobal || WritesThroughPointerParam || HasUnknownWrite;
  }
};

struct LoopSafety {
  SafetyVerdict Verdict = SafetyVerdict::Safe;

  /// Populated whenever Verdict != Safe; empty when Verdict == Safe.
  std::vector<std::string> Reasons;

  /// OpenMP data-sharing clauses for the scalars the loop writes, e.g.
  /// " private(j) reduction(+: sum)" (Phase 1.4); empty if none.
  std::string ScalarClauses;

  /// Phase 1.3's dependence-test tally (empty when the test did not run, e.g.
  /// for an unrecognized loop).
  std::string DependenceSummary;

  /// Every access is exactly `base[i]` on a base declared outside the loop:
  /// the only shape today's map(...[0:bound]) clauses size correctly. When
  /// false, profitability must not choose GPU offload (see Profitability.cpp)
  /// until Phase 2.1 sizes map regions from the affine ranges.
  bool GpuMappable = true;
};

/// Computes and memoizes FunctionEffects, and aggregates them (together with
/// CallResolver's reachability facts and a walk of the loop's own body) into
/// a per-loop verdict.
class SafetyAnalyzer {
public:
  explicit SafetyAnalyzer(CallResolver &Resolver) : Resolver(Resolver) {}

  /// Side effects of FD's own body. Memoized: several loops, or several
  /// functions' reachable sets, can ask about the same function.
  const FunctionEffects &getEffects(const clang::FunctionDecl *FD);

  /// Full verdict for one loop, given its accesses (AffineAnalyzer) and the
  /// classification of the scalars it writes (ScalarAnalyzer).
  ///
  /// Calls: what SummaryAnalyzer::composeCalls found (Phase 1.5), whose
  /// accesses are then already in Accesses. Null selects the pre-1.5
  /// call-site-independent rule (-ipa=coarse), kept for the ablation.
  LoopSafety analyzeLoop(const LoopInfo &LI, const LoopAccesses &Accesses,
                         const LoopScalars &Scalars,
                         const CallEffects *Calls = nullptr);

private:
  CallResolver &Resolver;
  llvm::DenseMap<const clang::FunctionDecl *, FunctionEffects> Cache;
};

/// Human-readable report for one loop's safety verdict. Lives here rather
/// than the driver so main.cpp stays wiring-only, mirroring printLoopReport
/// and printCallChains.
void printSafetyReport(llvm::raw_ostream &OS, const LoopSafety &Safety);

} // namespace p05

#endif // P05_ANALYSIS_SAFETYANALYSIS_H
