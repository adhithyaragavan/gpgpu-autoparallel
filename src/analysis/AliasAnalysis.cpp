#include "analysis/AliasAnalysis.h"

#include "analysis/Profitability.h" // optionCategory()

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;

namespace p05 {

static llvm::cl::opt<AliasMode> FlagAlias(
    "alias", llvm::cl::desc("Pointers that may overlap"),
    llvm::cl::values(
        clEnumValN(AliasMode::Check, "check",
                   "prove disjoint (restrict, types, callers) or check at "
                   "run time (default, Phase 1.6)"),
        clEnumValN(AliasMode::Assume, "assume",
                   "pre-1.6: assume differently named pointers never "
                   "overlap (ablation baseline; unsound)")),
    llvm::cl::init(AliasMode::Check), llvm::cl::cat(optionCategory()));

static llvm::cl::opt<bool> FlagWholeProgram(
    "whole-program",
    llvm::cl::desc("Declare that no other translation unit calls this "
                   "file's functions, so the calls seen here are all of "
                   "them (used to prove pointer arguments disjoint)"),
    llvm::cl::init(false), llvm::cl::cat(optionCategory()));

AliasMode aliasModeFromFlags() { return FlagAlias; }
bool wholeProgramFromFlags() { return FlagWholeProgram; }

namespace {

constexpr unsigned MaxCallerDepth = 3;
const char *const IntPtr = "__INTPTR_TYPE__";

const VarDecl *canon(const ValueDecl *V) {
  const auto *VD = dyn_cast_or_null<VarDecl>(V);
  return VD ? VD->getCanonicalDecl() : nullptr;
}

std::optional<unsigned> paramIndex(const ValueDecl *V, const FunctionDecl *F) {
  const auto *PVD = dyn_cast_or_null<ParmVarDecl>(V);
  if (!PVD || !F)
    return std::nullopt;
  for (unsigned K = 0; K < F->getNumParams(); ++K)
    if (F->getParamDecl(K)->getCanonicalDecl() == PVD->getCanonicalDecl())
      return K;
  return std::nullopt;
}

/// The innermost element type of T (arrays stripped), canonical, without
/// qualifiers; enums as their integer type.
QualType innermost(QualType T, ASTContext &Ctx) {
  T = T.getCanonicalType();
  while (const ArrayType *AT = Ctx.getAsArrayType(T))
    T = AT->getElementType().getCanonicalType();
  T = T.getUnqualifiedType();
  if (const auto *ET = T->getAs<EnumType>())
    T = ET->getDecl()->getIntegerType().getCanonicalType();
  return T.getUnqualifiedType();
}

/// May an object of element type A be accessed through an lvalue of type B
/// (or the other way round)? The strict-aliasing rule AffineAccess assumes.
bool typesMayAlias(QualType A, QualType B, ASTContext &Ctx) {
  for (QualType T : {A, B})
    if (T.isNull() || T->isCharType() || T->isVoidType() ||
        T->isRecordType() || T->isIncompleteType())
      return true;
  if (A == B)
    return true;
  // Pointer types: compilers' TBAA treats `void *` (and differently
  // qualified pointees) as aliasing other pointers, whatever ISO C says.
  if (A->isPointerType() && B->isPointerType())
    return true;
  // Integers: same width counts as aliasing (`long` vs `long long`, or
  // int64_t typedef'd differently across headers, are mixed in real code
  // even though ISO C calls the overlap undefined).
  if (A->isIntegerType() && B->isIntegerType() && !A->isBooleanType() &&
      !B->isBooleanType())
    return Ctx.getTypeSize(A) == Ctx.getTypeSize(B);
  return false;
}

/// What an argument points into: an object's own storage (a named array,
/// or `&v`), or the pointer value held in a variable.
struct ArgObj {
  const VarDecl *VD = nullptr;
  bool Value = false;
};

const Expr *peel(const Expr *E) {
  while (true) {
    E = E->IgnoreParens();
    const auto *ICE = dyn_cast<ImplicitCastExpr>(E);
    if (!ICE || (ICE->getCastKind() != CK_NoOp &&
                 ICE->getCastKind() != CK_LValueToRValue &&
                 ICE->getCastKind() != CK_ArrayToPointerDecay))
      return E;
    E = ICE->getSubExpr();
  }
}

std::optional<ArgObj> argRoot(const Expr *E) {
  E = peel(E);
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = canon(DRE->getDecl());
    if (!VD)
      return std::nullopt;
    if (VD->getType()->isArrayType())
      return ArgObj{VD, false};
    if (VD->getType()->isPointerType())
      return ArgObj{VD, true};
    return std::nullopt;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf) {
    const Expr *Sub = UO->getSubExpr()->IgnoreParens();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub))
      if (const VarDecl *VD = canon(DRE->getDecl()))
        return ArgObj{VD, false};
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Sub))
      return argRoot(ASE->getBase());
    if (const auto *ME = dyn_cast<MemberExpr>(Sub)) {
      if (ME->isArrow())
        return argRoot(ME->getBase());
      const auto *Base = dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreParens());
      if (const VarDecl *VD = Base ? canon(Base->getDecl()) : nullptr)
        return ArgObj{VD, false};
    }
    return std::nullopt;
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(E);
      BO && (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub)) {
    if (BO->getLHS()->getType()->isPointerType())
      return argRoot(BO->getLHS());
    if (BO->getRHS()->getType()->isPointerType() && BO->getOpcode() == BO_Add)
      return argRoot(BO->getRHS());
  }
  return std::nullopt;
}

std::string asIntPtr(const AffineExpr &E) {
  if (E.isConstant())
    return std::to_string(E.Const);
  return "(" + std::string(IntPtr) + ")(" + E.str() + ")";
}

std::string scaled(const std::string &X, int64_t Size) {
  return Size == 1 ? X : std::to_string(Size) + " * " + X;
}

std::string fold(const std::vector<std::string> &Xs, bool Min) {
  std::string R = Xs.front();
  for (size_t K = 1; K < Xs.size(); ++K)
    R = "(" + R + (Min ? " < " : " > ") + Xs[K] + " ? " + R + " : " + Xs[K] +
        ")";
  return R;
}

bool isAllDigits(llvm::StringRef S) {
  return !S.empty() && llvm::all_of(S, [](char C) { return isdigit(C); });
}

} // namespace

AliasAnalyzer::AliasAnalyzer(ASTContext &Ctx, bool WholeProgram)
    : Ctx(Ctx), WholeProgram(WholeProgram) {}

const FunctionDecl *AliasAnalyzer::enclosingFunction(const Stmt *S) {
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

void AliasAnalyzer::indexCalls() {
  if (Indexed)
    return;
  Indexed = true;
  struct V : RecursiveASTVisitor<V> {
    llvm::DenseMap<const FunctionDecl *, std::vector<const CallExpr *>> Calls;
    llvm::DenseSet<const DeclRefExpr *> CalleeRefs;
    std::vector<const DeclRefExpr *> Refs;
    bool VisitCallExpr(CallExpr *CE) {
      if (const FunctionDecl *FD = CE->getDirectCallee())
        Calls[FD->getCanonicalDecl()].push_back(CE);
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(CE->getCallee()->IgnoreParenImpCasts()))
        CalleeRefs.insert(DRE);
      return true;
    }
    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      if (isa<FunctionDecl>(DRE->getDecl()))
        Refs.push_back(DRE);
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
        if (VD->hasGlobalStorage() && !Escaped->count(VD->getCanonicalDecl()) &&
            escapes(DRE))
          Escaped->insert(VD->getCanonicalDecl());
      return true;
    }
    ASTContext *Ctx;
    llvm::DenseSet<const VarDecl *> *Escaped;
    const Expr *parent(const Expr *E) {
      DynTypedNodeList P = Ctx->getParents(*E);
      while (!P.empty()) {
        const auto *PE = P[0].get<Expr>();
        if (!PE)
          return nullptr;
        if (!isa<ParenExpr>(PE))
          return PE;
        P = Ctx->getParents(*PE);
      }
      return nullptr;
    }
    /// Can this use of an object (or of part of it) yield its address?
    bool escapes(const Expr *E) {
      const Expr *P = parent(E);
      if (!P)
        return false; // e.g. an initializer's own declaration
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(P)) {
        if (ICE->getCastKind() == CK_LValueToRValue)
          return false;
        if (ICE->getCastKind() == CK_ArrayToPointerDecay) {
          const auto *ASE = dyn_cast_or_null<ArraySubscriptExpr>(parent(ICE));
          return !ASE || ASE->getBase()->IgnoreParens() != ICE || escapes(ASE);
        }
        return true;
      }
      if (const auto *ME = dyn_cast<MemberExpr>(P))
        return ME->isArrow() || escapes(ME);
      if (const auto *UO = dyn_cast<UnaryOperator>(P))
        return !UO->isIncrementDecrementOp();
      if (const auto *BO = dyn_cast<BinaryOperator>(P))
        return !(BO->isAssignmentOp() && BO->getLHS()->IgnoreParens() == E);
      if (isa<UnaryExprOrTypeTraitExpr>(P))
        return false;
      return true;
    }
  } Vis;
  Vis.Ctx = &Ctx;
  Vis.Escaped = &Escaped;
  Vis.TraverseDecl(Ctx.getTranslationUnitDecl());
  CallsTo = std::move(Vis.Calls);
  for (const DeclRefExpr *DRE : Vis.Refs)
    if (!Vis.CalleeRefs.count(DRE))
      AddressTaken.insert(
          cast<FunctionDecl>(DRE->getDecl())->getCanonicalDecl());
}

AliasAnalyzer::Role AliasAnalyzer::role(const FunctionDecl *F,
                                        const ValueDecl *V) {
  const VarDecl *VD = canon(V);
  if (!VD || !F)
    return Role::Other;
  if (isa<ParmVarDecl>(VD)) {
    const FunctionDecl *Def = F->getDefinition();
    if (!paramIndex(VD, Def))
      return Role::Other;
    if (!VD->getType()->isPointerType())
      return Role::Object; // this call's own copy
    // A parameter the function reassigns or lets escape no longer holds
    // the caller's value.
    auto [It, New] = ModifiedParams.try_emplace(Def->getCanonicalDecl());
    if (New) {
      struct W : RecursiveASTVisitor<W> {
        llvm::DenseSet<const VarDecl *> *Out;
        void mark(const Expr *E) {
          if (const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
            if (const VarDecl *VD = canon(DRE->getDecl()))
              Out->insert(VD);
        }
        bool VisitBinaryOperator(BinaryOperator *BO) {
          if (BO->isAssignmentOp())
            mark(BO->getLHS());
          return true;
        }
        bool VisitUnaryOperator(UnaryOperator *UO) {
          if (UO->isIncrementDecrementOp() || UO->getOpcode() == UO_AddrOf)
            mark(UO->getSubExpr());
          return true;
        }
      } Vis;
      Vis.Out = &It->second;
      if (Def->getBody())
        Vis.TraverseStmt(Def->getBody());
    }
    return It->second.count(VD) ? Role::Other : Role::PtrParam;
  }
  return Role::Object;
}

AliasProof AliasAnalyzer::proof(const FunctionDecl *F, const ValueDecl *A,
                                const ValueDecl *B) {
  return proofAt(F, A, B, 0);
}

AliasProof AliasAnalyzer::proofAt(const FunctionDecl *F, const ValueDecl *A,
                                  const ValueDecl *B, unsigned Depth) {
  const Role RA = role(F, A), RB = role(F, B);
  if (RA == Role::Other || RB == Role::Other)
    return AliasProof::Unproven;
  if (canon(A) == canon(B))
    return AliasProof::Unproven;
  if (RA == Role::Object && RB == Role::Object)
    return AliasProof::Distinct;
  // A pointer parameter vs an object: it can't point into this call's own
  // automatic storage (that did not exist when the caller computed it).
  const ValueDecl *Obj = RA == Role::Object ? A : RB == Role::Object ? B : nullptr;
  if (Obj && !canon(Obj)->hasGlobalStorage())
    return AliasProof::Distinct;
  // A const object can't be written, and writing it through the pointer
  // would be undefined: an overlap with the loop's write can't happen.
  if (Obj && canon(Obj)->getType().isConstQualified())
    return AliasProof::Distinct;
  if (Obj) {
    const VarDecl *O = canon(Obj);
    indexCalls();
    if (!Escaped.count(O) &&
        (O->isStaticLocal() || !O->isExternallyVisible()))
      return AliasProof::Distinct;
  }

  auto Elem = [&](const ValueDecl *V, Role R) {
    QualType T = canon(V)->getType();
    if (R == Role::PtrParam)
      T = T->getPointeeType();
    return innermost(T, Ctx);
  };
  if (!typesMayAlias(Elem(A, RA), Elem(B, RB), Ctx))
    return AliasProof::Types;
  for (auto [V, R] : {std::pair{A, RA}, std::pair{B, RB}})
    if (R == Role::PtrParam && canon(V)->getType().isRestrictQualified())
      return AliasProof::Restrict;
  if (callersProve(F, A, B, Depth))
    return AliasProof::Callers;
  return AliasProof::Unproven;
}

bool AliasAnalyzer::callersProve(const FunctionDecl *F, const ValueDecl *A,
                                 const ValueDecl *B, unsigned Depth) {
  if (Depth >= MaxCallerDepth)
    return false;
  const FunctionDecl *Def = F->getDefinition();
  if (!Def || Def->isMain() || !Def->hasPrototype())
    return false;
  if (Def->isExternallyVisible() && !WholeProgram)
    return false;
  indexCalls();
  if (AddressTaken.count(Def->getCanonicalDecl()))
    return false;
  auto ObjFor = [&](const CallExpr *CE,
                    const ValueDecl *V) -> std::optional<ArgObj> {
    std::optional<unsigned> K = paramIndex(V, Def);
    if (!K)
      return ArgObj{canon(V), false}; // a global: the same object here
    if (*K >= CE->getNumArgs())
      return std::nullopt;
    return argRoot(CE->getArg(*K));
  };
  auto It = CallsTo.find(Def->getCanonicalDecl());
  if (It == CallsTo.end())
    return true; // never called: the loop never runs
  for (const CallExpr *CE : It->second) {
    const QualType CT = CE->getCallee()->getType();
    if (CT->isPointerType() &&
        CT->getPointeeType()->getAs<FunctionNoProtoType>())
      return false;
    const FunctionDecl *C = enclosingFunction(CE);
    std::optional<ArgObj> OA = ObjFor(CE, A), OB = ObjFor(CE, B);
    if (!C || !OA || !OB || !OA->VD || !OB->VD)
      return false;
    if (!OA->Value && !OB->Value) {
      if (OA->VD == OB->VD)
        return false;
      continue;
    }
    // At least one is a pointer value: it must be one of C's own
    // (unmodified) pointer parameters, proven disjoint from the other there.
    for (const ArgObj *O : {&*OA, &*OB})
      if (O->Value && role(C, O->VD) != Role::PtrParam)
        return false;
    if (OA->VD == OB->VD)
      return false;
    if (proofAt(C, OA->VD, OB->VD, Depth + 1) == AliasProof::Unproven)
      return false;
  }
  return true;
}

std::optional<std::string>
AliasAnalyzer::runtimeCheck(const FunctionDecl *F, SourceLocation At,
                            const ValueDecl *A, const ValueDecl *B,
                            const LoopAccesses &Acc, std::string &Why) {
  struct Piece {
    int64_t Size;
    std::optional<AffineExpr> Lo, Hi; // element indices, 1-D accesses
    std::string LoT, HiT;             // element indices as text
  };
  auto Quoted = [](const ValueDecl *V) {
    return "'" + V->getNameAsString() + "'";
  };

  // Where the bytes the loop touches through V start and end, relative to
  // V's address.
  auto Range = [&](const ValueDecl *V, std::string &Lo,
                   std::string &End) -> bool {
    std::vector<Piece> Ps;
    for (const ArrayAccess &X : Acc.Accesses) {
      if (X.Base != V)
        continue;
      if (!X.Affine) {
        Why = "the range of " + Quoted(V) + " it touches is unknown: " +
              X.Text + " is non-affine";
        return false;
      }
      QualType T = canon(V)->getType();
      // A read of a global by name (SafetyAnalysis, no expression) touches
      // the variable itself, whatever its type.
      const size_t Depth =
          !X.E ? 0 : X.Delinearized ? 1 : X.Subscripts.size();
      for (size_t D = 0; D < Depth; ++D) {
        if (T->isPointerType())
          T = T->getPointeeType();
        else if (const ArrayType *AT = Ctx.getAsArrayType(T))
          T = AT->getElementType();
        else if (!(D == 0 && Depth == 1 && X.Subscripts[0].isConstant() &&
                   X.Subscripts[0].Const == 0)) {
          Why = "can't size " + X.Text;
          return false;
        }
      }
      if (T->isIncompleteType() || T->isVariablyModifiedType() ||
          T->isFunctionType()) {
        Why = "can't size the elements of " + Quoted(V);
        return false;
      }
      Piece P;
      P.Size = Ctx.getTypeSizeInChars(T).getQuantity();
      std::vector<AffineExpr> Los, His;
      for (const AffineExpr &S : X.Subscripts) {
        AffineExpr L, H;
        if (!subscriptBounds(S, Acc, L, H)) {
          Why = "the range of " + Quoted(V) + " it touches is unknown: a "
                "loop range in " + X.Text + " can't be bounded";
          return false;
        }
        Los.push_back(L);
        His.push_back(H);
      }
      if (Los.size() == 1) {
        P.Lo = Los[0];
        P.Hi = His[0];
        P.LoT = asIntPtr(Los[0]);
        P.HiT = asIntPtr(His[0]);
      } else {
        // Row-major linear index; every inner extent is a row length or a
        // constant, so it is monotonic in each subscript.
        std::string LoT = asIntPtr(Los[0]), HiT = asIntPtr(His[0]);
        for (size_t D = 1; D < Los.size(); ++D) {
          std::string E;
          if (X.Delinearized && D - 1 < X.RowLengths.size())
            E = "(" + std::string(IntPtr) + ")" +
                X.RowLengths[D - 1]->getNameAsString();
          else if (D < X.DimExtents.size() && isAllDigits(X.DimExtents[D]))
            E = X.DimExtents[D];
          else {
            Why = "can't linearize " + X.Text + " (unknown row length)";
            return false;
          }
          LoT = "(" + LoT + ") * " + E + " + " + asIntPtr(Los[D]);
          HiT = "(" + HiT + ") * " + E + " + " + asIntPtr(His[D]);
        }
        P.LoT = LoT;
        P.HiT = HiT;
      }
      Ps.push_back(std::move(P));
    }
    if (Ps.empty()) {
      Why = "no access to " + Quoted(V);
      return false;
    }
    // Byte offsets: lows S*lo, ends S*(hi + 1). Affine ones with the same
    // element size that differ by a constant fold to the smaller/larger.
    struct Cand {
      int64_t Size;
      std::optional<AffineExpr> E;
      std::string T;
    };
    auto Reduce = [&](std::vector<Cand> Cs, bool Min) {
      std::vector<Cand> Out;
      for (Cand &C : Cs) {
        bool Folded = false;
        for (Cand &O : Out) {
          if (C.Size != O.Size)
            continue;
          if (!C.E || !O.E) {
            if (!C.E && !O.E && C.T == O.T) {
              Folded = true;
              break;
            }
            continue;
          }
          AffineExpr D = *C.E;
          for (const auto &[V, K] : O.E->IVTerms)
            D.IVTerms[V] -= K;
          for (const auto &[V, K] : O.E->SymTerms)
            D.SymTerms[V] -= K;
          D.Const -= O.E->Const;
          D.IVTerms.remove_if([](const auto &P) { return P.second == 0; });
          D.SymTerms.remove_if([](const auto &P) { return P.second == 0; });
          if (!D.isConstant())
            continue;
          if (Min ? D.Const < 0 : D.Const > 0)
            O = C;
          Folded = true;
          break;
        }
        if (!Folded)
          Out.push_back(C);
      }
      return Out;
    };
    std::vector<Cand> LoC, HiC;
    for (const Piece &P : Ps) {
      LoC.push_back({P.Size, P.Lo, P.LoT});
      HiC.push_back({P.Size, P.Hi, P.HiT});
    }
    std::vector<std::string> Los, Ends;
    auto Bytes = [](const AffineExpr &E, int64_t Size) {
      return E.isConstant() ? std::to_string(E.Const * Size)
                            : scaled(asIntPtr(E), Size);
    };
    for (const Cand &C : Reduce(LoC, true))
      Los.push_back(C.E ? Bytes(*C.E, C.Size) : scaled("(" + C.T + ")", C.Size));
    for (const Cand &C : Reduce(HiC, false)) {
      if (C.E && C.E->isConstant())
        Ends.push_back(std::to_string((C.E->Const + 1) * C.Size));
      else
        Ends.push_back(scaled(
            "(" + (C.E ? asIntPtr(*C.E) : C.T) + " + 1)", C.Size));
    }
    Lo = fold(Los, true);
    End = fold(Ends, false);
    return true;
  };

  // Every name the check writes must mean, just before the loop, the
  // declaration it stands for: nothing else in F (a variable, enumerator,
  // typedef or function declared anywhere in it, in any block) may share
  // its name, and a global must already be declared at the loop.
  llvm::StringMap<std::vector<const NamedDecl *>> InF;
  if (const FunctionDecl *Def = F->getDefinition()) {
    for (const ParmVarDecl *P : Def->parameters())
      InF[P->getName()].push_back(P);
    struct N : RecursiveASTVisitor<N> {
      llvm::StringMap<std::vector<const NamedDecl *>> *Out;
      bool VisitNamedDecl(NamedDecl *D) {
        // Tags and labels live in their own namespaces.
        if (!isa<TagDecl>(D) && !isa<LabelDecl>(D) && D->getIdentifier())
          (*Out)[D->getName()].push_back(D);
        return true;
      }
    } Vis;
    Vis.Out = &InF;
    if (Def->getBody())
      Vis.TraverseStmt(Def->getBody());
  }
  const SourceManager &SM = Ctx.getSourceManager();
  auto Nameable = [&](const ValueDecl *D) -> bool {
    const auto *VD = canon(D);
    if (!VD || !VD->getIdentifier()) {
      Why = "the check would need a name for " + Quoted(D);
      return false;
    }
    auto It = InF.find(VD->getName());
    if (It != InF.end())
      for (const NamedDecl *Other : It->second)
        if (Other->getCanonicalDecl() != VD) {
          Why = Quoted(D) + " is shadowed in '" + F->getNameAsString() +
                "', so the check can't name it";
          return false;
        }
    if (VD->isFileVarDecl()) {
      bool Before = false;
      for (const VarDecl *R : VD->redecls())
        Before |= SM.isBeforeInTranslationUnit(R->getLocation(), At);
      if (!Before) {
        Why = "global " + Quoted(D) + " is declared after the loop";
        return false;
      }
      return true;
    }
    // Otherwise it must be F's own parameter or local (declared in F).
    if (It == InF.end()) {
      Why = Quoted(D) + " is not visible in '" + F->getNameAsString() + "'";
      return false;
    }
    return true;
  };
  // Every name in A's and B's accesses: subscripts' symbols and enclosing
  // loops' variables (what bounds leave behind), and row lengths.
  llvm::SmallPtrSet<const VarDecl *, 8> UsedIVs;
  for (const ArrayAccess &X : Acc.Accesses) {
    if (X.Base != A && X.Base != B)
      continue;
    for (const AffineExpr &S : X.Subscripts) {
      for (const auto &[V, C] : S.IVTerms)
        UsedIVs.insert(V);
      for (const auto &[V, C] : S.SymTerms)
        if (!Nameable(V))
          return std::nullopt;
      for (const auto &[V, C] : S.IVTerms)
        if (std::find(Acc.OuterIVs.begin(), Acc.OuterIVs.end(), V) !=
                Acc.OuterIVs.end() &&
            !Nameable(V))
          return std::nullopt;
    }
    for (const VarDecl *N : X.RowLengths)
      if (!Nameable(N))
        return std::nullopt;
  }
  // Symbols in the loop variables' ranges, which bounds substitute in.
  for (const auto &[V, R] : Acc.Ranges)
    for (const std::optional<AffineExpr> *E : {&R.Lo, &R.Hi})
      if (UsedIVs.count(V) && *E)
        for (const auto &[S, C] : (*E)->SymTerms)
          if (!Nameable(S))
            return std::nullopt;

  // The base's address, as an integer.
  auto Addr = [&](const ValueDecl *V, std::string &Out) -> bool {
    const VarDecl *VD = canon(V);
    if (!isa<ParmVarDecl>(VD) && (VD->isStaticLocal() || !VD->isFileVarDecl())) {
      Why = Quoted(V) + " is not a file-scope variable the check could name";
      return false;
    }
    if (!Nameable(V))
      return false;
    Out = "(" + std::string(IntPtr) + ")" +
          (isa<ParmVarDecl>(VD) || VD->getType()->isArrayType() ? "" : "&") +
          VD->getNameAsString();
    return true;
  };

  std::string BA, BB, LA, EA, LB, EB;
  if (!Addr(A, BA) || !Addr(B, BB) || !Range(A, LA, EA) || !Range(B, LB, EB))
    return std::nullopt;
  return "(" + BA + " + " + EA + " <= " + BB + " + " + LB + " || " + BB +
         " + " + EB + " <= " + BA + " + " + LA + ")";
}

} // namespace p05
