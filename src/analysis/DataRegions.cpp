#include "analysis/DataRegions.h"
#include "analysis/DependenceTest.h"
#include "analysis/Profitability.h"
#include "analysis/SourceText.h"

#include "clang/AST/ParentMapContext.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;

namespace p05 {

static llvm::cl::opt<MapMode> FlagMaps(
    "maps", llvm::cl::desc("How GPU map() sections are sized"),
    llvm::cl::values(
        clEnumValN(MapMode::Ranges, "ranges",
                   "from the subscripts' ranges (Phase 2.1, default)"),
        clEnumValN(MapMode::Bound, "bound",
                   "base[0:bound] and per-iteration pricing (pre-2.1, "
                   "ablation baseline)")),
    llvm::cl::init(MapMode::Ranges), llvm::cl::cat(optionCategory()));

MapMode mapModeFromFlags() { return FlagMaps; }

std::string ArrayRegion::listItem() const {
  if (!SectionText.empty())
    return SectionText;
  return Base->getNameAsString() + "[0:" + ExtentText + "]";
}

namespace {

const VarDecl *canon(const ValueDecl *V) {
  const auto *VD = dyn_cast_or_null<VarDecl>(V);
  return VD ? VD->getCanonicalDecl() : nullptr;
}

std::string quoted(const ValueDecl *V) {
  return "'" + V->getNameAsString() + "'";
}

/// A - B, or nullopt on overflow.
std::optional<AffineExpr> minus(const AffineExpr &A, const AffineExpr &B) {
  AffineExpr D = A;
  for (const auto &[V, K] : B.IVTerms)
    if (__builtin_sub_overflow(D.IVTerms[V], K, &D.IVTerms[V]))
      return std::nullopt;
  for (const auto &[V, K] : B.SymTerms)
    if (__builtin_sub_overflow(D.SymTerms[V], K, &D.SymTerms[V]))
      return std::nullopt;
  if (__builtin_sub_overflow(D.Const, B.Const, &D.Const))
    return std::nullopt;
  D.IVTerms.remove_if([](const auto &P) { return P.second == 0; });
  D.SymTerms.remove_if([](const auto &P) { return P.second == 0; });
  return D;
}

/// A - B as a constant, when it is one.
std::optional<int64_t> constDiff(const AffineExpr &A, const AffineExpr &B) {
  std::optional<AffineExpr> D = minus(A, B);
  if (!D || !D->isConstant())
    return std::nullopt;
  return D->Const;
}

/// E as C text: a constant, a bare variable, or a cast expression.
std::string text(const AffineExpr &E) {
  if (E.isConstant())
    return std::to_string(E.Const);
  // A bare signed variable reads best as itself; anything else (an unsigned
  // one, whose `lo < 0 ? 0 : lo` or `hi - lo + 1` would wrap) is cast.
  auto Bare = [](const ValueDecl *V) {
    return V->getType()->isSignedIntegerType() &&
           !V->getType()->isEnumeralType();
  };
  if (E.Const == 0 && E.IVTerms.size() + E.SymTerms.size() == 1) {
    if (E.IVTerms.size() == 1 && E.IVTerms.begin()->second == 1 &&
        Bare(E.IVTerms.begin()->first))
      return E.IVTerms.begin()->first->getNameAsString();
    if (E.SymTerms.size() == 1 && E.SymTerms.begin()->second == 1 &&
        Bare(E.SymTerms.begin()->first))
      return E.SymTerms.begin()->first->getNameAsString();
  }
  return asIntPtr(E);
}

/// `E >= 0` as text, with the constant moved to the right: "n >= 1".
std::string nonNegative(AffineExpr E) {
  int64_t K = -E.Const;
  E.Const = 0;
  return text(E) + " >= " + std::to_string(K);
}

/// Row-major step: Prev * N + Next, skipping zero terms.
std::string linear(const std::string &Prev, const std::string &N,
                   const AffineExpr &Next) {
  const bool NextZero = Next.isConstant() && Next.Const == 0;
  if (Prev == "0")
    return asIntPtr(Next);
  const std::string Row = "(" + Prev + ") * " + N;
  return NextZero ? Row : Row + " + " + asIntPtr(Next);
}

/// How a base is laid out, and what a section of it maps.
struct Layout {
  enum Kind { Object, PlainPtr, Rows } K = PlainPtr;
  /// Object / Rows: number of subscripts an access must have.
  unsigned Rank = 1;
  /// Object: extent of the outermost dimension.
  int64_t OuterExtent = 0;
  /// Extents of the inner dimensions (Object / Rows), outermost first.
  std::vector<int64_t> Inner;
  /// Bytes of one unit of the outermost dimension; of one element.
  uint64_t UnitBytes = 0, ElemBytes = 0;
};

/// Constant extents of the nested arrays in T (T itself included).
bool constantExtents(QualType T, ASTContext &Ctx, std::vector<int64_t> &Out,
                     QualType &Elem) {
  while (const ArrayType *AT = Ctx.getAsArrayType(T)) {
    const auto *CAT = dyn_cast<ConstantArrayType>(AT);
    if (!CAT)
      return false;
    Out.push_back(CAT->getSize().getSExtValue());
    T = AT->getElementType();
  }
  Elem = T;
  return true;
}

bool sizable(QualType T) {
  return !T.isNull() && !T->isIncompleteType() && !T->isVariablyModifiedType() &&
         !T->isFunctionType() && !T->isVoidType();
}

std::optional<Layout> layoutOf(const VarDecl *VD, ASTContext &Ctx,
                               std::string &Why) {
  Layout L;
  QualType T = VD->getType();
  QualType Elem;
  std::vector<int64_t> Ext;
  if (T->isPointerType()) {
    QualType P = T->getPointeeType();
    if (Ctx.getAsArrayType(P)) {
      if (!constantExtents(P, Ctx, Ext, Elem) || !sizable(Elem)) {
        Why = quoted(VD) + " points to arrays of non-constant size";
        return std::nullopt;
      }
      L.K = Layout::Rows;
      L.Rank = 1 + Ext.size();
      L.Inner = Ext;
      L.UnitBytes = Ctx.getTypeSizeInChars(P).getQuantity();
    } else {
      if (!sizable(P)) {
        Why = "can't size the elements of " + quoted(VD);
        return std::nullopt;
      }
      Elem = P;
      L.K = Layout::PlainPtr;
      L.UnitBytes = Ctx.getTypeSizeInChars(P).getQuantity();
    }
  } else if (Ctx.getAsArrayType(T)) {
    if (!constantExtents(T, Ctx, Ext, Elem) || !sizable(Elem)) {
      Why = quoted(VD) + " is not an array of constant size";
      return std::nullopt;
    }
    L.K = Layout::Object;
    L.Rank = Ext.size();
    L.OuterExtent = Ext.front();
    L.Inner.assign(Ext.begin() + 1, Ext.end());
    L.UnitBytes =
        Ctx.getTypeSizeInChars(Ctx.getAsArrayType(T)->getElementType())
            .getQuantity();
  } else {
    Why = quoted(VD) + " is neither an array nor a pointer";
    return std::nullopt;
  }
  L.ElemBytes = Ctx.getTypeSizeInChars(Elem).getQuantity();
  return L;
}

/// Does Body (a function's) return or jump anywhere but its very end?
bool exitsEarly(const Stmt *Body) {
  const auto *CS = dyn_cast_or_null<CompoundStmt>(Body);
  if (!CS)
    return true;
  std::function<bool(const Stmt *, bool)> Walk = [&](const Stmt *S,
                                                     bool Last) -> bool {
    if (!S)
      return false;
    if (isa<GotoStmt>(S) || isa<IndirectGotoStmt>(S))
      return true;
    if (isa<ReturnStmt>(S) && !Last)
      return true;
    for (const Stmt *C : S->children())
      if (Walk(C, false))
        return true;
    return false;
  };
  const Stmt *LastStmt = CS->body_empty() ? nullptr : CS->body_back();
  for (const Stmt *S : CS->body())
    if (Walk(S, S == LastStmt))
      return true;
  return false;
}

} // namespace

RegionPlanner::RegionPlanner(ASTContext &Ctx, const std::vector<LoopInfo> &Loops)
    : Ctx(Ctx), Loops(Loops) {
  for (const LoopInfo &L : Loops)
    LoopOf[L.Loop] = &L;
}

RegionPlan RegionPlanner::plan(const LoopInfo &LI, const LoopAccesses &A,
                               const FunctionDecl *F) {
  RegionPlan Plan;
  auto Fail = [&](std::string Why) {
    Plan.Mappable = false;
    if (!llvm::is_contained(Plan.Why, Why))
      Plan.Why.push_back(std::move(Why));
  };
  if (!F || !A.RootIV || A.Poisoned) {
    Fail("the loop's iteration space is not known");
    return Plan;
  }
  const NameScope Names(Ctx, F, LI.Loop->getBeginLoc());
  auto RangeOf = [&](const VarDecl *V) -> const IVRange * {
    auto It = A.Ranges.find(V);
    return It == A.Ranges.end() ? nullptr : &It->second;
  };
  auto IsFixed = [&](const VarDecl *V) {
    return llvm::is_contained(A.OuterIVs, V);
  };

  // Is E executed exactly once per iteration of the loops around it, up to
  // Stop (the analyzed loop's body, or a callee's body)?
  auto Unconditional = [&](const Stmt *E, const Stmt *Stop) -> bool {
    if (E == Stop) // a brace-less body that is the call itself
      return true;
    DynTypedNode Node = DynTypedNode::create(*E);
    while (true) {
      DynTypedNodeList Ps = Ctx.getParents(Node);
      if (Ps.empty())
        return false;
      const DynTypedNode &P = Ps[0];
      if (const auto *VD = P.get<VarDecl>()) { // an initializer
        Node = DynTypedNode::create(*VD);
        continue;
      }
      const Stmt *PS = P.get<Stmt>();
      if (!PS)
        return false;
      const Stmt *Child = Node.get<Stmt>();
      if (const auto *CO = dyn_cast<AbstractConditionalOperator>(PS)) {
        if (Child != CO->getCond())
          return false;
      } else if (const auto *BO = dyn_cast<BinaryOperator>(PS)) {
        if (BO->isLogicalOp() && Child == BO->getRHS())
          return false;
      } else if (const auto *GS = dyn_cast<GenericSelectionExpr>(PS)) {
        // Only the selected association is evaluated.
        if (GS->isResultDependent() || Child != GS->getResultExpr())
          return false;
      } else if (const auto *CE = dyn_cast<ChooseExpr>(PS)) {
        if (CE->isConditionDependent() || Child != CE->getChosenSubExpr())
          return false;
      } else if (const auto *IS = dyn_cast<IfStmt>(PS)) {
        if (Child != IS->getCond())
          return false;
      } else if (const auto *SS = dyn_cast<SwitchStmt>(PS)) {
        if (Child != SS->getCond())
          return false;
      } else if (const auto *FS = dyn_cast<ForStmt>(PS)) {
        if (Child != FS->getInit()) {
          if (Child != FS->getBody())
            return false;
          auto It = LoopOf.find(FS);
          if (It == LoopOf.end() || !It->second->InductionVar)
            return false;
          const IVRange *R = RangeOf(canon(It->second->InductionVar));
          if (!R || !R->Exact)
            return false;
        }
      } else if (!isa<Expr>(PS) && !isa<CompoundStmt>(PS) &&
                 !isa<DeclStmt>(PS) && !isa<AttributedStmt>(PS) &&
                 !isa<LabelStmt>(PS) && !isa<ReturnStmt>(PS)) {
        return false;
      }
      // Checked after PS itself: a brace-less body can be the if.
      if (PS == Stop)
        return true;
      Node = P;
    }
  };
  // Every name a bound leaves behind: symbols and enclosing loops'
  // variables (the analyzed loop's and inner ones never survive bounds).
  auto NameableExpr = [&](const AffineExpr &E, std::string &Bad) {
    for (const auto &[V, K] : E.SymTerms)
      if (!Names.nameable(V, Bad))
        return false;
    for (const auto &[V, K] : E.IVTerms)
      if (!IsFixed(V) || !Names.nameable(V, Bad))
        return false;
    return true;
  };
  const IVRange *RootRange = RangeOf(A.RootIV);
  const bool RootExact = RootRange && RootRange->Exact;

  // An access whose subscript's extremes are surely reached.
  auto Exact = [&](const ArrayAccess &X, unsigned Dims) -> bool {
    if (!RootExact || !X.E)
      return false;
    if (!Unconditional(X.E, LI.Loop->getBody()))
      return false;
    for (const Expr *W : X.Within) {
      const FunctionDecl *Fn = nullptr;
      for (DynTypedNodeList Ps = Ctx.getParents(*W); !Ps.empty();) {
        if ((Fn = Ps[0].get<FunctionDecl>()))
          break;
        if (const auto *S = Ps[0].get<Stmt>())
          Ps = Ctx.getParents(*S);
        else if (const auto *D = Ps[0].get<Decl>())
          Ps = Ctx.getParents(*D);
        else
          break;
      }
      if (!Fn || !Fn->getBody() || exitsEarly(Fn->getBody()) ||
          !Unconditional(W, Fn->getBody()))
        return false;
    }
    for (unsigned D = 0; D < Dims; ++D)
      for (const auto &[V, K] : X.Subscripts[D].IVTerms) {
        if (IsFixed(V))
          continue;
        const IVRange *R = RangeOf(V);
        if (!R || !R->Exact)
          return false;
      }
    return true;
  };

  // Loop variables whose loops must run at least once for the ends to be
  // reached.
  llvm::SetVector<const VarDecl *> MustRun;
  std::vector<std::string> Guards;

  // Bases, in order of first appearance.
  llvm::SetVector<const VarDecl *> Bases;
  for (const ArrayAccess &X : A.Accesses) {
    if (X.Kind == AccessKind::AddressTaken) {
      Fail("the address of " + X.Text + " is passed on");
      continue;
    }
    const VarDecl *VD = canon(X.Base);
    if (!VD) {
      Fail(X.Text + " has no named base");
      continue;
    }
    if (A.DeclaredInLoop.count(VD) && VD->getType()->isArrayType())
      continue; // private to each iteration: a local on the device
    if (A.DeclaredInLoop.count(VD)) {
      Fail(quoted(VD) + " is a pointer set inside the loop");
      continue;
    }
    if (!X.Affine) {
      Fail(X.Text + " is non-affine");
      continue;
    }
    if (X.BaseMayAlias) {
      Fail(quoted(VD) + " may point anywhere");
      continue;
    }
    Bases.insert(VD);
  }
  if (!Plan.Mappable)
    return Plan;

  for (const VarDecl *VD : Bases) {
    std::string Why;
    std::optional<Layout> L = layoutOf(VD, Ctx, Why);
    if (!L) {
      Fail(Why);
      continue;
    }
    std::string Bad;
    if (!Names.nameable(VD, Bad)) {
      Fail("the map() clause can't name " + quoted(VD) + ": " + Bad);
      continue;
    }
    ArrayRegion R;
    R.Base = VD;
    R.ElemBytes = L->UnitBytes;

    // Per access: the outer index range (1-D / rows), or the linear one.
    struct One {
      const ArrayAccess *X;
      AffineExpr Lo, Hi;           // outer index (not Linear)
      std::string LoT, HiT;        // Linear: as text
      bool Exact = false;
    };
    std::vector<One> Ones;
    bool Linear = false, Ok = true;
    for (const ArrayAccess &X : A.Accesses) {
      if (canon(X.Base) != VD)
        continue;
      R.Read |= X.Kind == AccessKind::Read || X.Kind == AccessKind::ReadWrite;
      R.Written |=
          X.Kind == AccessKind::Write || X.Kind == AccessKind::ReadWrite;
      // Writing a member of an element leaves the rest as it was.
      if (X.ViaMember && X.Kind != AccessKind::Read)
        R.Read = true;
      One O{&X};
      const bool Lin = L->K == Layout::PlainPtr && X.Delinearized;
      if (L->K == Layout::PlainPtr ? (!Lin && X.Subscripts.size() != 1)
                                   : (X.Delinearized ||
                                      X.Subscripts.size() != L->Rank)) {
        Fail("can't map " + X.Text + ": its subscripts don't match the "
             "shape of " + quoted(VD));
        Ok = false;
        break;
      }
      std::vector<AffineExpr> Los, His;
      for (const AffineExpr &S : X.Subscripts) {
        AffineExpr Lo, Hi;
        if (!subscriptBounds(S, A, Lo, Hi)) {
          Fail("the range of " + X.Text + " can't be bounded");
          Ok = false;
          break;
        }
        Los.push_back(Lo);
        His.push_back(Hi);
      }
      if (!Ok)
        break;
      for (const AffineExpr *E : {&Los[0], &His[0]})
        if (!NameableExpr(*E, Bad)) {
          Fail("the map() clause can't name a bound of " + X.Text + ": " +
               Bad);
          Ok = false;
        }
      if (!Ok)
        break;
      if (Lin) {
        Linear = true;
        std::string LoT = asIntPtr(Los[0]), HiT = asIntPtr(His[0]);
        for (size_t D = 1; D < Los.size(); ++D) {
          if (D - 1 >= X.RowLengths.size() ||
              !Names.nameable(X.RowLengths[D - 1], Bad)) {
            Fail("can't linearize " + X.Text);
            Ok = false;
            break;
          }
          for (const AffineExpr *E : {&Los[D], &His[D]})
            if (!NameableExpr(*E, Bad)) {
              Fail("the map() clause can't name a bound of " + X.Text +
                   ": " + Bad);
              Ok = false;
            }
          const std::string N = "(" + std::string(IntPtrType) + ")" +
                                X.RowLengths[D - 1]->getNameAsString();
          LoT = linear(LoT, N, Los[D]);
          HiT = linear(HiT, N, His[D]);
        }
        if (!Ok)
          break;
        O.LoT = LoT;
        O.HiT = HiT;
        O.Exact = Exact(X, X.Subscripts.size());
        if (L->K != Layout::Object && !O.Exact) {
          Fail("can't map " + X.Text + ": it may not run on every iteration "
               "(or a loop around it may stop early), so the ends of its "
               "range aren't surely touched");
          Ok = false;
          break;
        }
      } else {
        O.Lo = Los[0];
        O.Hi = His[0];
        O.Exact = Exact(X, 1);
      }
      Ones.push_back(std::move(O));
    }
    if (!Ok)
      continue;
    if (Linear && llvm::any_of(Ones, [](const One &O) { return O.LoT.empty(); })) {
      Fail("can't map " + quoted(VD) + ": linearized and plain subscripts "
           "mixed");
      continue;
    }

    // The section's ends: Object from every access (clamped below);
    // otherwise from exact ones, everything else inside them.
    std::string LoT, HiT;
    std::optional<AffineExpr> LoE, HiE; // when a single affine expression
    if (Linear) {
      std::vector<std::string> Ls, Hs;
      for (const One &O : Ones) {
        if (!llvm::is_contained(Ls, O.LoT))
          Ls.push_back(O.LoT);
        if (!llvm::is_contained(Hs, O.HiT))
          Hs.push_back(O.HiT);
      }
      LoT = fold(Ls, true);
      HiT = fold(Hs, false);
    } else {
      std::vector<AffineExpr> Ls, Hs;
      auto Add = [](std::vector<AffineExpr> &Into, const AffineExpr &E,
                    bool Min) {
        for (AffineExpr &O : Into)
          if (std::optional<int64_t> D = constDiff(E, O)) {
            if (Min ? *D < 0 : *D > 0)
              O = E;
            return;
          }
        Into.push_back(E);
      };
      const bool NeedExact = L->K != Layout::Object;
      for (const One &O : Ones)
        if (!NeedExact || O.Exact) {
          Add(Ls, O.Lo, true);
          Add(Hs, O.Hi, false);
        }
      if (Ls.empty()) {
        Fail("can't map " + quoted(VD) + ": no access to it runs on every "
             "iteration, so the ends of its range aren't surely touched");
        continue;
      }
      // Every other access must lie inside the exact ones' span.
      for (const One &O : Ones) {
        if (!NeedExact || O.Exact)
          continue;
        auto Inside = [&](const AffineExpr &E, const std::vector<AffineExpr> &Ends,
                          bool Low) {
          return llvm::any_of(Ends, [&](const AffineExpr &End) {
            std::optional<int64_t> D = constDiff(E, End);
            return D && (Low ? *D >= 0 : *D <= 0);
          });
        };
        if (!Inside(O.Lo, Ls, true) || !Inside(O.Hi, Hs, false)) {
          Fail("can't map " + O.X->Text + ": it may not run on every "
               "iteration, and its range isn't inside what the other "
               "accesses to " + quoted(VD) + " surely touch");
          Ok = false;
          break;
        }
      }
      if (!Ok)
        continue;
      if (Ls.size() == 1)
        LoE = Ls[0];
      if (Hs.size() == 1)
        HiE = Hs[0];
      std::vector<std::string> LT, HT;
      for (const AffineExpr &E : Ls)
        LT.push_back(text(E));
      for (const AffineExpr &E : Hs)
        HT.push_back(text(E));
      LoT = fold(LT, true);
      HiT = fold(HT, false);
    }

    // Loops that must run for the ends to be reached (pointer layouts).
    // Every loop around an end-defining access, and the analyzed loop: an
    // access inside an inner loop that doesn't run reaches nothing.
    if (L->K != Layout::Object) {
      MustRun.insert(A.RootIV);
      for (const One &O : Ones)
        if (O.Exact)
          for (const VarDecl *V : O.X->InnerIVs)
            if (!IsFixed(V))
              MustRun.insert(V);
    }

    // Clamp an Object to its extent; a pointer's lowest index must be >= 0.
    std::string StartT, LenT;
    std::optional<int64_t> Units;
    if (L->K == Layout::Object) {
      const int64_t Last = L->OuterExtent - 1;
      if (LoE && LoE->isConstant())
        LoT = std::to_string(std::max<int64_t>(LoE->Const, 0));
      else
        LoT = "(" + LoT + " < 0 ? 0 : " + LoT + ")";
      if (HiE && HiE->isConstant())
        HiT = std::to_string(std::min<int64_t>(HiE->Const, Last));
      else
        HiT = "(" + HiT + " > " + std::to_string(Last) + " ? " +
              std::to_string(Last) + " : " + HiT + ")";
      if (LoE && HiE && LoE->isConstant() && HiE->isConstant()) {
        const int64_t Lo = std::max<int64_t>(LoE->Const, 0);
        const int64_t Hi = std::min<int64_t>(HiE->Const, Last);
        Units = std::max<int64_t>(Hi - Lo + 1, 0);
      }
    } else if (LoE && LoE->isConstant()) {
      if (LoE->Const < 0) {
        Fail("can't map " + quoted(VD) + ": the loop reaches index " +
             std::to_string(LoE->Const) + ", below the pointer");
        continue;
      }
    } else if (!isAllDigits(LoT)) {
      const std::string G = LoE ? nonNegative(*LoE) : LoT + " >= 0";
      if (!llvm::is_contained(Guards, G))
        Guards.push_back(G);
    }
    StartT = LoT;
    if (Units) {
      LenT = std::to_string(*Units);
    } else if (LoE && HiE && L->K != Layout::Object) {
      std::optional<AffineExpr> Len = minus(*HiE, *LoE);
      if (Len && !__builtin_add_overflow(Len->Const, 1, &Len->Const)) {
        if (Len->isConstant()) {
          Units = std::max<int64_t>(Len->Const, 0);
          LenT = std::to_string(*Units);
        } else {
          LenT = text(*Len);
        }
      }
    }
    if (LenT.empty()) {
      const std::string D = StartT == "0" ? "(" + HiT + ") + 1"
                                          : "(" + HiT + ") - (" + StartT +
                                                ") + 1";
      // A pointer's ends are touched elements whenever MapGuard holds (so
      // the last is >= the first); an Object's clamped ends may cross when
      // the loop doesn't run.
      LenT = L->K == Layout::Object ? "(" + D + " > 0 ? " + D + " : 0)" : D;
    }
    R.ConstUnits = Units;
    // An Object's clamped length is at most Hi - Lo + 1 (and its extent):
    // when that is a constant (`g[i][j]` in the inner loop: one row), the
    // model prices that, though the clause keeps its clamps.
    if (!Units && L->K == Layout::Object && LoE && HiE)
      if (std::optional<AffineExpr> Len = minus(*HiE, *LoE);
          Len && Len->isConstant() && Len->Const < L->OuterExtent)
        R.ConstUnits = std::max<int64_t>(Len->Const + 1, 0);
    R.SectionText = VD->getNameAsString() + "[" + StartT + ":" + LenT + "]";
    for (int64_t E : L->Inner)
      R.SectionText += "[0:" + std::to_string(E) + "]";

    // `from`: an exact plain write whose outer subscript is one loop
    // variable (+-1) and whose inner subscripts each run over a whole row,
    // spanning the section exactly.
    if (R.Written && !R.Read && !Linear && LoE && HiE) {
      for (const One &O : Ones) {
        const ArrayAccess &X = *O.X;
        if (X.Kind != AccessKind::Write || X.ViaMember || !O.Exact)
          continue;
        auto OneIV = [&](const AffineExpr &S, bool Unit) {
          const VarDecl *Only = nullptr;
          for (const auto &[V, K] : S.IVTerms) {
            if (IsFixed(V))
              continue;
            if (Only || (K != 1 && K != -1) || (Unit && K != 1))
              return (const VarDecl *)nullptr;
            Only = V;
          }
          return Only;
        };
        const VarDecl *Outer = OneIV(X.Subscripts[0], false);
        bool Dense = Outer != nullptr;
        llvm::SmallPtrSet<const VarDecl *, 4> Used;
        if (Outer)
          Used.insert(Outer);
        for (size_t D = 1; Dense && D < X.Subscripts.size(); ++D) {
          const AffineExpr &S = X.Subscripts[D];
          const VarDecl *V = OneIV(S, true);
          const IVRange *VR = V ? RangeOf(V) : nullptr;
          Dense = V && S.SymTerms.empty() && S.Const == 0 &&
                  Used.insert(V).second && VR && VR->Exact && VR->Lo &&
                  VR->Hi && VR->Lo->isConstant() && VR->Lo->Const == 0 &&
                  VR->Hi->isConstant() && D - 1 < L->Inner.size() &&
                  VR->Hi->Const == L->Inner[D - 1] - 1;
        }
        std::optional<int64_t> DL = constDiff(O.Lo, *LoE),
                               DH = constDiff(O.Hi, *HiE);
        if (Dense && DL && *DL == 0 && DH && *DH == 0) {
          // The write covers the section only if the loops around it run:
          // a section that doesn't depend on a loop that runs zero times
          // (an Object's, or one set by an inner loop) is still copied back.
          R.WriteCovers = true;
          MustRun.insert(A.RootIV);
          for (const VarDecl *V : X.InnerIVs)
            if (!IsFixed(V))
              MustRun.insert(V);
          break;
        }
      }
    }
    Plan.Regions.push_back(std::move(R));
  }
  if (!Plan.Mappable)
    return Plan;

  // Every loop whose variable an end depends on must run at least once.
  for (const VarDecl *V : MustRun) {
    const IVRange *R = RangeOf(V);
    if (!R || !R->Lo || !R->Hi) {
      Fail("the range of '" + V->getNameAsString() + "' is not known");
      return Plan;
    }
    std::optional<AffineExpr> Span = minus(*R->Hi, *R->Lo);
    if (!Span) {
      Fail("the range of '" + V->getNameAsString() + "' overflows");
      return Plan;
    }
    if (Span->isConstant()) {
      if (Span->Const < 0) {
        Fail("the loop over '" + V->getNameAsString() + "' never runs");
        return Plan;
      }
      continue;
    }
    // Bounds that mention another loop's variable can't be tested before
    // the loop.
    bool Named = Span->IVTerms.empty() ||
                 llvm::all_of(Span->IVTerms, [&](const auto &P) {
                   return IsFixed(P.first);
                 });
    std::string Bad;
    Named = Named && NameableExpr(*Span, Bad);
    if (!Named) {
      Fail("can't test before the loop that the loop over '" +
           V->getNameAsString() + "' runs");
      return Plan;
    }
    const std::string G = nonNegative(*Span);
    if (!llvm::is_contained(Guards, G))
      Guards.push_back(G);
  }
  for (const std::string &G : Guards)
    Plan.MapGuard += (Plan.MapGuard.empty() ? "" : " && ") + G;
  return Plan;
}

} // namespace p05
