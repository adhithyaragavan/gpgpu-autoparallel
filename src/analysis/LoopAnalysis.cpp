#include "analysis/LoopAnalysis.h"
#include "analysis/CallResolver.h"
#include "analysis/CrossTU.h"
#include "analysis/Profitability.h" // optionCategory()

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/CommandLine.h"

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
enum class OnOff { On, Off };
} // namespace

static llvm::cl::opt<OnOff> FlagLinkedCounters(
    "linked-counters",
    llvm::cl::desc("Headers that declare and step several counters "
                   "(Phase 2.5)"),
    llvm::cl::values(
        clEnumValN(OnOff::On, "on",
                   "the others follow the tested one (default)"),
        clEnumValN(OnOff::Off, "off",
                   "such loops are not recognized (ablation baseline)")),
    llvm::cl::init(OnOff::On), llvm::cl::cat(optionCategory()));

bool linkedCountersFromFlags() { return FlagLinkedCounters == OnOff::On; }

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
/// map cleanly back to source (macro expansions), and when the source text
/// spans several lines: codegen pastes this text into a single-line
/// `#pragma omp ... if(...)` clause, where a newline would end the pragma
/// early and a `//` comment would swallow the rest of it.
std::string getSourceText(const Expr *E, ASTContext &Ctx) {
  const SourceManager &SM = Ctx.getSourceManager();
  llvm::StringRef Text = Lexer::getSourceText(
      CharSourceRange::getTokenRange(E->getSourceRange()), SM,
      Ctx.getLangOpts());
  if (!Text.empty() && !Text.contains('\n') && !Text.contains('\r'))
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

/// A GNU statement expression `({ ... })` can run arbitrary statements
/// (loops, assignments) every time it is evaluated, the same hazard as a call.
bool containsStmtExpr(const Stmt *S) {
  if (!S)
    return false;
  if (isa<StmtExpr>(S))
    return true;
  for (const Stmt *Child : S->children())
    if (containsStmtExpr(Child))
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
      auto &List = isKnownFunction(Callee) ? LI.KnownCallees : LI.Callees;
      if (std::find(List.begin(), List.end(), Callee) == List.end())
        List.push_back(Callee);
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

/// True when E mentions any of Vars anywhere (by canonical declaration).
bool referencesAnyVar(const Stmt *S, const std::vector<const VarDecl *> &Vars) {
  if (!S || Vars.empty())
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
    for (const VarDecl *VD : Vars)
      if (VD && DRE->getDecl()->getCanonicalDecl() == VD->getCanonicalDecl())
        return true;
  for (const Stmt *Child : S->children())
    if (referencesAnyVar(Child, Vars))
      return true;
  return false;
}

/// True when a loop body is exactly one for statement, looking through
/// single-statement compounds and attribute wrappers. Anything else alongside
/// it (even an empty statement or a declaration) makes the nest imperfect.
bool bodyIsSingleFor(const Stmt *Body) {
  while (Body) {
    if (isa<ForStmt>(Body))
      return true;
    if (const auto *AS = dyn_cast<AttributedStmt>(Body)) {
      Body = AS->getSubStmt();
    } else if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
      if (CS->size() != 1)
        return false;
      Body = CS->body_front();
    } else {
      return false; // includes LabelStmt: a goto could enter the inner loop
    }
  }
  return false;
}

/// One counter update in an increment clause: `v++`, `--v`, `v += c`,
/// `v -= c` with c a constant. The variable and its signed step, or why not.
bool parseUpdate(const Expr *E, ASTContext &Ctx, const VarDecl *&Var,
                 int64_t &Step, std::string &Why) {
  E = E->IgnoreParenImpCasts();
  const Expr *Target = nullptr;
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->isIncrementDecrementOp()) {
    Target = UO->getSubExpr();
    Step = UO->isIncrementOp() ? 1 : -1;
  } else if (const auto *CAO = dyn_cast<CompoundAssignOperator>(E);
             CAO && (CAO->getOpcode() == BO_AddAssign ||
                     CAO->getOpcode() == BO_SubAssign)) {
    std::optional<int64_t> Delta = evalInt(CAO->getRHS(), Ctx);
    if (!Delta || *Delta == std::numeric_limits<int64_t>::min()) {
      Why = "a counter's step is not a compile-time constant";
      return false;
    }
    Target = CAO->getLHS();
    Step = CAO->getOpcode() == BO_AddAssign ? *Delta : -*Delta;
  } else {
    Why = "increment clause is not a list of counter updates";
    return false;
  }
  const auto *DRE = dyn_cast<DeclRefExpr>(Target->IgnoreParenImpCasts());
  Var = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
  if (!Var) {
    Why = "increment updates something other than a variable";
    return false;
  }
  return true;
}

/// The operands of a comma list, left to right.
void flattenComma(const Expr *E, llvm::SmallVectorImpl<const Expr *> &Out) {
  E = E->IgnoreParens();
  if (const auto *BO = dyn_cast<BinaryOperator>(E);
      BO && BO->getOpcode() == BO_Comma) {
    flattenComma(BO->getLHS(), Out);
    flattenComma(BO->getRHS(), Out);
    return;
  }
  Out.push_back(E);
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

  // Phase 2.5: several counters declared together (LinkedCounter).
  std::vector<const VarDecl *> Counters;
  if (const auto *DS = dyn_cast<DeclStmt>(Init);
      DS && !DS->isSingleDecl() && linkedCountersFromFlags()) {
    for (const Decl *D : DS->decls()) {
      const auto *VD = dyn_cast<VarDecl>(D);
      if (!VD)
        return reject("init declares something other than a variable");
      if (!VD->getType()->isIntegerType())
        return reject("a counter the init declares is not of integer type");
      if (!VD->getInit())
        return reject("a counter the init declares has no initializer");
      if (referencesAnyVar(VD->getInit(), Counters))
        return reject("a counter's initializer uses another counter");
      Counters.push_back(VD);
    }
  } else if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
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
    LI.StartExpr = VD->getInit();
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
    LI.StartExpr = BO->getRHS();
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

  // The counter the condition tests is the induction variable.
  if (!Counters.empty()) {
    for (const VarDecl *VD : Counters)
      if (refersToVar(CmpBO->getLHS(), VD))
        LI.InductionVar = VD;
    if (!LI.InductionVar)
      return reject("condition does not test one of the counters on its "
                    "left");
    LI.StartExpr = LI.InductionVar->getInit();
    LI.Start = evalInt(LI.StartExpr, Ctx);
    if (referencesAnyVar(CmpBO->getRHS(), Counters))
      return reject("bound expression uses a counter");
  }

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
  if (containsStmtExpr(Bound))
    return reject("bound expression contains a statement expression");

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
  if (!Counters.empty()) {
    // Every counter stepped exactly once, by a constant.
    llvm::SmallVector<const Expr *, 4> Updates;
    flattenComma(Inc, Updates);
    llvm::DenseMap<const VarDecl *, int64_t> Steps;
    for (const Expr *U : Updates) {
      const VarDecl *VD = nullptr;
      int64_t S = 0;
      std::string Why;
      if (!parseUpdate(U, Ctx, VD, S, Why))
        return reject(Why);
      if (!llvm::is_contained(Counters, VD))
        return reject("increment updates a variable the init does not "
                      "declare");
      if (!Steps.try_emplace(VD->getCanonicalDecl(), S).second)
        return reject("increment updates a counter twice");
    }
    for (const VarDecl *VD : Counters) {
      auto It = Steps.find(VD->getCanonicalDecl());
      if (It == Steps.end())
        return reject("a counter the init declares is never stepped");
      if (VD == LI.InductionVar) {
        LI.Step = It->second;
      } else {
        // The loop's end bounds the induction variable, nothing bounds the
        // others: one that may wrap (unsigned, or narrower than int and
        // converted back) would leave the affine form behind.
        const QualType T = VD->getType();
        if (!T->isSignedIntegerType() ||
            Ctx.getTypeSize(T) < Ctx.getTypeSize(Ctx.IntTy))
          return reject("a counter other than the induction variable could "
                        "wrap (unsigned, or narrower than int)");
        LI.Linked.push_back({VD, VD->getInit(), It->second});
      }
    }
    if (*LI.Step == 0)
      return reject("increment step is zero");
    for (const LinkedCounter &K : LI.Linked)
      if (K.Step % *LI.Step != 0)
        return reject("a counter's step is not a multiple of the induction "
                      "variable's");
  } else if (const auto *UO = dyn_cast<UnaryOperator>(IncE)) {
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

void LoopCollector::recordLoop(const ForStmt *FS) {
  LoopInfo LI = analyzeForStmt(FS, Ctx);
  const int Idx = static_cast<int>(Loops.size());

  if (!OpenLoops.empty()) {
    const LoopInfo &P = Loops[OpenLoops.back()];
    LI.Parent = OpenLoops.back();
    LI.Depth = P.Depth + 1;
    // P.NestIVs already ends with P's own variable when P is recognized.
    LI.NestIVs = P.NestIVs;
    LI.EnclosedByUnrecognized =
        P.EnclosedByUnrecognized || P.Kind == LoopKind::Unrecognized;
  }
  LI.InsideRepeatingContext = OpenRepeating > 0;
  LI.PerfectlyNested = bodyIsSingleFor(FS->getBody());
  LI.Foreign = isForeignLocation(Ctx, FS->getBeginLoc());

  if (LI.Kind != LoopKind::Unrecognized) {
    // NestIVs holds only the enclosing loops' variables at this point. A
    // start or bound that folded to a constant cannot depend on them, even if
    // it mentions one inside an unevaluated operand (sizeof).
    LI.BoundsDependOnOuterIV =
        (!LI.Start && referencesAnyVar(LI.StartExpr, LI.NestIVs)) ||
        (!LI.ConstBound && referencesAnyVar(LI.BoundExpr, LI.NestIVs));
    LI.NestIVs.push_back(LI.InductionVar);
  }

  if (LI.Parent >= 0)
    Loops[LI.Parent].Children.push_back(Idx);
  Loops.push_back(std::move(LI));
}

bool LoopCollector::TraverseForStmt(ForStmt *FS, DataRecursionQueue *) {
  if (!WalkUpFromForStmt(FS))
    return false;
  // Record before traversing anything inside, so loops stay in pre-order
  // (parents before children), matching the order the report has always used.
  recordLoop(FS);
  const int Idx = static_cast<int>(Loops.size()) - 1;

  // The clauses are not this loop's body: traverse them with the stack
  // unchanged, so a loop inside one (via a statement expression) gets this
  // loop's enclosing loop as its parent, not this loop. The init runs once;
  // the condition and increment run every iteration, so a loop inside them
  // is in a repeating context.
  if (!TraverseStmt(FS->getInit()))
    return false;
  ++OpenRepeating;
  const bool ClausesOk = TraverseStmt(FS->getConditionVariableDeclStmt()) &&
                         TraverseStmt(FS->getCond()) &&
                         TraverseStmt(FS->getInc());
  --OpenRepeating;
  if (!ClausesOk)
    return false;

  OpenLoops.push_back(Idx);
  const bool Ok = TraverseStmt(FS->getBody());
  OpenLoops.pop_back();
  return Ok;
}

// The Queue argument is deliberately not forwarded. With a null queue,
// TraverseStmt finishes the whole subtree before returning, so the counter is
// decremented only after every loop inside has been recorded.
bool LoopCollector::TraverseWhileStmt(WhileStmt *WS, DataRecursionQueue *) {
  ++OpenRepeating;
  const bool Ok = Base::TraverseWhileStmt(WS, nullptr);
  --OpenRepeating;
  return Ok;
}

bool LoopCollector::TraverseDoStmt(DoStmt *DS, DataRecursionQueue *) {
  ++OpenRepeating;
  const bool Ok = Base::TraverseDoStmt(DS, nullptr);
  --OpenRepeating;
  return Ok;
}

bool LoopCollector::TraverseBlockExpr(BlockExpr *BE, DataRecursionQueue *) {
  // Fresh nest inside the block: no enclosing loops, no repeating context.
  std::vector<int> SavedLoops;
  SavedLoops.swap(OpenLoops);
  const unsigned SavedRepeating = OpenRepeating;
  OpenRepeating = 0;
  const bool Ok = Base::TraverseBlockExpr(BE, nullptr);
  OpenRepeating = SavedRepeating;
  OpenLoops.swap(SavedLoops);
  return Ok;
}

void printLoopReport(llvm::raw_ostream &OS, const LoopInfo &LI,
                     const std::vector<LoopInfo> &AllLoops) {
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

  OS << "  nest       : depth " << LI.Depth;
  if (LI.Parent >= 0) {
    const LoopInfo &P = AllLoops[LI.Parent];
    OS << ", parent ";
    if (P.File != LI.File) // e.g. a loop in an #include'd fragment
      OS << P.File << ":";
    OS << P.Line << ":" << P.Col;
  }
  OS << ", " << LI.Children.size() << " child loop(s)";
  if (LI.PerfectlyNested)
    OS << ", perfect nest";
  if (LI.BoundsDependOnOuterIV)
    OS << ", triangular";
  if (LI.EnclosedByUnrecognized)
    OS << ", inside an unrecognized loop";
  if (LI.InsideRepeatingContext)
    OS << ", inside while/do or a loop header";
  OS << "\n";

  OS << "  nest ivs   : ";
  if (LI.NestIVs.empty())
    OS << "<none>";
  for (size_t I = 0; I < LI.NestIVs.size(); ++I)
    OS << (I ? ", " : "") << LI.NestIVs[I]->getNameAsString();
  OS << "\n";

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
  if (!LI.KnownCallees.empty()) {
    OS << "  known calls: ";
    for (size_t I = 0; I < LI.KnownCallees.size(); ++I)
      OS << (I ? ", " : "") << LI.KnownCallees[I]->getNameAsString();
    OS << " (no memory effects)\n";
  }

  OS << "\n";
}

} // namespace p05
