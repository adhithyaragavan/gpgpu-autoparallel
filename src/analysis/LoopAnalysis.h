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
/// get their own record; the list is flat and in source order.
class LoopCollector : public clang::RecursiveASTVisitor<LoopCollector> {
public:
  explicit LoopCollector(clang::ASTContext &Ctx) : Ctx(Ctx) {}

  bool VisitForStmt(clang::ForStmt *FS);

  const std::vector<LoopInfo> &getLoops() const { return Loops; }

private:
  clang::ASTContext &Ctx;
  std::vector<LoopInfo> Loops;
};

/// Human-readable report for one loop. Lives here rather than in the driver so
/// that main.cpp stays pure wiring.
void printLoopReport(llvm::raw_ostream &OS, const LoopInfo &LI);

} // namespace p05

#endif // P05_ANALYSIS_LOOPANALYSIS_H
