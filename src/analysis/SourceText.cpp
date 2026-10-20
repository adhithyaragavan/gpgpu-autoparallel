#include "analysis/SourceText.h"
#include "analysis/CrossTU.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/STLExtras.h"

#include <cctype>

using namespace clang;

namespace p05 {

const char *const IntPtrType = "__INTPTR_TYPE__";

std::string asIntPtr(const AffineExpr &E) {
  if (E.isConstant())
    return std::to_string(E.Const);
  // Each variable is cast before any arithmetic: `(T)(n - k)` would
  // subtract in n's type, which wraps for unsigned n < k.
  std::string S;
  unsigned Parts = 0;
  auto Term = [&](int64_t C, const ValueDecl *V) {
    if (C == 0)
      return;
    ++Parts;
    const bool Neg = C < 0;
    const uint64_t Mag = Neg ? 0 - static_cast<uint64_t>(C) : C;
    S += S.empty() ? (Neg ? "-" : "") : (Neg ? " - " : " + ");
    if (!V) {
      S += std::to_string(Mag);
      return;
    }
    if (Mag != 1)
      S += std::to_string(Mag) + " * ";
    S += "(" + std::string(IntPtrType) + ")" + V->getNameAsString();
  };
  for (const auto &[IV, C] : E.IVTerms)
    Term(C, IV);
  for (const auto &[Sym, C] : E.SymTerms)
    Term(C, Sym);
  Term(E.Const, nullptr);
  // One positive variable term is already a single operand.
  if (Parts == 1 && S[0] != '-' && S.find(" * ") == std::string::npos)
    return S;
  return "(" + S + ")";
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

NameScope::NameScope(ASTContext &Ctx, const FunctionDecl *F, SourceLocation At)
    : Ctx(Ctx), F(F), At(At) {
  const FunctionDecl *Def = F ? F->getDefinition() : nullptr;
  if (!Def)
    return;
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

bool NameScope::nameable(const ValueDecl *D, std::string &Why) const {
  auto Quoted = [](const ValueDecl *V) {
    return "'" + V->getNameAsString() + "'";
  };
  const auto *VD = dyn_cast_or_null<VarDecl>(D);
  VD = VD ? VD->getCanonicalDecl() : nullptr;
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
    const SourceManager &SM = Ctx.getSourceManager();
    // Phase 2.5: an imported declaration is spelled in another file; this
    // file may give its name to a different object (its own static).
    bool Before = false;
    for (const VarDecl *R : VD->redecls())
      Before |= !isForeign(R) &&
                SM.isBeforeInTranslationUnit(R->getLocation(), At);
    if (!Before && llvm::any_of(VD->redecls(), [](const VarDecl *R) {
          return isForeign(R);
        }) && llvm::none_of(VD->redecls(), [](const VarDecl *R) {
          return !isForeign(R);
        })) {
      Why = "global " + Quoted(D) + " is only declared in another file";
      return false;
    }
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
}

} // namespace p05
