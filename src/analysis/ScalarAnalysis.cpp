#include "analysis/ScalarAnalysis.h"

#include "analysis/Profitability.h" // optionCategory()

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/CommandLine.h"

#include <functional>
#include <map>

using namespace clang;

namespace p05 {

static llvm::cl::opt<bool> FlagFPReductions(
    "fp-reductions",
    llvm::cl::desc("Allow floating-point reductions (results may differ in "
                   "the last bits from the sequential run; off by default)"),
    llvm::cl::init(false), llvm::cl::cat(optionCategory()));

bool fpReductionsFromFlags() { return FlagFPReductions; }

std::string LoopScalars::clauseText() const {
  std::vector<std::string> Priv, Last;
  std::map<std::string, std::vector<std::string>> Red; // op -> vars
  for (const ScalarClause &C : Clauses) {
    const std::string N = C.Var->getNameAsString();
    if (C.K == ScalarClause::Private)
      Priv.push_back(N);
    else if (C.K == ScalarClause::LastPrivate)
      Last.push_back(N);
    else
      Red[C.Op].push_back(N);
  }
  auto Join = [](const std::vector<std::string> &V) {
    std::string S;
    for (size_t I = 0; I < V.size(); ++I)
      S += (I ? ", " : "") + V[I];
    return S;
  };
  std::string Out;
  if (!Priv.empty())
    Out += " private(" + Join(Priv) + ")";
  if (!Last.empty())
    Out += " lastprivate(" + Join(Last) + ")";
  for (const auto &[Op, Vars] : Red)
    Out += " reduction(" + Op + ": " + Join(Vars) + ")";
  return Out;
}

bool LoopScalars::needsHost() const {
  for (const ScalarClause &C : Clauses)
    if (C.K != ScalarClause::Private)
      return true;
  return false;
}

namespace {

const VarDecl *declOf(const Expr *E) {
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return VD->getCanonicalDecl();
  return nullptr;
}

/// Does S mention V anywhere (any DeclRefExpr to it)?
bool mentions(const Stmt *S, const VarDecl *V) {
  if (!S)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
    if (DRE->getDecl()->getCanonicalDecl() == V)
      return true;
  for (const Stmt *C : S->children())
    if (mentions(C, V))
      return true;
  return false;
}

void collectRefs(const Stmt *S, const VarDecl *V,
                 llvm::SmallVectorImpl<const DeclRefExpr *> &Out) {
  if (!S)
    return;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
    if (DRE->getDecl()->getCanonicalDecl() == V)
      Out.push_back(DRE);
  for (const Stmt *C : S->children())
    collectRefs(C, V, Out);
}

const DeclRefExpr *refTo(const Expr *E, const VarDecl *V) {
  const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
  return DRE && DRE->getDecl()->getCanonicalDecl() == V ? DRE : nullptr;
}

std::string text(const Expr *E, ASTContext &Ctx) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  E->IgnoreParenImpCasts()->printPretty(OS, nullptr,
                                        PrintingPolicy(Ctx.getLangOpts()));
  return OS.str();
}

const FunctionDecl *enclosingFunction(const Stmt *S, ASTContext &Ctx) {
  DynTypedNodeList Parents = Ctx.getParents(*S);
  while (!Parents.empty()) {
    if (const auto *FD = Parents[0].get<FunctionDecl>())
      return FD;
    if (const auto *PS = Parents[0].get<Stmt>())
      Parents = Ctx.getParents(*PS);
    else if (const auto *PD = Parents[0].get<Decl>())
      Parents = Ctx.getParents(*PD);
    else
      break;
  }
  return nullptr;
}

/// Every variable written by name in S (`x = `, `x op= `, `x++`), with
/// whether the writing statement is inside S.
class WriteCollector : public RecursiveASTVisitor<WriteCollector> {
public:
  llvm::SmallVector<const VarDecl *, 8> Written;
  llvm::DenseSet<const VarDecl *> DeclaredInside; // automatic, per iteration
  llvm::DenseSet<const VarDecl *> StaticInside;
  void add(const Expr *LHS) {
    if (const VarDecl *V = declOf(LHS))
      if (!llvm::is_contained(Written, V))
        Written.push_back(V);
  }
  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      add(BO->getLHS());
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      add(UO->getSubExpr());
    return true;
  }
  bool VisitVarDecl(VarDecl *VD) {
    (VD->hasLocalStorage() ? DeclaredInside : StaticInside)
        .insert(VD->getCanonicalDecl());
    return true;
  }
};

/// Is there a `continue` that targets the loop whose body is S?
bool hasOwnContinue(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ContinueStmt>(S))
    return true;
  if (isa<ForStmt>(S) || isa<WhileStmt>(S) || isa<DoStmt>(S))
    return false; // a continue in there targets the inner loop
  for (const Stmt *C : S->children())
    if (hasOwnContinue(C))
      return true;
  return false;
}

/// Is V written on every path through the body Body, at its top level?
bool writtenUnconditionally(const Stmt *Body, const VarDecl *V) {
  auto Writes = [V](const Stmt *S) {
    if (const auto *E = dyn_cast<Expr>(S)) {
      const auto *BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
      return BO && BO->getOpcode() == BO_Assign && refTo(BO->getLHS(), V);
    }
    if (const auto *FS = dyn_cast<ForStmt>(S)) {
      const auto *Init = dyn_cast_or_null<BinaryOperator>(FS->getInit());
      return Init && Init->getOpcode() == BO_Assign && refTo(Init->getLHS(), V);
    }
    return false;
  };
  if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
    for (const Stmt *S : CS->body())
      if (Writes(S))
        return true;
    return false;
  }
  return Writes(Body);
}

/// The reduction operator a single update of V uses, or "" if S is not one.
/// Fills Covered with the DeclRefExprs to V that the update accounts for.
std::string updateOp(const Stmt *S, const VarDecl *V, ASTContext &Ctx,
                     llvm::SmallVectorImpl<const DeclRefExpr *> &Covered) {
  // min/max: `cond ? a : b` or `if (cond) V = e;` comparing V with e.
  auto MinMax = [&](const Expr *Cond, const Expr *Chosen,
                    const DeclRefExpr *LHS) -> std::string {
    const auto *Cmp = dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts());
    if (!Cmp || !(Cmp->getOpcode() == BO_LT || Cmp->getOpcode() == BO_GT ||
                  Cmp->getOpcode() == BO_LE || Cmp->getOpcode() == BO_GE))
      return "";
    const DeclRefExpr *VL = refTo(Cmp->getLHS(), V);
    const DeclRefExpr *VR = refTo(Cmp->getRHS(), V);
    if ((VL != nullptr) == (VR != nullptr))
      return "";
    const Expr *E = VL ? Cmp->getRHS() : Cmp->getLHS();
    if (mentions(E, V) || !Chosen || text(Chosen, Ctx) != text(E, Ctx))
      return "";
    // The comparison and the stored value must be in V's own type: comparing
    // in `unsigned` but storing into an `int` (or narrowing) is not max/min
    // of V's values.
    auto Same = [&](QualType T) {
      return Ctx.hasSameUnqualifiedType(T, V->getType());
    };
    if (!Same(Cmp->getLHS()->getType()) || !Same(Cmp->getRHS()->getType()) ||
        !Same(Chosen->IgnoreParenImpCasts()->getType()))
      return "";
    const bool Less = Cmp->getOpcode() == BO_LT || Cmp->getOpcode() == BO_LE;
    // Is E the larger one when the condition holds?
    const bool EBigger = VL ? Less : !Less;
    Covered.push_back(VL ? VL : VR);
    Covered.push_back(LHS);
    return EBigger ? "max" : "min";
  };

  if (const auto *IS = dyn_cast<IfStmt>(S)) {
    if (IS->getElse() || IS->getInit() || IS->getConditionVariable())
      return "";
    const Stmt *Then = IS->getThen();
    if (const auto *CS = dyn_cast<CompoundStmt>(Then)) {
      if (CS->size() != 1)
        return "";
      Then = CS->body_front();
    }
    const auto *A = dyn_cast<BinaryOperator>(Then);
    const DeclRefExpr *LHS = A ? refTo(A->getLHS(), V) : nullptr;
    if (!A || A->getOpcode() != BO_Assign || !LHS)
      return "";
    return MinMax(IS->getCond(), A->getRHS(), LHS);
  }

  const auto *E = dyn_cast<Expr>(S);
  if (!E)
    return "";
  E = E->IgnoreParenImpCasts();
  if (const auto *CA = dyn_cast<CompoundAssignOperator>(E)) {
    const DeclRefExpr *LHS = refTo(CA->getLHS(), V);
    if (!LHS || mentions(CA->getRHS(), V))
      return "";
    // `s += 0.5 * x` with an integer s truncates every step: not associative.
    if (CA->getComputationResultType()->isFloatingType() &&
        !V->getType()->isFloatingType())
      return "";
    Covered.push_back(LHS);
    switch (CA->getOpcode()) {
    case BO_AddAssign:
    case BO_SubAssign: // OpenMP combines `-` reductions with +
      return "+";
    case BO_MulAssign:
      return "*";
    case BO_AndAssign:
      return "&";
    case BO_OrAssign:
      return "|";
    case BO_XorAssign:
      return "^";
    default:
      Covered.pop_back();
      return "";
    }
  }
  const auto *A = dyn_cast<BinaryOperator>(E);
  const DeclRefExpr *LHS = A ? refTo(A->getLHS(), V) : nullptr;
  if (!A || A->getOpcode() != BO_Assign || !LHS)
    return "";
  const Expr *R = A->getRHS()->IgnoreParenImpCasts();
  if (const auto *CO = dyn_cast<ConditionalOperator>(R)) {
    // `V = cond ? e : V` / `V = cond ? V : e`
    const DeclRefExpr *TV = refTo(CO->getTrueExpr(), V);
    const DeclRefExpr *FV = refTo(CO->getFalseExpr(), V);
    if ((TV != nullptr) == (FV != nullptr))
      return "";
    const Expr *Chosen = TV ? CO->getFalseExpr() : CO->getTrueExpr();
    std::string Op = MinMax(CO->getCond(), Chosen, LHS);
    if (Op.empty())
      return "";
    // Chosen when the condition is false: the other direction.
    if (TV)
      Op = Op == "max" ? "min" : "max";
    Covered.push_back(TV ? TV : FV);
    return Op;
  }
  const auto *B = dyn_cast<BinaryOperator>(R);
  if (!B)
    return "";
  if (B->getType()->isFloatingType() && !V->getType()->isFloatingType())
    return ""; // truncated every step, see the compound case
  const DeclRefExpr *BL = refTo(B->getLHS(), V);
  const DeclRefExpr *BR = refTo(B->getRHS(), V);
  if ((BL != nullptr) == (BR != nullptr))
    return "";
  const Expr *Other = BL ? B->getRHS() : B->getLHS();
  if (mentions(Other, V))
    return "";
  std::string Op;
  switch (B->getOpcode()) {
  case BO_Add:
    Op = "+";
    break;
  case BO_Sub:
    if (!BL) // e - V is not a reduction
      return "";
    Op = "+";
    break;
  case BO_Mul:
    Op = "*";
    break;
  case BO_And:
    Op = "&";
    break;
  case BO_Or:
    Op = "|";
    break;
  case BO_Xor:
    Op = "^";
    break;
  default:
    return "";
  }
  Covered.push_back(LHS);
  Covered.push_back(BL ? BL : BR);
  return Op;
}

/// If every mention of V inside Body belongs to an update with one common
/// operator, that operator; else "".
std::string reductionOp(const Stmt *Body, const VarDecl *V, ASTContext &Ctx) {
  llvm::SmallVector<const DeclRefExpr *, 8> All, Covered;
  collectRefs(Body, V, All);
  std::string Op;
  bool Mixed = false;
  // Only an update whose value is thrown away is a reduction step:
  // `a[i] = (s += b[i])` or `if ((s += x) > 0)` uses the running value, a
  // prefix sum in disguise. Discarded positions: a statement in a body or
  // compound, an if/else branch, a for-loop's init or increment, a loop body,
  // and the left side of a comma (whose right side inherits).
  std::function<void(const Stmt *, bool)> Walk = [&](const Stmt *S,
                                                     bool Discarded) {
    if (!S)
      return;
    llvm::SmallVector<const DeclRefExpr *, 4> C;
    std::string O = Discarded ? updateOp(S, V, Ctx, C) : std::string();
    if (!O.empty()) {
      if (!Op.empty() && Op != O)
        Mixed = true;
      Op = O;
      Covered.append(C.begin(), C.end());
      // An if-form update's children are its condition and assignment, all
      // accounted for; an expression update's operands were checked not to
      // mention V elsewhere. Don't descend.
      return;
    }
    if (const auto *E = dyn_cast<Expr>(S)) {
      const Expr *P = E->IgnoreParens();
      if (const auto *BO = dyn_cast<BinaryOperator>(P);
          BO && BO->getOpcode() == BO_Comma) {
        Walk(BO->getLHS(), true);
        Walk(BO->getRHS(), Discarded);
        return;
      }
      if (P != E) {
        Walk(P, Discarded);
        return;
      }
      for (const Stmt *Ch : S->children())
        Walk(Ch, false);
      return;
    }
    if (const auto *IS = dyn_cast<IfStmt>(S)) {
      Walk(IS->getInit(), true);
      Walk(IS->getCond(), false);
      Walk(IS->getThen(), true);
      Walk(IS->getElse(), true);
      return;
    }
    if (const auto *FS = dyn_cast<ForStmt>(S)) {
      Walk(FS->getInit(), true);
      Walk(FS->getCond(), false);
      Walk(FS->getInc(), true);
      Walk(FS->getBody(), true);
      return;
    }
    if (const auto *WS = dyn_cast<WhileStmt>(S)) {
      Walk(WS->getCond(), false);
      Walk(WS->getBody(), true);
      return;
    }
    if (const auto *DS = dyn_cast<DoStmt>(S)) {
      Walk(DS->getBody(), true);
      Walk(DS->getCond(), false);
      return;
    }
    // Compound, label, switch case bodies... : statements are discarded;
    // anything else (a declaration's initializer, a switch condition) uses
    // the value.
    const bool Child = isa<CompoundStmt>(S) || isa<LabelStmt>(S) ||
                       isa<CaseStmt>(S) || isa<DefaultStmt>(S) ||
                       isa<AttributedStmt>(S);
    for (const Stmt *Ch : S->children())
      Walk(Ch, Child);
  };
  Walk(Body, true);
  if (Mixed || Op.empty())
    return "";
  for (const DeclRefExpr *R : All)
    if (!llvm::is_contained(Covered, R))
      return "";
  return Op;
}

} // namespace

ScalarAnalyzer::ScalarAnalyzer(ASTContext &Ctx, bool AllowFPReductions)
    : Ctx(Ctx), AllowFP(AllowFPReductions) {}
ScalarAnalyzer::~ScalarAnalyzer() = default;

ScalarAnalyzer::FunctionLiveness &
ScalarAnalyzer::livenessFor(const FunctionDecl *FD) {
  auto &Slot = Cache[FD];
  if (Slot)
    return *Slot;
  Slot = std::make_unique<FunctionLiveness>();
  FunctionLiveness &L = *Slot;

  // Address-taken variables: never candidates.
  struct AddrV : RecursiveASTVisitor<AddrV> {
    llvm::DenseSet<const VarDecl *> &Out;
    explicit AddrV(llvm::DenseSet<const VarDecl *> &O) : Out(O) {}
    bool VisitUnaryOperator(UnaryOperator *UO) {
      if (UO->getOpcode() == UO_AddrOf)
        if (const VarDecl *V = declOf(UO->getSubExpr()))
          Out.insert(V);
      return true;
    }
    // A `cleanup` attribute reads the variable at scope exit, invisibly to
    // the dataflow below: never privatize it.
    bool VisitVarDecl(VarDecl *VD) {
      if (VD->hasAttr<CleanupAttr>())
        Out.insert(VD->getCanonicalDecl());
      return true;
    }
  } AV(L.AddressTaken);
  AV.TraverseStmt(FD->getBody());

  CFG::BuildOptions BO;
  BO.setAllAlwaysAdd();
  L.Graph = CFG::buildCFG(FD, FD->getBody(), &Ctx, BO);
  if (!L.Graph)
    return L;

  // Index every automatic scalar variable mentioned in the function.
  struct Vars : RecursiveASTVisitor<Vars> {
    FunctionLiveness &L;
    explicit Vars(FunctionLiveness &L) : L(L) {}
    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        VD = VD->getCanonicalDecl();
        if (VD->hasLocalStorage() && VD->getType()->isScalarType() &&
            !VD->getType().isVolatileQualified() && !L.AddressTaken.count(VD) &&
            !L.Index.count(VD))
          L.Index.try_emplace(VD, L.Index.size());
      }
      return true;
    }
  } VV(L);
  VV.TraverseStmt(FD->getBody());
  const unsigned N = L.Index.size();

  // Backward transfer through one block, starting from LiveOut.
  auto Transfer = [&](const CFGBlock *B, llvm::BitVector Live) {
    for (auto It = B->rbegin(); It != B->rend(); ++It) {
      std::optional<CFGStmt> CS = It->getAs<CFGStmt>();
      if (!CS)
        continue;
      const Stmt *S = CS->getStmt();
      auto Idx = [&](const Expr *E) -> int {
        if (const VarDecl *V = declOf(E)) {
          auto F = L.Index.find(V);
          if (F != L.Index.end())
            return F->second;
        }
        return -1;
      };
      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->getOpcode() == BO_Assign &&
            isa<DeclRefExpr>(BO->getLHS()->IgnoreParens())) {
          if (int I = Idx(BO->getLHS()); I >= 0)
            Live.reset(I); // a plain assignment kills
        } else if (BO->isCompoundAssignmentOp() &&
                   isa<DeclRefExpr>(BO->getLHS()->IgnoreParens())) {
          if (int I = Idx(BO->getLHS()); I >= 0)
            Live.set(I); // reads the old value
        }
      } else if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
        if ((UO->isIncrementDecrementOp() || UO->getOpcode() == UO_AddrOf) &&
            isa<DeclRefExpr>(UO->getSubExpr()->IgnoreParens()))
          if (int I = Idx(UO->getSubExpr()); I >= 0)
            Live.set(I);
      } else if (const auto *ICE = dyn_cast<ImplicitCastExpr>(S)) {
        if (ICE->getCastKind() == CK_LValueToRValue &&
            isa<DeclRefExpr>(ICE->getSubExpr()->IgnoreParens()))
          if (int I = Idx(ICE->getSubExpr()); I >= 0)
            Live.set(I);
      } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        for (const Decl *D : DS->decls())
          if (const auto *VD = dyn_cast<VarDecl>(D)) {
            auto F = L.Index.find(VD->getCanonicalDecl());
            if (F != L.Index.end() && VD->hasInit())
              Live.reset(F->second);
          }
      }
    }
    return Live;
  };

  for (const CFGBlock *B : *L.Graph)
    L.LiveIn[B] = llvm::BitVector(N);
  bool Changed = true;
  while (Changed) { // backward fixpoint; CFGs here are small
    Changed = false;
    for (auto BI = L.Graph->rbegin(); BI != L.Graph->rend(); ++BI) {
      const CFGBlock *B = *BI;
      llvm::BitVector Out(N);
      for (const CFGBlock *Succ : B->succs())
        if (Succ)
          Out |= L.LiveIn[Succ];
      llvm::BitVector In = Transfer(B, Out);
      if (In != L.LiveIn[B]) {
        L.LiveIn[B] = std::move(In);
        Changed = true;
      }
    }
  }
  return L;
}

LoopScalars ScalarAnalyzer::analyzeLoop(const LoopInfo &LI) {
  LoopScalars R;
  if (LI.Kind == LoopKind::Unrecognized || !LI.InductionVar)
    return R; // the safety pass already says UNKNOWN
  const ForStmt *FS = LI.Loop;
  const VarDecl *IV = LI.InductionVar->getCanonicalDecl();

  WriteCollector WC;
  WC.TraverseStmt(const_cast<Stmt *>(FS->getBody()));

  const FunctionDecl *FD = enclosingFunction(FS, Ctx);
  FunctionLiveness *L = FD && FD->getBody() ? &livenessFor(FD) : nullptr;
  const CFGBlock *Cond = nullptr;
  if (L && L->Graph)
    for (const CFGBlock *B : *L->Graph)
      if (B->getTerminatorStmt() == FS)
        Cond = B;
  const CFGBlock *BodyEntry = nullptr, *Exit = nullptr;
  if (Cond && Cond->succ_size() == 2) {
    BodyEntry = *Cond->succ_begin();
    Exit = *(Cond->succ_begin() + 1);
  }
  auto LiveAt = [&](const CFGBlock *B, const VarDecl *V) {
    if (!B)
      return false;
    auto I = L->Index.find(V);
    return I != L->Index.end() && L->LiveIn[B].test(I->second);
  };

  for (const VarDecl *V : WC.Written) {
    if (V == IV || WC.DeclaredInside.count(V))
      continue; // the loop's own variable; a fresh variable per iteration
    const std::string Q = "'" + V->getNameAsString() + "'";
    if (WC.StaticInside.count(V) || V->hasGlobalStorage()) {
      R.UnsafeReasons.push_back("writes to " + Q +
                                ", a global or static shared by every "
                                "iteration");
      continue;
    }
    if (!L || !Cond) {
      R.UnknownReasons.push_back("cannot analyze the uses of " + Q);
      continue;
    }
    if (L->AddressTaken.count(V)) {
      R.UnsafeReasons.push_back("writes to " + Q +
                                ", whose address is taken in this function "
                                "(a pointer could reach the shared copy)");
      continue;
    }
    if (!L->Index.count(V)) {
      R.UnsafeReasons.push_back("writes to " + Q +
                                ", which is not a plain scalar variable");
      continue;
    }
    if (mentions(FS->getCond(), V) || mentions(FS->getInc(), V)) {
      R.UnsafeReasons.push_back("writes to " + Q +
                                ", which the loop's own condition or "
                                "increment uses");
      continue;
    }
    if (!LiveAt(BodyEntry, V)) {
      // Every iteration writes V before reading it.
      if (!LiveAt(Exit, V)) {
        R.Clauses.push_back({ScalarClause::Private, V, ""});
      } else if (writtenUnconditionally(FS->getBody(), V) &&
                 !hasOwnContinue(FS->getBody())) {
        R.Clauses.push_back({ScalarClause::LastPrivate, V, ""});
      } else {
        R.UnsafeReasons.push_back(Q + " is used after the loop but not "
                                      "written on every iteration");
      }
      continue;
    }
    // Read before written in an iteration: only a reduction survives.
    std::string Op = reductionOp(FS->getBody(), V, Ctx);
    // `_Bool` normalizes every step (`!= 0`), so `-=`/`^=` aren't the
    // reductions they look like; complex types are out of scope.
    if (Op.empty() || !V->getType()->isArithmeticType() ||
        V->getType()->isBooleanType() || V->getType()->isAnyComplexType()) {
      R.UnsafeReasons.push_back("loop-carried dependence: " + Q +
                                " carries a value from one iteration to the "
                                "next");
      continue;
    }
    if (V->getType()->isFloatingType() && !AllowFP) {
      R.UnsafeReasons.push_back("floating-point reduction on " + Q +
                                " (changes the last bits of the result; "
                                "enable with -fp-reductions)");
      continue;
    }
    R.Clauses.push_back({ScalarClause::Reduction, V, Op});
  }

  // The loop's own variable, declared outside and read after the loop.
  const bool IVOutside = isa_and_nonnull<BinaryOperator>(FS->getInit());
  if (IVOutside && L && Cond) {
    const std::string Q = "'" + IV->getNameAsString() + "'";
    const bool Tracked = L->Index.count(IV);
    if (!Tracked || LiveAt(Exit, IV)) {
      if (Tracked && LI.TripCount && *LI.TripCount >= 1)
        R.Clauses.push_back({ScalarClause::LastPrivate, IV, ""});
      else
        R.UnknownReasons.push_back(
            "induction variable " + Q +
            (Tracked ? " is used after the loop, which may run zero times"
                     : " is global, static or address-taken"));
    }
  }
  return R;
}

} // namespace p05
