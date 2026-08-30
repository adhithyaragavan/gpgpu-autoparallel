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
//   2. Intraprocedural: does the loop body itself reassign its own induction
//      variable, or carry a dependence across iterations — an array/pointer
//      access not indexed by exactly the induction variable, or a scalar
//      written outside the set of variables declared fresh inside the body?
//      (SafetyAnalyzer::analyzeLoop's body walk)
//
// Both were open TODOs left in LoopInfo.h since Day 2, explicitly deferred to
// "the Week 2 safety pass" — this is it, and closing them here (rather than
// leaving them for codegen) is what makes a SAFE verdict actually mean safe
// to parallelize, not just "the callees are pure".
//
// Deliberately call-site independent for (1): a function that writes through
// *some* pointer parameter is flagged regardless of which argument any given
// call site passed for it. That is what lets this stay coarse without alias
// analysis between call sites — precision traded for soundness, in the
// conservative direction only (more false UNSAFE, never false SAFE).
//
// The dependence rule for (2) is deliberately strict: every array/pointer
// access in the loop body, read or write alike, must be indexed by exactly
// the induction variable. This is sound *without* alias analysis for
// accesses through the *same* base object: two references to the same array
// parameter can never collide across iterations, because element i and
// element i' of one object are the same location only when i == i'.
//
// IMPORTANT — this does NOT cover aliasing *between distinct* pointer
// parameters. `void f(double *a, double *b, int n) { for (i) a[i] = b[i]
// + 1.0; }` verifies SAFE by this rule, but if the caller passes b == a + 1
// (overlapping, shifted by one), the loop is really `a[i] = a[i+1] + 1.0` in
// disguise: iteration k reads a[k+1], and iteration k+1 later writes a[k+1]
// — a genuine cross-iteration hazard this check cannot see, because it never
// reasons about whether two *different* array parameters might alias. That
// is an unstated assumption this whole layer rests on, not a proven
// guarantee: distinct pointer/array parameters are assumed not to alias
// (the same promise C's `restrict` keyword makes explicit, here made
// implicitly and unverified). Verifying it for real is exactly the
// points-to analysis ROADMAP.md calls out as out of scope.
//
// Known, documented over-approximations (real safe patterns marked UNSAFE
// rather than the reverse — see NOTES.md for the write-up):
//   - `helper(&a[i])` where helper writes through its pointer parameter is
//     flagged UNSAFE even though it is actually fine — the call-site-
//     independence trade above, working as designed.
//   - A local variable assigned a pointer returned from a call, then written
//     through, is not traced back to what it points to.
//   - `*(p + i) = x` (raw pointer arithmetic, not `p[i]`) is not recognized
//     as induction-variable-indexed and is conservatively flagged.
//   - Reductions (`sum += a[i]`) are flagged UNSAFE rather than recognized as
//     a parallelizable pattern.
//   - Any 2D-style access (`m[i][j]`, `a[i * cols + j]`) inside a loop nested
//     over i then j is flagged UNSAFE: the outer subscript's index is j
//     (or an affine expression), not the bare induction variable i of the
//     loop being judged, and this layer checks every access against a single
//     loop's own induction variable with no notion of "belongs to an inner
//     loop nested inside this one." No 2D loop nest can currently verify
//     SAFE — a real gap for any future matrix-shaped benchmark, not fixed
//     here because none of the three current demo benchmarks are 2D.

#ifndef P05_ANALYSIS_SAFETYANALYSIS_H
#define P05_ANALYSIS_SAFETYANALYSIS_H

#include "analysis/CallResolver.h"
#include "analysis/LoopInfo.h"

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

  /// Full verdict for one loop.
  LoopSafety analyzeLoop(const LoopInfo &LI);

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
