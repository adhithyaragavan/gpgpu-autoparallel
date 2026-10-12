#include "analysis/DependenceTest.h"

#include "llvm/ADT/SmallVector.h"

#include <cstdlib>
#include <numeric>
#include <optional>
#include <set>

using namespace clang;

namespace p05 {

namespace {

using Int = __int128;

/// A += K * B. False on overflow (the caller then treats the value as
/// unknown, never as something it isn't).
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

bool isZero(const AffineExpr &E) { return E.isConstant() && E.Const == 0; }

bool sameExpr(const AffineExpr &X, const AffineExpr &Y) {
  AffineExpr D = X;
  return addScaled(D, Y, -1) && isZero(D);
}

/// Is E < 0 for every value of its symbols allowed by Facts (each Fact >= 0)?
/// Constant E: direct. Otherwise look for one fact F and a rational
/// lambda > 0 with E + lambda * F constant and negative; then
/// E = (E + lambda F) - lambda F <= that constant < 0.
bool provablyNegative(const AffineExpr &E,
                      const std::vector<AffineExpr> &Facts) {
  if (E.isConstant())
    return E.Const < 0;
  for (const AffineExpr &F : Facts) {
    // Pivot: any term of E.
    int64_t Et = 0, Ft = 0;
    if (!E.IVTerms.empty()) {
      const auto &[V, C] = *E.IVTerms.begin();
      Et = C;
      auto It = F.IVTerms.find(V);
      Ft = It == F.IVTerms.end() ? 0 : It->second;
    } else {
      const auto &[V, C] = *E.SymTerms.begin();
      Et = C;
      auto It = F.SymTerms.find(V);
      Ft = It == F.SymTerms.end() ? 0 : It->second;
    }
    if (Ft == 0 || (Et > 0) == (Ft > 0)) // lambda = -Et/Ft must be > 0
      continue;
    // Proportional: E_s * Ft == Et * F_s for every term s of E or F.
    bool Prop = true;
    auto Check = [&](int64_t Es, int64_t Fs) {
      Prop &= Int(Es) * Ft == Int(Et) * Fs;
    };
    for (const auto &[V, C] : E.IVTerms)
      Check(C, F.coeff(V));
    for (const auto &[V, C] : F.IVTerms)
      Check(E.coeff(V), C);
    for (const auto &[V, C] : E.SymTerms) {
      auto It = F.SymTerms.find(V);
      Check(C, It == F.SymTerms.end() ? 0 : It->second);
    }
    for (const auto &[V, C] : F.SymTerms) {
      auto It = E.SymTerms.find(V);
      Check(It == E.SymTerms.end() ? 0 : It->second, C);
    }
    if (!Prop)
      continue;
    // Constant of E + lambda F is (E.c * Ft - Et * F.c) / Ft.
    const Int Num = Int(E.Const) * Ft - Int(Et) * F.Const;
    if (Ft > 0 ? Num < 0 : Num > 0)
      return true;
  }
  return false;
}

bool provablyPositive(const AffineExpr &E,
                      const std::vector<AffineExpr> &Facts) {
  AffineExpr N;
  return addScaled(N, E, -1) && provablyNegative(N, Facts);
}

/// A bound (range side) that only mentions values fixed while the analyzed
/// loop runs: symbols and enclosing loops' variables.
bool onlyFixed(const AffineExpr &E, const LoopAccesses &A) {
  for (const auto &[V, C] : E.IVTerms)
    if (std::find(A.OuterIVs.begin(), A.OuterIVs.end(), V) == A.OuterIVs.end())
      return false;
  return true;
}

IVRange usableRange(const LoopAccesses &A, const VarDecl *V) {
  auto It = A.Ranges.find(V);
  if (It == A.Ranges.end())
    return IVRange();
  IVRange R = It->second;
  if (R.Lo && !onlyFixed(*R.Lo, A))
    R.Lo.reset();
  if (R.Hi && !onlyFixed(*R.Hi, A))
    R.Hi.reset();
  return R;
}

/// Running min or max of a sum of terms; Inf = unbounded on that side.
struct Extreme {
  bool Inf = false;
  AffineExpr E;
  void add(const AffineExpr &X, int64_t K) {
    if (!Inf && !addScaled(E, X, K))
      Inf = true;
  }
  /// Add K * v for v in [Lo, Hi]; WantMax picks the maximizing end.
  void addTerm(int64_t K, const std::optional<AffineExpr> &Lo,
               const std::optional<AffineExpr> &Hi, bool WantMax) {
    if (K == 0 || Inf)
      return;
    const bool UseHi = (K > 0) == WantMax;
    const std::optional<AffineExpr> &Side = UseHi ? Hi : Lo;
    if (!Side)
      Inf = true;
    else
      add(*Side, K);
  }
};

/// One dimension of the dependence equation
///   a*i1 - b*i2 + sum(p*x) - sum(q*y) + Fixed = 0
/// where i1/i2 are the analyzed loop's variable in the two accesses, x/y the
/// inner loops' variables (independent copies per access), and Fixed holds
/// symbols and enclosing loops' variables (shared) plus the constant.
struct DimEq {
  int64_t A = 0, B = 0;
  llvm::SmallVector<std::pair<const VarDecl *, int64_t>, 4> In1, In2;
  AffineExpr Fixed;
  bool Ok = true;
};

DimEq makeEq(const AffineExpr &S1, const AffineExpr &S2,
             const LoopAccesses &A) {
  DimEq Q;
  auto Split = [&](const AffineExpr &S, int64_t &Root,
                   llvm::SmallVectorImpl<std::pair<const VarDecl *, int64_t>>
                       &Inner,
                   int64_t Sign) {
    for (const auto &[V, C] : S.IVTerms) {
      if (V == A.RootIV)
        Root = C;
      else if (std::find(A.OuterIVs.begin(), A.OuterIVs.end(), V) !=
               A.OuterIVs.end()) {
        AffineExpr T;
        T.IVTerms[V] = C;
        Q.Ok &= addScaled(Q.Fixed, T, Sign);
      } else
        Inner.push_back({V, C});
    }
    AffineExpr Rest;
    Rest.SymTerms = S.SymTerms;
    Rest.Const = S.Const;
    Q.Ok &= addScaled(Q.Fixed, Rest, Sign);
  };
  Split(S1, Q.A, Q.In1, 1);
  Split(S2, Q.B, Q.In2, -1);
  return Q;
}

int64_t gcd64(int64_t X, int64_t Y) {
  return std::gcd(X < 0 ? -X : X, Y < 0 ? -Y : Y);
}

enum class Proof { None, ZIV, GCD, SIV, Banerjee };

struct DimResult {
  /// Proven unsatisfiable regardless of iteration order.
  Proof Indep = Proof::None;
  /// Banerjee: unsatisfiable when i1 < i2 / when i1 > i2.
  bool IndepLT = false, IndepGT = false;
  /// Exact shape: Distance set for a strong-SIV dimension (i2 - i1, in index
  /// units); Free when the dimension holds for any i1, i2 (identical sides
  /// with no root variable).
  std::optional<int64_t> Distance;
  bool Free = false;
};

DimResult testDim(const DimEq &Q, const LoopAccesses &A, int64_t Step,
                  const std::vector<AffineExpr> &Facts) {
  DimResult R;
  if (!Q.Ok)
    return R;
  const bool NoInner = Q.In1.empty() && Q.In2.empty();

  // ZIV: nothing varies with the iteration.
  if (Q.A == 0 && Q.B == 0 && NoInner) {
    if (isZero(Q.Fixed))
      R.Free = true;
    else if (provablyNegative(Q.Fixed, Facts) ||
             provablyPositive(Q.Fixed, Facts))
      R.Indep = Proof::ZIV;
    return R;
  }

  // GCD over every coefficient, symbols included (they are integers too).
  int64_t G = gcd64(Q.A, Q.B);
  for (const auto &[V, C] : Q.In1)
    G = gcd64(G, C);
  for (const auto &[V, C] : Q.In2)
    G = gcd64(G, C);
  for (const auto &[V, C] : Q.Fixed.IVTerms)
    G = gcd64(G, C);
  for (const auto &[V, C] : Q.Fixed.SymTerms)
    G = gcd64(G, C);
  if (G != 0 && Q.Fixed.Const % G != 0) {
    R.Indep = Proof::GCD;
    return R;
  }

  const IVRange Root = usableRange(A, A.RootIV);

  // Strong SIV: a*i1 - a*i2 + c = 0, so i2 - i1 = c / a.
  if (Q.A == Q.B && Q.A != 0 && NoInner && Q.Fixed.isConstant()) {
    const int64_t Dist = Q.Fixed.Const / Q.A; // exact: GCD passed
    if (Dist == 0 || Dist % Step != 0) {
      R.Indep = Proof::SIV;
      return R;
    }
    // Needs more iterations than |Dist| / |Step|: Hi - Lo >= |Dist|.
    if (Root.Lo && Root.Hi) {
      AffineExpr Span = *Root.Hi;
      if (addScaled(Span, *Root.Lo, -1) && Span.isConstant() &&
          Span.Const < std::llabs(Dist)) {
        R.Indep = Proof::SIV;
        return R;
      }
    }
    R.Distance = Dist;
  }

  // An inner-only dimension with identical sides holds whenever the inner
  // variables coincide, for any i1, i2.
  // Only claimed when every inner variable's range is known without the
  // analyzed loop's variable (a triangular inner range may differ, or be
  // empty, from one iteration to the next).
  if (Q.A == 0 && Q.B == 0 && isZero(Q.Fixed) && Q.In1.size() == Q.In2.size()) {
    std::multiset<std::pair<const VarDecl *, int64_t>> S1(Q.In1.begin(),
                                                          Q.In1.end()),
        S2(Q.In2.begin(), Q.In2.end());
    bool RangesKnown = true;
    for (const auto &[V, C] : Q.In1) {
      IVRange IR = usableRange(A, V);
      RangesKnown &= IR.Lo.has_value() && IR.Hi.has_value();
    }
    R.Free = S1 == S2 && RangesKnown;
  }

  // Banerjee, per iteration order. d = |i2 - i1| >= 1 ranges over
  // [1, Hi - Lo]; the earlier iteration's variable over [Lo, Hi - 1].
  std::optional<AffineExpr> EarlyHi, DHi;
  if (Root.Hi) {
    EarlyHi = *Root.Hi;
    if (__builtin_add_overflow(EarlyHi->Const, -1, &EarlyHi->Const))
      EarlyHi.reset();
  }
  if (Root.Lo && Root.Hi) {
    DHi = *Root.Hi;
    if (!addScaled(*DHi, *Root.Lo, -1))
      DHi.reset();
  }
  AffineExpr One;
  One.Const = 1;
  const std::optional<AffineExpr> DLo = One;

  for (int Order = 0; Order < 2; ++Order) {
    // Order 0: i1 < i2 = i1 + d: (a - b) * i1 - b * d.
    // Order 1: i1 = i2 + d > i2: (a - b) * i2 + a * d.
    const int64_t KEarly = Q.A - Q.B;
    const int64_t KD = Order == 0 ? -Q.B : Q.A;
    Extreme Min, Max;
    for (Extreme *X : {&Min, &Max}) {
      const bool WantMax = X == &Max;
      X->add(Q.Fixed, 1);
      X->addTerm(KEarly, Root.Lo, EarlyHi, WantMax);
      X->addTerm(KD, DLo, DHi, WantMax);
      for (const auto &[V, C] : Q.In1) {
        IVRange IR = usableRange(A, V);
        X->addTerm(C, IR.Lo, IR.Hi, WantMax);
      }
      for (const auto &[V, C] : Q.In2) {
        IVRange IR = usableRange(A, V);
        X->addTerm(-C, IR.Lo, IR.Hi, WantMax);
      }
    }
    const bool Indep = (!Max.Inf && provablyNegative(Max.E, Facts)) ||
                       (!Min.Inf && provablyPositive(Min.E, Facts));
    (Order == 0 ? R.IndepLT : R.IndepGT) = Indep;
  }
  return R;
}

bool writes(const ArrayAccess &X) {
  return X.Kind == AccessKind::Write || X.Kind == AccessKind::ReadWrite;
}

std::string describe(const ArrayAccess &X) {
  return toString(X.Kind).str() + " " + X.Text;
}

/// Exactly `[RootIV]`: the only subscript today's map() clauses size right.
bool exactlyRootIV(const ArrayAccess &X, const LoopAccesses &A) {
  // Subscript syntax only: the map regions are collected from `a[i]`
  // expressions (Profitability.cpp's RegionVisitor), not from `*(a + i)`.
  if (!X.Base || !X.Affine || X.Subscripts.size() != 1 || X.Delinearized ||
      X.BaseSubstituted || !llvm::isa_and_nonnull<clang::ArraySubscriptExpr>(X.E))
    return false;
  const AffineExpr &S = X.Subscripts[0];
  return S.SymTerms.empty() && S.Const == 0 && S.IVTerms.size() == 1 &&
         S.coeff(A.RootIV) == 1;
}

} // namespace

llvm::StringRef toString(AliasProof P) {
  switch (P) {
  case AliasProof::Distinct:
    return "distinct objects";
  case AliasProof::Types:
    return "incompatible element types";
  case AliasProof::Restrict:
    return "restrict";
  case AliasProof::Callers:
    return "every call passes disjoint objects";
  case AliasProof::Unproven:
    return "unproven";
  }
  return "unproven";
}

bool subscriptBounds(const AffineExpr &S, const LoopAccesses &A,
                     AffineExpr &Lo, AffineExpr &Hi) {
  Extreme Min, Max;
  for (Extreme *X : {&Min, &Max}) {
    const bool WantMax = X == &Max;
    AffineExpr Fixed;
    Fixed.SymTerms = S.SymTerms;
    Fixed.Const = S.Const;
    X->add(Fixed, 1);
    for (const auto &[V, C] : S.IVTerms) {
      if (std::find(A.OuterIVs.begin(), A.OuterIVs.end(), V) !=
          A.OuterIVs.end()) {
        AffineExpr T;
        T.IVTerms[V] = C;
        X->add(T, 1);
        continue;
      }
      IVRange R = usableRange(A, V);
      X->addTerm(C, R.Lo, R.Hi, WantMax);
    }
  }
  if (Min.Inf || Max.Inf)
    return false;
  Lo = Min.E;
  Hi = Max.E;
  return true;
}

LoopDependence testLoopDependences(const LoopAccesses &A, int64_t RootStep,
                                   const AliasOracle *Alias) {
  LoopDependence D;
  auto Note = [&D](DepOutcome O, std::string Reason) {
    if (O > D.Outcome)
      D.Outcome = O;
    if (std::find(D.Reasons.begin(), D.Reasons.end(), Reason) ==
        D.Reasons.end())
      D.Reasons.push_back(std::move(Reason));
  };

  bool AnyWrite = false;
  for (const ArrayAccess &X : A.Accesses) {
    AnyWrite |= writes(X);
    const auto *BaseVD = llvm::dyn_cast_or_null<VarDecl>(X.Base);
    if (!exactlyRootIV(X, A) || X.BaseMayAlias ||
        (BaseVD && A.DeclaredInLoop.count(BaseVD->getCanonicalDecl())))
      D.GpuMappable = false;
  }
  if (!A.RootIV) {
    Note(DepOutcome::Unknown, "loop shape not recognized");
    return D;
  }
  const int64_t Step = RootStep == 0 ? 1 : std::llabs(RootStep);
  const IVRange RootRange = usableRange(A, A.RootIV);

  // map(...[0:bound]) covers indices 0..bound-1: the loop must start at a
  // known index >= 0 (a negative or unknown start reads below the region).
  if (!RootRange.Lo || !RootRange.Lo->isConstant() || RootRange.Lo->Const < 0)
    D.GpuMappable = false;

  // At most one iteration: nothing can be carried — provided the header's
  // range is the real iteration space (not when the loop's variable is
  // modified inside it).
  if (RootRange.Lo && RootRange.Hi && !A.Poisoned) {
    AffineExpr Span = *RootRange.Hi;
    if (addScaled(Span, *RootRange.Lo, -1) && Span.isConstant() &&
        Span.Const < Step) {
      D.Reasons.clear();
      D.SingleIteration = true;
      return D;
    }
  }

  // Facts the sign test may rely on. A carried dependence needs at least two
  // iterations of the analyzed loop (Hi - Lo - 1 >= 0): always usable. An
  // inner loop's "it runs" (Hi - Lo >= 0) is only true when the access is
  // inside that loop, so it is attached per access and used only for pairs
  // that include such an access.
  auto SpanFact = [&](const VarDecl *V, int64_t Minus)
      -> std::optional<AffineExpr> {
    IVRange R = usableRange(A, V);
    if (!R.Lo || !R.Hi)
      return std::nullopt;
    AffineExpr F = *R.Hi;
    if (!addScaled(F, *R.Lo, -1) ||
        __builtin_add_overflow(F.Const, -Minus, &F.Const))
      return std::nullopt;
    return F;
  };
  std::vector<AffineExpr> RootFacts;
  if (std::optional<AffineExpr> F = SpanFact(A.RootIV, 1))
    RootFacts.push_back(*F);
  auto FactsFor = [&](const ArrayAccess &X, const ArrayAccess &Y) {
    std::vector<AffineExpr> Facts = RootFacts;
    for (const ArrayAccess *Z : {&X, &Y})
      for (const VarDecl *V : Z->InnerIVs)
        if (std::optional<AffineExpr> F = SpanFact(V, 0))
          Facts.push_back(*F);
    return Facts;
  };

  // Accesses whose base can't be named.
  for (const ArrayAccess &X : A.Accesses) {
    if (X.Base)
      continue;
    if (writes(X))
      Note(DepOutcome::Unknown, "cannot tell what " + describe(X) +
                                    " writes (" + X.NonAffineReason + ")");
    else if (AnyWrite)
      Note(DepOutcome::Unknown,
           describe(X) + " reads memory this analysis cannot name (" +
               X.NonAffineReason + "), which a write in the loop could alias");
  }

  for (size_t K = 0; K < A.Accesses.size(); ++K) {
    for (size_t L = K; L < A.Accesses.size(); ++L) {
      const ArrayAccess &X = A.Accesses[K], &Y = A.Accesses[L];
      if (!X.Base || !Y.Base || !(writes(X) || writes(Y)))
        continue;
      // A pointer this analysis couldn't pin down (an unresolved local, a
      // reassigned or escaped parameter, a global) may point into any other
      // array: different bases are not known to be disjoint.
      if (X.Base != Y.Base) {
        if (X.BaseMayAlias || Y.BaseMayAlias) {
          const ArrayAccess &P = X.BaseMayAlias ? X : Y;
          Note(DepOutcome::Unknown,
               "'" + P.Base->getNameAsString() +
                   "' is a pointer that may point into '" +
                   (X.BaseMayAlias ? Y : X).Base->getNameAsString() +
                   "' (" + describe(X) + ", " + describe(Y) + ")");
          continue;
        }
        if (Alias) {
          const AliasProof P = (*Alias)(X.Base, Y.Base);
          if (P != AliasProof::Distinct &&
              llvm::none_of(D.AliasPairs, [&](const AliasPair &Q) {
                return (Q.A == X.Base && Q.B == Y.Base) ||
                       (Q.A == Y.Base && Q.B == X.Base);
              }))
            D.AliasPairs.push_back({X.Base, Y.Base, P});
        }
        continue;
      }
      const auto *BaseVD = llvm::dyn_cast<VarDecl>(X.Base);
      if (BaseVD && A.DeclaredInLoop.count(BaseVD->getCanonicalDecl())) {
        ++D.PrivatePairsSkipped;
        continue;
      }
      ++D.PairsTested;
      const std::string Pair =
          K == L ? describe(X) : describe(X) + " and " + describe(Y);
      if (!X.Affine || !Y.Affine) {
        const ArrayAccess &Bad = X.Affine ? Y : X;
        Note(DepOutcome::Unknown, "cannot test " + Pair + ": " +
                                      describe(Bad) + " is non-affine (" +
                                      Bad.NonAffineReason + ")");
        continue;
      }
      if (X.Subscripts.size() != Y.Subscripts.size() ||
          X.Delinearized != Y.Delinearized || X.RowLengths != Y.RowLengths) {
        Note(DepOutcome::Unknown,
             "cannot test " + Pair + ": they index '" +
                 X.Base->getNameAsString() + "' with different shapes");
        continue;
      }

      Proof Proved = Proof::None;
      bool LT = false, GT = false, AllExact = true;
      std::optional<int64_t> Dist;
      bool DistancesDisagree = false;
      for (size_t Dim = 0; Dim < X.Subscripts.size(); ++Dim) {
        DimResult R = testDim(makeEq(X.Subscripts[Dim], Y.Subscripts[Dim], A),
                              A, Step, FactsFor(X, Y));
        if (R.Indep != Proof::None) {
          Proved = R.Indep;
          break;
        }
        LT |= R.IndepLT;
        GT |= R.IndepGT;
        if (R.Distance) {
          if (Dist && *Dist != *R.Distance)
            DistancesDisagree = true;
          Dist = R.Distance;
        } else if (!R.Free) {
          AllExact = false;
        }
      }
      if (Proved == Proof::None && LT && GT)
        Proved = Proof::Banerjee;
      // Two dimensions demanding different distances can't both hold.
      if (Proved == Proof::None && AllExact && DistancesDisagree)
        Proved = Proof::SIV;

      switch (Proved) {
      case Proof::ZIV:
        ++D.IndependentByZIV;
        continue;
      case Proof::GCD:
        ++D.IndependentByGCD;
        continue;
      case Proof::SIV:
        ++D.IndependentBySIV;
        continue;
      case Proof::Banerjee:
        ++D.IndependentByBanerjee;
        continue;
      case Proof::None:
        break;
      }

      // Field-insensitive: s[i].x and s[i + 1].y meet on an element, not
      // necessarily on a field. Don't claim a proven dependence for them.
      if (AllExact && (X.ViaMember || Y.ViaMember)) {
        Note(DepOutcome::Unknown,
             "could not prove " + Pair +
                 " independent (they reach the same element; fields are not "
                 "distinguished)");
      } else if (AllExact && !Dist) {
        Note(DepOutcome::Dependent,
             K == L ? Pair + ": every iteration touches the same element"
                    : Pair + ": every iteration touches the same elements");
      } else if (AllExact) {
        const int64_t Iters = std::llabs(*Dist) / Step;
        Note(DepOutcome::Dependent,
             Pair + ": the same element is touched " + std::to_string(Iters) +
                 " iteration(s) apart");
      } else {
        Note(DepOutcome::Unknown, "could not prove " + Pair + " independent");
      }
    }
  }
  return D;
}

std::string dependenceSummary(const LoopDependence &D) {
  if (D.SingleIteration)
    return "at most one iteration: nothing can be carried";
  const unsigned Indep = D.IndependentByZIV + D.IndependentByGCD +
                         D.IndependentBySIV + D.IndependentByBanerjee;
  std::string S = std::to_string(D.PairsTested) + " pair(s), " +
                  std::to_string(Indep) + " independent";
  std::string By;
  auto Add = [&By](unsigned N, const char *Name) {
    if (N)
      By += (By.empty() ? "" : ", ") + std::to_string(N) + " " + Name;
  };
  Add(D.IndependentByZIV, "ZIV");
  Add(D.IndependentByGCD, "GCD");
  Add(D.IndependentBySIV, "SIV");
  Add(D.IndependentByBanerjee, "Banerjee");
  if (!By.empty())
    S += " (" + By + ")";
  if (D.PrivatePairsSkipped)
    S += "; " + std::to_string(D.PrivatePairsSkipped) +
         " on loop-private arrays skipped";
  return S;
}

} // namespace p05
