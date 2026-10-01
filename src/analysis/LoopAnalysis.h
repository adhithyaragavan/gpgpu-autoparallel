// LoopAnalysis — classify `for` loops into the LoopKind taxonomy and pull out
// trip-count information where it is statically determinable.

#ifndef P05_ANALYSIS_LOOPANALYSIS_H
#define P05_ANALYSIS_LOOPANALYSIS_H

#include "analysis/LoopInfo.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace p05 {

/// Classify a single loop. Pure with respect to the AST — it reads and folds
/// constants but prints nothing and mutates nothing, so it can be driven from a
/// test harness or from the interprocedural walker without the visitor below.
LoopInfo analyzeForStmt(const clang::ForStmt *FS, clang::ASTContext &Ctx);

/// Collects one LoopInfo per ForStmt in the translation unit. Nested loops each
/// get their own record; the list is flat and in source (pre-)order, and each
/// record's nest facts (Parent, Children, Depth, NestIVs, ...) link it to the
/// others by index. See LoopInfo.h's "Nest facts" section.
class LoopCollector : public clang::RecursiveASTVisitor<LoopCollector> {
  using Base = clang::RecursiveASTVisitor<LoopCollector>;

public:
  explicit LoopCollector(clang::ASTContext &Ctx) : Ctx(Ctx) {}

  /// Overridden (rather than only VisitForStmt) so that the enclosing-loop
  /// stack covers exactly the loop's body: the init/cond/inc clauses are
  /// traversed before this loop is pushed, so a loop inside them is not
  /// recorded as its child.
  bool TraverseForStmt(clang::ForStmt *FS, DataRecursionQueue *Queue = nullptr);
  bool TraverseWhileStmt(clang::WhileStmt *WS,
                         DataRecursionQueue *Queue = nullptr);
  bool TraverseDoStmt(clang::DoStmt *DS, DataRecursionQueue *Queue = nullptr);
  /// A block literal's body runs when the block is invoked, not in place, so
  /// it starts a fresh nest (same rule as a called function).
  bool TraverseBlockExpr(clang::BlockExpr *BE,
                         DataRecursionQueue *Queue = nullptr);

  const std::vector<LoopInfo> &getLoops() const { return Loops; }

private:
  /// Classify FS and record it with its nest facts relative to OpenLoops.
  void recordLoop(const clang::ForStmt *FS);

  clang::ASTContext &Ctx;
  std::vector<LoopInfo> Loops;
  /// Indices of the for loops whose body is currently being traversed,
  /// outermost first.
  std::vector<int> OpenLoops;
  /// How many repeating non-body contexts (while/do loops, for-loop cond/inc
  /// clauses) currently enclose the traversal point.
  unsigned OpenRepeating = 0;
};

/// Human-readable report for one loop. Lives here rather than in the driver so
/// that main.cpp stays pure wiring. AllLoops is the collector's vector LI
/// belongs to, used to print the parent loop's location.
void printLoopReport(llvm::raw_ostream &OS, const LoopInfo &LI,
                     const std::vector<LoopInfo> &AllLoops);

} // namespace p05

#endif // P05_ANALYSIS_LOOPANALYSIS_H
