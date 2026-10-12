// AliasAnalysis — Phase 1.6: aliasing made explicit instead of assumed.
//
// Until Phase 1.5 the dependence tests never compared accesses through two
// differently named bases: `f(double *a, double *b)` writing `a[i] = b[i+1]`
// was SAFE even though a caller may pass b == a + 1. This answers, for the
// function containing a loop, whether two of its bases can overlap at all:
//
//   Distinct  two different objects: arrays, scalars or structs declared in
//             this function, globals and statics (two named objects never
//             overlap); a pointer parameter never points into this call's own
//             automatic objects.
//   Types     element types C's strict-aliasing rule keeps apart (`double *`
//             vs `int *`; never char, void, records or unions). The same
//             assumption AffineAccess already makes.
//   Restrict  either is a `restrict` pointer parameter: an overlap with a
//             write in the loop would be undefined behaviour.
//   Unreached a static or global object whose address never escapes (it
//             is only read, written or subscripted by name), when every use
//             is visible: a static local or a `static` global (never an
//             external global, even under -whole-program, which only says
//             who calls this file's functions). (Reported as Distinct.)
//   Callers   every call of the function passes disjoint objects for the two
//             (distinct named objects, or the caller's own parameters, proven
//             disjoint the same way, a few levels up). Needs every call to
//             be visible: the function is `static` (or -whole-program says
//             no other translation unit calls it), never has its address
//             taken, and every call has a prototype.
//   Unproven  a pointer parameter and another pointer parameter or a global:
//             runtimeCheck() builds a test that the byte ranges the loop
//             touches in the two do not overlap, emitted into the OpenMP
//             if() (a false test runs the loop on one thread: still
//             correct). When a range can't be computed the loop is UNKNOWN.
//
// The ranges come from DependenceTest's subscriptBounds (each subscript's
// extremes over the loop and inner-loop variable ranges); multi-dimensional
// accesses are linearized with their row lengths. The test is integer
// arithmetic on __INTPTR_TYPE__, so it never forms an out-of-bounds pointer
// or compares pointers into different objects.
//
// -alias=assume restores the pre-1.6 assumption (ablation baseline).

#ifndef P05_ANALYSIS_ALIASANALYSIS_H
#define P05_ANALYSIS_ALIASANALYSIS_H

#include "analysis/AffineAccess.h"
#include "analysis/DependenceTest.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <optional>
#include <string>
#include <vector>

namespace p05 {

enum class AliasMode { Check, Assume };
AliasMode aliasModeFromFlags();
bool wholeProgramFromFlags();

class AliasAnalyzer {
public:
  AliasAnalyzer(clang::ASTContext &Ctx, bool WholeProgram);

  /// Can A and B, bases of accesses in a loop of F, overlap?
  AliasProof proof(const clang::FunctionDecl *F, const clang::ValueDecl *A,
                   const clang::ValueDecl *B);

  /// A C expression, placed just before the loop starting at At, that is
  /// true when the bytes the loop touches through A and through B (every
  /// access in Acc on each) cannot overlap; nullopt, with Why, when a range
  /// can't be computed or a name it needs can't be written there (shadowed
  /// anywhere in F, or a global declared after the loop).
  std::optional<std::string> runtimeCheck(const clang::FunctionDecl *F,
                                          clang::SourceLocation At,
                                          const clang::ValueDecl *A,
                                          const clang::ValueDecl *B,
                                          const LoopAccesses &Acc,
                                          std::string &Why);

  /// The function whose body contains S.
  const clang::FunctionDecl *enclosingFunction(const clang::Stmt *S);

private:
  enum class Role { PtrParam, Object, Other };
  Role role(const clang::FunctionDecl *F, const clang::ValueDecl *V);
  AliasProof proofAt(const clang::FunctionDecl *F, const clang::ValueDecl *A,
                     const clang::ValueDecl *B, unsigned Depth);
  bool callersProve(const clang::FunctionDecl *F, const clang::ValueDecl *A,
                    const clang::ValueDecl *B, unsigned Depth);
  void indexCalls();

  clang::ASTContext &Ctx;
  bool WholeProgram;
  bool Indexed = false;
  llvm::DenseMap<const clang::FunctionDecl *,
                 std::vector<const clang::CallExpr *>>
      CallsTo;
  llvm::DenseSet<const clang::FunctionDecl *> AddressTaken;
  /// Objects with static storage whose address some use lets escape (taken
  /// with `&`, or decayed to a pointer other than to subscript it). One that
  /// never escapes can't be reached through any pointer.
  llvm::DenseSet<const clang::VarDecl *> Escaped;
  /// Pointer parameters a function writes or takes the address of.
  llvm::DenseMap<const clang::FunctionDecl *,
                 llvm::DenseSet<const clang::VarDecl *>>
      ModifiedParams;
};

} // namespace p05

#endif // P05_ANALYSIS_ALIASANALYSIS_H
