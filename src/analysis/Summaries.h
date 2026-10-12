// Summaries — Phase 1.5: call-site-aware interprocedural summaries.
//
// Replaces the call-site-independent rule ("a callee that writes through
// *some* pointer parameter makes every loop calling it UNSAFE") with one
// question asked per call: which elements of the caller's arrays does THIS
// call touch?
//
// A function's summary is the list of memory accesses its body makes (its
// own, and those of everything it calls), each in AffineAccess's form but in
// the function's own terms:
//   - the base is one of its pointer parameters, a global or static object
//     (globals are implicit parameters: the same object in every caller), or
//     unknown (null) when the function can't name what it touches;
//   - each subscript is affine in its integer parameters and in the
//     variables of loops inside it (with their ranges, also in terms of the
//     parameters), or non-affine with a reason.
// Summaries are built bottom-up on demand (a callee's before its caller's)
// and memoized. A function on a call-graph cycle, or one that reaches a
// function with no visible body, has an incomplete summary: the loop-level
// call-chain walk in SafetyAnalysis reports why, and the loop is UNKNOWN.
//
// At a call site the callee's parameters are replaced by the call's
// arguments (CallSite, recorded by AffineAccess in the caller's terms):
// `helper(&a[i])` with helper writing `p[0]` becomes a write to `a[i]`, and
// `row_op(&a[i * n], n)` with row_op writing `row[j]`, 0 <= j < n, becomes a
// write to `a[i][j]` of rows of length n. The resulting accesses are appended
// to the loop's own, and the ordinary dependence tests (DependenceTest.h)
// decide; nothing is decided here. The callee's loop variables become inner
// variables of the loop, with their ranges substituted the same way.
//
// Sound in the conservative direction: anything not expressible stays
// visible as a non-affine access or an unknown base (UNKNOWN), never
// disappears. Dropped are only accesses to a callee's own automatic objects
// (fresh on every call). Limits, each costing precision only:
//   - a callee subscript or loop range that mentions a global variable is
//     non-affine at the loop (the loop could change it between calls);
//   - a pointer parameter the callee reassigns or lets escape, a global
//     pointer, or a local pointer that can't be resolved is an unknown base;
//   - a call without a prototype (K&R), or an argument whose type is not
//     exactly the parameter's, gives the callee nothing to substitute;
//   - arguments must be a named pointer/1-D array plus an affine offset, a
//     row start `&a[X * N]`, or `&v` for a whole non-array, non-pointer v;
//   - a row-start argument composes only with a 1-D callee subscript
//     `j + c` over a callee loop that provably stays inside the row.
// Inherited unchanged: distinct pointer parameters are assumed not to alias
// (Phase 1.6), and AffineAccess's no-overflow and strict-aliasing
// assumptions.

#ifndef P05_ANALYSIS_SUMMARIES_H
#define P05_ANALYSIS_SUMMARIES_H

#include "analysis/AffineAccess.h"

#include "clang/AST/Decl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <memory>
#include <string>
#include <vector>

namespace p05 {

/// -ipa: Summaries (default) or the pre-1.5 call-site-independent rule,
/// kept as an ablation baseline.
enum class IpaMode { Summaries, Coarse };
IpaMode ipaModeFromFlags();

/// A hazard inside a function's own body, kept with the function that has
/// it so reasons can name the chain ("'f' transitively reaches 'g', which
/// ...").
struct SummaryReason {
  const clang::FunctionDecl *Where = nullptr;
  std::string Text;
  bool Unsafe = false; ///< else UNKNOWN
};

struct FunctionSummary {
  std::vector<ArrayAccess> Accesses;
  /// Ranges of the loop variables the accesses mention (this function's
  /// loops and its callees').
  llvm::DenseMap<const clang::VarDecl *, IVRange> Ranges;
  std::vector<SummaryReason> Reasons;
  /// Reaches a function with no visible body, or recurses.
  bool Incomplete = false;
};

/// What composing a loop's calls found, besides the accesses it appended.
struct CallEffects {
  std::vector<std::string> UnsafeReasons, UnknownReasons;
  /// Loop callees whose summary is incomplete.
  std::vector<const clang::FunctionDecl *> Incomplete;
};

class SummaryAnalyzer {
public:
  explicit SummaryAnalyzer(AffineAnalyzer &Affine) : Affine(Affine) {
    Recursive.Incomplete = true;
  }

  const FunctionSummary &summarize(const clang::FunctionDecl *FD);

  /// Appends to A the accesses of every call in it (with a visible callee),
  /// in the loop's terms, and the callees' loop-variable ranges; drops the
  /// `&a[..]` accesses that only pass an address to such a call (the
  /// composed accesses replace them).
  CallEffects composeCalls(LoopAccesses &A);

private:
  AffineAnalyzer &Affine;
  llvm::DenseMap<const clang::FunctionDecl *, std::unique_ptr<FunctionSummary>>
      Cache;
  llvm::DenseSet<const clang::FunctionDecl *> InProgress;
  FunctionSummary Recursive; ///< returned for a call back into InProgress
};

} // namespace p05

#endif // P05_ANALYSIS_SUMMARIES_H
