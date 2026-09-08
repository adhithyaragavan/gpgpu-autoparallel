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
/// through undetected, producing invalid doubly-nested target teams.
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

/// #pragma omp target teams distribute parallel for [if(target: ...)]
/// map(...) ... — see the header comment for why `target:` is required on
/// the if() rather than bare.
std::string buildGpuPragma(const ProfitabilityVerdict &V) {
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

/// #pragma omp parallel for [if(parallel: ...)] — no map() clauses (shared
/// memory) and no private()/reduction() clauses; see the header comment for
/// why neither is needed. `parallel:` on the if() is required for the same
/// ambiguity reason as the GPU path's `target:`: `parallel for` is a combined
/// construct whose constituent `parallel` directive is the one that accepts
/// `if`, and an unqualified `if()` doesn't say so explicitly.
std::string buildCpuPragma(const ProfitabilityVerdict &V) {
  std::string Pragma = "#pragma omp parallel for";
  if (V.CpuGuardExpr)
    Pragma += " if(parallel: " + *V.CpuGuardExpr + ")";
  return Pragma;
}

} // namespace

bool OmpRewriter::insertPragma(const LoopInfo &LI, const std::string &Pragma) {
  const SourceManager &SM = Rewrite.getSourceMgr();
  SourceLocation Begin = LI.Loop->getBeginLoc();

  auto skip = [&](llvm::StringRef Reason) {
    Summary.Skipped.push_back("line " + std::to_string(LI.Line) + ": " +
                              Reason.str());
  };

  if (!SM.isWrittenInMainFile(Begin)) {
    skip("loop is not written in the main file, declined");
    return false;
  }
  if (Begin.isMacroID() || !Rewriter::isRewritable(Begin)) {
    skip("loop location is inside a macro expansion, declined");
    return false;
  }
  SourceRange Range = LI.Loop->getSourceRange();
  if (isNestedInAny(SM, Range, AnnotatedRanges)) {
    skip("loop is lexically nested inside an already-annotated loop, "
        "declined (nested target/parallel constructs are invalid or "
        "harmful OpenMP in every combination)");
    return false;
  }

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
    return false;
  }
  return true;
}

void OmpRewriter::rewriteLoop(const LoopInfo &LI,
                              const ProfitabilityVerdict &V) {
  if (!V.Evaluated)
    return; // never priced at all — not this pass's loop.

  auto skip = [&](llvm::StringRef Reason) {
    Summary.Skipped.push_back("line " + std::to_string(LI.Line) + ": " +
                              Reason.str());
  };

  if (V.Target == OffloadTarget::GpuOffload) {
    // See the mapped-region-model comment in Profitability.h and this
    // file's header comment: a descending loop's printed bound is a lower
    // limit, not an element count, so the map() extent this verdict's
    // Regions carry does not describe it. `parallel for` has no map()
    // clause, so degrade to the CPU pragma instead of emitting a pragma
    // whose data-mapping is wrong — but only when the verdict's own numbers
    // still endorse host threading as a target; otherwise there is nothing
    // sound left to emit.
    if (LI.CmpOp == BO_GT || LI.CmpOp == BO_GE) {
      if (!V.CpuBeatsSeq) {
        skip("loop counts down; the mapped-region model assumes an "
            "ascending loop, and host threading doesn't beat sequential "
            "here either, declined rather than emitting an unsound or "
            "unprofitable pragma");
        return;
      }
      std::string Pragma = buildCpuPragma(V);
      if (!insertPragma(LI, Pragma))
        return;
      AnnotatedRanges.push_back(LI.Loop->getSourceRange());
      Summary.Sites.push_back(
          RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::CpuParallel});
      skip("loop counts down; the mapped-region model assumes an ascending "
          "loop, degraded to CPU-threaded #pragma omp parallel for instead "
          "of the GPU_OFFLOAD verdict (a stated substitution, not a silent "
          "one)");
      // No collectDeclareTargets: this site emitted the CPU pragma, which
      // calls ordinary host-compiled functions.
      return;
    }

    std::string Pragma = buildGpuPragma(V);
    if (!insertPragma(LI, Pragma))
      return;
    AnnotatedRanges.push_back(LI.Loop->getSourceRange());
    Summary.Sites.push_back(
        RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::GpuOffload});
    collectDeclareTargets(LI);
    return;
  }

  if (V.Target == OffloadTarget::CpuParallel) {
    // Should always hold for a CpuParallel verdict — Profitability.cpp only
    // chooses this target when CpuBeatsSeq is true. Checked here rather
    // than trusted blindly, the same discipline the GPU path already
    // applies to its own inputs: a codegen pass emitting a pragma the cost
    // model itself contradicts would be a worse bug than declining one.
    if (!V.CpuBeatsSeq) {
      skip("profitability verdict is CPU_PARALLEL but the model's own "
          "CpuBeatsSeq is false, declined rather than emitting a pragma "
          "the cost model contradicts");
      return;
    }
    std::string Pragma = buildCpuPragma(V);
    if (!insertPragma(LI, Pragma))
      return;
    AnnotatedRanges.push_back(LI.Loop->getSourceRange());
    Summary.Sites.push_back(
        RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::CpuParallel});
    return;
  }

  // Sequential (or any future target this pass doesn't know about yet): not
  // this pass's loop, not an error.
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
          "emitted (the device build will need it available some other "
          "way)");
      continue;
    }

    // A function that itself lexically contains one of today's offloaded
    // loops would, once wrapped, have a `target` construct executing inside
    // an active `target` region when called from another offloaded loop —
    // unspecified behavior under OpenMP, and invisible to a syntax-only
    // check because the nesting is dynamic (through a call), not lexical.
    // Filtered to GpuOffload sites only: a function containing a
    // CPU-threaded `parallel for` has no such problem — that construct runs
    // on the host either way, and `parallel for` inside a device function
    // is perfectly legal OpenMP.
    SourceRange DefRange = Def->getSourceRange();
    auto ContainedSite = llvm::find_if(
        Summary.Sites, [&](const RewriteSite &Site) {
          return Site.Kind == OffloadTarget::GpuOffload &&
                contains(SM, DefRange, Site.Loop->getBeginLoc());
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
    // silently leaving an unbalanced declare target pair in the output.
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
  size_t GpuCount = llvm::count_if(S.Sites, [](const RewriteSite &Site) {
    return Site.Kind == OffloadTarget::GpuOffload;
  });
  size_t CpuCount = S.Sites.size() - GpuCount;
  OS << "rewrite: " << GpuCount << " GPU-offload loop(s), " << CpuCount
     << " CPU-threaded loop(s) annotated";
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
