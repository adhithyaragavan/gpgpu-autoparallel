// AffineAccess — Phase 1.2: every array/pointer access in a loop's body,
// with each subscript expressed as an affine function of the loop nest's
// induction variables, or marked non-affine with a reason.
//
// This is the input Phase 1.3's dependence tests consume. It replaces the
// hackathon rule "every subscript must be exactly the induction variable"
// (SafetyAnalysis.cpp's BodyDependenceVisitor) with a representation that can
// say *how* a subscript depends on each loop: `m[i][j]`, `a[i - 1]`,
// `a[i * n + j]`. On its own it makes no verdict: a non-affine subscript here
// means "the dependence tests cannot reason about this access", which 1.3
// turns into UNKNOWN, never SAFE.
//
// What counts as affine:
//   subscript = sum(c_k * IV_k) + sum(d_s * symbol_s) + const
// with integer c_k, d_s, const. IV_k are the induction variables of the
// recognized loops enclosing the access (LoopInfo::NestIVs). A symbol is any
// other integer variable that provably does not change while the analyzed
// loop runs (see isInvariantIn below). Products of an IV and a symbol (the
// `i * n` in a linearized 2D access) are only accepted through
// delinearization, which turns `a[i * n + j]` into the 2D access `a[i][j]`
// when the inner index provably stays in [0, n).
//
// The rule everything rests on: a variable is an induction variable at an
// access only if a recognized loop enclosing that access binds it AND nothing
// writes it except that loop's own header; a symbol only if nothing in the
// loop can write it at all (by name, through a pointer of an aliasing type, or
// through a call when it is global or address-taken). Every write is recorded
// with its exact statement, so "an inner loop's j++" is legitimate inside that
// inner loop and a write everywhere else. If the analyzed loop's own variable
// (or an enclosing loop's) is written inside it, every access is non-affine:
// the iteration space itself is unknown.
//
// Assumptions and limits (the first two can make a form wrong if violated;
// the rest only lose precision):
//   - Signed integer overflow in subscript arithmetic does not happen, and
//     unsigned arithmetic does not wrap past its range (same assumption as
//     LoopInfo's trip counts). NOT assumed away, because they are explicit:
//     narrowing or sign-changing casts, unsigned constants that encode a
//     negative offset (`u + -1`), and negating an unsigned value. Those make a
//     subscript non-affine.
//   - Pointer substitution never crosses a conversion that changes the
//     element type (a `void *` round-trip, GNU `void *` arithmetic).
//   - C's strict-aliasing rule holds: a write through `long *` cannot change
//     an `int` symbol. Code compiled with -fno-strict-aliasing violates this.
//   - A local is forward-substituted only if it is non-static, declared in the
//     analyzed loop with an initializer, and never written again or
//     address-taken; likewise a pointer declared in the loop as `P + e`,
//     `P - e` or `&P[e]` is replaced by P with the offset folded in.
//   - Only the delinearization pattern `X * N + j + c` is recognized (c
//     constant, possibly peeled from outside), with j a clean induction
//     variable whose whole range shifted by c provably stays in [0, N - 1]
//     and N fixed for as long as j's loop runs. Each delinearized access
//     records its row lengths; accesses with different ones are different
//     shapes.
//   - Subscripts stay inside the object's declared bounds (ISO C: `m[i][j+1]`
//     past the end of a row is undefined behaviour, and is treated as if it
//     can't reach the next row).
//   - A local pointer set outside the loop is replaced by what it points
//     into when it is initialized once with a constant offset; otherwise it is
//     marked BaseMayAlias. Parameters and globals keep the no-alias
//     assumption (DependenceTest.h).
//   - A pointer passed to a call (`f(a)`, `f(a + i)`) is not an access here;
//     each call's arguments are recorded in the same terms (CallSite), and
//     what the callee does with them is Phase 1.5's summaries (Summaries.h).
//   - Operands of sizeof/alignof are not accesses (they are not evaluated).

#ifndef P05_ANALYSIS_AFFINEACCESS_H
#define P05_ANALYSIS_AFFINEACCESS_H

#include "analysis/LoopInfo.h"
#include "analysis/Rejection.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p05 {

/// sum(IVTerms) + sum(SymTerms) + Const. Terms with a zero coefficient are
/// never stored. MapVector keeps insertion order, so printing is stable.
struct AffineExpr {
  llvm::MapVector<const clang::VarDecl *, int64_t> IVTerms;
  llvm::MapVector<const clang::ValueDecl *, int64_t> SymTerms;
  int64_t Const = 0;

  /// Coefficient of IV, 0 if absent.
  int64_t coeff(const clang::VarDecl *IV) const;
  bool isConstant() const { return IVTerms.empty() && SymTerms.empty(); }
  std::string str() const;
};

enum class AccessKind {
  Read,
  Write,
  ReadWrite,    ///< compound assignment or ++/--
  AddressTaken, ///< `&a[i]`: what happens through it is the callee's business
};

llvm::StringRef toString(AccessKind K);

/// One access in the loop body, e.g. `m[i][j] = ...`.
struct ArrayAccess {
  const clang::Expr *E = nullptr;
  std::string Text;
  AccessKind Kind = AccessKind::Read;

  /// The array or pointer variable accessed. Null when the base is opaque
  /// (a struct member, a pointer loaded from memory, a call result), in which
  /// case Affine is false.
  const clang::ValueDecl *Base = nullptr;

  /// True when every subscript is affine.
  bool Affine = false;

  /// Why not, when Affine is false.
  std::string NonAffineReason;

  /// One entry per dimension, outermost first. 1 entry for `p[i]` / `*(p+i)`.
  std::vector<AffineExpr> Subscripts;

  /// Extent of each dimension as source text, aligned with Subscripts; empty
  /// when unknown (always the case for the outermost dimension of a pointer).
  std::vector<std::string> DimExtents;

  /// Set when Subscripts came from delinearizing a 1-D linearized subscript.
  bool Delinearized = false;

  /// Set when the pointer written in the source was a local declared in the
  /// loop (`double *q = a + k; q[i]`) and was replaced by the pointer it is
  /// computed from: Base is then `a`, and the subscript includes the offset.
  bool BaseSubstituted = false;

  /// Induction variables of the inner loops (strictly inside the analyzed
  /// loop) that enclose this access, outermost first. A fact about one of
  /// those loops ("it runs") may only be used for this access.
  std::vector<const clang::VarDecl *> InnerIVs;

  /// For a delinearized access: the row-length variable N of each inner
  /// dimension. Two delinearized accesses have the same shape only if these
  /// match exactly.
  std::vector<const clang::VarDecl *> RowLengths;

  /// Part of a `.field` access (`s[i].x`): the analysis is field-insensitive,
  /// so two such accesses to one element may still touch different fields.
  bool ViaMember = false;

  /// Base is a local pointer set outside the loop to something this analysis
  /// could not resolve: it may point into any other array.
  bool BaseMayAlias = false;

  /// Phase 2.5: a subscript is non-affine because it loads a value, and
  /// whether that value can change between iterations (`a[idx[i]]`, or
  /// `k = idx[i]; a[k]`: an indirect subscript) or not (`a[i * m->stride]`).
  /// None when not known: no load, or an access composed from a callee.
  enum class SubscriptLoad { None, Invariant, Varying };
  SubscriptLoad Load = SubscriptLoad::None;

  /// Phase 2.1: for an access composed from a callee's summary (E is then
  /// the call in the analyzed code), the expressions it came from, deepest
  /// first: the access in the deepest callee, then each call on the way back
  /// up. Empty for the analyzed code's own accesses.
  std::vector<const clang::Expr *> Within;
};

/// Inclusive range of an induction variable, as affine bounds. Each bound may
/// mention symbols and enclosing loops' induction variables (fixed while the
/// analyzed loop runs); a bound that mentions the analyzed loop's or another
/// inner loop's variable (a triangular nest) is reported as given and is the
/// dependence test's job to treat as unknown. Missing = unbounded on that
/// side (could not be expressed, or `!=` loops).
struct IVRange {
  std::optional<AffineExpr> Lo, Hi;
  /// Phase 2.1: the variable takes every integer in [Lo, Hi] on each run of
  /// its loop (step +1 or -1, and nothing in the loop's body can leave it or
  /// skip ahead: no break, continue, goto or return), so both ends are
  /// actually reached whenever Lo <= Hi.
  bool Exact = false;
};

/// One argument of a call, in the same affine terms as the accesses around
/// it (Phase 1.5): what Summaries.h substitutes for the callee's parameter.
struct CallArg {
  enum class Kind {
    Value,   ///< integer-typed: Offset is its value when Affine
    Pointer, ///< pointer-typed: Base plus Offset (elements) when Affine
    Other,   ///< anything else (floating point, struct by value, ...)
  };
  Kind K = Kind::Other;
  std::string Text;
  bool Affine = false;
  /// Why not, when Affine is false.
  std::string Why;
  /// Pointer: the object it points into (null when it can't be named), and
  /// whether that is a pointer that may point anywhere (as for accesses).
  const clang::VarDecl *Base = nullptr;
  bool BaseMayAlias = false;
  AffineExpr Offset;
  /// Pointer to the start of a row, `&P[X * N]` / `P + X * N`: RowIndex is X
  /// and RowLength N (a variable fixed in the loop); Offset holds any other
  /// (affine) part of the offset.
  std::optional<AffineExpr> RowIndex;
  const clang::VarDecl *RowLength = nullptr;
};

/// A direct call inside the analyzed code, with its arguments.
struct CallSite {
  const clang::CallExpr *Call = nullptr;
  /// Null for a call through a function pointer.
  const clang::FunctionDecl *Callee = nullptr;
  std::vector<CallArg> Args;
  /// Inner induction variables enclosing the call (as ArrayAccess::InnerIVs).
  std::vector<const clang::VarDecl *> InnerIVs;
};

struct LoopAccesses {
  std::vector<ArrayAccess> Accesses;
  /// Every call in the analyzed code, in source order.
  std::vector<CallSite> Calls;

  /// The analyzed loop's induction variable (canonical); null when the loop
  /// is unrecognized.
  const clang::VarDecl *RootIV = nullptr;
  /// Induction variables of enclosing recognized loops, outermost first: they
  /// hold one value for the whole run of the analyzed loop.
  std::vector<const clang::VarDecl *> OuterIVs;
  /// Ranges of RootIV and of every inner recognized loop's variable. A
  /// variable bound by two different loops inside gets an empty range.
  llvm::DenseMap<const clang::VarDecl *, IVRange> Ranges;
  /// Variables declared inside the analyzed loop (body or header clauses):
  /// an array among them is private to each iteration.
  llvm::DenseSet<const clang::VarDecl *> DeclaredInLoop;
  /// The analyzed loop's iteration space can't be trusted (its variable, or
  /// an enclosing one, is written inside it): every access is non-affine.
  bool Poisoned = false;

  unsigned numAffine() const;
};

class AffineAnalyzer {
public:
  /// Loops is LoopCollector's flat vector; nest facts (NestIVs, Parent) are
  /// read from it.
  AffineAnalyzer(clang::ASTContext &Ctx, const std::vector<LoopInfo> &Loops);

  /// Every access in Loops[Index]'s body, including inside nested loops.
  LoopAccesses analyzeLoop(size_t Index);

  /// Every access in a whole function body (Phase 1.5's callee summaries).
  /// There is no analyzed loop: RootIV is null and every recognized loop in
  /// the body is an inner loop with a range. A symbol is a variable fixed for
  /// the whole call (in practice an unmodified parameter); an induction
  /// variable only has to be clean within its own loop.
  LoopAccesses analyzeFunction(const clang::FunctionDecl *FD);

private:
  friend class AccessCollector;

  clang::ASTContext &Ctx;
  const std::vector<LoopInfo> &Loops;
  llvm::DenseMap<const clang::ForStmt *, size_t> IndexOf;
};

void printAccessReport(llvm::raw_ostream &OS, const LoopAccesses &A);

/// Phase 2.5: the rejection category of a non-affine access (Rejection.h),
/// from the reason this file wrote for it: an unnamed base, a subscript that
/// reads a value that changes with the loop (also through a substituted
/// local, `k = idx[i]`; for an access composed from a callee, any load), a
/// modified induction variable, or any other non-affine subscript.
RejectCategory nonAffineCategory(const ArrayAccess &X);

} // namespace p05

#endif // P05_ANALYSIS_AFFINEACCESS_H
