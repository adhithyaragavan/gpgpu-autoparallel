// ScalarAnalysis — Phase 1.4: what to do with each scalar a loop writes.
//
// The hackathon rule was "a scalar declared outside the loop body and
// written in it is a loop-carried dependence". That rejects the two most
// common shapes in real C: a temporary or an inner loop's counter declared at
// the top of the function (`int i, j; double t;` — PolyBench style), and a
// reduction (`sum += a[i]`). This pass classifies every scalar the loop
// writes by name, using liveness computed over Clang's CFG of the enclosing
// function:
//
//   private(x)       x is dead at the start of the body (every iteration
//                    writes it before reading it) and dead after the loop.
//   lastprivate(x)   as private, but x is read after the loop; then the body
//                    must write x unconditionally (a top-level `x = ...` or a
//                    top-level `for (x = ...)`, with no `continue` that could
//                    skip it), so the last iteration leaves the value the
//                    sequential code would.
//   reduction(op:x)  every mention of x in the loop is an update of one
//                    associative operator: `x op= e`, `x = x op e`,
//                    `x = e op x` (op in + - * & | ^), or a min/max update
//                    (`x = x < e ? e : x`, `if (e > x) x = e;`), with e not
//                    mentioning x. Floating-point reductions reassociate and
//                    change the last bits of the result, so they are only
//                    allowed with -fp-reductions (off by default).
//   otherwise        UNSAFE: x carries a value from one iteration to the
//                    next.
//
// Never privatized: globals and statics (one object shared by everything),
// a variable whose address is taken anywhere in the function (a pointer could
// read or write the shared copy), a variable the loop's own condition or
// increment uses, and non-arithmetic variables.
//
// The loop's own induction variable is private in OpenMP automatically. When
// it is declared outside the loop and read after it, the sequential value
// after the loop must be preserved: `lastprivate(i)` does that only if the
// loop runs at least once (on a zero-trip loop it leaves i untouched, where
// the sequential code ran `i = start`), so that is required to be provable
// from a constant trip count; otherwise the loop is UNKNOWN.

#ifndef P05_ANALYSIS_SCALARANALYSIS_H
#define P05_ANALYSIS_SCALARANALYSIS_H

#include "analysis/LoopInfo.h"
#include "analysis/Rejection.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <memory>
#include <string>
#include <vector>

namespace clang {
class CFG;
class CFGBlock;
} // namespace clang

namespace p05 {

struct ScalarClause {
  enum Kind { Private, LastPrivate, Reduction } K;
  const clang::VarDecl *Var;
  /// Reduction operator in OpenMP spelling: + * & | ^ min max.
  std::string Op;
};

struct LoopScalars {
  std::vector<ScalarClause> Clauses;
  /// A scalar that carries a value across iterations (or can't be copied
  /// per thread): the loop is UNSAFE.
  std::vector<Reason> UnsafeReasons;
  /// Couldn't be decided: the loop is UNKNOWN.
  std::vector<Reason> UnknownReasons;

  /// The OpenMP clauses, e.g. " private(j, t) reduction(+: sum)"; empty when
  /// there are none. Leading space included, ready to append to a pragma.
  std::string clauseText() const;
  /// A lastprivate or reduction clause is present. On a GPU those need their
  /// own map() handling, which this version doesn't emit (Phase 2.1).
  bool needsHost() const;
};

/// -fp-reductions (off by default): allow floating-point reductions.
bool fpReductionsFromFlags();

class ScalarAnalyzer {
public:
  ScalarAnalyzer(clang::ASTContext &Ctx, bool AllowFPReductions);
  ~ScalarAnalyzer();

  LoopScalars analyzeLoop(const LoopInfo &LI);

private:
  /// Liveness over one function's CFG, for its automatic scalar variables
  /// whose address is never taken (everything else is never privatized).
  struct FunctionLiveness {
    std::unique_ptr<clang::CFG> Graph;
    llvm::DenseMap<const clang::VarDecl *, unsigned> Index;
    llvm::DenseMap<const clang::CFGBlock *, llvm::BitVector> LiveIn;
    llvm::DenseSet<const clang::VarDecl *> AddressTaken;
  };
  FunctionLiveness &livenessFor(const clang::FunctionDecl *FD);

  clang::ASTContext &Ctx;
  bool AllowFP;
  llvm::DenseMap<const clang::FunctionDecl *, std::unique_ptr<FunctionLiveness>>
      Cache;
};

} // namespace p05

#endif // P05_ANALYSIS_SCALARANALYSIS_H
