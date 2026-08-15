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

  /// A call in the body whose callee could not be resolved statically (a call
  /// through a function pointer). Tracked separately rather than dropped: an
  /// unresolvable call must block parallelization later, so losing it silently
  /// would be a soundness hole.
  bool HasIndirectCall = false;

  /// Why Kind == Unrecognized. Empty otherwise.
  std::string UnrecognizedReason;

  // --- Reporting -------------------------------------------------------------

  std::string File;
  unsigned Line = 0;
  unsigned Col = 0;
};

// TODO(week2, safety): none of this verifies that the induction variable is
// left alone by the loop body. `for (int i = 0; i < n; i++) { ...; i = 0; }`
// classifies as a clean VariableBound loop here. That check is body analysis
// and belongs with the safety pass — but it is a hard prerequisite for emitting
// any pragma, so it must land before codegen does.
//
// TODO(week2, safety): unsigned wraparound and signed-overflow semantics of the
// bound are ignored; trip counts assume the loop terminates normally.
//
// TODO: WhileStmt and DoStmt are not classified at all. Every benchmark loop we
// care about is a ForStmt, so this is a deliberate scope cut, not an oversight.

} // namespace p05

#endif // P05_ANALYSIS_LOOPINFO_H
