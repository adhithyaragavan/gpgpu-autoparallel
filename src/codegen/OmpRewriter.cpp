#include "codegen/OmpRewriter.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace clang;

namespace p05 {

OmpRewriter::OmpRewriter(Rewriter &Rewrite, CallResolver &Resolver,
                         ASTContext &Ctx)
    : Rewrite(Rewrite), Resolver(Resolver), Ctx(Ctx) {}

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
  if (V.Collapse2)
    Pragma += " collapse(2)";
  // Phase 2.1: the map sections' run-time conditions are a safety
  // condition, conjoined with the cost model's guard, never replaced by it.
  std::string Cond = V.GuardExpr ? *V.GuardExpr : std::string();
  if (!V.MapGuard.empty())
    Cond = Cond.empty() ? V.MapGuard : "(" + Cond + ") && " + V.MapGuard;
  if (!Cond.empty())
    Pragma += " if(target: " + Cond + ")";
  for (const ArrayRegion &R : V.Regions) {
    if (!R.Base)
      continue;
    Pragma += " map(" + R.mapKind().str() + ": " + R.listItem() + ")";
  }
  return Pragma + V.ScalarMaps + V.ScalarClauses;
}

/// #pragma omp parallel for [if(parallel: ...)] [private/lastprivate/
/// reduction(...)] — no map() clauses (shared memory). The scalar clauses
/// come from ScalarAnalysis (Phase 1.4). `parallel:` on the if() is required for the same
/// ambiguity reason as the GPU path's `target:`: `parallel for` is a combined
/// construct whose constituent `parallel` directive is the one that accepts
/// `if`, and an unqualified `if()` doesn't say so explicitly.
/// The CPU pragma's if(parallel:) condition (empty: none).
std::string cpuCondition(const ProfitabilityVerdict &V) {
  // Phase 1.6: the run-time no-overlap check is a safety condition, so it is
  // conjoined with the cost model's guard, never replaced by it.
  std::string Cond = V.CpuGuardExpr ? *V.CpuGuardExpr : std::string();
  if (!V.AliasGuard.empty())
    Cond = Cond.empty() ? V.AliasGuard : "(" + Cond + ") && " + V.AliasGuard;
  return Cond;
}

std::string buildCpuPragma(const ProfitabilityVerdict &V, bool WithIf = true) {
  std::string Pragma = "#pragma omp parallel for";
  std::string Cond = WithIf ? cpuCondition(V) : std::string();
  if (!Cond.empty())
    Pragma += " if(parallel: " + Cond + ")";
  return Pragma + V.ScalarClauses;
}

enum class CpuBranches { On, Off };
llvm::cl::opt<CpuBranches> FlagCpuBranches(
    "cpu-branches",
    llvm::cl::desc("A guarded CPU loop inside another loop runs as `if (guard) "
                   "<CPU loop> else <original loop>`: a false if(parallel:) "
                   "still enters a serialized parallel region every time "
                   "(Phase 3.2)"),
    llvm::cl::values(
        clEnumValN(CpuBranches::On, "on", "branch on the guard (default)"),
        clEnumValN(CpuBranches::Off, "off",
                   "the guard stays in if(parallel:) (ablation baseline)")),
    llvm::cl::init(CpuBranches::On));

/// Whether LI's CPU pragma should become a branch: it runs once per
/// iteration of something (an enclosing loop), and it has a condition.
bool cpuBranch(const LoopInfo &LI, const ProfitabilityVerdict &V) {
  return FlagCpuBranches == CpuBranches::On &&
         (LI.Depth > 0 || LI.InsideRepeatingContext) && !cpuCondition(V).empty();
}

} // namespace

bool OmpRewriter::insertPragma(const LoopInfo &LI, const std::string &Pragma,
                               const std::string *Check) {
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
  // `#pragma clang loop` / `#pragma unroll` must directly precede its loop.
  if (DynTypedNodeList Ps = Ctx.getParents(*LI.Loop);
      !Ps.empty() && Ps[0].get<AttributedStmt>()) {
    skip("loop carries a loop-hint pragma (#pragma clang loop / unroll), "
         "and no OpenMP pragma can go between them, declined");
    return false;
  }
  SourceRange Range = LI.Loop->getSourceRange();
  if (isNestedInAny(SM, Range, AnnotatedRanges)) {
    skip("loop is lexically nested inside an already-annotated loop, "
        "declined (nested target/parallel constructs are invalid or "
        "harmful OpenMP in every combination)");
    return false;
  }

  if (Check)
    return insertBranch(LI, *Check, Pragma);

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
  ElsePragma.clear();
  ElseCheck.clear();

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
    // Phase 2.1: sections from the ranges map a descending loop correctly.
    if (!V.RangeMaps && (LI.CmpOp == BO_GT || LI.CmpOp == BO_GE)) {
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

    if (V.AliasBranch) {
      if (std::string Why = branchBlocker(LI); !Why.empty()) {
        // As the Phase 1.6 rule: the loop stays on the host.
        if (!V.CpuBeatsSeq) {
          skip("GPU loop behind a run-time overlap check, but " + Why +
               ", and host threading doesn't beat sequential, declined");
          return;
        }
        std::string Pragma = buildCpuPragma(V);
        if (!insertPragma(LI, Pragma))
          return;
        AnnotatedRanges.push_back(LI.Loop->getSourceRange());
        Summary.Sites.push_back(
            RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::CpuParallel});
        skip("GPU loop behind a run-time overlap check, but " + Why +
             ", degraded to the guarded CPU pragma");
        return;
      }
      // The branch test carries the target's whole condition: the cost
      // guard (cheapest, first), the map guard, then the overlap checks.
      // Left in if(target:), a false guard still runs teams on the host,
      // once per execution of the loop (gemm's inner loop on the T4: 1.2M
      // times, 10x slower than sequential), where the else branch runs
      // the host threads or the plain loop.
      ProfitabilityVerdict G = V;
      std::string Test;
      auto conj = [&Test](const std::string &C) {
        Test += Test.empty() ? C : " && " + C;
      };
      if (G.GuardExpr)
        conj("(" + *G.GuardExpr + ")");
      if (!G.MapGuard.empty())
        conj(G.MapGuard);
      conj(V.BranchCheck);
      G.GuardExpr.reset();
      G.MapGuard.clear();
      std::string Pragma = buildGpuPragma(G);
      // The else branch keeps the host threads: its accesses are still
      // checked apart by the CPU pragma's own AliasGuard.
      ElsePragma = V.CpuBeatsSeq ? buildCpuPragma(V) : "";
      if (V.CpuBeatsSeq && cpuBranch(LI, V)) {
        // `else if (guard) { CPU loop } else { loop }`, as for a CPU loop.
        ElseCheck = cpuCondition(V);
        ElsePragma = buildCpuPragma(V, /*WithIf=*/false);
      }
      if (!insertPragma(LI, Pragma, &Test))
        return;
      AnnotatedRanges.push_back(LI.Loop->getSourceRange());
      Summary.Sites.push_back(
          RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::GpuOffload});
      Summary.Branches.push_back(
          "line " + std::to_string(LI.Line) + ": if (" + Test +
          ") the GPU loop, else " +
          (ElseCheck.empty() ? "" : "if (" + ElseCheck + ") ") +
          "the original loop" +
          (ElsePragma.empty() ? "" : " with " + ElsePragma) +
          (ElseCheck.empty() ? "" : ", else the original loop"));
      collectDeclareTargets(LI);
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
    if (cpuBranch(LI, V) && branchBlocker(LI).empty()) {
      // A false if(parallel:) still enters a serialized parallel region,
      // once per execution of the loop (gemm's inner loop on the T4: 1.2M
      // times); a plain if around a copy costs a compare.
      std::string Cond = cpuCondition(V);
      std::string Pragma = buildCpuPragma(V, /*WithIf=*/false);
      if (!insertPragma(LI, Pragma, &Cond))
        return;
      AnnotatedRanges.push_back(LI.Loop->getSourceRange());
      Summary.Sites.push_back(
          RewriteSite{LI.Loop, Pragma, LI.Line, OffloadTarget::CpuParallel});
      Summary.Branches.push_back("line " + std::to_string(LI.Line) + ": if (" +
                                 Cond + ") the CPU loop, else the original loop");
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

namespace {

/// Where text after S goes: past its `;` when the statement ends with one
/// that isn't part of its range (an expression statement, `do ... while`).
SourceLocation afterStmt(const Stmt *S, const SourceManager &SM,
                         const LangOptions &LO) {
  SourceLocation End = S->getEndLoc();
  std::optional<Token> Next = Lexer::findNextToken(End, SM, LO);
  if (Next && Next->is(tok::semi) && !isa<CompoundStmt>(S) &&
      !isa<DeclStmt>(S))
    return Next->getLocation();
  return End;
}

/// Nothing but blanks after Loc's token, up to the end of its line.
bool restOfLineEmpty(SourceLocation Loc, const SourceManager &SM,
                     const LangOptions &LO) {
  SourceLocation After = Lexer::getLocForEndOfToken(Loc, 0, SM, LO);
  bool Invalid = false;
  const char *P = SM.getCharacterData(After, &Invalid);
  if (Invalid)
    return false;
  while (*P == ' ' || *P == '\t')
    ++P;
  return *P == '\n' || *P == '\r' || *P == '\0';
}

} // namespace

namespace {

/// A label, or a `case` of a switch outside S: a copy of S would define the
/// label twice, or be entered from outside.
struct Labels : RecursiveASTVisitor<Labels> {
  int Switches = 0;
  bool Found = false;
  bool TraverseSwitchStmt(SwitchStmt *SS) {
    ++Switches;
    bool R = RecursiveASTVisitor::TraverseSwitchStmt(SS);
    --Switches;
    return R;
  }
  bool VisitLabelStmt(LabelStmt *) { return !(Found = true); }
  bool VisitSwitchCase(SwitchCase *) {
    if (Switches == 0)
      Found = true;
    return !Found;
  }
  bool VisitAddrLabelExpr(AddrLabelExpr *) { return !(Found = true); }
};

} // namespace

std::string OmpRewriter::branchBlocker(const LoopInfo &LI) const {
  const SourceManager &SM = Rewrite.getSourceMgr();
  const LangOptions &LO = Rewrite.getLangOpts();
  SourceLocation Begin = LI.Loop->getBeginLoc();
  SourceLocation End = afterStmt(LI.Loop, SM, LO);
  if (Begin.isMacroID() || End.isMacroID() || !SM.isWrittenInMainFile(Begin) ||
      !SM.isWrittenInMainFile(End) || !Rewriter::isRewritable(End))
    return "the loop begins or ends inside a macro, so it can't be copied";
  Labels L;
  L.TraverseStmt(const_cast<ForStmt *>(LI.Loop));
  if (L.Found)
    return "its body has a label, which a copy would define twice";
  // Only the original text is copied: nothing inside may be rewritten.
  SourceRange Range = LI.Loop->getSourceRange();
  for (SourceRange R : AnnotatedRanges)
    if (contains(SM, Range, R.getBegin()))
      return "a loop inside it already has a pragma";
  // A conditional directive in the loop may be closed after it (`#ifdef`
  // in the body, `#endif` past the loop): a copy would split it. Any
  // directive line declines, conservatively.
  bool Invalid = false;
  llvm::StringRef Text = Lexer::getSourceText(
      CharSourceRange::getTokenRange(Begin, End), SM, LO, &Invalid);
  if (Invalid)
    return "its text could not be read";
  llvm::SmallVector<llvm::StringRef, 16> Lines;
  Text.split(Lines, '\n');
  for (llvm::StringRef Line : Lines)
    if (Line.ltrim().starts_with("#"))
      return "it contains a preprocessor directive, which a copy could split";
  return "";
}

bool OmpRewriter::insertBranch(const LoopInfo &LI, const std::string &Check,
                               const std::string &Pragma) {
  const SourceManager &SM = Rewrite.getSourceMgr();
  const LangOptions &LO = Rewrite.getLangOpts();
  SourceLocation Begin = LI.Loop->getBeginLoc();
  SourceLocation End = afterStmt(LI.Loop, SM, LO);
  // The original text, before this pass changes anything in it.
  bool Invalid = false;
  llvm::StringRef Orig = Lexer::getSourceText(
      CharSourceRange::getTokenRange(Begin, End), SM, LO, &Invalid);
  if (Invalid || Orig.empty()) {
    Summary.Skipped.push_back("line " + std::to_string(LI.Line) +
                              ": loop text could not be read, declined");
    return false;
  }
  unsigned Col = SM.getPresumedColumnNumber(Begin);
  std::string Indent(Col > 0 ? Col - 1 : 0, ' ');
  // Braced as a whole: the result is one statement wherever the loop was
  // (an unbraced `if (c) for ...; else ...` keeps its else).
  std::string Open = "{ if (" + Check + ") {\n" + Indent + Pragma + "\n" + Indent;
  std::string Close =
      ElseCheck.empty()
          ? "\n" + Indent + "} else {\n" + Indent +
                (ElsePragma.empty() ? "" : ElsePragma + "\n" + Indent) +
                Orig.str() + "\n" + Indent + "} }"
          : "\n" + Indent + "} else if (" + ElseCheck + ") {\n" + Indent +
                ElsePragma + "\n" + Indent + Orig.str() + "\n" + Indent +
                "} else {\n" + Indent + Orig.str() + "\n" + Indent + "} }";
  if (Rewrite.InsertTextBefore(Begin, Open) ||
      Rewrite.InsertTextAfterToken(End, Close)) {
    Summary.Skipped.push_back("line " + std::to_string(LI.Line) +
                              ": source location was not rewritable, "
                              "declined (output may be incomplete)");
    return false;
  }
  return true;
}

void OmpRewriter::emitRegions(const std::vector<DataRegion> &Regions) {
  const SourceManager &SM = Rewrite.getSourceMgr();
  const LangOptions &LO = Rewrite.getLangOpts();
  auto Placeable = [&](SourceLocation L) {
    return L.isValid() && !L.isMacroID() && SM.isWrittenInMainFile(L) &&
           Rewriter::isRewritable(L);
  };
  auto Indent = [&](SourceLocation L) {
    unsigned Col = SM.getPresumedColumnNumber(L);
    return std::string(Col > 0 ? Col - 1 : 0, ' ');
  };
  for (const DataRegion &D : Regions) {
    std::vector<SourceLocation> Locs{D.First->getBeginLoc(),
                                     afterStmt(D.Last, SM, LO)};
    for (const HostUpdate &U : D.Updates) {
      Locs.push_back(U.S->getBeginLoc());
      Locs.push_back(afterStmt(U.S, SM, LO));
    }
    if (!llvm::all_of(Locs, Placeable)) {
      Summary.Skipped.push_back("line " + std::to_string(D.Line) +
                                ": data region not emitted (a location is "
                                "inside a macro); its loops map their own "
                                "data");
      continue;
    }
    auto Before = [&](const Stmt *S, const std::string &Text) {
      const std::string In = Indent(S->getBeginLoc());
      std::string T = Text + "\n" + In;
      if (!startsOwnLine(SM, S->getBeginLoc()))
        T = "\n" + T;
      Rewrite.InsertTextBefore(S->getBeginLoc(), T);
    };
    auto After = [&](const Stmt *S, const std::string &Text) {
      SourceLocation L = afterStmt(S, SM, LO);
      const std::string In = Indent(S->getBeginLoc());
      std::string T = "\n" + In + Text;
      if (!restOfLineEmpty(L, SM, LO))
        T += "\n" + In;
      Rewrite.InsertTextAfterToken(L, T);
    };
    for (const HostUpdate &U : D.Updates) {
      if (!U.Before.empty())
        Before(U.S, U.Before);
      if (!U.After.empty())
        After(U.S, U.After);
    }
    const std::string In = Indent(D.First->getBeginLoc());
    std::string Open = D.Pragma;
    if (!D.Decls.empty())
      Open = "{\n" + In + D.Decls + "\n" + In + Open;
    if (D.Braces)
      Open += "\n" + In + "{";
    Before(D.First, Open);
    if (D.Braces || !D.Decls.empty())
      After(D.Last, std::string(D.Braces ? "}" : "") +
                        (D.Braces && !D.Decls.empty() ? "\n" + In : "") +
                        (D.Decls.empty() ? "" : "}"));
    Summary.DataRegions.push_back("line " + std::to_string(D.Line) + ": " +
                                  (D.Decls.empty() ? "" : D.Decls + " ") +
                                  D.Pragma);
    for (const HostUpdate &U : D.Updates) {
      const unsigned L = SM.getPresumedLineNumber(U.S->getBeginLoc());
      for (const std::string *T : {&U.Before, &U.After})
        if (!T->empty())
          Summary.DataRegions.push_back("  line " + std::to_string(L) + ": " +
                                        *T);
    }
  }
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

  // Phase 2.5: the inspectors a guard calls (AliasAnalyzer::boundChecks),
  // `p05_all_le_<type>` / `p05_any_gt_<type>`, defined once at the top of
  // the file.
  // Phase 3.2: a branch carries its guard in a plain if, so its text (in
  // Summary.Branches) is scanned too.
  std::set<std::string> Helpers;
  auto scan = [&Helpers](const std::string &Text) {
    for (size_t At = Text.find("p05_"); At != std::string::npos;
         At = Text.find("p05_", At + 1)) {
      const size_t Paren = Text.find('(', At);
      if (Paren != std::string::npos)
        Helpers.insert(Text.substr(At, Paren - At));
    }
  };
  for (const RewriteSite &Site : Summary.Sites)
    scan(Site.Pragma);
  for (const std::string &B : Summary.Branches)
    scan(B);
  std::string Defs;
  for (const std::string &H : Helpers) {
    const bool All = llvm::StringRef(H).starts_with("p05_all_le_");
    if (!All && !llvm::StringRef(H).starts_with("p05_any_gt_"))
      continue;
    std::string Type = H.substr(std::strlen("p05_all_le_"));
    std::replace(Type.begin(), Type.end(), '_', ' ');
    Defs += "static int " + H + "(const " + Type +
            " *b, __INTPTR_TYPE__ lo, __INTPTR_TYPE__ hi, __INTPTR_TYPE__ k) "
            "{ for (__INTPTR_TYPE__ i = lo; i <= hi; i++) if (b[i] > k) "
            "return " + (All ? "0" : "1") + "; return " + (All ? "1" : "0") +
            "; }\n";
  }
  if (!Defs.empty())
    Rewrite.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()),
                             Defs);

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
  for (const std::string &D : S.DataRegions)
    OS << "  data region " << D << "\n";
  for (const std::string &B : S.Branches)
    OS << "  branch " << B << "\n";
  for (const FunctionDecl *FD : S.DeclareTargets)
    OS << "  declare target: " << FD->getNameAsString() << "\n";
  for (const std::string &Reason : S.Skipped)
    OS << "  skipped: " << Reason << "\n";
}

} // namespace p05
