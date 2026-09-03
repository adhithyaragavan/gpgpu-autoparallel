#include "codegen/OmpRewriter.h"

#include "clang/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace p05 {

OmpRewriter::OmpRewriter(Rewriter &Rewrite, CallResolver &Resolver)
    : Rewrite(Rewrite), Resolver(Resolver) {}

namespace {

/// Whether Range is lexically contained in any range already annotated —
/// both endpoints ordered by SourceManager, which is what "lexically nested"
/// means for two ForStmts in the same file.
bool isNestedInAny(const SourceManager &SM, SourceRange Range,
                   const std::vector<SourceRange> &Annotated) {
  for (const SourceRange &Outer : Annotated) {
    bool StartsAfterOuterBegin =
        !SM.isBeforeInTranslationUnit(Range.getBegin(), Outer.getBegin());
    bool EndsBeforeOuterEnd =
        SM.isBeforeInTranslationUnit(Range.getEnd(), Outer.getEnd());
    if (StartsAfterOuterBegin && EndsBeforeOuterEnd)
      return true;
  }
  return false;
}

std::string buildPragma(const ProfitabilityVerdict &V) {
  std::string Pragma = "#pragma omp target teams distribute parallel for";
  if (V.GuardExpr)
    Pragma += " if(target: " + *V.GuardExpr + ")";
  for (const ArrayRegion &R : V.Regions) {
    if (!R.Base)
      continue;
    Pragma += " map(" + R.mapKind().str() + ": " +
              R.Base->getNameAsString() + "[0:" + R.ExtentText + "])";
  }
  return Pragma;
}

} // namespace

void OmpRewriter::rewriteLoop(const LoopInfo &LI,
                              const ProfitabilityVerdict &V) {
  if (!V.Evaluated || V.Target != OffloadTarget::GpuOffload)
    return; // not this pass's loop — not an error, just out of scope today.

  const SourceManager &SM = Rewrite.getSourceMgr();
  SourceLocation Begin = LI.Loop->getBeginLoc();

  auto skip = [&](llvm::StringRef Reason) {
    Summary.Skipped.push_back("line " + std::to_string(LI.Line) + ": " +
                              Reason.str());
  };

  if (!SM.isWrittenInMainFile(Begin)) {
    skip("loop is not written in the main file, declined");
    return;
  }
  if (Begin.isMacroID()) {
    skip("loop location is inside a macro expansion, declined");
    return;
  }
  SourceRange Range = LI.Loop->getSourceRange();
  if (isNestedInAny(SM, Range, AnnotatedRanges)) {
    skip("loop is lexically nested inside an already-offloaded loop, "
        "declined (nested target teams is invalid OpenMP)");
    return;
  }

  std::string Pragma = buildPragma(V);

  // Indent the pragma to line up with the `for` it precedes, rather than
  // jamming it to column 0.
  unsigned Col = SM.getPresumedColumnNumber(Begin);
  std::string Indent(Col > 0 ? Col - 1 : 0, ' ');

  Rewrite.InsertTextBefore(Begin, Pragma + "\n" + Indent);

  AnnotatedRanges.push_back(Range);
  Summary.Sites.push_back(RewriteSite{LI.Loop, Pragma, LI.Line});

  collectDeclareTargets(LI);
}

void OmpRewriter::collectDeclareTargets(const LoopInfo &LI) {
  for (const FunctionDecl *Callee : LI.Callees) {
    if (Callee->hasBody())
      PendingDeclareTargets.insert(Callee->getCanonicalDecl());
    CallChainSummary CCS = Resolver.getReachable(Callee);
    for (const ResolvedCallee &RC : CCS.Reachable)
      if (RC.HasVisibleBody)
        PendingDeclareTargets.insert(RC.Callee->getCanonicalDecl());
  }
}

void OmpRewriter::finalize() {
  const SourceManager &SM = Rewrite.getSourceMgr();

  for (const FunctionDecl *FD : PendingDeclareTargets) {
    const FunctionDecl *Def = FD->getDefinition();
    if (!Def) {
      Summary.Skipped.push_back(FD->getNameAsString() +
                                ": no visible definition, cannot wrap in "
                                "declare target");
      continue;
    }
    if (!SM.isWrittenInMainFile(Def->getBeginLoc())) {
      Summary.Skipped.push_back(
          Def->getNameAsString() +
          ": definition is outside the main file, declare target not "
          "emitted (Day 15 will need it available to the device build some "
          "other way)");
      continue;
    }

    Rewrite.InsertTextBefore(Def->getBeginLoc(),
                             "#pragma omp declare target\n");
    Rewrite.InsertTextAfterToken(Def->getEndLoc(),
                                 "\n#pragma omp end declare target");
    Summary.DeclareTargets.push_back(Def);
  }
}

void printRewriteReport(llvm::raw_ostream &OS, const RewriteSummary &S) {
  OS << "rewrite: " << S.Sites.size() << " loop(s) annotated";
  if (!S.DeclareTargets.empty())
    OS << ", " << S.DeclareTargets.size() << " function(s) marked declare target";
  OS << "\n";
  for (const RewriteSite &Site : S.Sites)
    OS << "  line " << Site.Line << ": " << Site.Pragma << "\n";
  for (const FunctionDecl *FD : S.DeclareTargets)
    OS << "  declare target: " << FD->getNameAsString() << "\n";
  for (const std::string &Reason : S.Skipped)
    OS << "  skipped: " << Reason << "\n";
}

} // namespace p05
