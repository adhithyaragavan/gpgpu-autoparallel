#include "analysis/Hoisting.h"
#include "analysis/SourceText.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

using namespace clang;

namespace p05 {

static llvm::cl::opt<HoistMode> FlagHoist(
    "hoist", llvm::cl::desc("Keep data on the device across GPU loops"),
    llvm::cl::values(
        clEnumValN(HoistMode::On, "on",
                   "one target data region around loops that share data "
                   "(Phase 2.2, default; needs -maps=ranges)"),
        clEnumValN(HoistMode::Off, "off",
                   "per-loop map() clauses only (ablation baseline)")),
    llvm::cl::init(HoistMode::On), llvm::cl::cat(optionCategory()));

HoistMode hoistModeFromFlags() { return FlagHoist; }

namespace {

const VarDecl *canon(const ValueDecl *V) {
  const auto *VD = dyn_cast_or_null<VarDecl>(V);
  return VD ? VD->getCanonicalDecl() : nullptr;
}

std::string quoted(const ValueDecl *V) {
  return "'" + V->getNameAsString() + "'";
}

const Expr *strip(const Expr *E) {
  while (true) {
    if (const auto *P = dyn_cast<ParenExpr>(E))
      E = P->getSubExpr();
    else if (const auto *C = dyn_cast<ImplicitCastExpr>(E))
      E = C->getSubExpr();
    else
      return E;
  }
}

const VarDecl *rootOf(const Expr *E);

/// The variable a pointer value B comes from: a pointer variable read by
/// name, or an array object decaying to a pointer. Null for a pointer loaded
/// from memory (`s.d`, `ptrs[0]`) or computed (`a + 1`, a call).
const VarDecl *throughPtr(const Expr *B) {
  B = B->IgnoreParens();
  if (const auto *IC = dyn_cast<ImplicitCastExpr>(B)) {
    if (IC->getCastKind() == CK_ArrayToPointerDecay)
      return rootOf(IC->getSubExpr());
    if (IC->getCastKind() == CK_LValueToRValue) {
      const auto *DRE = dyn_cast<DeclRefExpr>(IC->getSubExpr()->IgnoreParens());
      return DRE ? canon(DRE->getDecl()) : nullptr;
    }
    if (IC->getCastKind() == CK_NoOp || IC->getCastKind() == CK_BitCast)
      return throughPtr(IC->getSubExpr());
    return nullptr;
  }
  return nullptr;
}

/// The variable whose storage the lvalue E is part of (through subscripts
/// and `.` members of arrays and structs), or the pointer variable `*p`,
/// `p[i]` or `p->f` goes through. Null when the memory is reached through a
/// pointer loaded from memory or computed.
const VarDecl *rootOf(const Expr *E) {
  E = E->IgnoreParens();
  while (true) {
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return canon(DRE->getDecl());
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
      return throughPtr(ASE->getBase());
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->isArrow())
        return throughPtr(ME->getBase());
      E = ME->getBase()->IgnoreParens();
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(E);
        UO && UO->getOpcode() == UO_Deref)
      return throughPtr(UO->getSubExpr());
    return nullptr;
  }
}

/// Memory the program reaches through a VarDecl: an array object or a
/// pointer.
bool isMemoryVar(const VarDecl *V) {
  QualType T = V->getType();
  return T->isPointerType() || T->isArrayType();
}

} // namespace

HoistPlanner::HoistPlanner(ASTContext &Ctx, const std::vector<LoopInfo> &Loops,
                           CallResolver &Resolver, SafetyAnalyzer &Safety,
                           AliasAnalyzer *Alias, ProfitabilityAnalyzer &Profit,
                           Policy PolicyMode)
    : Ctx(Ctx), Loops(Loops), Resolver(Resolver), SafetyA(Safety),
      Alias(Alias), Profit(Profit), PolicyMode(PolicyMode) {}

struct HoistPlanner::Impl {
  HoistPlanner &P;
  const std::vector<LoopSafety> &Safety;
  const std::vector<std::optional<RegionPlan>> &Plans;
  std::vector<ProfitabilityVerdict> &V;
  const SourceManager &SM;
  llvm::DenseMap<const ForStmt *, int> IndexOf;
  std::vector<bool> Emitted, GpuAble, Potential;
  std::map<int, std::optional<ProfitabilityVerdict>> ResidentCache;
  std::vector<DataRegion> Out;

  Impl(HoistPlanner &P, const std::vector<LoopSafety> &Safety,
       const std::vector<std::optional<RegionPlan>> &Plans,
       std::vector<ProfitabilityVerdict> &V)
      : P(P), Safety(Safety), Plans(Plans), V(V),
        SM(P.Ctx.getSourceManager()) {}

  // ---- source ranges -------------------------------------------------------

  bool inRange(SourceRange R, SourceLocation L) const {
    return !SM.isBeforeInTranslationUnit(L, R.getBegin()) &&
           !SM.isBeforeInTranslationUnit(R.getEnd(), L);
  }
  SourceRange rangeOf(const std::vector<const Stmt *> &Stmts) const {
    return {Stmts.front()->getBeginLoc(), Stmts.back()->getEndLoc()};
  }
  bool rewritable(int K) const {
    SourceLocation B = P.Loops[K].Loop->getBeginLoc();
    return SM.isWrittenInMainFile(B) && !B.isMacroID() &&
           !underLoopHint(P.Loops[K].Loop);
  }
  /// `#pragma clang loop` / `#pragma unroll` hints: nothing can go between
  /// one and its loop (OmpRewriter declines such loops too).
  bool underLoopHint(const Stmt *S) const {
    DynTypedNodeList Ps = P.Ctx.getParents(*S);
    return !Ps.empty() && Ps[0].get<AttributedStmt>();
  }
  bool contains(const Stmt *S, int K) const {
    return inRange(S->getSourceRange(), P.Loops[K].Loop->getBeginLoc());
  }
  bool containsAny(const Stmt *S, const std::vector<bool> &Set) const {
    for (size_t K = 0; K < Set.size(); ++K)
      if (Set[K] && contains(S, K))
        return true;
    return false;
  }

  /// Variables with static storage whose address is taken anywhere in the
  /// translation unit (any of them may then change through a pointer).
  llvm::DenseSet<const VarDecl *> GlobalAddrTaken;
  void indexGlobalAddrs() {
    struct A : RecursiveASTVisitor<A> {
      llvm::DenseSet<const VarDecl *> *Out;
      bool VisitUnaryOperator(UnaryOperator *UO) {
        if (UO->getOpcode() == UO_AddrOf)
          if (const auto *D = dyn_cast<DeclRefExpr>(strip(UO->getSubExpr())))
            if (const VarDecl *V = canon(D->getDecl()); V && V->hasGlobalStorage())
              Out->insert(V);
        return true;
      }
    } Vis;
    Vis.Out = &GlobalAddrTaken;
    Vis.TraverseDecl(P.Ctx.getTranslationUnitDecl());
  }

  static bool neverReturns(llvm::StringRef N) {
    return N == "exit" || N == "_exit" || N == "_Exit" || N == "quick_exit" ||
           N == "abort" || N == "longjmp" || N == "_longjmp" ||
           N == "siglongjmp" || N == "__longjmp_chk" || N == "pthread_exit" ||
           N == "thrd_exit";
  }

  /// Adds to C the condition under which loop L (around a GPU loop in a
  /// region) runs at least once, and its names to Names; false, with Why,
  /// when that can't be written.
  template <typename AddFn>
  bool loopRuns(const Stmt *L, AddFn &AddPart,
                llvm::SetVector<const VarDecl *> &Names, std::string &Why) {
    if (isa<DoStmt>(L))
      return true;
    const auto *FS = dyn_cast<ForStmt>(L);
    auto It = FS ? IndexOf.find(FS) : IndexOf.end();
    if (It == IndexOf.end()) {
      Why = "a while loop around a GPU loop may not run, and a pointer's "
            "section is owned memory only when the loop touches it";
      return false;
    }
    const LoopInfo &LI = P.Loops[It->second];
    const char *Op = LI.CmpOp == BO_LT   ? "<"
                     : LI.CmpOp == BO_LE ? "<="
                     : LI.CmpOp == BO_GT ? ">"
                     : LI.CmpOp == BO_GE ? ">="
                     : LI.CmpOp == BO_NE ? "!="
                                         : nullptr;
    if (LI.Kind == LoopKind::Unrecognized || !Op || !LI.StartExpr ||
        !LI.BoundExpr || !LI.InductionVar) {
      Why = "a loop around a GPU loop isn't a counted loop, so whether it "
            "runs can't be tested before the region";
      return false;
    }
    auto Text = [&](const Expr *E, std::string &Out) {
      SourceRange SR = E->getSourceRange();
      if (SR.getBegin().isMacroID() || SR.getEnd().isMacroID())
        return false;
      Out = Lexer::getSourceText(CharSourceRange::getTokenRange(SR), SM,
                                 P.Ctx.getLangOpts())
                .str();
      struct N : RecursiveASTVisitor<N> {
        llvm::SetVector<const VarDecl *> *Names;
        bool Ok = true;
        bool VisitDeclRefExpr(DeclRefExpr *D) {
          if (const VarDecl *V = canon(D->getDecl()))
            Names->insert(V);
          return true;
        }
        bool VisitCallExpr(CallExpr *) { return Ok = false; }
      } Vis;
      Vis.Names = &Names;
      Vis.TraverseStmt(const_cast<Expr *>(E));
      return Vis.Ok && !Out.empty();
    };
    std::string St, Bd;
    if (!Text(LI.StartExpr, St) || !Text(LI.BoundExpr, Bd)) {
      Why = "the bounds of a loop around a GPU loop can't be written before "
            "the region";
      return false;
    }
    AddPart("(" + LI.InductionVar->getType().getAsString() + ")(" + St +
            ") " + Op + " (" + Bd + ")");
    return true;
  }

  /// Prefix + the first number that names nothing in the translation unit.
  unsigned NextFresh = 0;
  std::string freshName(const std::string &Prefix) {
    while (true) {
      std::string N = Prefix + std::to_string(NextFresh++);
      if (P.Ctx.Idents.find(N) == P.Ctx.Idents.end())
        return N;
    }
  }

  // ---- what a region moves (Phase 2.3) ---------------------------------------

  /// How often a statement runs per entry of a region: Coef * t^Pow, t
  /// standing for any trip count not known at compile time.
  struct Mono {
    double Coef = 1.0;
    unsigned Pow = 0;
  };
  /// A sum of those.
  struct Runs {
    std::map<unsigned, double> Terms;
    void add(Mono M, double Times) { Terms[M.Pow] += M.Coef * Times; }
    double at(double T) const {
      double V = 0.0;
      for (const auto &[P, C] : Terms)
        V += C * std::pow(T, static_cast<double>(P));
      return V;
    }
    /// This over O, the most it is at t = 1, 2, 4, ..., 2^30 and as t ->
    /// infinity (a ratio of two such sums can peak in between, so it is
    /// sampled rather than taken at the ends).
    double ratioTo(const Runs &O) const {
      if (O.Terms.empty())
        return 0.0;
      double Max = 0.0;
      for (int E = 0; E <= 30; ++E) {
        const double T = std::ldexp(1.0, E);
        const double B = O.at(T);
        if (B > 0.0)
          Max = std::max(Max, at(T) / B);
      }
      double Lim = 0.0;
      if (!Terms.empty()) {
        const auto &[PA, CA] = *Terms.rbegin();
        const auto &[PB, CB] = *O.Terms.rbegin();
        Lim = PA > PB   ? std::numeric_limits<double>::infinity()
              : PA < PB ? 0.0
                        : CA / CB;
      }
      return std::max(Max, Lim);
    }
    std::string text() const {
      std::string Out;
      for (auto It = Terms.rbegin(); It != Terms.rend(); ++It) {
        if (!Out.empty())
          Out += " + ";
        std::string C = fmtCount(It->second);
        Out += It->first == 0 ? C
               : (C == "1" ? "" : C + "*") + "t" +
                     (It->first > 1 ? "^" + std::to_string(It->first) : "");
      }
      return Out.empty() ? "0" : Out;
    }
  };
  static std::string fmtCount(double X) {
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    if (X == std::floor(X) && X < 1e15)
      OS << static_cast<int64_t>(X);
    else
      OS << llvm::format("%.3g", X);
    return Buf;
  }
  static std::string fmtRatio(double X) {
    if (std::isinf(X))
      return "unbounded";
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    OS << llvm::format("%.3g", X) << "x";
    return Buf;
  }

  /// Runs of S per entry of the region whose top statements are Top: the
  /// trip counts of the loops between them (not S itself).
  Mono runsIn(const Stmt *S, const llvm::DenseSet<const Stmt *> &Top) {
    Mono M;
    const Stmt *Cur = S;
    while (!Top.count(Cur)) {
      DynTypedNodeList Ps = P.Ctx.getParents(*Cur);
      const Stmt *Par = Ps.empty() ? nullptr : Ps[0].get<Stmt>();
      if (!Par)
        break;
      Cur = Par;
      if (const auto *FS = dyn_cast<ForStmt>(Cur)) {
        auto It = IndexOf.find(FS);
        // A loop it can break out of may stop after any step.
        if (It != IndexOf.end() && !breaksOut(FS->getBody()) &&
            P.Loops[It->second].TripCount &&
            *P.Loops[It->second].TripCount > 0) {
          M.Coef *= static_cast<double>(*P.Loops[It->second].TripCount);
          continue;
        }
      }
      if (isa<ForStmt>(Cur) || isa<WhileStmt>(Cur) || isa<DoStmt>(Cur))
        ++M.Pow;
    }
    return M;
  }

  /// Whether S has a break that leaves the loop S is the body of.
  static bool breaksOut(const Stmt *S) {
    if (!S)
      return false;
    if (isa<BreakStmt>(S))
      return true;
    if (isa<ForStmt>(S) || isa<WhileStmt>(S) || isa<DoStmt>(S) ||
        isa<SwitchStmt>(S))
      return false; // its breaks are its own
    for (const Stmt *C : S->children())
      if (breaksOut(C))
        return true;
    return false;
  }

  /// Stands in C's parts for loop K's cost guard until it is priced.
  static std::string guardSlot(int K) { return "\x01" + std::to_string(K); }
  static std::optional<int> slotOf(const std::string &G) {
    if (G.empty() || G[0] != '\x01')
      return std::nullopt;
    return std::stoi(G.substr(1));
  }

  // ---- loop facts ------------------------------------------------------------

  const ProfitabilityVerdict *resident(int K) {
    auto It = ResidentCache.find(K);
    if (It == ResidentCache.end()) {
      std::optional<ProfitabilityVerdict> R;
      if (GpuAble[K])
        R = P.Profit.priceResident(P.Loops[K], Safety[K], *Plans[K], 0.0);
      It = ResidentCache.emplace(K, std::move(R)).first;
    }
    return It->second ? &*It->second : nullptr;
  }
  bool residentGpu(int K) {
    if (PolicyNaive())
      return GpuAble[K];
    const ProfitabilityVerdict *R = resident(K);
    return R && R->Target == OffloadTarget::GpuOffload;
  }
  bool PolicyNaive() const { return P.PolicyMode == Policy::Naive; }

  void classify() {
    const size_t N = P.Loops.size();
    for (size_t K = 0; K < N; ++K)
      IndexOf[P.Loops[K].Loop] = K;
    Emitted.assign(N, false);
    GpuAble.assign(N, false);
    Potential.assign(N, false);
    for (size_t K = 0; K < N; ++K) {
      const LoopSafety &S = Safety[K];
      const bool Ok = rewritable(K);
      // The rewriter annotates a loop unless one around it already is.
      bool Under = false;
      for (int A = P.Loops[K].Parent; A >= 0; A = P.Loops[A].Parent)
        Under |= Emitted[A];
      // As OmpRewriter::rewriteLoop: a CPU verdict the model's own numbers
      // contradict is declined there.
      Emitted[K] = Ok && !Under && V[K].Evaluated &&
                   (V[K].Target == OffloadTarget::GpuOffload ||
                    (V[K].Target == OffloadTarget::CpuParallel &&
                     V[K].CpuBeatsSeq));
      GpuAble[K] = Ok && V[K].Evaluated && V[K].RangeMaps &&
                   S.Verdict == SafetyVerdict::Safe && Plans[K] &&
                   Plans[K]->Mappable && S.CalleesMappable &&
                   S.AliasGuard.empty();
    }
    // A target construct may not run inside another construct
    // (declineNestedConstructs): no loop goes to the GPU here that calls a
    // function with a candidate loop, or sits in one a candidate calls.
    {
      std::vector<const FunctionDecl *> Fn(N);
      std::vector<llvm::DenseSet<const FunctionDecl *>> Reach(N);
      llvm::DenseSet<const FunctionDecl *> HasCand, CalledByCand;
      for (size_t K = 0; K < N; ++K) {
        Fn[K] = functionOf(P.Ctx, P.Loops[K].Loop);
        Reach[K] = reachableFrom(P.Resolver, P.Loops[K]);
        if (Emitted[K] || GpuAble[K]) {
          HasCand.insert(Fn[K]);
          CalledByCand.insert(Reach[K].begin(), Reach[K].end());
        }
      }
      for (size_t K = 0; K < N; ++K)
        if (GpuAble[K] &&
            (CalledByCand.count(Fn[K]) ||
             llvm::any_of(Reach[K], [&](const FunctionDecl *F) {
               return HasCand.count(F) > 0;
             })))
          GpuAble[K] = false;
    }
    for (size_t K = 0; K < N; ++K)
      Potential[K] = GpuAble[K] &&
                     (V[K].Target == OffloadTarget::GpuOffload || residentGpu(K));
  }

  // ---- candidates ------------------------------------------------------------

  void processFunction(const FunctionDecl *F) {
    const auto *Body = dyn_cast_or_null<CompoundStmt>(F->getBody());
    if (!Body || !containsAny(Body, Potential))
      return;
    // A goto could jump into a region: none in functions that have one.
    struct G : RecursiveASTVisitor<G> {
      bool Found = false;
      bool VisitGotoStmt(GotoStmt *) { return !(Found = true); }
      bool VisitIndirectGotoStmt(IndirectGotoStmt *) { return !(Found = true); }
    } Gotos;
    Gotos.TraverseStmt(const_cast<CompoundStmt *>(Body));
    if (Gotos.Found)
      return;
    processCompound(F, Body);
  }

  void processCompound(const FunctionDecl *F, const CompoundStmt *CS) {
    std::vector<const Stmt *> Kids(CS->body_begin(), CS->body_end());
    int First = -1, Last = -1;
    for (int I = 0; I < static_cast<int>(Kids.size()); ++I)
      if (containsAny(Kids[I], Potential)) {
        if (First < 0)
          First = I;
        Last = I;
      }
    if (First < 0)
      return;
    if (First == Last) {
      tryStmt(F, Kids[First]);
      return;
    }
    std::vector<const Stmt *> Span(Kids.begin() + First,
                                   Kids.begin() + Last + 1);
    std::vector<const Stmt *> After(Kids.begin() + Last + 1, Kids.end());
    if (tryRegion(F, Span, CS, After))
      return;
    for (int I = First; I <= Last; ++I)
      if (containsAny(Kids[I], Potential))
        tryStmt(F, Kids[I]);
  }

  void tryStmt(const FunctionDecl *F, const Stmt *S) {
    if (!S || !containsAny(S, Potential))
      return;
    if (const auto *FS = dyn_cast<ForStmt>(S)) {
      auto It = IndexOf.find(FS);
      // An annotated loop covers everything inside it; a GPU loop alone
      // runs once.
      if (It != IndexOf.end() && (Emitted[It->second] || Potential[It->second]))
        return;
    }
    if (isa<ForStmt>(S) || isa<WhileStmt>(S) || isa<DoStmt>(S)) {
      if (tryRegion(F, {S}, nullptr, {}))
        return;
      const Stmt *Body = isa<ForStmt>(S)     ? cast<ForStmt>(S)->getBody()
                         : isa<WhileStmt>(S) ? cast<WhileStmt>(S)->getBody()
                                             : cast<DoStmt>(S)->getBody();
      tryStmt(F, Body);
      return;
    }
    if (const auto *CS = dyn_cast<CompoundStmt>(S))
      return processCompound(F, CS);
    if (const auto *IS = dyn_cast<IfStmt>(S)) {
      tryStmt(F, IS->getThen());
      tryStmt(F, IS->getElse());
      return;
    }
    if (const auto *SS = dyn_cast<SwitchStmt>(S))
      return tryStmt(F, SS->getBody());
    if (const auto *LS = dyn_cast<LabelStmt>(S))
      return tryStmt(F, LS->getSubStmt());
    if (const auto *AS = dyn_cast<AttributedStmt>(S)) {
      // The hinted loop itself can't take a pragma; its body can.
      const Stmt *Sub = AS->getSubStmt();
      if (const auto *FS = dyn_cast<ForStmt>(Sub))
        return tryStmt(F, FS->getBody());
      if (const auto *WS = dyn_cast<WhileStmt>(Sub))
        return tryStmt(F, WS->getBody());
      if (const auto *DS = dyn_cast<DoStmt>(Sub))
        return tryStmt(F, DS->getBody());
      return tryStmt(F, Sub);
    }
  }

  // ---- one candidate region ---------------------------------------------------

  struct Touch {
    llvm::SetVector<const VarDecl *> Reads, Writes;
  };

  /// Collects what code outside the GPU loops does to the mapped bases M.
  /// Returns false, with Why, when it does something no update can repair.
  bool hostTouch(const FunctionDecl *F, const Stmt *S,
                 const llvm::SetVector<const VarDecl *> &M, Touch &T,
                 std::string &Why) {
    struct W : RecursiveASTVisitor<W> {
      Impl &I;
      const FunctionDecl *F;
      const llvm::SetVector<const VarDecl *> &M;
      Touch &T;
      std::string &Why;
      bool Ok = true;
      llvm::SetVector<const VarDecl *> Others;
      W(Impl &I, const FunctionDecl *F, const llvm::SetVector<const VarDecl *> &M,
        Touch &T, std::string &Why)
          : I(I), F(F), M(M), T(T), Why(Why) {}
      bool fail(std::string Msg) {
        if (Ok)
          Why = std::move(Msg);
        Ok = false;
        return false;
      }
      const Stmt *parent(const Stmt *S) {
        DynTypedNodeList Ps = I.P.Ctx.getParents(*S);
        return Ps.empty() ? nullptr : Ps[0].get<Stmt>();
      }
      /// How the lvalue chain starting at E (an access) is used.
      void accessUse(const Expr *E, const VarDecl *B) {
        const Stmt *Cur = E;
        const Stmt *Par = parent(Cur);
        while (Par) {
          if (isa<ParenExpr>(Par) || isa<ImplicitCastExpr>(Par)) {
          } else if (const auto *A = dyn_cast<ArraySubscriptExpr>(Par)) {
            if (strip(A->getBase()) != strip(cast<Expr>(Cur)) &&
                A->getBase() != Cur)
              break;
          } else if (const auto *ME = dyn_cast<MemberExpr>(Par)) {
            if (ME->isArrow())
              break;
          } else {
            break;
          }
          Cur = Par;
          Par = parent(Cur);
        }
        bool Write = false, Read = true;
        if (const auto *BO = dyn_cast_or_null<BinaryOperator>(Par)) {
          if (BO->isAssignmentOp() && BO->getLHS() == Cur) {
            Write = true;
            Read = BO->isCompoundAssignmentOp();
          }
        } else if (const auto *UO = dyn_cast_or_null<UnaryOperator>(Par)) {
          if (UO->isIncrementDecrementOp())
            Write = true;
          else if (UO->getOpcode() == UO_AddrOf)
            fail("host code takes the address of " + quoted(B) +
                 "'s elements");
        }
        if (Read)
          T.Reads.insert(B);
        if (Write)
          T.Writes.insert(B);
      }
      bool VisitDeclRefExpr(DeclRefExpr *DRE) {
        const VarDecl *VD = canon(DRE->getDecl());
        if (!VD || !isMemoryVar(VD))
          return true;
        if (!M.count(VD)) {
          Others.insert(VD);
          return true;
        }
        // Only a direct subscript, *p or p->f: the pointer must not escape.
        const Stmt *Cur = DRE, *Par = parent(DRE);
        while (Par && (isa<ImplicitCastExpr>(Par) || isa<ParenExpr>(Par))) {
          Cur = Par;
          Par = parent(Par);
        }
        const Expr *Acc = nullptr;
        if (const auto *A = dyn_cast_or_null<ArraySubscriptExpr>(Par);
            A && strip(A->getBase()) == DRE)
          Acc = A;
        else if (const auto *U = dyn_cast_or_null<UnaryOperator>(Par);
                 U && U->getOpcode() == UO_Deref)
          Acc = U;
        else if (const auto *ME = dyn_cast_or_null<MemberExpr>(Par);
                 ME && ME->isArrow())
          Acc = ME;
        if (!Acc)
          return fail("host code uses " + quoted(VD) +
                      " other than to subscript it");
        accessUse(Acc, VD);
        return Ok;
      }
      bool opaqueBase(const Expr *E) {
        if (!rootOf(E))
          return fail("host code reaches memory through a pointer it "
                      "loaded or computed");
        return true;
      }
      bool VisitArraySubscriptExpr(ArraySubscriptExpr *A) {
        return opaqueBase(A);
      }
      bool VisitUnaryOperator(UnaryOperator *U) {
        return U->getOpcode() != UO_Deref || opaqueBase(U);
      }
      bool VisitMemberExpr(MemberExpr *ME) {
        return !ME->isArrow() || opaqueBase(ME);
      }
      bool VisitCallExpr(CallExpr *CE) {
        const FunctionDecl *Callee = CE->getDirectCallee();
        if (!Callee)
          return fail("host code calls through a function pointer");
        // A function without a visible body may reach mapped data through
        // a pointer stored anywhere (another file's global). Only library
        // functions (declared in system headers) are trusted to reach
        // memory through their arguments alone, which are checked here like
        // any host code; a function-pointer argument could call back.
        std::vector<const FunctionDecl *> Fns;
        std::vector<const FunctionDecl *> Opaque;
        if (const FunctionDecl *Def = Callee->getDefinition()) {
          Fns.push_back(Def);
          for (const ResolvedCallee &RC :
               I.P.Resolver.getReachable(Def).Reachable) {
            if (!RC.HasVisibleBody)
              Opaque.push_back(RC.Callee);
            else
              Fns.push_back(RC.Callee->getDefinition()
                                ? RC.Callee->getDefinition()
                                : RC.Callee);
          }
        } else {
          Opaque.push_back(Callee);
        }
        for (const FunctionDecl *O : Opaque)
          if (!I.SM.isInSystemHeader(O->getLocation()))
            return fail("host code calls '" + O->getNameAsString() +
                        "' with no visible body, which may reach mapped "
                        "data through a pointer stored elsewhere");
        for (const Expr *Arg : CE->arguments()) {
          QualType T = Arg->IgnoreParenImpCasts()->getType();
          if (T->isFunctionPointerType() || T->isFunctionType())
            return fail("host code passes a function to '" +
                        Callee->getNameAsString() + "'");
        }
        for (const FunctionDecl *Fn : Fns) {
          // Its own GPU loops would find the data present and use the
          // device's copy while the host changes its own.
          if (Fn->getBody())
            for (size_t K = 0; K < I.P.Loops.size(); ++K)
              if ((I.V[K].Target == OffloadTarget::GpuOffload ||
                   I.Potential[K]) &&
                  I.contains(Fn->getBody(), K))
                return fail("host code calls '" + Fn->getNameAsString() +
                            "', which offloads loops of its own");
          const FunctionEffects &E = I.P.SafetyA.getEffects(Fn);
          if (E.AccessesThroughPointer || E.HasUnknownWrite)
            return fail("host code calls '" + Fn->getNameAsString() +
                        "', which reaches memory through a pointer");
          for (const VarDecl *G : E.GlobalsUsed)
            if (M.count(canon(G))) {
              T.Reads.insert(canon(G));
              T.Writes.insert(canon(G));
            }
        }
        return true;
      }
    } Walk(*this, F, M, T, Why);
    Walk.TraverseStmt(const_cast<Stmt *>(S));
    if (!Walk.Ok)
      return false;
    // Any other memory host code touches must be apart from the mapped data.
    // A local or global pointer may point anywhere, mapped data included
    // (AliasAnalyzer's proofs are about parameters and objects).
    for (const VarDecl *O : Walk.Others)
      if (O->getType()->isPointerType() && !isa<ParmVarDecl>(O)) {
        Why = "host code uses pointer " + quoted(O) +
              ", which may point into mapped data";
        return false;
      }
    if (P.Alias)
      for (const VarDecl *O : Walk.Others)
        for (const VarDecl *B : M) {
          const AliasProof Pr = P.Alias->proof(F, O, B);
          if (Pr == AliasProof::Unproven) {
            Why = "host code uses " + quoted(O) + ", which may overlap " +
                  quoted(B);
            return false;
          }
        }
    return true;
  }

  /// break/continue leaving S, or return/goto anywhere in it.
  static bool jumpsOut(const Stmt *S, bool AllowBreakContinue) {
    std::function<bool(const Stmt *, bool)> Walk = [&](const Stmt *X,
                                                       bool InLoop) -> bool {
      if (!X)
        return false;
      if (isa<ReturnStmt>(X) || isa<GotoStmt>(X) || isa<IndirectGotoStmt>(X))
        return true;
      if ((isa<BreakStmt>(X) || isa<ContinueStmt>(X)) && !InLoop &&
          !AllowBreakContinue)
        return true;
      // A case label inside S belonging to a switch outside it is a way in.
      if (isa<SwitchCase>(X) && !InLoop)
        return true;
      const bool Nested = InLoop || isa<ForStmt>(X) || isa<WhileStmt>(X) ||
                          isa<DoStmt>(X) || isa<SwitchStmt>(X);
      for (const Stmt *C : X->children())
        if (Walk(C, Nested))
          return true;
      return false;
    };
    return Walk(S, false);
  }

  bool tryRegion(const FunctionDecl *F, const std::vector<const Stmt *> &R,
                 const CompoundStmt *Block,
                 const std::vector<const Stmt *> &After) {
    const SourceRange RR = rangeOf(R);
    const unsigned Line = SM.getPresumedLineNumber(RR.getBegin());
    const bool Braces = Block != nullptr;
    auto InR = [&](int K) { return inRange(RR, P.Loops[K].Loop->getBeginLoc()); };

    // --- sites -----------------------------------------------------------
    std::vector<int> Members; // loops in R, pre-order
    for (size_t K = 0; K < P.Loops.size(); ++K)
      if (InR(K))
        Members.push_back(K);
    auto AncestorInR = [&](int K, auto Pred) {
      for (int A = P.Loops[K].Parent; A >= 0 && InR(A); A = P.Loops[A].Parent)
        if (Pred(A))
          return true;
      return false;
    };
    std::set<int> Sites;
    std::set<int> HostLoops; // annotated CPU loops staying on the host
    for (int K : Members) {
      if (AncestorInR(K, [&](int A) { return Sites.count(A) || HostLoops.count(A); }))
        continue;
      if (Potential[K])
        Sites.insert(K);
      else if (Emitted[K])
        HostLoops.insert(K);
    }
    if (Sites.empty())
      return false;
    // Reported even when a smaller region inside succeeds later: both are
    // part of the story.
    auto Fail = [&](const std::string &Why) {
      for (int K : Sites)
        V[K].Reasons.push_back("data region (line " + std::to_string(Line) +
                               "): not hoisted: " + Why);
      return false;
    };

    // Mapped bases; then every other loop that touches them and can go to
    // the GPU joins (a host loop on resident data would read stale values).
    llvm::SetVector<const VarDecl *> M;
    auto AddBases = [&](int K) {
      for (const ArrayRegion &Reg : Plans[K]->Regions)
        M.insert(canon(Reg.Base));
    };
    for (int K : Sites)
      AddBases(K);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (int K : Members) {
        if (Sites.count(K) || !GpuAble[K] ||
            AncestorInR(K, [&](int A) { return Sites.count(A) > 0; }))
          continue;
        if (AncestorInR(K, [&](int A) { return HostLoops.count(A) > 0; }) &&
            !HostLoops.count(K))
          continue;
        bool Touches = llvm::any_of(Plans[K]->Regions, [&](const ArrayRegion &Reg) {
          return M.count(canon(Reg.Base)) > 0;
        });
        if (!Touches)
          continue;
        // Its descendants' sites fold into it.
        for (auto It = Sites.begin(); It != Sites.end();)
          It = AncestorInR(*It, [&](int A) { return A == K; }) ? Sites.erase(It)
                                                                : std::next(It);
        Sites.insert(K);
        HostLoops.erase(K);
        AddBases(K);
        Changed = true;
      }
    }
    if (M.empty())
      return Fail("its GPU loops map no arrays");
    for (const VarDecl *B : M)
      if (B->getType()->isPointerType() && !isa<ParmVarDecl>(B))
        return Fail(quoted(B) + " is a local or global pointer: what else "
                    "points into its data can't be told");
    if (isa<AttributedStmt>(R.front()))
      return Fail("it starts with a loop hint pragma");
    std::vector<bool> IsSite(P.Loops.size(), false);
    for (int K : Sites)
      IsSite[K] = true;

    // --- runs at least twice per entry -------------------------------------
    bool Repeats = Sites.size() >= 2;
    for (int K : Sites) {
      if (Repeats)
        break;
      const Stmt *Cur = P.Loops[K].Loop;
      while (true) {
        DynTypedNodeList Ps = P.Ctx.getParents(*Cur);
        const Stmt *Par = Ps.empty() ? nullptr : Ps[0].get<Stmt>();
        if (!Par || !inRange(RR, Par->getBeginLoc()))
          break;
        if (isa<ForStmt>(Par) || isa<WhileStmt>(Par) || isa<DoStmt>(Par)) {
          Repeats = true;
          break;
        }
        Cur = Par;
      }
    }
    if (!Repeats)
      return Fail("its GPU loops run only once each time it is entered");

    // --- control flow ------------------------------------------------------
    for (const Stmt *S : R)
      if (jumpsOut(S, /*AllowBreakContinue=*/!Braces))
        return Fail(Braces ? "a statement in it can jump out of it"
                           : "it contains a return or goto");
    if (Braces) {
      // Declarations in the run would end with the new { }.
      llvm::DenseSet<const VarDecl *> Declared;
      for (const Stmt *S : R)
        if (const auto *DS = dyn_cast<DeclStmt>(S))
          for (const Decl *D : DS->decls()) {
            if (const auto *VD = dyn_cast<VarDecl>(D))
              Declared.insert(VD->getCanonicalDecl());
            else
              return Fail("it declares a type or tag, which the new { } "
                          "would hide from what follows");
          }
      if (!Declared.empty()) {
        struct U : RecursiveASTVisitor<U> {
          llvm::DenseSet<const VarDecl *> *D;
          std::string Used;
          bool VisitDeclRefExpr(DeclRefExpr *E) {
            if (const VarDecl *V = canon(E->getDecl()); V && D->count(V)) {
              Used = V->getNameAsString();
              return false;
            }
            return true;
          }
        } Uses;
        Uses.D = &Declared;
        for (const Stmt *S : After)
          if (!Uses.TraverseStmt(const_cast<Stmt *>(S)))
            return Fail("it declares '" + Uses.Used + "', used after it");
      }
    }

    // --- the condition C and the names it and the sections use --------------
    std::vector<std::string> Parts;
    llvm::SetVector<const VarDecl *> Names;
    auto AddPart = [&](const std::string &G) {
      if (!G.empty() && !llvm::is_contained(Parts, G))
        Parts.push_back(G);
    };
    for (int K : Sites) {
      for (const ValueDecl *N : Plans[K]->Names)
        if (const VarDecl *VD = canon(N))
          Names.insert(VD);
      AddPart(Plans[K]->MapGuard);
      // Its cost guard, once the transfer it shares is known (below).
      Parts.push_back(guardSlot(K));
    }
    // Every GPU loop must run on each entry of R: a hull's ends are owned
    // memory only because loops touch them, and a region whose loops don't
    // run would still map it. On the way to R only blocks and loop bodies;
    // for a pointer, each loop around must also be known to run (its first
    // test goes into C), so a while loop is refused.
    const bool Pointers = llvm::any_of(M, [](const VarDecl *B) {
      return !B->getType()->isArrayType();
    });
    llvm::DenseSet<const Stmt *> RSet(R.begin(), R.end());
    for (int K : Sites) {
      const Stmt *Cur = P.Loops[K].Loop;
      while (true) {
        const bool Top = RSet.count(Cur);
        if (Cur != P.Loops[K].Loop &&
            (isa<ForStmt>(Cur) || isa<WhileStmt>(Cur) || isa<DoStmt>(Cur))) {
          std::string Why;
          if (Pointers && !loopRuns(Cur, AddPart, Names, Why))
            return Fail(Why);
        }
        if (Top)
          break;
        DynTypedNodeList Ps = P.Ctx.getParents(*Cur);
        const Stmt *Par = Ps.empty() ? nullptr : Ps[0].get<Stmt>();
        if (!Par)
          return Fail("a GPU loop sits where this pass can't follow it");
        const bool Body =
            isa<CompoundStmt>(Par) || isa<LabelStmt>(Par) ||
            isa<AttributedStmt>(Par) ||
            (isa<ForStmt>(Par) && cast<ForStmt>(Par)->getBody() == Cur) ||
            (isa<WhileStmt>(Par) && cast<WhileStmt>(Par)->getBody() == Cur) ||
            (isa<DoStmt>(Par) && cast<DoStmt>(Par)->getBody() == Cur);
        if (!Body)
          return Fail("a GPU loop in it runs only under a condition");
        Cur = Par;
      }
    }

    // Written or address-taken in R (by name), or address-taken in F.
    llvm::DenseSet<const VarDecl *> WrittenInR, AddrTaken;
    struct Wr : RecursiveASTVisitor<Wr> {
      llvm::DenseSet<const VarDecl *> *Out;
      bool AddrOnly = false;
      llvm::DenseSet<const VarDecl *> *Addr;
      void mark(const Expr *E) {
        if (const auto *D = dyn_cast<DeclRefExpr>(strip(E)))
          if (const VarDecl *V = canon(D->getDecl()))
            Out->insert(V);
      }
      bool VisitBinaryOperator(BinaryOperator *BO) {
        if (!AddrOnly && BO->isAssignmentOp())
          mark(BO->getLHS());
        return true;
      }
      bool VisitUnaryOperator(UnaryOperator *UO) {
        if (UO->getOpcode() == UO_AddrOf) {
          if (const auto *D = dyn_cast<DeclRefExpr>(strip(UO->getSubExpr())))
            if (const VarDecl *V = canon(D->getDecl()))
              Addr->insert(V);
        } else if (!AddrOnly && UO->isIncrementDecrementOp()) {
          mark(UO->getSubExpr());
        }
        return true;
      }
      bool VisitDeclStmt(DeclStmt *DS) {
        if (!AddrOnly)
          for (const Decl *D : DS->decls())
            if (const auto *V = dyn_cast<VarDecl>(D))
              Out->insert(V->getCanonicalDecl());
        return true;
      }
    } WrV;
    WrV.Out = &WrittenInR;
    WrV.Addr = &AddrTaken;
    for (const Stmt *S : R)
      WrV.TraverseStmt(const_cast<Stmt *>(S));
    WrV.AddrOnly = true;
    WrV.TraverseStmt(F->getBody());

    // Calls anywhere in R (device helpers included): what they may write.
    struct Calls : RecursiveASTVisitor<Calls> {
      std::vector<const CallExpr *> Out;
      bool VisitCallExpr(CallExpr *CE) {
        Out.push_back(CE);
        return true;
      }
    } CallsV;
    for (const Stmt *S : R)
      CallsV.TraverseStmt(const_cast<Stmt *>(S));

    // A call that never returns (or jumps away) would skip the region's end.
    for (const CallExpr *CE : CallsV.Out) {
      const FunctionDecl *Callee = CE->getDirectCallee();
      if (!Callee)
        continue;
      std::vector<const FunctionDecl *> Fns{Callee};
      if (const FunctionDecl *Def = Callee->getDefinition())
        for (const ResolvedCallee &RC : P.Resolver.getReachable(Def).Reachable)
          Fns.push_back(RC.Callee);
      for (const FunctionDecl *Fn : Fns)
        if (Fn->isNoReturn() || neverReturns(Fn->getName()))
          return Fail("it calls '" + Fn->getNameAsString() +
                      "', which may not return to the region's end");
    }

    auto CallsMayWrite = [&](const VarDecl *G, std::string &Who) {
      for (const CallExpr *CE : CallsV.Out) {
        const FunctionDecl *Callee = CE->getDirectCallee();
        if (Callee && !Callee->getDefinition() &&
            SM.isInSystemHeader(Callee->getLocation()))
          continue; // a library function writes user globals only via &
        if (!Callee || !Callee->getDefinition()) {
          Who = Callee ? Callee->getNameAsString() : "a function pointer";
          return true;
        }
        std::vector<const FunctionDecl *> Fns{Callee->getDefinition()};
        for (const ResolvedCallee &RC :
             P.Resolver.getReachable(Callee->getDefinition()).Reachable) {
          if (!RC.HasVisibleBody) {
            if (SM.isInSystemHeader(RC.Callee->getLocation()))
              continue;
            Who = RC.Callee->getNameAsString();
            return true;
          }
          Fns.push_back(RC.Callee->getDefinition());
        }
        for (const FunctionDecl *Fn : Fns) {
          const FunctionEffects &E = P.SafetyA.getEffects(Fn);
          bool Uses = llvm::any_of(E.GlobalsUsed, [&](const VarDecl *U) {
            return canon(U) == G;
          });
          if ((E.WritesGlobal && Uses) || E.HasUnknownWrite ||
              E.WritesThroughPointerParam) {
            Who = Fn->getNameAsString();
            return true;
          }
        }
      }
      return false;
    };

    const NameScope Scope(P.Ctx, F, RR.getBegin());
    // Why a name can't be used in C or a section, if it can't.
    auto NameBad = [&](const VarDecl *N) -> std::optional<std::string> {
      const bool Array = N->getType()->isArrayType();
      if (N->getType().isVolatileQualified())
        return std::optional<std::string>(quoted(N) + " is volatile");
      if (!Array && N->hasGlobalStorage() && GlobalAddrTaken.count(N))
        return std::optional<std::string>("the address of global " + quoted(N) +
                    " is taken, so it may change through a pointer");
      if (!Array && WrittenInR.count(N))
        return std::optional<std::string>(quoted(N) + " changes inside it");
      if (!Array && AddrTaken.count(N))
        return std::optional<std::string>("the address of " + quoted(N) + " is taken");
      std::string Bad;
      if (!Scope.nameable(N, Bad))
        return Bad;
      if (N->hasGlobalStorage() && !N->isLocalVarDecl()) {
        std::string Who;
        if (!Array && !N->getType().isConstQualified() &&
            CallsMayWrite(N, Who))
          return std::optional<std::string>("global " + quoted(N) + " may be changed by a call to '" +
                      Who + "' inside it");
      } else if (!isa<ParmVarDecl>(N)) {
        // A local must be declared before R, in a block around it.
        if (!SM.isBeforeInTranslationUnit(N->getLocation(), RR.getBegin()))
          return std::optional<std::string>(quoted(N) + " is declared inside it");
        DynTypedNodeList Ps = P.Ctx.getParents(*N);
        const Stmt *Decl = Ps.empty() ? nullptr : Ps[0].get<Stmt>();
        const Stmt *Scope = nullptr;
        if (Decl) {
          DynTypedNodeList Qs = P.Ctx.getParents(*Decl);
          Scope = Qs.empty() ? nullptr : Qs[0].get<Stmt>();
        }
        // Its block, or the for loop whose header declares it.
        if (!Scope || !(isa<CompoundStmt>(Scope) || isa<ForStmt>(Scope)) ||
            !inRange(Scope->getSourceRange(), RR.getBegin()))
          return std::optional<std::string>(quoted(N) + " is not in scope before it");
      }
      return std::nullopt;
    };
    for (const VarDecl *N : Names)
      if (std::optional<std::string> Bad = NameBad(N))
        return Fail(*Bad);

    // --- mapped bases: apart, one shape each -------------------------------
    llvm::DenseSet<const VarDecl *> SiteWritten;
    for (int K : Sites)
      for (const ArrayRegion &Reg : Plans[K]->Regions)
        if (Reg.Written)
          SiteWritten.insert(canon(Reg.Base));
    if (P.Alias)
      for (size_t I = 0; I < M.size(); ++I)
        for (size_t J = I + 1; J < M.size(); ++J) {
          const AliasProof Pr = P.Alias->proof(F, M[I], M[J]);
          const bool AnyWrite = SiteWritten.count(M[I]) || SiteWritten.count(M[J]);
          if (Pr == AliasProof::Unproven ||
              (Pr == AliasProof::Restrict && !AnyWrite))
            return Fail(quoted(M[I]) + " and " + quoted(M[J]) +
                        " may overlap, and one region can't map both");
        }

    // --- host code ----------------------------------------------------------
    struct Pending {
      const Stmt *S;
      llvm::SetVector<const VarDecl *> Both, Writes;
    };
    std::vector<Pending> Updates;
    llvm::SetVector<const VarDecl *> HostWritten;
    std::string Why;
    std::function<bool(const Stmt *, const Stmt *)> Visit;
    auto Header = [&](const Stmt *E) {
      if (!E)
        return true;
      Touch T;
      if (!hostTouch(F, E, M, T, Why))
        return false;
      if (!T.Reads.empty() || !T.Writes.empty()) {
        Why = "a loop or branch condition around its GPU loops uses " +
              quoted(T.Reads.empty() ? T.Writes[0] : T.Reads[0]);
        return false;
      }
      return true;
    };
    Visit = [&](const Stmt *S, const Stmt *Parent) -> bool {
      if (!S)
        return true;
      if (const auto *FS = dyn_cast<ForStmt>(S)) {
        auto It = IndexOf.find(FS);
        if (It != IndexOf.end() && IsSite[It->second])
          return true;
      }
      if (!containsAny(S, IsSite)) {
        Touch T;
        if (!hostTouch(F, S, M, T, Why))
          return false;
        if (T.Reads.empty() && T.Writes.empty())
          return true;
        if (!Parent || !isa<CompoundStmt>(Parent)) {
          Why = "host code using " +
                quoted(T.Reads.empty() ? T.Writes[0] : T.Reads[0]) +
                " is not in a { } block, so no update can go around it";
          return false;
        }
        if (jumpsOut(S, false)) {
          Why = "host code using mapped data can jump out of itself";
          return false;
        }
        Pending U{S, T.Reads, T.Writes};
        U.Both.insert(T.Writes.begin(), T.Writes.end());
        HostWritten.insert(T.Writes.begin(), T.Writes.end());
        Updates.push_back(std::move(U));
        return true;
      }
      if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
        for (const Stmt *Kid : CS->body())
          if (!Visit(Kid, CS))
            return false;
        return true;
      }
      if (const auto *FS = dyn_cast<ForStmt>(S))
        return Header(FS->getInit()) && Header(FS->getCond()) &&
               Header(FS->getInc()) && Visit(FS->getBody(), FS);
      if (const auto *WS = dyn_cast<WhileStmt>(S))
        return Header(WS->getCond()) && Visit(WS->getBody(), WS);
      if (const auto *DS = dyn_cast<DoStmt>(S))
        return Header(DS->getCond()) && Visit(DS->getBody(), DS);
      if (const auto *IS = dyn_cast<IfStmt>(S))
        return Header(IS->getInit()) && Header(IS->getCond()) &&
               Visit(IS->getThen(), IS) && Visit(IS->getElse(), IS);
      if (const auto *SS = dyn_cast<SwitchStmt>(S))
        return Header(SS->getInit()) && Header(SS->getCond()) &&
               Visit(SS->getBody(), SS);
      if (const auto *LS = dyn_cast<LabelStmt>(S))
        return Visit(LS->getSubStmt(), LS);
      if (const auto *AS = dyn_cast<AttributedStmt>(S))
        return Visit(AS->getSubStmt(), AS);
      if (const auto *SC = dyn_cast<SwitchCase>(S))
        return Visit(SC->getSubStmt(), SC);
      Why = "a GPU loop sits inside a statement this pass doesn't handle";
      return false;
    };
    for (const Stmt *S : R)
      if (!Visit(S, Block))
        return Fail(Why);

    // --- what the region costs each loop (Phase 2.3) -------------------------
    llvm::SetVector<const VarDecl *> RegionWritten;
    for (const VarDecl *B : M)
      if (SiteWritten.count(B) || HostWritten.count(B))
        RegionWritten.insert(B);
    std::map<int, std::string> GuardOf;
    std::map<int, std::string> ShareNote;
    std::map<int, bool> SharedGpu;
    if (!PolicyNaive()) {
      // Copies per entry, in sections (one base one way): the hull in and
      // out once, and each update each time it runs; kernel runs per entry.
      Runs Copies, Kernels;
      for (const VarDecl *B : M)
        Copies.add(Mono{}, RegionWritten.count(B) ? 2.0 : 1.0);
      for (const Pending &U : Updates)
        Copies.add(runsIn(U.S, RSet), U.Both.size() + U.Writes.size());
      for (int K : Sites)
        Kernels.add(runsIn(P.Loops[K].Loop, RSet), 1.0);
      // Sections assumed alike in size: a run of loop K costs its own
      // transfer times (copies per kernel run) / (its own sections).
      const double PerRun = Copies.ratioTo(Kernels);
      auto OwnOf = [&](int K) {
        double Own = 0.0;
        for (const ArrayRegion &Reg : Plans[K]->Regions)
          Own += Reg.mapKind() == "tofrom" ? 2.0 : 1.0;
        return Own;
      };
      bool Any = false;
      for (int K : Sites) {
        const double Own = OwnOf(K);
        const double Scale = Own > 0.0 ? PerRun / Own : 0.0;
        ProfitabilityVerdict A =
            P.Profit.priceResident(P.Loops[K], Safety[K], *Plans[K], Scale);
        const bool Gpu = A.Target == OffloadTarget::GpuOffload;
        if (V[K].Target == OffloadTarget::GpuOffload && !Gpu)
          return Fail("a loop that picks the GPU with its own maps no longer "
                      "does with its share of the region's transfer (" +
                      fmtRatio(Scale) + " of its own maps per run)");
        Any |= Gpu;
        SharedGpu[K] = Gpu;
        ShareNote[K] = "; transfer " + fmtRatio(Scale) +
                       " of its own maps per run (" + Copies.text() +
                       " section copies over " + Kernels.text() +
                       " kernel runs per entry)";
        if (Gpu && A.GuardExpr)
          GuardOf[K] = *A.GuardExpr;
      }
      if (!Any)
        return Fail("with the transfer shared across its kernel runs, no "
                    "loop in it is cheapest on the GPU");
      // A region pays only when it copies no more than per-loop maps would
      // for the loops it puts on the GPU by their price (the others would
      // stay on the host and copy nothing).
      Runs OwnCopies;
      for (int K : Sites)
        if (SharedGpu[K])
          OwnCopies.add(runsIn(P.Loops[K].Loop, RSet), OwnOf(K));
      if (Copies.ratioTo(OwnCopies) > 1.0)
        return Fail("it would copy more than its loops' own maps (" +
                    Copies.text() + " section copies per entry against " +
                    OwnCopies.text() + ")");
      // A guard is the loop's bound text: every variable in it.
      for (const auto &[K, G] : GuardOf) {
        struct B : RecursiveASTVisitor<B> {
          llvm::SetVector<const VarDecl *> N;
          bool Ok = true;
          bool VisitDeclRefExpr(DeclRefExpr *E) {
            if (const VarDecl *V = canon(E->getDecl()))
              N.insert(V);
            return true;
          }
          bool VisitCallExpr(CallExpr *) { return Ok = false; }
        } Bound;
        if (P.Loops[K].BoundExpr)
          Bound.TraverseStmt(const_cast<Expr *>(P.Loops[K].BoundExpr));
        if (!Bound.Ok)
          return Fail("a loop's bound calls a function");
        for (const VarDecl *N : Bound.N)
          if (!Names.count(N))
            if (std::optional<std::string> Bad = NameBad(N))
              return Fail(*Bad);
      }
    }
    std::string C;
    {
      std::vector<std::string> Done;
      for (const std::string &G : Parts) {
        std::string Part = G;
        if (std::optional<int> K = slotOf(G)) {
          auto It = GuardOf.find(*K);
          Part = It == GuardOf.end() ? "" : It->second;
        }
        if (Part.empty() || llvm::is_contained(Done, Part))
          continue;
        Done.push_back(Part);
        C += (C.empty() ? "" : " && ") + Part;
      }
    }

    // --- the region ---------------------------------------------------------
    std::map<const VarDecl *, std::string> SectionOf;
    std::string Maps, Decls;
    for (const VarDecl *B : M) {
      std::vector<const ArrayRegion *> Regs;
      for (int K : Sites)
        for (const ArrayRegion &Reg : Plans[K]->Regions)
          if (canon(Reg.Base) == B)
            Regs.push_back(&Reg);
      std::string Bad;
      Section Hull = hullSection(Regs, Bad);
      if (Hull.empty())
        return Fail(Bad);
      // Clang 22 turns a `?:` inside a `target data` section into invalid
      // IR: such ends go into constants declared just before the region.
      if (Hull.Start.find('?') != std::string::npos ||
          Hull.Len.find('?') != std::string::npos) {
        const std::string Lo = freshName("p05_lo"), Len = freshName("p05_len");
        Decls += "const " + std::string(IntPtrType) + " " + Lo + " = " +
                 Hull.Start + ", " + Len + " = " + Hull.Len + "; ";
        Hull.Start = Lo;
        Hull.Len = Len;
      }
      const std::string Sec = Hull.text();
      SectionOf[B] = Sec;
      // Never `from`: a loop that would have written the whole section may
      // not run on this entry, and the region's end would copy the device's
      // uninitialized memory back.
      const bool W = RegionWritten.count(B) > 0;
      Maps += std::string(" map(") + (W ? "tofrom" : "to") + ": " + Sec + ")";
    }
    const std::string If = C.empty() ? "" : " if(" + C + ")";

    DataRegion D;
    D.First = R.front();
    D.Last = R.back();
    D.Braces = Braces;
    D.Line = Line;
    D.Pragma = "#pragma omp target data" + Maps + If;
    if (!Decls.empty())
      D.Decls = Decls.substr(0, Decls.size() - 1);
    auto Render = [&](const char *Dir,
                      const llvm::SetVector<const VarDecl *> &Bs) {
      if (Bs.empty())
        return std::string();
      std::string List;
      for (const VarDecl *B : Bs)
        List += (List.empty() ? "" : ", ") + SectionOf[B];
      return "#pragma omp target update" + If + " " + Dir + "(" + List + ")";
    };
    for (const Pending &U : Updates)
      D.Updates.push_back(
          HostUpdate{U.S, Render("from", U.Both), Render("to", U.Writes)});
    D.Sites.assign(Sites.begin(), Sites.end());

    for (int K : Sites) {
      ProfitabilityVerdict &X = V[K];
      const OffloadTarget Was = X.Target;
      X.Target = OffloadTarget::GpuOffload;
      X.GuardExpr.reset();
      X.MapGuard = C;
      std::string Note = "data region (line " + std::to_string(Line) +
                         "): resident in target data" + If;
      Note += ShareNote[K];
      if (Was != OffloadTarget::GpuOffload)
        Note += PolicyNaive() || SharedGpu[K]
                    ? "; GPU_OFFLOAD there: its share of the region's "
                      "transfer makes it the cheapest"
                    : "; GPU_OFFLOAD there: it touches data the region "
                      "keeps on the device";
      X.Reasons.push_back(Note);
    }
    Out.push_back(std::move(D));
    return true;
  }

};

std::vector<DataRegion>
HoistPlanner::plan(const std::vector<LoopSafety> &Safety,
                   const std::vector<std::optional<RegionPlan>> &Plans,
                   std::vector<ProfitabilityVerdict> &Verdicts) {
  // Only the policies that may choose the GPU (all-cpu never does).
  if (PolicyMode != Policy::Gated && PolicyMode != Policy::Naive)
    return {};
  Impl I(*this, Safety, Plans, Verdicts);
  I.classify();
  I.indexGlobalAddrs();
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
    const auto *F = dyn_cast<FunctionDecl>(D);
    if (!F || !F->doesThisDeclarationHaveABody() ||
        !Ctx.getSourceManager().isWrittenInMainFile(F->getLocation()))
      continue;
    I.processFunction(F);
  }
  return std::move(I.Out);
}

} // namespace p05
