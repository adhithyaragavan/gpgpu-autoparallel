#include "analysis/AffineAccess.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseSet.h"

#include <utility>

using namespace clang;

namespace p05 {

int64_t AffineExpr::coeff(const VarDecl *IV) const {
  auto It = IVTerms.find(IV);
  return It == IVTerms.end() ? 0 : It->second;
}

std::string AffineExpr::str() const {
  std::string S;
  auto Term = [&S](int64_t C, llvm::StringRef Name) {
    if (C == 0)
      return;
    const bool Neg = C < 0;
    const uint64_t Mag = Neg ? 0 - static_cast<uint64_t>(C) : C;
    if (S.empty())
      S += Neg ? "-" : "";
    else
      S += Neg ? " - " : " + ";
    if (Name.empty()) {
      S += std::to_string(Mag);
      return;
    }
    if (Mag != 1)
      S += std::to_string(Mag) + "*";
    S += Name.str();
  };
  for (const auto &[IV, C] : IVTerms)
    Term(C, IV->getName());
  for (const auto &[Sym, C] : SymTerms)
    Term(C, Sym->getName());
  Term(Const, "");
  return S.empty() ? "0" : S;
}

llvm::StringRef toString(AccessKind K) {
  switch (K) {
  case AccessKind::Read:
    return "R";
  case AccessKind::Write:
    return "W";
  case AccessKind::ReadWrite:
    return "RW";
  case AccessKind::AddressTaken:
    return "&";
  }
  return "?";
}

unsigned LoopAccesses::numAffine() const {
  unsigned N = 0;
  for (const ArrayAccess &A : Accesses)
    N += A.Affine;
  return N;
}

namespace {

using Int = __int128;

/// Single-line text of an expression: source text when it is on one line,
/// otherwise Clang's pretty-printer (same rule as LoopAnalysis's
/// getSourceText, for the same reason: reports and CHECK lines are per line).
std::string exprText(const Expr *E, ASTContext &Ctx) {
  llvm::StringRef Text = Lexer::getSourceText(
      CharSourceRange::getTokenRange(E->getSourceRange()),
      Ctx.getSourceManager(), Ctx.getLangOpts());
  if (!Text.empty() && !Text.contains('\n') && !Text.contains('\r'))
    return Text.str();
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  E->printPretty(OS, nullptr, PrintingPolicy(Ctx.getLangOpts()));
  return OS.str();
}

const VarDecl *canon(const VarDecl *VD) {
  return VD ? VD->getCanonicalDecl() : nullptr;
}

/// The element type a pointer or array variable steps over, canonical and
/// unqualified; null for anything else.
QualType elementType(QualType T) {
  T = T.getCanonicalType();
  if (const auto *PT = T->getAs<PointerType>())
    return PT->getPointeeType().getCanonicalType().getUnqualifiedType();
  if (const auto *AT = T->getAsArrayTypeUnsafe())
    return AT->getElementType().getCanonicalType().getUnqualifiedType();
  return QualType();
}

const VarDecl *asVar(const Expr *E) {
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
    return canon(dyn_cast<VarDecl>(DRE->getDecl()));
  return nullptr;
}

std::string quoted(const VarDecl *VD) {
  return "'" + VD->getNameAsString() + "'";
}

/// Facts about everything that runs on every iteration of one loop: its body
/// and its condition and increment clauses (not its init, which runs once).
/// Gathered once, before any subscript is examined.
struct BodyFacts {
  /// Every direct write to a variable by name (`x = `, `x += `, `x++`),
  /// keyed by variable, with the statement doing it. Includes the header
  /// updates of loops nested inside: whether such a write is legitimate
  /// depends on where the variable is used (see isCleanIV), so nothing is
  /// filtered out here.
  llvm::DenseMap<const VarDecl *, llvm::SmallVector<const Stmt *, 2>>
      WriteSites;
  /// Variables whose address is taken (`&x`).
  llvm::DenseSet<const VarDecl *> AddressTaken;
  /// Types written through a pointer, array element or member (`*p = `).
  std::vector<QualType> IndirectWriteTypes;
  bool HasCall = false;
  /// Locals declared here, with their initializer (null if none).
  llvm::DenseMap<const VarDecl *, const VarDecl *> DeclaredHere;
};

class BodyFactsVisitor : public RecursiveASTVisitor<BodyFactsVisitor> {
public:
  explicit BodyFactsVisitor(BodyFacts &F) : F(F) {}

  bool VisitDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(D))
        F.DeclaredHere[canon(VD)] = VD;
    return true;
  }
  // Also called for compound assignments (`+=`): RAV walks a
  // CompoundAssignOperator up through VisitBinaryOperator.
  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      write(BO->getLHS(), BO);
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      write(UO->getSubExpr(), UO);
    else if (UO->getOpcode() == UO_AddrOf)
      if (const VarDecl *VD = asVar(UO->getSubExpr()))
        F.AddressTaken.insert(VD);
    return true;
  }
  bool VisitCallExpr(CallExpr *) {
    F.HasCall = true;
    return true;
  }
  // Inline asm writes its output operands, and may write memory or anything
  // else (clobbers): record the outputs and treat it like a call.
  bool VisitAsmStmt(AsmStmt *AS) {
    F.HasCall = true;
    for (unsigned I = 0, E = AS->getNumOutputs(); I != E; ++I)
      write(AS->getOutputExpr(I), AS);
    return true;
  }

private:
  void write(const Expr *LHS, const Stmt *Site) {
    if (const VarDecl *VD = asVar(LHS))
      F.WriteSites[VD].push_back(Site);
    else
      F.IndirectWriteTypes.push_back(LHS->getType());
  }
  BodyFacts &F;
};

BodyFacts factsOf(const ForStmt *FS) {
  BodyFacts F;
  BodyFactsVisitor V(F);
  V.TraverseStmt(const_cast<Expr *>(FS->getCond()));
  V.TraverseStmt(const_cast<Expr *>(FS->getInc()));
  V.TraverseStmt(const_cast<Stmt *>(FS->getBody()));
  return F;
}

/// Variables whose address is taken anywhere in a function: such a variable
/// can be written through a pointer without its name appearing.
llvm::DenseSet<const VarDecl *> addressTakenIn(const FunctionDecl *FD) {
  struct V : RecursiveASTVisitor<V> {
    llvm::DenseSet<const VarDecl *> Out;
    bool VisitUnaryOperator(UnaryOperator *UO) {
      if (UO->getOpcode() == UO_AddrOf)
        if (const VarDecl *VD = asVar(UO->getSubExpr()))
          Out.insert(VD);
      return true;
    }
  } Vis;
  if (FD && FD->getBody())
    Vis.TraverseStmt(FD->getBody());
  return std::move(Vis.Out);
}

/// Could a write through a pointer to an object of type Written modify a
/// variable of type Var? Assumes C's strict-aliasing (effective type) rule:
/// character types alias anything; otherwise the types must match up to
/// qualifiers, with signed/unsigned integer variants allowed. Code built with
/// -fno-strict-aliasing breaks this assumption (see NOTES.md).
bool mayAlias(QualType Written, QualType Var, ASTContext &Ctx) {
  Written = Written.getCanonicalType().getUnqualifiedType();
  Var = Var.getCanonicalType().getUnqualifiedType();
  if (Written->isCharType() || Written->isVoidType())
    return true;
  if (Written == Var)
    return true;
  if (Written->isIntegerType() && Var->isIntegerType())
    return Ctx.getCorrespondingUnsignedType(Written) ==
           Ctx.getCorrespondingUnsignedType(Var);
  return false;
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

/// The statements through which a recognized loop legitimately updates its
/// own induction variable: an assignment-form init, and the increment.
llvm::SmallVector<const Stmt *, 2> headerUpdates(const LoopInfo &L) {
  llvm::SmallVector<const Stmt *, 2> Out;
  if (const auto *Init = dyn_cast_or_null<BinaryOperator>(L.Loop->getInit()))
    Out.push_back(Init);
  if (const Expr *Inc = L.Loop->getInc())
    Out.push_back(Inc->IgnoreParenImpCasts());
  return Out;
}

/// An induction variable in scope at an access, with the loop that binds it
/// there: the innermost enclosing recognized loop whose variable it is.
struct IVBinding {
  const VarDecl *Var;
  const LoopInfo *Loop;
};

/// Does S contain a statement that leaves or skips part of a loop around it
/// (break, continue, goto, return)? Conservative: one belonging to a nested
/// loop or switch counts too.
bool hasJump(const Stmt *S) {
  if (!S)
    return false;
  if (isa<BreakStmt>(S) || isa<ContinueStmt>(S) || isa<GotoStmt>(S) ||
      isa<IndirectGotoStmt>(S) || isa<ReturnStmt>(S))
    return true;
  for (const Stmt *C : S->children())
    if (hasJump(C))
      return true;
  return false;
}

/// Everything the affine builder needs about the loop being analyzed.
struct Context {
  ASTContext &Ctx;
  const ForStmt *Root;
  BodyFacts Facts;
  llvm::DenseSet<const VarDecl *> FunctionAddressTaken;
  /// Facts over the whole enclosing function: used to tell whether a local
  /// pointer set outside the loop is ever reassigned.
  BodyFacts FunctionFacts;
  /// Analyzing a whole function body (AffineAnalyzer::analyzeFunction): Root
  /// is null and Facts covers the function. An induction variable then only
  /// has to be clean within its own loop (ivFacts).
  bool FunctionMode = false;
  using LoopFactsMap = llvm::DenseMap<const ForStmt *, BodyFacts>;
  mutable LoopFactsMap LoopFacts = LoopFactsMap();

  /// "loop" or "function": what Facts covers, for reasons.
  const char *scope() const { return FunctionMode ? "function" : "loop"; }
};

/// The facts an induction variable bound by loop L must be clean in: the
/// analyzed loop's (which cover every loop inside it), or in function mode
/// L's own.
const BodyFacts &ivFacts(const LoopInfo &L, const Context &C) {
  if (!C.FunctionMode)
    return C.Facts;
  auto It = C.LoopFacts.find(L.Loop);
  if (It == C.LoopFacts.end())
    It = C.LoopFacts.try_emplace(L.Loop, factsOf(L.Loop)).first;
  return It->second;
}

/// Could something other than the listed Allowed statements write VD while
/// the code described by F runs? Direct writes (anywhere in F) other than
/// Allowed, an address taken in the loop, and (for a global or an
/// address-taken local) any call or aliasing indirect write all count.
bool otherWrites(const VarDecl *VD, const BodyFacts &F,
                 llvm::ArrayRef<const Stmt *> Allowed, const Context &C,
                 std::string &Why) {
  // Why gets a predicate ("is modified inside the loop"); callers prefix
  // the subject ("uses 'n', which ...", "induction variable 'i' ...").
  auto It = F.WriteSites.find(VD);
  if (It != F.WriteSites.end())
    for (const Stmt *S : It->second)
      if (!llvm::is_contained(Allowed, S)) {
        Why = std::string("is modified inside the ") + C.scope();
        return true;
      }
  if (F.AddressTaken.count(VD)) {
    Why = std::string("has its address taken inside the ") + C.scope();
    return true;
  }
  if (VD->getType().isVolatileQualified()) {
    Why = "is volatile";
    return true;
  }
  if (VD->hasGlobalStorage() || C.FunctionAddressTaken.count(VD)) {
    if (F.HasCall) {
      Why = std::string("could be modified by a call inside the ") +
            C.scope() + " (global or address-taken)";
      return true;
    }
    for (QualType T : F.IndirectWriteTypes)
      if (mayAlias(T, VD->getType(), C.Ctx)) {
        Why = std::string("could be modified by a write through a pointer "
                          "inside the ") +
              C.scope();
        return true;
      }
  }
  return false;
}

/// Is the binding's variable a well-behaved induction variable: changed only
/// by its own loop's header, and nothing else in F?
bool isCleanIV(const IVBinding &B, const BodyFacts &F, const Context &C,
               std::string &Why) {
  return !otherWrites(B.Var, F, headerUpdates(*B.Loop), C, Why);
}

/// Is VD (not an induction variable at this point) fixed while the code
/// described by F runs?
bool isInvariant(const VarDecl *VD, const BodyFacts &F, const Context &C,
                 std::string &Why) {
  if (F.DeclaredHere.count(VD)) {
    Why = std::string("is assigned inside the ") + C.scope();
    return false;
  }
  return !otherWrites(VD, F, {}, C, Why);
}

/// Does S mention VD (e.g. a self-referencing initializer `int k = k + 1;`)?
bool mentions(const Stmt *S, const VarDecl *VD) {
  if (!S)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
    if (canon(dyn_cast<VarDecl>(DRE->getDecl())) == VD)
      return true;
  for (const Stmt *Child : S->children())
    if (mentions(Child, VD))
      return true;
  return false;
}

/// Builds the affine form of one subscript expression.
class AffineBuilder {
public:
  using DeclIVMap =
      llvm::DenseMap<const VarDecl *, llvm::SmallVector<IVBinding, 4>>;

  /// IVs: the bindings in scope at the point being analyzed, outermost first.
  AffineBuilder(const Context &C, llvm::ArrayRef<IVBinding> IVs,
                const DeclIVMap &IVsAtDecl)
      : C(C), IVs(IVs), IVsAtDecl(IVsAtDecl) {}

  /// The innermost binding of VD in scope, if VD is an induction variable here.
  const IVBinding *binding(const VarDecl *VD) const {
    for (auto It = IVs.rbegin(); It != IVs.rend(); ++It)
      if (It->Var == VD)
        return &*It;
    return nullptr;
  }

  bool build(const Expr *E, AffineExpr &Out, std::string &Why,
             unsigned Depth = 0) const {
    if (Depth > 8) {
      Why = "subscript substitution is too deep";
      return false;
    }
    E = E->IgnoreParens();

    // Integer constants, including folded expressions like `N * 2`.
    if (!E->isValueDependent()) {
      Expr::EvalResult R;
      if (E->EvaluateAsInt(R, C.Ctx) && R.Val.isInt())
        if (std::optional<int64_t> V = R.Val.getInt().tryExtValue()) {
          Out = AffineExpr();
          Out.Const = *V;
          return true;
        }
    }

    if (const auto *CE = dyn_cast<CastExpr>(E)) {
      switch (CE->getCastKind()) {
      case CK_LValueToRValue:
      case CK_NoOp:
        return build(CE->getSubExpr(), Out, Why, Depth);
      case CK_IntegralCast:
        if (!valuePreserving(CE->getSubExpr()->getType(), CE->getType())) {
          Why = "subscript has a narrowing or sign-changing conversion ('" +
                exprText(CE, C.Ctx) + "'), which wraps around";
          return false;
        }
        return build(CE->getSubExpr(), Out, Why, Depth);
      default:
        Why = "subscript contains a non-integer conversion";
        return false;
      }
    }

    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      const auto *VD = canon(dyn_cast<VarDecl>(DRE->getDecl()));
      if (!VD) {
        Why = "subscript refers to a non-variable";
        return false;
      }
      if (const IVBinding *B = binding(VD)) {
        if (!isCleanIV(*B, ivFacts(*B->Loop, C), C, Why)) {
          Why = "induction variable " + quoted(VD) + " " + Why;
          return false;
        }
        Out = AffineExpr();
        Out.IVTerms[VD] = 1;
        return true;
      }
      // A non-static local declared in the loop with an initializer and
      // never written again: substitute its initializer, evaluated with the
      // induction variables that were in scope where it was declared (not
      // where it is used: an inner loop's variable may only be in scope at
      // the use).
      auto Decl = C.Facts.DeclaredHere.find(VD);
      if (Decl != C.Facts.DeclaredHere.end()) {
        const VarDecl *Def = Decl->second;
        auto At = IVsAtDecl.find(VD);
        std::string W;
        if (Def->hasLocalStorage() && Def->getInit() &&
            !mentions(Def->getInit(), VD) &&
            VD->getType()->isIntegerType() && At != IVsAtDecl.end() &&
            !otherWrites(VD, C.Facts, {}, C, W)) {
          AffineBuilder Sub(C, At->second, IVsAtDecl);
          if (Sub.build(Def->getInit(), Out, Why, Depth + 1))
            return true;
          Why = "uses " + quoted(VD) + " (= " + exprText(Def->getInit(), C.Ctx) +
                "): " + Why;
          return false;
        }
      }
      if (!VD->getType()->isIntegerType()) {
        Why = "uses " + quoted(VD) + ", which is not an integer variable";
        return false;
      }
      if (!isInvariant(VD, C.Facts, C, Why)) {
        Why = "uses " + quoted(VD) + ", which " + Why;
        return false;
      }
      Out = AffineExpr();
      Out.SymTerms[VD] = 1;
      return true;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      if (UO->getOpcode() == UO_Plus)
        return build(UO->getSubExpr(), Out, Why, Depth);
      if (UO->getOpcode() == UO_Minus) {
        if (wrapsUnsigned(UO->getType())) {
          Why = "subscript negates an unsigned value ('" + exprText(UO, C.Ctx) +
                "'), which wraps around";
          return false;
        }
        if (!build(UO->getSubExpr(), Out, Why, Depth))
          return false;
        return scale(Out, -1, Why);
      }
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      const BinaryOperatorKind Op = BO->getOpcode();
      if (Op == BO_Add || Op == BO_Sub || Op == BO_Mul) {
        AffineExpr L, R;
        if (!build(BO->getLHS(), L, Why, Depth) ||
            !build(BO->getRHS(), R, Why, Depth))
          return false;
        // In unsigned arithmetic narrower than 64 bits, a constant at or
        // above 2^(w-1) is a negative offset in disguise (`u + -1` folds to
        // `u + 4294967295`). Taking it at face value would put the access
        // ~4e9 elements away. (64-bit ones already fail tryExtValue.)
        if (wrapsUnsigned(BO->getType()))
          for (const AffineExpr *Op : {&L, &R})
            if (Op->isConstant() &&
                static_cast<uint64_t>(Op->Const) >=
                    (uint64_t(1) << (C.Ctx.getIntWidth(BO->getType()) - 1))) {
              Why = "subscript adds an unsigned constant that wraps around "
                    "('" + exprText(BO, C.Ctx) + "')";
              return false;
            }
        if (Op == BO_Mul) {
          if (L.isConstant()) {
            Out = std::move(R);
            return scale(Out, L.Const, Why);
          }
          if (R.isConstant()) {
            Out = std::move(L);
            return scale(Out, R.Const, Why);
          }
          Why = "subscript multiplies two non-constant terms ('" +
                exprText(BO, C.Ctx) + "')";
          return false;
        }
        if (Op == BO_Sub && !scale(R, -1, Why))
          return false;
        Out = std::move(L);
        return add(Out, R, Why);
      }
      Why = "subscript uses operator '" +
            BinaryOperator::getOpcodeStr(Op).str() + "'";
      return false;
    }

    if (isa<ArraySubscriptExpr>(E) || isa<MemberExpr>(E) ||
        (isa<UnaryOperator>(E) &&
         cast<UnaryOperator>(E)->getOpcode() == UO_Deref)) {
      Why = "subscript reads memory ('" + exprText(E, C.Ctx) + "')";
      return false;
    }
    if (isa<CallExpr>(E)) {
      Why = "subscript contains a call ('" + exprText(E, C.Ctx) + "')";
      return false;
    }
    Why = "subscript is not an affine expression ('" + exprText(E, C.Ctx) +
          "')";
    return false;
  }

  static bool scale(AffineExpr &A, int64_t K, std::string &Why) {
    auto Mul = [&](int64_t &V) {
      if (__builtin_mul_overflow(V, K, &V)) {
        Why = "subscript coefficient overflows";
        return false;
      }
      return true;
    };
    for (auto &[IV, Cf] : A.IVTerms)
      if (!Mul(Cf))
        return false;
    for (auto &[S, Cf] : A.SymTerms)
      if (!Mul(Cf))
        return false;
    if (!Mul(A.Const))
      return false;
    prune(A);
    return true;
  }

  static bool add(AffineExpr &A, const AffineExpr &B, std::string &Why) {
    auto Add = [&](int64_t &V, int64_t W) {
      if (__builtin_add_overflow(V, W, &V)) {
        Why = "subscript coefficient overflows";
        return false;
      }
      return true;
    };
    for (const auto &[IV, Cf] : B.IVTerms)
      if (!Add(A.IVTerms[IV], Cf))
        return false;
    for (const auto &[S, Cf] : B.SymTerms)
      if (!Add(A.SymTerms[S], Cf))
        return false;
    if (!Add(A.Const, B.Const))
      return false;
    prune(A);
    return true;
  }

private:
  /// Unsigned and narrower than 64 bits: arithmetic in it wraps modulo 2^w,
  /// which the int64 coefficients here cannot represent.
  bool wrapsUnsigned(QualType T) const {
    return T->isUnsignedIntegerOrEnumerationType() &&
           C.Ctx.getIntWidth(T) < 64;
  }

  /// Does converting From to To preserve every value? Widening keeps the
  /// value unless it turns a signed type into an unsigned one (negative
  /// values wrap); same width needs the same signedness.
  bool valuePreserving(QualType From, QualType To) const {
    if (!From->isIntegerType() || !To->isIntegerType())
      return false;
    const unsigned WF = C.Ctx.getIntWidth(From), WT = C.Ctx.getIntWidth(To);
    const bool SF = From->isSignedIntegerOrEnumerationType();
    const bool ST = To->isSignedIntegerOrEnumerationType();
    if (WT > WF)
      return !(SF && !ST);
    return WT == WF && SF == ST;
  }

  static void prune(AffineExpr &A) {
    A.IVTerms.remove_if([](const auto &P) { return P.second == 0; });
    A.SymTerms.remove_if([](const auto &P) { return P.second == 0; });
  }

  const Context &C;
  llvm::ArrayRef<IVBinding> IVs;
  const DeclIVMap &IVsAtDecl;
};

} // namespace

namespace {
/// Split a pointer-valued expression into a named base variable plus signed
/// integer offsets: `p`, `p + e`, `e + p`, `p - e`, nested (`a + n - i` is
/// `(a + n) - i`), and `&P[e]`. Base must be a pointer or a 1-D array (an
/// array of arrays decays to a pointer to rows, which is not split).
/// Strip parentheses and only those implicit casts that keep a pointer's
/// element type and value: no-op, lvalue-to-rvalue, array-to-pointer decay.
/// Never a pointer bitcast (`void *v = a; char *c = v;`), which changes the
/// element size and so the meaning of every offset.
const Expr *peelPointerCasts(const Expr *E) {
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

bool splitPointer(const Expr *E, const VarDecl *&Base,
                  llvm::SmallVectorImpl<std::pair<const Expr *, int>> &Offs,
                  int Sign = 1) {
  E = peelPointerCasts(E);
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = canon(dyn_cast<VarDecl>(DRE->getDecl()));
    if (!VD)
      return false;
    QualType T = VD->getType();
    // GNU void* (and function-pointer) arithmetic steps in bytes.
    QualType Elt = elementType(T);
    if (Elt.isNull() || Elt->isVoidType() || Elt->isFunctionType())
      return false;
    const bool Ok =
        (T->isPointerType() && !T->getPointeeType()->isArrayType()) ||
        (T->isArrayType() &&
         !T->getAsArrayTypeUnsafe()->getElementType()->isArrayType());
    if (!Ok)
      return false;
    Base = VD;
    return true;
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) {
      const bool L = BO->getLHS()->getType()->isPointerType();
      const bool R = BO->getRHS()->getType()->isPointerType();
      if (L && !R) {
        Offs.push_back({BO->getRHS(), BO->getOpcode() == BO_Sub ? -Sign : Sign});
        return splitPointer(BO->getLHS(), Base, Offs, Sign);
      }
      if (R && !L && BO->getOpcode() == BO_Add) {
        Offs.push_back({BO->getLHS(), Sign});
        return splitPointer(BO->getRHS(), Base, Offs, Sign);
      }
    }
    return false;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf && Sign == 1)
    if (const auto *ASE =
            dyn_cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParens())) {
      Offs.push_back({ASE->getIdx(), 1});
      return splitPointer(ASE->getBase(), Base, Offs, 1);
    }
  return false;
}
} // namespace

/// Walks one loop's body (and its condition/increment), recording every
/// access.
class AccessCollector : public RecursiveASTVisitor<AccessCollector> {
public:
  /// RootIndex for a whole function body: there is no analyzed loop.
  static constexpr size_t NoRoot = static_cast<size_t>(-1);

  AccessCollector(AffineAnalyzer &A, const Context &C, size_t RootIndex,
                  LoopAccesses &Out)
      : A(A), C(C), RootIndex(RootIndex), Out(Out) {
    if (RootIndex == NoRoot)
      return;
    // Bindings of the analyzed loop and its enclosing recognized loops,
    // outermost first.
    llvm::SmallVector<IVBinding, 8> Chain;
    for (int P = static_cast<int>(RootIndex); P >= 0; P = A.Loops[P].Parent) {
      const LoopInfo &L = A.Loops[P];
      if (L.Kind != LoopKind::Unrecognized && L.InductionVar)
        Chain.insert(Chain.begin(), IVBinding{canon(L.InductionVar), &L});
    }
    IVs.assign(Chain.begin(), Chain.end());
    ChainLen = Chain.size();
    const LoopInfo &Root = A.Loops[RootIndex];
    if (Root.Kind != LoopKind::Unrecognized && Root.InductionVar) {
      Out.RootIV = canon(Root.InductionVar);
      for (size_t K = 0; K + 1 < Chain.size(); ++K)
        Out.OuterIVs.push_back(Chain[K].Var);
      // The root's bounds are evaluated with only the enclosing loops in scope.
      recordRange(Root, llvm::ArrayRef<IVBinding>(Chain).drop_back());
    }

    // If the analyzed loop's own variable, or an enclosing loop's, can change
    // inside this loop other than through its own header, its iteration space
    // is not what the header says, and no subscript of it can be trusted.
    for (size_t K = 0; K < Chain.size(); ++K) {
      const IVBinding &B = Chain[K];
      // An enclosing loop that reuses the analyzed loop's variable
      // (`int i; for (i...) for (i...)`) is shadowed: inside the analyzed
      // loop that variable is the analyzed loop's own, which the next binding
      // checks. (The enclosing loop itself gets poisoned when it is analyzed.)
      bool Shadowed = false;
      for (size_t L = K + 1; L < Chain.size(); ++L)
        Shadowed |= Chain[L].Var == B.Var;
      if (Shadowed)
        continue;
      std::string Why;
      if (!isCleanIV(B, C.Facts, C, Why)) {
        Poison = "induction variable " + quoted(B.Var) + " " + Why +
                 ", so the loop's iteration space is unknown";
        Out.Poisoned = true;
        break;
      }
    }
  }

  // Inner loops: their induction variable is in scope in their cond, inc and
  // body (not their init). Recognized loops only: an unrecognized loop's
  // variable is not an induction variable, and its increment writes it, so
  // isInvariant already rejects it.
  bool TraverseForStmt(ForStmt *FS, DataRecursionQueue * = nullptr) {
    if (!WalkUpFromForStmt(FS) || !TraverseStmt(FS->getInit()))
      return false;
    bool Pushed = false;
    auto It = A.IndexOf.find(FS);
    if (It != A.IndexOf.end()) {
      const LoopInfo &LI = A.Loops[It->second];
      if (LI.Kind != LoopKind::Unrecognized && LI.InductionVar) {
        recordRange(LI, IVs); // bounds see only the loops around this one
        IVs.push_back(IVBinding{canon(LI.InductionVar), &LI});
        Pushed = true;
      }
    }
    const bool Ok = TraverseStmt(FS->getConditionVariableDeclStmt()) &&
                    TraverseStmt(FS->getCond()) &&
                    TraverseStmt(FS->getInc()) &&
                    TraverseStmt(FS->getBody());
    if (Pushed)
      IVs.pop_back();
    return Ok;
  }

  // sizeof/alignof do not evaluate their operand (unless its type is a VLA,
  // and even then no element is read), so nothing inside is an access.
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE,
                                        DataRecursionQueue * = nullptr) {
    // Except for a variably modified operand (VLA): then the size
    // expressions inside it are evaluated, so traverse as usual.
    const QualType T = UE->isArgumentType()
                           ? UE->getArgumentType()
                           : UE->getArgumentExpr()->getType();
    if (T->isVariablyModifiedType())
      return RecursiveASTVisitor<AccessCollector>::
          TraverseUnaryExprOrTypeTraitExpr(UE, nullptr);
    return true;
  }

  bool VisitDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(D))
        IVsAtDecl[canon(VD)].assign(IVs.begin(), IVs.end());
    return true;
  }

  // The kind of an access is decided by its parent (an assignment, ++/--,
  // or &), which RAV visits first (pre-order).
  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      markKind(BO->getLHS(), BO->isCompoundAssignmentOp()
                                 ? AccessKind::ReadWrite
                                 : AccessKind::Write);
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      markKind(UO->getSubExpr(), AccessKind::ReadWrite);
    else if (UO->getOpcode() == UO_AddrOf)
      markKind(UO->getSubExpr(), AccessKind::AddressTaken);
    else if (UO->getOpcode() == UO_Deref)
      recordPointerAccess(UO, UO->getSubExpr());
    return true;
  }
  bool VisitMemberExpr(MemberExpr *ME) {
    // `p->f` reads or writes the object p points to: an access to p[0].
    if (ME->isArrow())
      recordPointerAccess(ME, ME->getBase());
    return true;
  }
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (!Consumed.count(ASE))
      recordSubscriptChain(ASE);
    return true;
  }

  /// Phase 1.5: record each call with its arguments in affine terms, for
  /// Summaries.h to substitute into the callee's summary.
  bool VisitCallExpr(CallExpr *CE) {
    if (!RecordedCalls.insert(CE).second)
      return true;
    CallSite CS;
    CS.Call = CE;
    CS.Callee = CE->getDirectCallee();
    for (size_t K = ChainLen; K < IVs.size(); ++K)
      CS.InnerIVs.push_back(IVs[K].Var);
    for (const Expr *Arg : CE->arguments())
      CS.Args.push_back(callArg(Arg));
    Out.Calls.push_back(std::move(CS));
    return true;
  }

private:
  CallArg callArg(const Expr *Arg) {
    CallArg A;
    A.Text = exprText(Arg, C.Ctx);
    AffineBuilder B(C, IVs, IVsAtDecl);
    const QualType T = Arg->getType();
    if (T->isIntegerType()) {
      A.K = CallArg::Kind::Value;
      A.Affine = B.build(Arg, A.Offset, A.Why);
    } else if (T->isPointerType()) {
      A.K = CallArg::Kind::Pointer;
      pointerArg(Arg, B, A);
    } else {
      A.Why = "argument is neither an integer nor a pointer";
    }
    // Same rule as push(): in a loop whose iteration space is unknown no
    // affine form can be trusted (the base still can).
    if (A.Affine && !Poison.empty()) {
      A.Affine = false;
      A.Why = Poison;
      A.RowIndex.reset();
      A.RowLength = nullptr;
    }
    return A;
  }

  /// `P`, `P + e`, `&P[e]` (as splitPointer), `&P[X * N]` (a row start), or
  /// `&v` for a whole variable v that is neither an array nor a pointer (a
  /// pointer to its only element). Base is left null when the argument
  /// can't be named; Affine false (with Base kept) when only the offset
  /// can't be expressed.
  void pointerArg(const Expr *Arg, const AffineBuilder &B, CallArg &A) {
    const Expr *P = peelPointerCasts(Arg);
    if (const auto *UO = dyn_cast<UnaryOperator>(P);
        UO && UO->getOpcode() == UO_AddrOf)
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParens())) {
        const VarDecl *VD = canon(dyn_cast<VarDecl>(DRE->getDecl()));
        // `&q` of a pointer q would make "element 0 of q" (the pointer
        // itself) indistinguishable from what q points to.
        if (VD && !VD->getType()->isArrayType() &&
            !VD->getType()->isPointerType()) {
          A.Base = VD;
          A.Affine = true;
          return;
        }
        A.Why = "argument is the address of " + exprText(DRE, C.Ctx) +
                ", an array or pointer variable";
        return;
      }
    const VarDecl *VD = nullptr;
    llvm::SmallVector<std::pair<const Expr *, int>, 4> Parts;
    if (!splitPointer(P, VD, Parts)) {
      A.Why = "argument is not a named pointer or array plus an offset";
      return;
    }
    if (!resolveBase(VD, A.Offset, A.Base, A.Why, A.BaseMayAlias)) {
      A.Base = nullptr;
      return;
    }
    const Expr *RowPart = nullptr;
    std::string FirstWhy;
    for (const auto &[E, Sign] : Parts) {
      AffineExpr Part;
      std::string Why;
      if (!B.build(E, Part, Why)) {
        // At most one part may be a row start `X * N`, and only added.
        if (RowPart || Sign < 0) {
          A.Why = FirstWhy.empty() ? Why : FirstWhy;
          return;
        }
        RowPart = E;
        FirstWhy = Why;
        continue;
      }
      if ((Sign < 0 && !AffineBuilder::scale(Part, -1, A.Why)) ||
          !AffineBuilder::add(A.Offset, Part, A.Why))
        return;
    }
    if (RowPart && !rowStart(RowPart, B, A)) {
      A.Why = FirstWhy;
      return;
    }
    A.Affine = true;
  }

  /// `X * N` or `N * X` with X affine and N an integer variable that is fixed
  /// here (built as a symbol, so the usual invariance checks apply).
  bool rowStart(const Expr *E, const AffineBuilder &B, CallArg &A) {
    const auto *Mul = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
    if (!Mul || Mul->getOpcode() != BO_Mul)
      return false;
    for (int Order = 0; Order < 2; ++Order) {
      const Expr *NE = Order ? Mul->getLHS() : Mul->getRHS();
      const Expr *XE = Order ? Mul->getRHS() : Mul->getLHS();
      const VarDecl *N = asVar(NE);
      if (!N || B.binding(N) || !N->getType()->isIntegerType())
        continue;
      std::string Why;
      AffineExpr NA, X;
      if (!B.build(NE, NA, Why) || !NA.IVTerms.empty() || NA.Const != 0 ||
          NA.SymTerms.size() != 1 || NA.SymTerms.begin()->first != N ||
          NA.SymTerms.begin()->second != 1 || !B.build(XE, X, Why))
        continue;
      A.RowIndex = std::move(X);
      A.RowLength = N;
      return true;
    }
    return false;
  }

  /// Range of loop L's induction variable, from its start and bound, built
  /// with the induction variables in scope at L's header.
  void recordRange(const LoopInfo &L, llvm::ArrayRef<IVBinding> InScope) {
    const VarDecl *V = canon(L.InductionVar);
    if (Out.Ranges.count(V)) { // bound by two loops: no single range
      Out.Ranges[V] = IVRange();
      return;
    }
    AffineBuilder B(C, InScope, IVsAtDecl);
    std::string Why;
    std::optional<AffineExpr> Start, Bound;
    AffineExpr E;
    if (L.StartExpr && B.build(L.StartExpr, E, Why))
      Start = E;
    if (L.BoundExpr && B.build(L.BoundExpr, E, Why))
      Bound = E;
    auto Shift = [&](std::optional<AffineExpr> X, int64_t K) {
      if (X && __builtin_add_overflow(X->Const, K, &X->Const))
        X.reset();
      return X;
    };
    IVRange R;
    if (L.Step && *L.Step > 0) {
      R.Lo = Start;
      if (L.CmpOp == BO_LT)
        R.Hi = Shift(Bound, -1);
      else if (L.CmpOp == BO_LE)
        R.Hi = Bound;
    } else if (L.Step && *L.Step < 0) {
      R.Hi = Start;
      if (L.CmpOp == BO_GT)
        R.Lo = Shift(Bound, 1);
      else if (L.CmpOp == BO_GE)
        R.Lo = Bound;
    }
    R.Exact = R.Lo && R.Hi && L.Step && (*L.Step == 1 || *L.Step == -1) &&
              !hasJump(L.Loop->getBody());
    Out.Ranges[V] = R;
  }

  /// Record that the access E (and any element it is a member of) has kind
  /// K. Writing `s[i].x` writes element s[i]; writing `s[i].v[k]` or
  /// `p->v[k]` writes the object holding the array member v.
  void markKind(const Expr *E, AccessKind K) {
    E = E->IgnoreParens();
    KindOf[E] = K;
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (!ME->isArrow())
        markKind(ME->getBase(), K);
      return;
    }
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      const Expr *B = ASE->getBase()->IgnoreParens();
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(B);
          ICE && ICE->getCastKind() == CK_ArrayToPointerDecay)
        if (const auto *Member =
                dyn_cast<MemberExpr>(ICE->getSubExpr()->IgnoreParens()))
          markKind(Member, K);
    }
  }

  AccessKind kindOf(const Expr *E) const {
    auto It = KindOf.find(E);
    return It == KindOf.end() ? AccessKind::Read : It->second;
  }

  ArrayAccess start(const Expr *E) {
    ArrayAccess Acc;
    Acc.E = E;
    Acc.Text = exprText(E, C.Ctx);
    Acc.Kind = kindOf(E);
    for (size_t K = ChainLen; K < IVs.size(); ++K)
      Acc.InnerIVs.push_back(IVs[K].Var);
    // `s[i].x`: is the element only reached through a `.` member?
    DynTypedNodeList P = C.Ctx.getParents(*E);
    while (!P.empty()) {
      if (const auto *PE = P[0].get<Expr>()) {
        if (isa<ParenExpr>(PE) || isa<ImplicitCastExpr>(PE)) {
          P = C.Ctx.getParents(*PE);
          continue;
        }
        if (const auto *ME = dyn_cast<MemberExpr>(PE))
          Acc.ViaMember = !ME->isArrow();
      }
      break;
    }
    return Acc;
  }

  void push(ArrayAccess Acc) {
    // The same expression can be reached twice (a VLA size inside sizeof is
    // traversed through both the expression and its type); record it once.
    if (!Recorded.insert(Acc.E).second)
      return;
    if (!Poison.empty() && Acc.Affine) {
      Acc.Affine = false;
      Acc.NonAffineReason = Poison;
      Acc.Subscripts.clear();
      Acc.DimExtents.clear();
      Acc.Delinearized = false;
    }
    Out.Accesses.push_back(std::move(Acc));
  }

  void nonAffine(ArrayAccess &Acc, std::string Why, bool KeepBase = true) {
    Acc.Affine = false;
    if (!KeepBase)
      Acc.Base = nullptr;
    Acc.NonAffineReason = std::move(Why);
    Acc.Subscripts.clear();
    Acc.DimExtents.clear();
    Acc.Delinearized = false;
    push(std::move(Acc));
  }

  /// Resolve a pointer/array variable used as an access base to a variable
  /// that is fixed for the whole loop, plus an affine offset. An array is
  /// always fixed. A pointer must not change inside the loop, or be a local
  /// declared in the loop as `q = P`, `q = P + e`, `q = P - e` or
  /// `q = &P[e]` and never written again, in which case q[k] is P[e + k].
  bool resolveBase(const VarDecl *VD, AffineExpr &Offset, const VarDecl *&Base,
                   std::string &Why, bool &MayAlias, unsigned Depth = 0) {
    if (Depth > 8) {
      Why = "base pointer substitution is too deep";
      return false;
    }
    if (!VD->getType()->isPointerType()) {
      Base = VD; // a named array cannot be reassigned
      return true;
    }
    auto Decl = C.Facts.DeclaredHere.find(VD);
    if (Decl == C.Facts.DeclaredHere.end()) {
      if (otherWrites(VD, C.Facts, {}, C, Why)) {
        Why = "base pointer " + quoted(VD) + " " + Why;
        return false;
      }
      // A pointer parameter keeps the documented no-alias assumption only
      // while it still holds the caller's value: one the function reassigns
      // or lets escape (`b = a;`, `&b`) may point anywhere. A global pointer
      // can be set anywhere in the program, so it may always alias.
      if (const auto *PVD = dyn_cast<ParmVarDecl>(VD)) {
        if (C.FunctionFacts.WriteSites.count(VD) ||
            C.FunctionAddressTaken.count(VD))
          MayAlias = true;
        Base = VD;
        return true;
      }
      if (VD->hasGlobalStorage()) {
        MayAlias = true;
        Base = VD;
        return true;
      }
      // A local pointer set outside the loop (`double *q = a + 1;`) can point
      // into another named array. If it is initialized and never written
      // again anywhere in the function, and the offset is a constant, it is
      // replaced by what it points into; otherwise it may alias anything.
      if (VD->hasLocalStorage()) {
        const VarDecl *P = nullptr;
        llvm::SmallVector<std::pair<const Expr *, int>, 4> Parts;
        AffineExpr ConstOff;
        bool Resolved =
            VD->getInit() && !C.FunctionFacts.WriteSites.count(VD) &&
            !C.FunctionAddressTaken.count(VD) &&
            splitPointer(VD->getInit(), P, Parts) &&
            elementType(VD->getType()) == elementType(P->getType());
        for (const auto &[E, Sign] : Parts) {
          Expr::EvalResult R;
          std::optional<int64_t> K;
          if (Resolved && !E->isValueDependent() && E->EvaluateAsInt(R, C.Ctx) &&
              R.Val.isInt())
            K = R.Val.getInt().tryExtValue();
          if (!K || __builtin_add_overflow(ConstOff.Const, Sign * *K,
                                           &ConstOff.Const))
            Resolved = false;
        }
        if (Resolved) {
          if (!resolveBase(P, Offset, Base, Why, MayAlias, Depth + 1))
            return false;
          return AffineBuilder::add(Offset, ConstOff, Why);
        }
        MayAlias = true;
      }
      Base = VD;
      return true;
    }
    const VarDecl *Def = Decl->second;
    std::string W;
    if (!Def->hasLocalStorage() || !Def->getInit() ||
        otherWrites(VD, C.Facts, {}, C, W)) {
      Why = "base pointer " + quoted(VD) + " is set inside the " + C.scope();
      return false;
    }
    // Pointer to rows (`double (*r)[4] = ...`) is not substituted.
    if (VD->getType()->getPointeeType()->isArrayType()) {
      Why = "base pointer " + quoted(VD) + " is set inside the " + C.scope();
      return false;
    }
    const VarDecl *P = nullptr;
    llvm::SmallVector<std::pair<const Expr *, int>, 4> Parts;
    // q[k] means P[e + k] only if q and P step over the same element type.
    if (!splitPointer(Def->getInit(), P, Parts) ||
        elementType(VD->getType()) != elementType(P->getType())) {
      Why = "base pointer " + quoted(VD) + " is set inside the " + C.scope();
      return false;
    }
    AffineExpr Off;
    if (!Parts.empty()) {
      auto At = IVsAtDecl.find(VD);
      if (At == IVsAtDecl.end()) {
        Why = "base pointer " + quoted(VD) + " is set inside the " + C.scope();
        return false;
      }
      AffineBuilder B(C, At->second, IVsAtDecl);
      for (const auto &[E, Sign] : Parts) {
        AffineExpr Part;
        if (!B.build(E, Part, Why) ||
            (Sign < 0 && !AffineBuilder::scale(Part, -1, Why)) ||
            !AffineBuilder::add(Off, Part, Why)) {
          Why = "base pointer " + quoted(VD) + " (= " +
                exprText(Def->getInit(), C.Ctx) + "): " + Why;
          return false;
        }
      }
    }
    if (!resolveBase(P, Offset, Base, Why, MayAlias, Depth + 1))
      return false;
    return AffineBuilder::add(Offset, Off, Why);
  }

  /// `a[i]`, `m[i][j]`, `a[i * n + j]`.
  void recordSubscriptChain(const ArraySubscriptExpr *Outer) {
    ArrayAccess Acc = start(Outer);
    // A subscript that still has array type (`m[i]` of `double m[N][M]`) is
    // not an element access: it decays to a pointer to a row.
    if (Outer->getType()->isArrayType())
      Acc.Kind = AccessKind::AddressTaken;

    // Subscripts outermost-dimension first, walking down through
    // array-to-pointer decays of inner subscripts (true multi-dimensional
    // arrays). Stop at anything else.
    llvm::SmallVector<const Expr *, 4> Idx;
    const Expr *BaseE = nullptr;
    const ArraySubscriptExpr *Cur = Outer;
    while (true) {
      Idx.insert(Idx.begin(), Cur->getIdx());
      const Expr *B = Cur->getBase()->IgnoreParens();
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(B);
          ICE && ICE->getCastKind() == CK_ArrayToPointerDecay) {
        const Expr *Inner = ICE->getSubExpr()->IgnoreParens();
        if (const auto *InnerASE = dyn_cast<ArraySubscriptExpr>(Inner)) {
          Consumed.insert(InnerASE);
          Cur = InnerASE;
          continue;
        }
        BaseE = Inner;
        break;
      }
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(B);
          ICE && ICE->getCastKind() == CK_LValueToRValue) {
        const Expr *Inner = ICE->getSubExpr()->IgnoreParens();
        if (isa<ArraySubscriptExpr>(Inner) || isa<MemberExpr>(Inner) ||
            (isa<UnaryOperator>(Inner) &&
             cast<UnaryOperator>(Inner)->getOpcode() == UO_Deref)) {
          // `double **a; a[i][j]`, `s->p[i]`: the base is a pointer loaded
          // from memory. The load is recorded as its own (read) access.
          nonAffine(Acc,
                    "base is a pointer loaded from memory ('" +
                        exprText(Inner, C.Ctx) + "')",
                    /*KeepBase=*/false);
          return;
        }
        BaseE = Inner;
        break;
      }
      BaseE = B;
      break;
    }

    const VarDecl *BaseVD = asVar(BaseE);
    if (!BaseVD) {
      nonAffine(Acc,
                isa<MemberExpr>(BaseE->IgnoreParenImpCasts())
                    ? "base is a struct member ('" + exprText(BaseE, C.Ctx) +
                          "')"
                    : "base is not a named array or pointer ('" +
                          exprText(BaseE, C.Ctx) + "')",
                /*KeepBase=*/false);
      return;
    }

    AffineExpr BaseOffset;
    const VarDecl *Base = nullptr;
    std::string Why;
    if (Idx.size() == 1) {
      if (!resolveBase(BaseVD, BaseOffset, Base, Why, Acc.BaseMayAlias)) {
        nonAffine(Acc, Why, /*KeepBase=*/false);
        return;
      }
    } else if (BaseVD->getType()->isPointerType() &&
               (C.Facts.DeclaredHere.count(BaseVD) ||
                otherWrites(BaseVD, C.Facts, {}, C, Why))) {
      // Multi-dimensional through a pointer to rows: no substitution.
      nonAffine(Acc,
                C.Facts.DeclaredHere.count(BaseVD)
                    ? "base pointer " + quoted(BaseVD) + " is set inside the " + C.scope()
                    : "base pointer " + quoted(BaseVD) + " " + Why,
                /*KeepBase=*/false);
      return;
    } else {
      Base = BaseVD;
      // A pointer to rows that isn't an unmodified parameter may point
      // anywhere (local, global, or a reassigned/escaped parameter).
      Acc.BaseMayAlias =
          BaseVD->getType()->isPointerType() &&
          (!isa<ParmVarDecl>(BaseVD) ||
           C.FunctionFacts.WriteSites.count(BaseVD) ||
           C.FunctionAddressTaken.count(BaseVD));
    }
    Acc.Base = Base;
    Acc.BaseSubstituted = Base != BaseVD;
    Acc.DimExtents = dimExtents(BaseVD->getType(), Idx.size());

    AffineBuilder B(C, IVs, IVsAtDecl);
    for (size_t D = 0; D < Idx.size(); ++D) {
      AffineExpr AE;
      if (B.build(Idx[D], AE, Why)) {
        if (D == 0 && !AffineBuilder::add(AE, BaseOffset, Why)) {
          nonAffine(Acc, Why);
          return;
        }
        Acc.Subscripts.push_back(std::move(AE));
        continue;
      }
      // A 1-D subscript that failed as a whole may be a linearized
      // multi-dimensional index (only without a substituted base offset,
      // which would shift the row structure).
      if (Idx.size() == 1 && BaseOffset.isConstant() && BaseOffset.Const == 0) {
        std::vector<AffineExpr> Subs;
        std::vector<std::string> Dims;
        std::vector<const VarDecl *> Ns;
        if (delinearize(Idx[0], B, Subs, Dims, Ns)) {
          Acc.Subscripts = std::move(Subs);
          Acc.RowLengths = std::move(Ns);
          // The outermost extent of a delinearized access is the row count,
          // which the declared 1-D size does not give.
          Dims.insert(Dims.begin(), std::string());
          Acc.DimExtents = std::move(Dims);
          Acc.Delinearized = true;
          break;
        }
      }
      nonAffine(Acc, Why);
      return;
    }
    Acc.Affine = true;
    push(std::move(Acc));
  }

  /// `*p`, `*(p + e)`, `*(e + p)`, `*(p - e)`, `p->f`, `(p + e)->f`.
  void recordPointerAccess(const Expr *Node, const Expr *PtrExpr) {
    ArrayAccess Acc = start(Node);
    const VarDecl *VD = nullptr;
    llvm::SmallVector<std::pair<const Expr *, int>, 4> Parts;
    if (!splitPointer(PtrExpr, VD, Parts)) {
      nonAffine(Acc,
                "dereferences something other than a named pointer plus an "
                "offset ('" +
                    exprText(Node, C.Ctx) + "')",
                /*KeepBase=*/false);
      return;
    }
    AffineExpr AE;
    const VarDecl *Base = nullptr;
    std::string Why;
    if (!resolveBase(VD, AE, Base, Why, Acc.BaseMayAlias)) {
      nonAffine(Acc, Why, /*KeepBase=*/false);
      return;
    }
    Acc.Base = Base;
    Acc.BaseSubstituted = Base != VD;
    Acc.DimExtents = {std::string()};
    AffineBuilder B(C, IVs, IVsAtDecl);
    for (const auto &[E, Sign] : Parts) {
      AffineExpr Part;
      if (!B.build(E, Part, Why) ||
          (Sign < 0 && !AffineBuilder::scale(Part, -1, Why)) ||
          !AffineBuilder::add(AE, Part, Why)) {
        nonAffine(Acc, Why);
        return;
      }
    }
    Acc.Subscripts.push_back(std::move(AE));
    Acc.Affine = true;
    push(std::move(Acc));
  }

  /// Extents of the first NumDims dimensions of an array/pointer type, as
  /// text; "" where unknown (always for a pointer's first dimension).
  std::vector<std::string> dimExtents(QualType T, size_t NumDims) {
    std::vector<std::string> Dims;
    for (size_t D = 0; D < NumDims; ++D) {
      T = T.getCanonicalType();
      if (const auto *CAT = C.Ctx.getAsConstantArrayType(T)) {
        Dims.push_back(std::to_string(CAT->getSize().getZExtValue()));
        T = CAT->getElementType();
      } else if (const auto *VAT = C.Ctx.getAsVariableArrayType(T)) {
        Dims.push_back(VAT->getSizeExpr() ? exprText(VAT->getSizeExpr(), C.Ctx)
                                          : std::string());
        T = VAT->getElementType();
      } else if (const auto *IAT = C.Ctx.getAsIncompleteArrayType(T)) {
        Dims.push_back(std::string());
        T = IAT->getElementType();
      } else if (const auto *PT = T->getAs<PointerType>()) {
        Dims.push_back(std::string());
        T = PT->getPointeeType();
      } else {
        Dims.push_back(std::string());
      }
    }
    return Dims;
  }

  /// `X * N + J` (any operand order), optionally plus or minus constants,
  /// where N is a fixed variable and J is `j + c` for a clean induction
  /// variable j whose whole range, shifted by c, stays inside [0, N - 1]
  /// (see rowIndexInRange). Then the 1-D index is the 2-D index [X][J] of
  /// rows of length N. Recursive in X, so `(i * n + j) * m + k` gives
  /// [i][j][k]; `i * n + j - 1` gives [i][j - 1]. Dims receives the extents
  /// of the inner dimensions only. Offset accumulates constants peeled off
  /// the outside (`(... ) - 1`).
  bool delinearize(const Expr *E, const AffineBuilder &B,
                   std::vector<AffineExpr> &Subs,
                   std::vector<std::string> &Dims,
                   std::vector<const VarDecl *> &Ns, int64_t Offset = 0) {
    E = E->IgnoreParenImpCasts();
    const auto *Add = dyn_cast<BinaryOperator>(E);
    if (!Add || (Add->getOpcode() != BO_Add && Add->getOpcode() != BO_Sub))
      return false;

    // Peel a constant term: `A + k`, `k + A`, `A - k`. Not in unsigned
    // arithmetic, where a "negative" constant is a huge one.
    if (!Add->getType()->isUnsignedIntegerOrEnumerationType()) {
      for (int Side = 0; Side < 2; ++Side) {
        if (Side == 0 && Add->getOpcode() == BO_Sub)
          continue; // `k - A` negates A: not a peelable offset
        const Expr *KE = Side ? Add->getRHS() : Add->getLHS();
        const Expr *Rest = Side ? Add->getLHS() : Add->getRHS();
        Expr::EvalResult R;
        if (KE->isValueDependent() || !KE->EvaluateAsInt(R, C.Ctx) ||
            !R.Val.isInt())
          continue;
        std::optional<int64_t> K = R.Val.getInt().tryExtValue();
        int64_t NewOff;
        if (!K || (Add->getOpcode() == BO_Sub
                       ? __builtin_sub_overflow(Offset, *K, &NewOff)
                       : __builtin_add_overflow(Offset, *K, &NewOff)))
          continue;
        return delinearize(Rest, B, Subs, Dims, Ns, NewOff);
      }
    }
    if (Add->getOpcode() != BO_Add)
      return false;

    for (int Order = 0; Order < 2; ++Order) {
      const Expr *MulE =
          (Order ? Add->getRHS() : Add->getLHS())->IgnoreParenImpCasts();
      const Expr *JE = Order ? Add->getLHS() : Add->getRHS();
      const auto *Mul = dyn_cast<BinaryOperator>(MulE);
      if (!Mul || Mul->getOpcode() != BO_Mul)
        continue;
      for (int MOrder = 0; MOrder < 2; ++MOrder) {
        const Expr *XE = MOrder ? Mul->getRHS() : Mul->getLHS();
        const VarDecl *N = asVar(MOrder ? Mul->getLHS() : Mul->getRHS());
        if (!N || B.binding(N))
          continue;
        // J must be exactly one induction variable plus a constant.
        std::string Why;
        AffineExpr JA;
        if (!B.build(JE, JA, Why) || !JA.SymTerms.empty() ||
            JA.IVTerms.size() != 1 || JA.IVTerms.begin()->second != 1)
          continue;
        const VarDecl *J = JA.IVTerms.begin()->first;
        int64_t C0;
        if (__builtin_add_overflow(JA.Const, Offset, &C0))
          continue;
        const IVBinding *JB = B.binding(J);
        if (!JB || !rowIndexInRange(*JB, N, C0))
          continue;
        std::vector<AffineExpr> Outer;
        std::vector<std::string> OuterDims;
        std::vector<const VarDecl *> OuterNs;
        AffineExpr X;
        if (B.build(XE, X, Why)) {
          Outer.push_back(std::move(X));
        } else if (!delinearize(XE, B, Outer, OuterDims, OuterNs)) {
          continue;
        }
        JA.Const = C0;
        Subs = std::move(Outer);
        Subs.push_back(std::move(JA));
        Dims = std::move(OuterDims);
        Dims.push_back(N->getNameAsString());
        Ns = std::move(OuterNs);
        Ns.push_back(N);
        return true;
      }
    }
    return false;
  }

  /// Does 0 <= j + Off <= N - 1 hold at every point j's loop (JB.Loop) runs?
  /// Needs: that loop counts up from a constant s to `j < N + e` (or
  /// `j <= N + e`), so j lies in [s, N + e - 1] (or [s, N + e]), with
  /// s + Off >= 0 and the top + Off <= N - 1; j changes only through that
  /// loop's header; N never changes while that loop runs (when j's loop
  /// encloses the analyzed loop, that covers more than the analyzed loop, so
  /// its own body is checked); and j and N have the same type (an unsigned j
  /// against a negative int N compares as unsigned; a narrower j can wrap).
  bool rowIndexInRange(const IVBinding &JB, const VarDecl *N, int64_t Off) {
    const LoopInfo &L = *JB.Loop;
    if (L.Kind == LoopKind::Unrecognized || !L.Start || !L.Step ||
        *L.Step <= 0 || (L.CmpOp != BO_LT && L.CmpOp != BO_LE) ||
        !L.BoundExpr)
      return false;
    if (JB.Var->getType().getCanonicalType().getUnqualifiedType() !=
        N->getType().getCanonicalType().getUnqualifiedType())
      return false;
    // The bound must be exactly N + e.
    AffineBuilder BB(C, {}, IVsAtDecl);
    AffineExpr Bound;
    std::string Why;
    if (!BB.build(L.BoundExpr, Bound, Why) || !Bound.IVTerms.empty() ||
        Bound.SymTerms.size() != 1 || Bound.SymTerms.begin()->first != N ||
        Bound.SymTerms.begin()->second != 1)
      return false;
    const Int Top = Int(Bound.Const) - (L.CmpOp == BO_LT ? 1 : 0); // j <= N + Top
    if (Int(*L.Start) + Off < 0 || Top + Off > -1)
      return false;
    const BodyFacts &F = factsFor(L);
    return isCleanIV(JB, F, C, Why) && isInvariant(N, F, C, Why);
  }

  /// Facts for loop L: the analyzed loop's own facts when L is the analyzed
  /// loop or inside it (they cover everything L's body can do), else facts
  /// of L itself (an enclosing loop), computed on demand.
  const BodyFacts &factsFor(const LoopInfo &L) {
    if (RootIndex == NoRoot)
      return ivFacts(L, C);
    for (int P = A.IndexOf.lookup(L.Loop); P >= 0; P = A.Loops[P].Parent)
      if (static_cast<size_t>(P) == RootIndex)
        return C.Facts;
    auto It = OuterFacts.find(L.Loop);
    if (It == OuterFacts.end())
      It = OuterFacts.try_emplace(L.Loop, factsOf(L.Loop)).first;
    return It->second;
  }

  AffineAnalyzer &A;
  const Context &C;
  size_t RootIndex;
  LoopAccesses &Out;
  /// Induction-variable bindings in scope at the traversal point, outermost
  /// first; the innermost binding of a variable wins.
  llvm::SmallVector<IVBinding, 8> IVs;
  AffineBuilder::DeclIVMap IVsAtDecl;
  llvm::DenseMap<const Expr *, AccessKind> KindOf;
  llvm::DenseSet<const ArraySubscriptExpr *> Consumed;
  llvm::DenseSet<const Expr *> Recorded;
  llvm::DenseSet<const CallExpr *> RecordedCalls;
  llvm::DenseMap<const ForStmt *, BodyFacts> OuterFacts;
  /// Non-empty when the analyzed loop's iteration space can't be trusted.
  std::string Poison;
  /// Number of bindings belonging to the analyzed loop and its enclosing
  /// loops; IVs beyond this index are inner loops'.
  size_t ChainLen = 0;
};

AffineAnalyzer::AffineAnalyzer(ASTContext &Ctx,
                               const std::vector<LoopInfo> &Loops)
    : Ctx(Ctx), Loops(Loops) {
  for (size_t I = 0; I < Loops.size(); ++I)
    IndexOf[Loops[I].Loop] = I;
}

LoopAccesses AffineAnalyzer::analyzeLoop(size_t Index) {
  const ForStmt *FS = Loops[Index].Loop;
  const FunctionDecl *FD = enclosingFunction(FS, Ctx);
  BodyFacts FnFacts;
  if (FD && FD->getBody()) {
    BodyFactsVisitor FV(FnFacts);
    FV.TraverseStmt(FD->getBody());
  }
  Context C{Ctx, FS, factsOf(FS), addressTakenIn(FD), std::move(FnFacts)};

  LoopAccesses Out;
  // Only automatic variables are fresh per iteration: a `static` or `extern`
  // declared in the body is one object shared by every iteration.
  for (const auto &[VD, Def] : C.Facts.DeclaredHere)
    if (Def->hasLocalStorage())
      Out.DeclaredInLoop.insert(VD);
  AccessCollector Coll(*this, C, Index, Out);
  Coll.TraverseStmt(const_cast<Expr *>(FS->getCond()));
  Coll.TraverseStmt(const_cast<Expr *>(FS->getInc()));
  Coll.TraverseStmt(const_cast<Stmt *>(FS->getBody()));
  return Out;
}

LoopAccesses AffineAnalyzer::analyzeFunction(const FunctionDecl *FD) {
  LoopAccesses Out;
  if (!FD || !FD->getBody())
    return Out;
  BodyFacts FnFacts;
  BodyFactsVisitor FV(FnFacts);
  FV.TraverseStmt(FD->getBody());
  Context C{Ctx, nullptr, FnFacts, addressTakenIn(FD), FnFacts};
  C.FunctionMode = true;
  for (const auto &[VD, Def] : C.Facts.DeclaredHere)
    if (Def->hasLocalStorage())
      Out.DeclaredInLoop.insert(VD);
  AccessCollector Coll(*this, C, AccessCollector::NoRoot, Out);
  Coll.TraverseStmt(FD->getBody());
  return Out;
}

void printAccessReport(llvm::raw_ostream &OS, const LoopAccesses &A) {
  OS << "  accesses   : " << A.Accesses.size() << " (" << A.numAffine()
     << " affine)\n";
  for (const ArrayAccess &Acc : A.Accesses) {
    OS << "    " << toString(Acc.Kind) << " " << Acc.Text << " : ";
    if (!Acc.Affine) {
      OS << "non-affine: " << Acc.NonAffineReason << "\n";
      continue;
    }
    for (const AffineExpr &S : Acc.Subscripts)
      OS << "[" << S.str() << "]";
    if (Acc.BaseSubstituted)
      OS << " of '" << Acc.Base->getName() << "'";
    if (Acc.Delinearized) {
      OS << " delinearized, dims ";
      for (size_t D = 0; D < Acc.DimExtents.size(); ++D)
        OS << "[" << (Acc.DimExtents[D].empty() ? "?" : Acc.DimExtents[D])
           << "]";
    }
    OS << "\n";
  }
}

} // namespace p05
