#include "analysis/Summaries.h"

#include "analysis/Profitability.h" // optionCategory()

#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/CommandLine.h"

#include <optional>

using namespace clang;

namespace p05 {

static llvm::cl::opt<IpaMode> FlagIpa(
    "ipa", llvm::cl::desc("Interprocedural analysis of calls in a loop"),
    llvm::cl::values(
        clEnumValN(IpaMode::Summaries, "summaries",
                   "call-site-aware summaries (default, Phase 1.5)"),
        clEnumValN(IpaMode::Coarse, "coarse",
                   "pre-1.5 rule: any write through a pointer parameter "
                   "rejects every call (ablation baseline)")),
    llvm::cl::init(IpaMode::Summaries), llvm::cl::cat(optionCategory()));

IpaMode ipaModeFromFlags() { return FlagIpa; }

namespace {

/// A += K * B; false on overflow.
bool addScaled(AffineExpr &A, const AffineExpr &B, int64_t K) {
  auto Step = [&](int64_t &Into, int64_t V) {
    int64_t P;
    return !__builtin_mul_overflow(V, K, &P) &&
           !__builtin_add_overflow(Into, P, &Into);
  };
  for (const auto &[V, C] : B.IVTerms)
    if (!Step(A.IVTerms[V], C))
      return false;
  for (const auto &[V, C] : B.SymTerms)
    if (!Step(A.SymTerms[V], C))
      return false;
  if (!Step(A.Const, B.Const))
    return false;
  A.IVTerms.remove_if([](const auto &P) { return P.second == 0; });
  A.SymTerms.remove_if([](const auto &P) { return P.second == 0; });
  return true;
}

using RangeMap = llvm::DenseMap<const VarDecl *, IVRange>;

bool isZero(const AffineExpr &E) { return E.isConstant() && E.Const == 0; }

bool sameExpr(const std::optional<AffineExpr> &X,
              const std::optional<AffineExpr> &Y) {
  if (!X || !Y)
    return !X && !Y;
  AffineExpr D = *X;
  return addScaled(D, *Y, -1) && isZero(D);
}

std::string quoted(const NamedDecl *D) {
  return "'" + D->getNameAsString() + "'";
}

/// The index of VD among Def's parameters, if it is one.
std::optional<unsigned> paramIndex(const ValueDecl *VD,
                                   const FunctionDecl *Def) {
  const auto *PVD = dyn_cast_or_null<ParmVarDecl>(VD);
  if (!PVD)
    return std::nullopt;
  for (unsigned K = 0; K < Def->getNumParams(); ++K)
    if (Def->getParamDecl(K)->getCanonicalDecl() == PVD->getCanonicalDecl())
      return K;
  return std::nullopt;
}

void makeNonAffine(ArrayAccess &X, std::string Why, bool KeepBase = true) {
  X.Affine = false;
  if (!KeepBase) {
    X.Base = nullptr;
    X.BaseMayAlias = false;
  }
  X.NonAffineReason = std::move(Why);
  X.Load = ArrayAccess::SubscriptLoad::None; // only the text is known here
  X.Subscripts.clear();
  X.Bounds.clear();
  X.DimExtents.clear();
  X.RowLengths.clear();
  X.Delinearized = false;
}

/// The call's argument for parameter K if its value IS the parameter's
/// value: the callee has a prototype at the call and in its definition (an
/// unprototyped call converts nothing at the call site, so a `char` or
/// `char *` parameter would reinterpret the promoted argument), and the
/// argument's type is the parameter's (after the call's implicit
/// conversion, up to qualifiers). Null otherwise, with Why.
const CallArg *usableArg(const CallSite &CS, const FunctionDecl *Def,
                         unsigned K, std::string &Why) {
  if (K >= CS.Args.size() || K >= Def->getNumParams()) {
    Why = "no argument";
    return nullptr;
  }
  const QualType CalleeT = CS.Call->getCallee()->getType();
  const bool NoProto =
      !Def->hasPrototype() ||
      (CalleeT->isPointerType() &&
       CalleeT->getPointeeType()->getAs<FunctionNoProtoType>());
  auto Plain = [](QualType T) {
    return T.getCanonicalType().getUnqualifiedType();
  };
  if (NoProto ||
      Plain(CS.Call->getArg(K)->getType()) !=
          Plain(Def->getParamDecl(K)->getType())) {
    Why = NoProto ? "the call has no prototype, so the argument is not "
                    "converted to the parameter's type"
                  : "the argument's type is not the parameter's";
    return nullptr;
  }
  return &CS.Args[K];
}

/// The call's argument for parameter K, described for reasons.
std::string argText(const CallSite &CS, unsigned K) {
  return K < CS.Args.size() ? "'" + CS.Args[K].Text + "'" : "no argument";
}

/// Replace Def's parameters in E by the call's (integer) arguments. A symbol
/// that is not a parameter (a global) is kept unless OnlyParams, in which
/// case it fails: at the loop, a global in a callee's subscript or range is
/// not known to be fixed while the loop runs.
bool substitute(AffineExpr &E, const FunctionDecl *Def, const CallSite &CS,
                bool OnlyParams, std::string &Why) {
  AffineExpr Out;
  Out.IVTerms = E.IVTerms;
  Out.Const = E.Const;
  for (const auto &[S, C] : E.SymTerms) {
    std::optional<unsigned> K = paramIndex(S, Def);
    if (!K) {
      if (OnlyParams) {
        Why = "'" + Def->getNameAsString() + "' indexes with " + quoted(S) +
              ", which is not one of its parameters (the loop could change "
              "it between calls)";
        return false;
      }
      AffineExpr T;
      T.SymTerms[S] = 1;
      if (!addScaled(Out, T, C)) {
        Why = "subscript coefficient overflows";
        return false;
      }
      continue;
    }
    std::string Bad;
    const CallArg *A = usableArg(CS, Def, *K, Bad);
    if (A && !A->Affine)
      Bad = A->Why;
    if (!A || A->K != CallArg::Kind::Value || !A->Affine) {
      Why = "'" + Def->getNameAsString() + "' indexes with parameter " +
            quoted(S) + ", which receives " + argText(CS, *K) +
            (!Bad.empty() ? " (" + Bad + ")" : std::string());
      return false;
    }
    if (!addScaled(Out, A->Offset, C)) {
      Why = "subscript coefficient overflows";
      return false;
    }
  }
  E = std::move(Out);
  return true;
}

/// Def's loop ranges in the caller's terms. A bound that can't be expressed
/// is dropped (unbounded on that side), which only loses precision.
RangeMap
substituteRanges(const FunctionSummary &S, const FunctionDecl *Def,
                 const CallSite &CS, bool OnlyParams) {
  llvm::DenseMap<const VarDecl *, IVRange> Out;
  for (const auto &[V, R] : S.Ranges) {
    IVRange N;
    std::string Why;
    for (auto [From, To] : {std::pair{&R.Lo, &N.Lo}, std::pair{&R.Hi, &N.Hi}})
      if (*From) {
        AffineExpr E = **From;
        if (substitute(E, Def, CS, OnlyParams, Why))
          *To = std::move(E);
      }
    N.Exact = R.Exact && N.Lo && N.Hi;
    Out[V] = std::move(N);
  }
  return Out;
}

/// Add R as V's range; a variable that gets two different ranges (one callee
/// called with two different arguments) has no single range.
void mergeRange(llvm::DenseMap<const VarDecl *, IVRange> &Into,
                const VarDecl *V, const IVRange &R) {
  auto [It, New] = Into.try_emplace(V, R);
  if (!New && !(sameExpr(It->second.Lo, R.Lo) && sameExpr(It->second.Hi, R.Hi)))
    It->second = IVRange();
  else if (!New)
    It->second.Exact = It->second.Exact && R.Exact;
}

/// `&a[X * N]` passed for p, and the callee touches p[e]: a[X][e] in rows of
/// length N, if e is `j + c` for a loop variable j that provably stays
/// inside the row (0 <= j + c <= N - 1 over j's whole range). j is the
/// callee's (Ranges, already in the caller's terms) or the caller's own
/// (Outer: `f(&a[i * n], j)` inside the caller's j loop).
bool composeRow(ArrayAccess &Y, const CallArg &Arg, const RangeMap &Ranges,
                const RangeMap &Outer, std::string &Why) {
  Why = "the argument " + std::string("'") + Arg.Text +
        "' starts a row of length " + quoted(Arg.RowLength) +
        ", and the callee's subscript can't be shown to stay inside it";
  if (Y.Subscripts.size() != 1 || Y.Delinearized || !isZero(Arg.Offset))
    return false;
  const AffineExpr &E = Y.Subscripts[0];
  if (!E.SymTerms.empty() || E.IVTerms.size() != 1 ||
      E.IVTerms.begin()->second != 1)
    return false;
  const VarDecl *J = E.IVTerms.begin()->first;
  auto It = Ranges.find(J);
  if (It == Ranges.end() && (It = Outer.find(J)) == Outer.end())
    return false;
  if (!It->second.Lo || !It->second.Hi)
    return false;
  const AffineExpr &Lo = *It->second.Lo, &Hi = *It->second.Hi;
  if (!Lo.isConstant() || !Hi.IVTerms.empty() || Hi.SymTerms.size() != 1 ||
      Hi.SymTerms.begin()->first != Arg.RowLength ||
      Hi.SymTerms.begin()->second != 1)
    return false;
  // Same type for j and N, as AffineAccess's rowIndexInRange requires.
  if (J->getType().getCanonicalType().getUnqualifiedType() !=
      Arg.RowLength->getType().getCanonicalType().getUnqualifiedType())
    return false;
  const __int128 C = E.Const;
  if (__int128(Lo.Const) + C < 0 || __int128(Hi.Const) + C > -1)
    return false;
  Y.Subscripts = {*Arg.RowIndex, E};
  Y.Delinearized = true;
  Y.RowLengths = {Arg.RowLength};
  Y.DimExtents = {std::string(), Arg.RowLength->getNameAsString()};
  return true;
}

/// One access of Def's summary, at call CS, in the caller's terms (with
/// Ranges the substituted callee ranges). OnlyParams at the loop itself.
ArrayAccess composeAccess(const ArrayAccess &X, const FunctionDecl *Def,
                          const CallSite &CS, const RangeMap &Ranges,
                          const RangeMap &Outer, bool OnlyParams) {
  ArrayAccess Y = X;
  // Phase 2.5: a value bound is in the callee's terms (its loop ranges and
  // parameters). The declared extents are assigned later, by base.
  if (!Y.Affine) {
    Y.Bounds.clear();
    Y.DimExtents.clear();
  }
  Y.Extents.clear();
  Y.E = CS.Call;
  Y.Within.push_back(X.E);
  Y.Text = Def->getNameAsString() + "(): " + X.Text;
  Y.InnerIVs = CS.InnerIVs;
  Y.InnerIVs.insert(Y.InnerIVs.end(), X.InnerIVs.begin(), X.InnerIVs.end());
  Y.BaseSubstituted = true;

  const CallArg *Arg = nullptr;
  if (std::optional<unsigned> K = paramIndex(X.Base, Def)) {
    std::string Bad;
    const CallArg *A = usableArg(CS, Def, *K, Bad);
    if (A && !A->Base)
      Bad = A->Why;
    if (!A || A->K != CallArg::Kind::Pointer || !A->Base) {
      makeNonAffine(Y,
                    "parameter " + quoted(X.Base) + " receives " +
                        argText(CS, *K) +
                        (!Bad.empty() ? " (" + Bad + ")" : std::string()),
                    /*KeepBase=*/false);
      return Y;
    }
    Arg = A;
    Y.Base = A->Base;
    Y.BaseMayAlias = A->BaseMayAlias;
    if (!A->Affine) {
      makeNonAffine(Y, "parameter " + quoted(X.Base) + " receives " +
                           argText(CS, *K) + " (" + A->Why + ")");
      return Y;
    }
  }
  // Otherwise the base is a global or static object (the same object here)
  // or unknown (null), and stays as it is.
  if (!X.Affine)
    return Y;

  std::string Why;
  for (AffineExpr &S : Y.Subscripts)
    if (!substitute(S, Def, CS, OnlyParams, Why)) {
      makeNonAffine(Y, Why);
      return Y;
    }
  for (const VarDecl *&N : Y.RowLengths) {
    std::optional<unsigned> K = paramIndex(N, Def);
    if (!K) {
      if (OnlyParams) {
        makeNonAffine(Y, "'" + Def->getNameAsString() +
                             "' uses global " + quoted(N) +
                             " as a row length, which the loop could change");
        return Y;
      }
      continue;
    }
    std::string Bad;
    const CallArg *A = usableArg(CS, Def, *K, Bad);
    const VarDecl *V = nullptr;
    if (A && A->K == CallArg::Kind::Value && A->Affine &&
        A->Offset.IVTerms.empty() && A->Offset.Const == 0 &&
        A->Offset.SymTerms.size() == 1 &&
        A->Offset.SymTerms.begin()->second == 1)
      V = dyn_cast<VarDecl>(A->Offset.SymTerms.begin()->first);
    if (!V) {
      makeNonAffine(Y, "row length " + quoted(N) + " receives " +
                           argText(CS, *K) + ", not a plain variable");
      return Y;
    }
    N = V;
  }
  if (!Arg)
    return Y;
  if (Arg->RowIndex) {
    if (!composeRow(Y, *Arg, Ranges, Outer, Why))
      makeNonAffine(Y, Why);
    return Y;
  }
  if (isZero(Arg->Offset))
    return Y;
  if (Y.Subscripts.size() != 1) {
    makeNonAffine(Y, "'" + Def->getNameAsString() + "' indexes " +
                         quoted(X.Base) +
                         " as a multi-dimensional array, but receives the "
                         "offset pointer '" + Arg->Text + "'");
    return Y;
  }
  if (!addScaled(Y.Subscripts[0], Arg->Offset, 1))
    makeNonAffine(Y, "subscript coefficient overflows");
  return Y;
}

/// Whether an access of Def (own or composed) belongs in Def's summary, and
/// its base in summary form: false for Def's own automatic objects (fresh
/// on every call, invisible to any caller); a base that is not the same
/// object in every caller becomes unknown.
bool keepInSummary(ArrayAccess &X, const FunctionDecl *Def) {
  if (!X.Base)
    return true;
  const auto *VD = dyn_cast<VarDecl>(X.Base);
  if (!VD) {
    makeNonAffine(X, "base is not a variable", /*KeepBase=*/false);
    return true;
  }
  if (paramIndex(VD, Def)) {
    // `&n` of a by-value parameter points at this call's own copy.
    if (!VD->getType()->isPointerType())
      return false;
    if (X.BaseMayAlias)
      makeNonAffine(X,
                    "pointer parameter " + quoted(VD) + " is reassigned or "
                    "escapes in '" + Def->getNameAsString() + "'",
                    /*KeepBase=*/false);
    return true;
  }
  if (VD->hasGlobalStorage()) {
    if (VD->getType()->isPointerType())
      makeNonAffine(X,
                    "goes through global pointer " + quoted(VD) +
                        ", which may change between calls",
                    /*KeepBase=*/false);
    return true;
  }
  // A local of Def. A pointer one that AffineAccess could not resolve to
  // what it points into may point anywhere; anything else is Def's own.
  if (VD->getType()->isPointerType()) {
    makeNonAffine(X,
                  "goes through local pointer " + quoted(VD) + " of '" +
                      Def->getNameAsString() + "', which may point anywhere",
                  /*KeepBase=*/false);
    return true;
  }
  return false;
}

/// Effects of a function's body that are not element accesses: writes to
/// globals and statics by name (UNSAFE, as before 1.5: every call writes
/// the same variable), atomics and inline asm (UNKNOWN).
class NameLevelVisitor : public RecursiveASTVisitor<NameLevelVisitor> {
public:
  struct Effect {
    std::string Text;
    bool Unsafe;
    RejectCategory Cat;
  };
  std::vector<Effect> Found;

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      write(BO->getLHS());
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      write(UO->getSubExpr());
    return true;
  }
  bool VisitAtomicExpr(AtomicExpr *) {
    note("uses an atomic builtin", false, RejectCategory::AtomicOrAsm);
    return true;
  }
  bool VisitAsmStmt(AsmStmt *) {
    note("contains inline asm", false, RejectCategory::AtomicOrAsm);
    return true;
  }

private:
  void note(std::string Text, bool Unsafe, RejectCategory Cat) {
    for (const Effect &F : Found)
      if (F.Text == Text)
        return;
    Found.push_back({std::move(Text), Unsafe, Cat});
  }
  /// `g = `, `g.f = ` (not through `->`, `*` or `[]`: those are accesses).
  void write(const Expr *E) {
    E = E->IgnoreParenCasts();
    while (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->isArrow())
        return;
      E = ME->getBase()->IgnoreParenCasts();
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
        if (VD->hasGlobalStorage())
          note("writes global " + quoted(VD), true,
               RejectCategory::CalleeSideEffect);
  }
};

/// The expressions of `&a[..]` / decayed-row accesses that are passed
/// directly as an argument of CS.
void addressArgs(const CallSite &CS, llvm::DenseSet<const Expr *> &Out) {
  for (const Expr *Arg : CS.Call->arguments()) {
    const Expr *P = Arg->IgnoreParenImpCasts();
    Out.insert(P);
    if (const auto *UO = dyn_cast<UnaryOperator>(P);
        UO && UO->getOpcode() == UO_AddrOf)
      Out.insert(UO->getSubExpr()->IgnoreParens());
  }
}

const FunctionDecl *visibleDefinition(const FunctionDecl *FD) {
  const FunctionDecl *Def = FD ? FD->getDefinition() : nullptr;
  return Def && Def->getBody() ? Def : nullptr;
}

void dropPassedAddresses(std::vector<ArrayAccess> &Accesses,
                         const llvm::DenseSet<const Expr *> &Passed) {
  llvm::erase_if(Accesses, [&](const ArrayAccess &X) {
    return X.Kind == AccessKind::AddressTaken && Passed.count(X.E);
  });
}

} // namespace

const FunctionSummary &SummaryAnalyzer::summarize(const FunctionDecl *FD) {
  FD = FD->getCanonicalDecl();
  if (auto It = Cache.find(FD); It != Cache.end())
    return *It->second;
  if (InProgress.count(FD))
    return Recursive;

  auto S = std::make_unique<FunctionSummary>();
  const FunctionDecl *Def = visibleDefinition(FD);
  if (!Def) {
    S->Incomplete = true;
    return *Cache.try_emplace(FD, std::move(S)).first->second;
  }
  InProgress.insert(FD);

  LoopAccesses A = Affine.analyzeFunction(Def);
  S->Ranges = A.Ranges;
  llvm::DenseSet<const Expr *> Passed;
  auto Reason = [&S](const SummaryReason &R) {
    for (const SummaryReason &Old : S->Reasons)
      if (Old.Where == R.Where && Old.Text == R.Text)
        return;
    S->Reasons.push_back(R);
  };

  for (const CallSite &CS : A.Calls) {
    if (!CS.Callee) {
      Reason({Def, "calls through a function pointer", false,
              RejectCategory::IndirectCall});
      continue;
    }
    const FunctionDecl *GDef = visibleDefinition(CS.Callee);
    if (!GDef) {
      S->Incomplete = true; // reported by the loop's call-chain walk
      continue;
    }
    const FunctionSummary &G = summarize(CS.Callee);
    S->Incomplete |= G.Incomplete;
    for (const SummaryReason &R : G.Reasons)
      Reason(R);
    auto Ranges = substituteRanges(G, GDef, CS, /*OnlyParams=*/false);
    for (const auto &[V, R] : Ranges)
      mergeRange(S->Ranges, V, R);
    for (const ArrayAccess &X : G.Accesses) {
      ArrayAccess Y =
          composeAccess(X, GDef, CS, Ranges, A.Ranges, /*OnlyParams=*/false);
      if (keepInSummary(Y, Def))
        S->Accesses.push_back(std::move(Y));
    }
    addressArgs(CS, Passed);
  }
  dropPassedAddresses(A.Accesses, Passed);
  // Own accesses first, in source order, then the composed ones.
  std::vector<ArrayAccess> Own;
  for (ArrayAccess &X : A.Accesses)
    if (keepInSummary(X, Def))
      Own.push_back(std::move(X));
  S->Accesses.insert(S->Accesses.begin(), std::make_move_iterator(Own.begin()),
                     std::make_move_iterator(Own.end()));

  NameLevelVisitor NV;
  NV.TraverseStmt(Def->getBody());
  for (const NameLevelVisitor::Effect &F : NV.Found)
    Reason({Def, F.Text, F.Unsafe, F.Cat});

  InProgress.erase(FD);
  return *Cache.try_emplace(FD, std::move(S)).first->second;
}

CallEffects SummaryAnalyzer::composeCalls(LoopAccesses &A) {
  CallEffects Fx;
  auto Add = [](std::vector<Reason> &V, RejectCategory Cat, std::string S) {
    if (llvm::none_of(V, [&](const Reason &R) { return R.Text == S; }))
      V.push_back({Cat, std::move(S)});
  };
  llvm::DenseSet<const Expr *> Passed;
  std::vector<ArrayAccess> Composed;
  const RangeMap Own = A.Ranges; // the loop's, before callees' are merged in
  for (const CallSite &CS : A.Calls) {
    // An indirect or invisible callee: LoopInfo::HasIndirectCall and the
    // loop-level "no visible body" check already make the loop UNKNOWN.
    const FunctionDecl *Def = visibleDefinition(CS.Callee);
    if (!Def)
      continue;
    const FunctionSummary &S = summarize(CS.Callee);
    const std::string Name = quoted(CS.Callee);
    for (const SummaryReason &R : S.Reasons) {
      std::string Text =
          R.Where->getCanonicalDecl() == Def->getCanonicalDecl()
              ? Name + " " + R.Text
              : Name + " transitively reaches " + quoted(R.Where) +
                    ", which " + R.Text;
      Add(R.Unsafe ? Fx.UnsafeReasons : Fx.UnknownReasons, R.Cat,
          std::move(Text));
    }
    if (S.Incomplete && !llvm::is_contained(Fx.Incomplete, CS.Callee))
      Fx.Incomplete.push_back(CS.Callee);
    auto Ranges = substituteRanges(S, Def, CS, /*OnlyParams=*/true);
    for (const auto &[V, R] : Ranges)
      mergeRange(A.Ranges, V, R);
    for (const ArrayAccess &X : S.Accesses)
      Composed.push_back(
          composeAccess(X, Def, CS, Ranges, Own, /*OnlyParams=*/true));
    addressArgs(CS, Passed);
  }
  dropPassedAddresses(A.Accesses, Passed);
  A.Accesses.insert(A.Accesses.end(), std::make_move_iterator(Composed.begin()),
                    std::make_move_iterator(Composed.end()));
  return Fx;
}

} // namespace p05
