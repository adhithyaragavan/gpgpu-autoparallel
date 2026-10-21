// LoopInfo — the structured result of classifying one `for` loop.
//
// This is deliberately a plain aggregate with no Clang printing logic in it: the
// later pipeline stages (interprocedural safety analysis, the GPU-profitability
// heuristic, and codegen) all consume these records, and none of them should have
// to re-walk the AST to recover facts we already established here.

#ifndef P05_ANALYSIS_LOOPINFO_H
#define P05_ANALYSIS_LOOPINFO_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p05 {

/// How well a loop matches the canonical counted-loop shape
/// `for (i = start; i < bound; i += step)`.
enum class LoopKind {
  /// Bound is an integer constant expression, so the upper limit is known at
  /// compile time. Note this says nothing about whether the *trip count* is
  /// known — see LoopInfo::TripCount.
  ConstantBound,

  /// Canonical shape, but the bound is not statically known (a parameter, a
  /// local, or an expression over them). Still parallelizable; the profitability
  /// heuristic will need a runtime trip-count guard.
  VariableBound,

  /// Does not match a simple canonical for-loop. This is a real classification,
  /// not a fallback: flagging these is what stops us silently mishandling them
  /// downstream. See LoopInfo::UnrecognizedReason.
  Unrecognized,
};

llvm::StringRef toString(LoopKind Kind);

/// -linked-counters (Phase 2.5; default on).
bool linkedCountersFromFlags();

/// Phase 2.5: a second counter the header declares and steps alongside the
/// induction variable (`for (int iOff = M * b, ii = 0; ii < n; ii++, iOff++)`).
/// Nothing but the header may write it, so at every point of the body it
/// equals Init + (Step / induction step) * (induction variable - start).
struct LinkedCounter {
  const clang::VarDecl *Var = nullptr;
  const clang::Expr *Init = nullptr;
  int64_t Step = 0; ///< a multiple of the induction variable's step
};

struct LoopInfo {
  const clang::ForStmt *Loop = nullptr;

  LoopKind Kind = LoopKind::Unrecognized;

  // --- Canonical-shape facts -------------------------------------------------
  //
  // InductionVar may be set even when Kind == Unrecognized (e.g. the init clause
  // parsed fine but the condition did not). Always check Kind before trusting
  // the shape facts as a group.

  const clang::VarDecl *InductionVar = nullptr;

  /// Initial value of the induction variable, when it is an integer constant.
  std::optional<int64_t> Start;

  /// Signed per-iteration delta: +1 for `i++`, -4 for `i -= 4`.
  std::optional<int64_t> Step;

  /// Phase 2.5: the other counters the header declares and steps, in
  /// declaration order. Such a loop is not in OpenMP's canonical form, so it
  /// never gets a pragma itself; the loops around it can.
  std::vector<LinkedCounter> Linked;

  /// Comparison in the condition clause. Only meaningful when Kind is
  /// ConstantBound or VariableBound; BO_Comma is used as an "unset" marker
  /// because it can never legitimately appear here.
  clang::BinaryOperatorKind CmpOp = clang::BO_Comma;

  // --- ConstantBound ---------------------------------------------------------

  /// The folded value of the bound expression.
  std::optional<int64_t> ConstBound;

  // --- VariableBound ---------------------------------------------------------

  const clang::Expr *BoundExpr = nullptr;

  /// Set only when the bound is a bare reference to a variable or parameter,
  /// so later stages can reason about it symbolically rather than textually.
  const clang::ValueDecl *BoundDecl = nullptr;

  /// Printed source text of the bound: "n", "n - 1".
  std::string BoundText;

  /// True when BoundDecl is a function parameter. Worth distinguishing: a
  /// parameter bound is a natural place to hang a runtime offload guard.
  bool BoundIsParameter = false;

  // --- Derived ---------------------------------------------------------------

  /// Exact iteration count. Set only when Start, ConstBound and Step are *all*
  /// known, so a ConstantBound loop can still have no trip count here (e.g.
  /// `for (int i = start; i < 1000; i++)`). Consumers must check the optional
  /// rather than infer a count from Kind.
  std::optional<int64_t> TripCount;

  // --- Body facts ------------------------------------------------------------

  /// Direct callees appearing anywhere in the loop body, in source order,
  /// deduplicated. This is the hook the interprocedural pass builds on.
  std::vector<const clang::FunctionDecl *> Callees;

  /// Phase 2.5: in a function imported from another input (CrossTU.h):
  /// kept for the summaries of the loops that call it, but analysed,
  /// reported and rewritten in its own file only.
  bool Foreign = false;

  /// Phase 2.5: callees with no memory effects (isKnownFunction), in source
  /// order, deduplicated; never in Callees.
  std::vector<const clang::FunctionDecl *> KnownCallees;

  /// A call in the body whose callee could not be resolved statically (a call
  /// through a function pointer). Tracked separately rather than dropped: an
  /// unresolvable call must block parallelization later, so losing it silently
  /// would be a soundness hole.
  bool HasIndirectCall = false;

  /// Why Kind == Unrecognized. Empty otherwise.
  std::string UnrecognizedReason;

  /// The expression the induction variable starts from (the initializer, or
  /// the right-hand side of the init assignment). Set whenever the init clause
  /// parsed, even if Start did not fold to a constant.
  const clang::Expr *StartExpr = nullptr;

  // --- Nest facts (Phase 1.1) -------------------------------------------------
  //
  // Filled in by LoopCollector, not by analyzeForStmt: they describe where a
  // loop sits relative to the other loops in the translation unit, which a
  // single ForStmt cannot know on its own. Nesting is lexical and counts only
  // loops in an enclosing loop's *body*. A loop in an enclosing loop's
  // init/cond/inc clause (possible via a GNU statement expression) is not a
  // child, and neither is a loop inside a function the body calls.
  //
  // Parent and Children are indices into LoopCollector's flat vector, not
  // pointers: the vector reallocates while it is being filled. Loops are
  // recorded in pre-order, so Parent is always less than the loop's own index.

  /// Index of the innermost enclosing for loop, or -1 for an outermost loop.
  int Parent = -1;

  /// Indices of the for loops directly inside this loop's body, in source order.
  /// A loop inside a block literal (`^{ ... }`, -fblocks) in the body is not a
  /// child: like a called function, it runs whenever the block is invoked.
  std::vector<int> Children;

  /// Number of enclosing for loops: 0 for an outermost loop.
  unsigned Depth = 0;

  /// Induction variables of the nest down to and including this loop,
  /// outermost first. Only recognized loops contribute: an enclosing
  /// Unrecognized loop's variable cannot be trusted as an induction variable
  /// (see EnclosedByUnrecognized), and this loop's own InductionVar is appended
  /// only when this loop is itself recognized. This is the variable vector
  /// subscripts become affine functions of in Phase 1.2.
  std::vector<const clang::VarDecl *> NestIVs;

  /// Some enclosing for loop is Unrecognized. Its variable is missing from
  /// NestIVs, so dependence analysis must treat any variable it updates as an
  /// unknown symbol, never as a well-behaved induction variable.
  bool EnclosedByUnrecognized = false;

  /// This loop's body is exactly one for statement and nothing else, looking
  /// through single-statement compound bodies (`{ { for } }`) and attribute
  /// wrappers (`#pragma clang loop ...`, which Clang represents as an
  /// AttributedStmt). A label in front of the inner loop makes the nest
  /// imperfect: a goto could enter it. So this loop and that child
  /// form a perfect two-level nest: the shape `collapse(2)` needs. A property
  /// of this (outer) loop, not of the child.
  bool PerfectlyNested = false;

  /// The start or bound expression refers to an enclosing loop's induction
  /// variable (e.g. `for (j = 0; j < i; j++)`): a triangular/non-rectangular
  /// nest. Only enclosing *recognized* loops' variables are checked, and a
  /// start or bound that folds to a compile-time constant is never flagged
  /// (`j < sizeof(a[i])` mentions i but cannot depend on it). Whether
  /// the bound depends on some other variable the outer body modifies is a
  /// loop-invariance question for Phase 1.2, not answered here.
  bool BoundsDependOnOuterIV = false;

  /// Lexically inside some repeating context that is not an enclosing for
  /// loop's body: a while or do loop (body or condition), or a for loop's
  /// condition or increment clause (re-evaluated every iteration; reachable
  /// via a GNU statement expression). Such a loop can run many times even when
  /// Depth == 0, which Phase 2's data-region hoisting must know. A for loop's
  /// init clause runs once, so it does not count. This does not affect
  /// whether this loop's own iterations are independent.
  bool InsideRepeatingContext = false;

  // --- Reporting -------------------------------------------------------------

  std::string File;
  unsigned Line = 0;
  unsigned Col = 0;
};

// Whether the loop body leaves the induction variable alone, and whether it
// carries a dependence across iterations (an array/pointer access not
// indexed by exactly the induction variable, or a scalar written outside the
// set of variables declared fresh in the body) is not this file's concern —
// that is body analysis, and lives in the Week 2 safety pass
// (src/analysis/SafetyAnalysis.h, SafetyAnalyzer::analyzeLoop). A LoopInfo
// alone is a shape classification, not a safety verdict; consumers must run
// it through SafetyAnalyzer before trusting it enough to emit a pragma.
//
// TODO(week2, safety): unsigned wraparound and signed-overflow semantics of the
// bound are ignored; trip counts assume the loop terminates normally.
//
// TODO: WhileStmt and DoStmt are not classified at all. Every benchmark loop we
// care about is a ForStmt, so this is a deliberate scope cut, not an oversight.
// They are only tracked as enclosing context (LoopInfo::InsideRepeatingContext).

} // namespace p05

#endif // P05_ANALYSIS_LOOPINFO_H
