#include "analysis/LoopAnalysis.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/APSInt.h"

#include <algorithm>
#include <limits>

using namespace clang;

namespace p05 {

llvm::StringRef toString(LoopKind Kind) {
  switch (Kind) {
  case LoopKind::ConstantBound:
    return "CONSTANT_BOUND";
  case LoopKind::VariableBound:
    return "VARIABLE_BOUND";
  case LoopKind::Unrecognized:
    return "UNRECOGNIZED";
  }
  return "UNRECOGNIZED";
}

namespace {

/// Fold an expression to an integer constant, if it is one.
///
/// We use Clang's constant folder rather than matching IntegerLiteral directly.
/// That deliberately widens what counts as a constant bound to include
/// `const int N = 512`, `SIZE * 2`, `sizeof(a) / sizeof(a[0])` and enum
/// constants — all genuinely known at compile time. Matching only literals
/// would classify those as variable-bound and hand the profitability heuristic
/// an "unknown trip count" for a loop whose size we actually know.
///
/// EvaluateAsInt defaults to SE_NoSideEffects, so an expression that has to run
/// code (a call) will not fold — which is what we want.
std::optional<int64_t> evalInt(const Expr *E, ASTContext &Ctx) {
  if (!E || E->isValueDependent() || E->isTypeDependent())
    return std::nullopt;

  Expr::EvalResult Result;
  if (!E->EvaluateAsInt(Result, Ctx) || !Result.Val.isInt())
    return std::nullopt;

  return Result.Val.getInt().tryExtValue();
}

/// Printed source text of an expression, for symbolic reporting of bounds we
/// cannot fold. Falls back to pretty-printing the AST when the range does not
/// map cleanly back to source (macro expansions).
std::string getSourceText(const Expr *E, ASTContext &Ctx) {
  const SourceManager &SM = Ctx.getSourceManager();
  llvm::StringRef Text = Lexer::getSourceText(
      CharSourceRange::getTokenRange(E->getSourceRange()), SM,
      Ctx.getLangOpts());
  if (!Text.empty())
    return Text.str();

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  E->printPretty(OS, nullptr, PrintingPolicy(Ctx.getLangOpts()));
  return OS.str();
}

bool isRelationalOp(BinaryOperatorKind Op) {
  return Op == BO_LT || Op == BO_LE || Op == BO_GT || Op == BO_GE ||
         Op == BO_NE;
}

/// True when E is a bare reference to VD, ignoring the implicit casts Clang
/// inserts around it. Compares canonical declarations rather than names, so
/// a shadowing variable of the same name does not match.
bool refersToVar(const Expr *E, const VarDecl *VD) {
  if (!E || !VD)
    return false;
  const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
  return DRE && DRE->getDecl()->getCanonicalDecl() == VD->getCanonicalDecl();
}

bool containsCall(const Stmt *S) {
  if (!S)
    return false;
  if (isa<CallExpr>(S))
    return true;
  for (const Stmt *Child : S->children())
    if (containsCall(Child))
      return true;
  return false;
}

/// Record every call in the loop body. Calls inside a nested loop count as
/// calls in the enclosing loop's body too, which is correct — the enclosing
/// loop does execute them.
void collectCalls(const Stmt *S, LoopInfo &LI) {
  if (!S)
    return;
  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
      if (std::find(LI.Callees.begin(), LI.Callees.end(), Callee) ==
          LI.Callees.end())
        LI.Callees.push_back(Callee);
    } else {
      LI.HasIndirectCall = true;
    }
  }
  for (const Stmt *Child : S->children())
    collectCalls(Child, LI);
}

/// Ceiling of A/B for strictly positive A and B, without the overflow that
/// the usual (A + B - 1) / B trick risks near INT64_MAX.
int64_t ceilDivPositive(int64_t A, int64_t B) {
  return A / B + (A % B != 0 ? 1 : 0);
}

/// Exact iteration count, given that Start, Bound and Step are all known and
/// the step direction already agrees with the comparison.
std::optional<int64_t> computeTripCount(int64_t Start, int64_t Bound,
                                        int64_t Step, BinaryOperatorKind Op) {
  int64_t Span = 0;

  switch (Op) {
  case BO_LE:
    if (Bound == std::numeric_limits<int64_t>::max())
      return std::nullopt;
    ++Bound;
    [[fallthrough]];
  case BO_LT:
    if (Start >= Bound)
      return 0;
    if (__builtin_sub_overflow(Bound, Start, &Span))
      return std::nullopt;
    return ceilDivPositive(Span, Step);

  case BO_GE:
    if (Bound == std::numeric_limits<int64_t>::min())
      return std::nullopt;
    --Bound;
    [[fallthrough]];
  case BO_GT:
    if (Start <= Bound)
      return 0;
    if (__builtin_sub_overflow(Start, Bound, &Span))
      return std::nullopt;
    return ceilDivPositive(Span, -Step);

  case BO_NE: {
    if (Start == Bound)
      return 0;
    // `i != bound` only terminates if the step actually moves toward the
    // bound and lands on it exactly.
    if ((Bound > Start && Step < 0) || (Bound < Start && Step > 0))
      return std::nullopt;
    if (__builtin_sub_overflow(Bound > Start ? Bound : Start,
                               Bound > Start ? Start : Bound, &Span))
      return std::nullopt;
    int64_t Magnitude = Step < 0 ? -Step : Step;
    if (Span % Magnitude != 0)
      return std::nullopt;
    return Span / Magnitude;
  }

  default:
    return std::nullopt;
  }
}

} // namespace

LoopInfo analyzeForStmt(const ForStmt *FS, ASTContext &Ctx) {
  LoopInfo LI;
  LI.Loop = FS;

  const SourceManager &SM = Ctx.getSourceManager();
  if (PresumedLoc PL = SM.getPresumedLoc(FS->getBeginLoc()); PL.isValid()) {
    LI.File = PL.getFilename();
    LI.Line = PL.getLine();
    LI.Col = PL.getColumn();
  }

  // Body facts are collected regardless of how the shape classifies — an
  // unrecognized loop still calls what it calls, and the safety pass will
  // want to know.
  collectCalls(FS->getBody(), LI);

  auto reject = [&LI](llvm::StringRef Reason) -> LoopInfo {
    LI.Kind = LoopKind::Unrecognized;
    LI.UnrecognizedReason = Reason.str();
    return LI;
  };

  // --- Init: induction variable and start value ------------------------------

  const Stmt *Init = FS->getInit();
  if (!Init)
    return reject("no init clause");

  if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
    if (!DS->isSingleDecl())
      return reject("init declares multiple induction variables");
    const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!VD)
      return reject("init declares something other than a variable");
    if (!VD->getType()->isIntegerType())
      return reject("induction variable is not of integer type");
    if (!VD->getInit())
      return reject("induction variable has no initializer");
    LI.InductionVar = VD;
    LI.Start = evalInt(VD->getInit(), Ctx);
  } else if (const auto *BO = dyn_cast<BinaryOperator>(Init)) {
    if (BO->getOpcode() == BO_Comma)
      return reject("init advances multiple induction variables");
    if (BO->getOpcode() != BO_Assign)
      return reject("init is not a simple assignment");
    const auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    const auto *VD = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
    if (!VD)
      return reject("init does not assign to a variable");
    if (!VD->getType()->isIntegerType())
      return reject("induction variable is not of integer type");
    LI.InductionVar = VD;
    LI.Start = evalInt(BO->getRHS(), Ctx);
  } else {
    return reject("init clause is not a declaration or assignment");
  }

  // --- Cond: comparison operator and bound -----------------------------------

  if (FS->getConditionVariable())
    return reject("condition declares a variable");

  const Expr *Cond = FS->getCond();
  if (!Cond)
    return reject("no condition clause");

  const auto *CmpBO = dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts());
  if (!CmpBO || !isRelationalOp(CmpBO->getOpcode()))
    return reject("condition is not a simple relational comparison");

  if (!refersToVar(CmpBO->getLHS(), LI.InductionVar)) {
    if (refersToVar(CmpBO->getRHS(), LI.InductionVar))
      return reject("mirrored condition (bound on the left) is not handled");
    return reject("condition does not test the induction variable");
  }
  LI.CmpOp = CmpBO->getOpcode();

  const Expr *Bound = CmpBO->getRHS()->IgnoreParenImpCasts();

  // A call in the bound is re-evaluated every iteration and may have side
  // effects, so the bound is not a fixed value we can reason about at all.
  if (containsCall(Bound))
    return reject("bound expression contains a function call");

  LI.BoundExpr = Bound;
  if (std::optional<int64_t> Folded = evalInt(Bound, Ctx)) {
    LI.Kind = LoopKind::ConstantBound;
    LI.ConstBound = Folded;
  } else {
    LI.Kind = LoopKind::VariableBound;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Bound)) {
      LI.BoundDecl = DRE->getDecl();
      LI.BoundText = DRE->getDecl()->getNameAsString();
      LI.BoundIsParameter = isa<ParmVarDecl>(DRE->getDecl());
    } else {
      LI.BoundText = getSourceText(Bound, Ctx);
    }
  }

  // --- Inc: step -------------------------------------------------------------

  const Expr *Inc = FS->getInc();
  if (!Inc)
    return reject("no increment clause");

  const Expr *IncE = Inc->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(IncE)) {
    if (!UO->isIncrementDecrementOp())
      return reject("increment is not ++ or --");
    if (!refersToVar(UO->getSubExpr(), LI.InductionVar))
      return reject("increment updates a variable other than the induction "
                    "variable");
    LI.Step = UO->isIncrementOp() ? 1 : -1;
  } else if (const auto *CAO = dyn_cast<CompoundAssignOperator>(IncE)) {
    const BinaryOperatorKind Op = CAO->getOpcode();
    if (Op != BO_AddAssign && Op != BO_SubAssign)
      return reject("increment is not affine (only += and -= are handled)");
    if (!refersToVar(CAO->getLHS(), LI.InductionVar))
      return reject("increment updates a variable other than the induction "
                    "variable");
    std::optional<int64_t> Delta = evalInt(CAO->getRHS(), Ctx);
    if (!Delta)
      return reject("increment step is not a compile-time constant");
    LI.Step = (Op == BO_AddAssign) ? *Delta : -*Delta;
  } else {
    return reject("increment clause is not a recognized update");
  }

  if (*LI.Step == 0)
    return reject("increment step is zero");

  // --- Direction consistency -------------------------------------------------

  const bool CountsUp = *LI.Step > 0;
  if ((LI.CmpOp == BO_LT || LI.CmpOp == BO_LE) && !CountsUp)
    return reject("loop counts down but the condition tests upward");
  if ((LI.CmpOp == BO_GT || LI.CmpOp == BO_GE) && CountsUp)
    return reject("loop counts up but the condition tests downward");

  // --- Trip count ------------------------------------------------------------

  // Only when every ingredient is known. A ConstantBound loop with a variable
  // start value legitimately ends up here with no trip count.
  if (LI.Start && LI.ConstBound && LI.Step)
    LI.TripCount =
        computeTripCount(*LI.Start, *LI.ConstBound, *LI.Step, LI.CmpOp);

  return LI;
}

bool LoopCollector::VisitForStmt(ForStmt *FS) {
  Loops.push_back(analyzeForStmt(FS, Ctx));
  return true;
}

void printLoopReport(llvm::raw_ostream &OS, const LoopInfo &LI) {
  OS << LI.File << ":" << LI.Line << ":" << LI.Col << "\n";
  OS << "  kind       : " << toString(LI.Kind) << "\n";

  if (LI.Kind == LoopKind::Unrecognized) {
    OS << "  reason     : " << LI.UnrecognizedReason << "\n";
  } else {
    OS << "  induction  : " << LI.InductionVar->getNameAsString() << "\n";

    OS << "  start      : ";
    if (LI.Start)
      OS << *LI.Start << "\n";
    else
      OS << "<not statically known>\n";

    OS << "  bound      : " << BinaryOperator::getOpcodeStr(LI.CmpOp) << " ";
    if (LI.Kind == LoopKind::ConstantBound) {
      OS << *LI.ConstBound << "\n";
    } else {
      OS << LI.BoundText;
      if (LI.BoundDecl)
        OS << (LI.BoundIsParameter ? "  (parameter)" : "  (local)");
      OS << "\n";
    }

    OS << "  step       : " << (*LI.Step > 0 ? "+" : "") << *LI.Step << "\n";

    OS << "  trip count : ";
    if (LI.TripCount)
      OS << *LI.TripCount << "\n";
    else
      OS << "<not statically known>\n";
  }

  if (!LI.Callees.empty()) {
    OS << "  callees    : ";
    for (size_t I = 0; I < LI.Callees.size(); ++I) {
      if (I)
        OS << ", ";
      OS << LI.Callees[I]->getNameAsString();
    }
    OS << "\n";
  }
  if (LI.HasIndirectCall)
    OS << "  callees    : <contains an unresolved indirect call>\n";

  OS << "\n";
}

} // namespace p05
