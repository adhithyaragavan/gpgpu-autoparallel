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
//   - Only the delinearization pattern `X * N + J` is recognized, with J a
//     clean induction variable of a loop running exactly 0..N-1 by 1 and N
//     fixed for as long as that loop runs.
//   - A pointer passed to a call (`f(a)`, `f(a + i)`) is not an access here;
//     what the callee does with it is Phase 1.5's call-site summaries.
//   - Operands of sizeof/alignof are not accesses (they are not evaluated).

#ifndef P05_ANALYSIS_AFFINEACCESS_H
#define P05_ANALYSIS_AFFINEACCESS_H

#include "analysis/LoopInfo.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
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
};

struct LoopAccesses {
  std::vector<ArrayAccess> Accesses;
  unsigned numAffine() const;
};

class AffineAnalyzer {
public:
  /// Loops is LoopCollector's flat vector; nest facts (NestIVs, Parent) are
  /// read from it.
  AffineAnalyzer(clang::ASTContext &Ctx, const std::vector<LoopInfo> &Loops);

  /// Every access in Loops[Index]'s body, including inside nested loops.
  LoopAccesses analyzeLoop(size_t Index);

private:
  friend class AccessCollector;

  clang::ASTContext &Ctx;
  const std::vector<LoopInfo> &Loops;
  llvm::DenseMap<const clang::ForStmt *, size_t> IndexOf;
};

void printAccessReport(llvm::raw_ostream &OS, const LoopAccesses &A);

} // namespace p05

#endif // P05_ANALYSIS_AFFINEACCESS_H
