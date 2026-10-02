// DependenceTest — Phase 1.3: can two different iterations of a loop touch
// the same array element, with at least one of them writing it?
//
// Input is AffineAccess's per-loop access list. For every pair of accesses to
// the same base where at least one writes (each writing access is also paired
// with itself), the subscripts of the two accesses are equated dimension by
// dimension, with the analyzed loop's variable as two different unknowns i1
// != i2, each inner loop's variable as two independent unknowns, and
// symbols / enclosing loops' variables shared (they are fixed while the loop
// runs). The pair is independent if, in either iteration order (i1 < i2 or
// i1 > i2), some dimension has no solution. Tests, cheapest first:
//
//   ZIV      no loop variable in the dimension: the two sides are fixed and
//            must be provably different.
//   GCD      the gcd of all coefficients must divide the constant term.
//   Strong   a*i + c1 vs a*i + c2 (no inner variables): an exact iteration
//   SIV      distance (c2 - c1) / a, which must be a nonzero multiple of the
//            step and smaller than the iteration span.
//   Banerjee the extreme values of the equation's left-hand side over the
//            variables' ranges must straddle zero. Ranges are affine in
//            symbols (`0 <= i <= n - 1`), and the sign of a symbolic extreme
//            is decided using the fact that the loop has at least two
//            iterations (so `a[i]` vs `a[i + n]` with `i < n` is independent).
//
// Outcome per pair, and for the loop as the worst over its pairs:
//   Independent  proven by one of the tests above;
//   Dependent    proven to exist (an exact distance, or the same element on
//                every iteration): the loop is UNSAFE;
//   Unknown      neither proven nor disproven (non-affine subscripts, unknown
//                bases, symbolic terms that don't cancel, ...): UNKNOWN.
//
// Assumptions inherited from AffineAccess (no signed overflow, strict
// aliasing) plus the pre-existing one this phase does not yet remove:
// accesses through two DIFFERENTLY NAMED pointers never overlap (the
// `restrict` promise, made implicitly; Phase 1.6 makes it explicit). Ranges
// treat every integer in [Lo, Hi] as a possible value, ignoring the step,
// which only adds solutions, so it is sound for proving independence.

#ifndef P05_ANALYSIS_DEPENDENCETEST_H
#define P05_ANALYSIS_DEPENDENCETEST_H

#include "analysis/AffineAccess.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p05 {

enum class DepOutcome { Independent, Unknown, Dependent };

struct LoopDependence {
  /// Worst outcome over all pairs (Independent when there are none).
  DepOutcome Outcome = DepOutcome::Independent;
  /// One line per Dependent / Unknown pair (or unanalyzable access).
  std::vector<std::string> Reasons;

  unsigned PairsTested = 0;
  unsigned IndependentByZIV = 0, IndependentByGCD = 0, IndependentBySIV = 0,
           IndependentByBanerjee = 0;
  /// Pairs skipped because their base is declared inside the loop (private
  /// to each iteration).
  unsigned PrivatePairsSkipped = 0;

  /// The loop runs at most one iteration (constant span smaller than the
  /// step), so no dependence can be carried.
  bool SingleIteration = false;

  /// Every access is a 1-D subscript that is exactly the loop's own variable
  /// on a named, non-substituted base declared outside the loop: the only
  /// shape today's `map(... [0:bound])` clauses size correctly. False means
  /// GPU offload must be declined until map regions are computed from the
  /// affine ranges (Phase 2.1); CPU threading needs no maps.
  bool GpuMappable = true;
};

/// RootStep: the analyzed loop's step (LoopInfo::Step), used to reject
/// distances that are not a multiple of it.
LoopDependence testLoopDependences(const LoopAccesses &A, int64_t RootStep);

/// One summary line, e.g. "4 pair(s): 4 independent (2 SIV, 1 GCD, 1
/// Banerjee)".
std::string dependenceSummary(const LoopDependence &D);

} // namespace p05

#endif // P05_ANALYSIS_DEPENDENCETEST_H
