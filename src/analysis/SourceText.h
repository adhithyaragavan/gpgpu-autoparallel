// SourceText — writing analysis facts back into the source as C expressions.
//
// Run-time alias checks (Phase 1.6) and map() sections and their guards
// (Phase 2.1) both print affine expressions just before a loop. Every name
// such an expression uses must mean, at that point, the declaration the
// analysis meant: NameScope checks that.

#ifndef P05_ANALYSIS_SOURCETEXT_H
#define P05_ANALYSIS_SOURCETEXT_H

#include "analysis/AffineAccess.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/StringMap.h"

#include <string>
#include <vector>

namespace p05 {

/// The integer type checks and sections are computed in: wide enough for
/// any address or index, and signed, so `hi - lo + 1` can't wrap.
extern const char *const IntPtrType;

/// E as text in IntPtrType: each variable is cast before any arithmetic, so
/// an unsigned `n - k` can't wrap. A constant is left as is.
std::string asIntPtr(const AffineExpr &E);
/// `Size * X`, or X when Size is 1.
std::string scaled(const std::string &X, int64_t Size);
/// Nested ?: picking the smallest (Min) or largest of Xs (non-empty).
std::string fold(const std::vector<std::string> &Xs, bool Min);
bool isAllDigits(llvm::StringRef S);

/// Which declarations can be written by name just before a loop at At in
/// function F: nothing else declared anywhere in F (a variable, enumerator,
/// typedef or function, in any block) may share the name, and a global must
/// already be declared at At.
class NameScope {
public:
  NameScope(clang::ASTContext &Ctx, const clang::FunctionDecl *F,
            clang::SourceLocation At);

  /// False, with Why, when D can't be written by name at At.
  bool nameable(const clang::ValueDecl *D, std::string &Why) const;

private:
  clang::ASTContext &Ctx;
  const clang::FunctionDecl *F;
  clang::SourceLocation At;
  llvm::StringMap<std::vector<const clang::NamedDecl *>> InF;
};

} // namespace p05

#endif // P05_ANALYSIS_SOURCETEXT_H
