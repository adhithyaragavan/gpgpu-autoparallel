#include "codegen/OmpRewriter.h"

#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace p05 {

OmpRewriter::OmpRewriter(Rewriter &Rewrite, CallResolver &Resolver)
    : Rewrite(Rewrite), Resolver(Resolver) {}

namespace {

/// Strict "Loc is inside [Outer.begin, Outer.end]" test, inclusive of both
/// endpoints. Inclusive on the end deliberately: two ForStmts that share an
/// end location — the common brace-less `for (...) for (...) body;` nesting,
/// where the inner loop's closing token *is* the outer loop's closing token —
/// must still count as nested. A strict `<` here previously let that shape
/// through undetected, producing invalid doubly-nested `target teams`.
bool contains(const SourceManager &SM, SourceRange Outer, SourceLocation Loc) {
  return !SM.isBeforeInTranslationUnit(Loc, Outer.getBegin()) &&
        !SM.isBeforeInTranslationUnit(Outer.getEnd(), Loc);
}

bool isNestedInAny(const SourceManager &SM, SourceRange Range,
                   const std::vector<SourceRange> &Annotated) {
  return llvm::any_of(Annotated, [&](SourceRange Outer) {
    return contains(SM, Outer, Range.getBegin());
  });
}

/// Whether Loc is preceded on its own line only by whitespace — i.e.
/// whether a `#pragma` inserted right before it would legally start its own
/// line, as `#pragma` requires. `for` is not guaranteed to hold this
/// position (`if (c) for (...)`, or a single-line function body), so this is
/// checked rather than assumed.
bool startsOwnLine(const SourceManager &SM, SourceLocation Loc) {
  bool Invalid = false;
  const char *Data = SM.getCharacterData(Loc, &Invalid);
  if (Invalid)
    return false;
  const char *BufStart = SM.getBufferData(SM.getFileID(Loc)).data();
  for (const char *P = Data; P > BufStart; --P) {
    char C = P[-1];
    if (C == '\n')
      return true;
    if (C != ' ' && C != '\t')
      return false;
  }
  return true; // reached the start of the file
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
  if (Begin.isMacroID() || !Rewriter::isRewritable(Begin)) {
    skip("loop location is inside a macro expansion, declined");
    return;
  }
  // The mapped-region model (ArrayRegion, ExtentText — see Profitability.h)
  // assumes a loop counts up from a low index to the printed bound: extent
  // is derived from the bound text alone, on the documented assumption that
  // it is also the element count. A descending loop's "bound" is its *lower*
  // limit, which is not an element count at all — offloading one under this
  // model would map a zero- or near-zero-length region while the kernel
  // still touches every element, out of bounds on the device. Declined
  // rather than emitting a pragma the cost model's own data volume estimate
  // does not actually describe; extending the model to reversed traversal is
  // future work, not a Day 14 fix.
  if (LI.CmpOp == BO_GT || LI.CmpOp == BO_GE) {
    skip("loop counts down; the mapped-region model assumes an ascending "
        "loop, declined rather than emitting an incorrect map() extent");
    return;
  }
  SourceRange Range = LI.Loop->getSourceRange();
  if (isNestedInAny(SM, Range, AnnotatedRanges)) {
    skip("loop is lexically nested inside an already-offloaded loop, "
        "declined (nested target teams is invalid OpenMP)");
    return;
  }

  std::string Pragma = buildPragma(V);

  // Indent the pragma to line up with the `for` it precedes.
  unsigned Col = SM.getPresumedColumnNumber(Begin);
  std::string Indent(Col > 0 ? Col - 1 : 0, ' ');

  // `#pragma` must be the first token on its line. When `for` already is
  // (the common case), inserting right before it is enough. Otherwise — `if
  // (c) for (...)`, a single-line function body — a leading newline is
  // required to give the pragma a line of its own; the text already on that
  // line (the `if (c)`, the `{`) is left exactly where it was, just now
  // followed by the pragma on the next line instead of `for`.
  std::string InsertText = Pragma + "\n" + Indent;
  if (!startsOwnLine(SM, Begin))
    InsertText = "\n" + InsertText;

  if (Rewrite.InsertTextBefore(Begin, InsertText)) {
    skip("source location was not rewritable, declined");
    return;
  }

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

  // Deterministic, source-order processing: PendingDeclareTargets is a
  // DenseSet keyed on pointer identity, so iterating it directly makes the
  // report (and which "skipped: no visible definition" lines appear, when
  // several such lines exist) depend on pointer values rather than on the
  // program being analyzed.
  std::vector<const FunctionDecl *> Ordered(PendingDeclareTargets.begin(),
                                            PendingDeclareTargets.end());
  llvm::sort(Ordered, [&](const FunctionDecl *A, const FunctionDecl *B) {
    return SM.isBeforeInTranslationUnit(A->getBeginLoc(), B->getBeginLoc());
  });

  for (const FunctionDecl *FD : Ordered) {
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

    // A function that itself lexically contains one of today's offloaded
    // loops would, once wrapped, have a `target` construct executing inside
    // an active `target` region when called from another offloaded loop —
    // unspecified behavior under OpenMP, and invisible to a syntax-only
    // check because the nesting is dynamic (through a call), not lexical.
    SourceRange DefRange = Def->getSourceRange();
    auto ContainedSite = llvm::find_if(
        Summary.Sites, [&](const RewriteSite &Site) {
          return contains(SM, DefRange, Site.Loop->getBeginLoc());
        });
    if (ContainedSite != Summary.Sites.end()) {
      Summary.Skipped.push_back(
          Def->getNameAsString() + ": its own body contains an offloaded "
          "loop (line " + std::to_string(ContainedSite->Line) + "); "
          "declare target not emitted to avoid a target region invoked "
          "from inside another target region");
      continue;
    }

    SourceLocation BeginLoc = Def->getBeginLoc();
    SourceLocation EndLoc = Def->getEndLoc();
    if (!Rewriter::isRewritable(BeginLoc) || !Rewriter::isRewritable(EndLoc)) {
      Summary.Skipped.push_back(Def->getNameAsString() +
                                ": definition location is not rewritable "
                                "(likely macro-generated), declare target "
                                "not emitted");
      continue;
    }

    // Pre-validated above, so both inserts are expected to succeed; still
    // checked rather than ignored, so a failure is reported instead of
    // silently leaving an unbalanced declare target region in the output.
    bool BeginFailed =
        Rewrite.InsertTextBefore(BeginLoc, "#pragma omp declare target\n");
    bool EndFailed = Rewrite.InsertTextAfterToken(
        EndLoc, "\n#pragma omp end declare target");
    if (BeginFailed || EndFailed) {
      Summary.Skipped.push_back(
          Def->getNameAsString() +
          ": rewrite failed after validation, declare target not emitted "
          "(output may be incomplete for this function — do not compile "
          "without checking)");
      continue;
    }
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
