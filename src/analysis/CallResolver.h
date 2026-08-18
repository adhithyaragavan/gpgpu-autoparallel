// CallResolver — interprocedural call resolution, built on clang::CallGraph.
//
// Answers "what does function X call", keyed by FunctionDecl* rather than by
// loop. That keying is deliberate: "what does X call" is a fact about X, shared
// by every call site that reaches it, so one CallGraph built once per
// translation unit can answer every query. Hanging the answer off LoopInfo
// instead would duplicate identical data whenever two loops share a callee, and
// would leave the transitive walk below with nowhere natural to live.
//
// This layer produces *reachability* facts only — which functions a loop can
// reach, and where the analysis cannot see. Whether a reachable function's side
// effects actually make the loop unsafe to parallelize is the Week 2 safety
// pass, and is deliberately not decided here.

#ifndef P05_ANALYSIS_CALLRESOLVER_H
#define P05_ANALYSIS_CALLRESOLVER_H

#include "analysis/LoopInfo.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CallGraph.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace p05 {

/// One callee reachable from a function, resolved via clang::CallGraph.
struct ResolvedCallee {
  const clang::FunctionDecl *Callee = nullptr;

  /// False when Callee is declared but not defined in this translation unit —
  /// a genuine external/library call, or a definition that lives in another TU.
  ///
  /// Distinct from LoopInfo::HasIndirectCall, which fires when a call resolves
  /// to no FunctionDecl at all (a call through a function pointer). Here we do
  /// know *which* function is called; we just cannot see what it does. Tracked
  /// explicitly for the same reason HasIndirectCall is: an invisible body must
  /// reach the safety pass as "unknown effects", never as "no effects".
  bool HasVisibleBody = true;

  /// Hops from the query root. 1 means called directly by the root.
  unsigned Depth = 0;
};

/// Everything reachable from one function, plus the facts about the walk that
/// the safety pass will need.
struct CallChainSummary {
  /// Transitively reachable functions, deduplicated, in breadth-first order,
  /// so Depth is the *shortest* hop distance from the root.
  ///
  /// Excludes the root itself, even when a cycle makes the root reachable from
  /// itself — the root is the subject of the query, not a result of it. Nothing
  /// is lost by this: every root passed in here came from LoopInfo::Callees, so
  /// the caller already knows the loop executes it. HasRecursion below is what
  /// reports the cycle.
  std::vector<ResolvedCallee> Reachable;

  /// True when any function in Reachable has no visible body. Hoisted to the
  /// summary because it is the first question the safety pass asks: a single
  /// opaque function anywhere in the chain makes the whole loop unanalyzable.
  ///
  /// IMPORTANT: this covers the reachable set only, which excludes the root.
  /// A caller that passes a root with no body of its own (a loop calling a
  /// library function directly) must check the root's own hasBody() — this flag
  /// will be false in that case, because nothing *downstream* is opaque.
  bool HasOpaqueCallee = false;

  /// A cycle was found somewhere reachable from the root — direct recursion,
  /// mutual recursion, or a longer loop. Recorded rather than merely survived:
  /// the walk terminates either way thanks to the visited set, but "this chain
  /// recurses" is a fact the safety pass may want to act on rather than a mere
  /// implementation detail of the traversal.
  bool HasRecursion = false;

  /// Largest Depth in Reachable; 0 when Reachable is empty.
  unsigned MaxDepth = 0;
};

/// Thin wrapper over clang::CallGraph. Builds the whole-TU graph once in the
/// constructor; each query is then proportional to the subgraph it walks rather
/// than to the size of the translation unit.
class CallResolver {
public:
  explicit CallResolver(clang::ASTContext &Ctx);

  /// Direct callees of Caller, deduplicated, each with Depth == 1. Mirrors the
  /// LoopInfo::Callees convention so the two lists read the same way.
  ///
  /// Both the caller and every returned callee are canonicalized here.
  /// clang::CallGraph stores its nodes under canonical declarations, while
  /// CallExpr::getDirectCallee() — the source of LoopInfo::Callees — hands back
  /// whichever redeclaration was in scope at the call site. If those two
  /// disagree the node lookup returns null, which is indistinguishable from
  /// "this function calls nothing": a silent wrong answer rather than a loud
  /// one. Canonicalizing is correct whether or not the lookup does it too.
  std::vector<ResolvedCallee>
  getDirectCallees(const clang::FunctionDecl *Caller) const;

  /// Everything reachable from Root, to any depth, with a cycle guard.
  CallChainSummary getReachable(const clang::FunctionDecl *Root) const;

private:
  /// Depth-first cycle search. State maps a function to 0 (on the current DFS
  /// path) or 1 (fully explored). Finding an edge back to a function still on
  /// the current path is a cycle; finding one to a fully explored function is
  /// not — that is a diamond, not a loop. A plain "have I seen this before"
  /// visited set cannot tell those two apart, which is why cycle detection is
  /// a separate DFS rather than a flag raised during the breadth-first walk.
  bool findCycle(const clang::FunctionDecl *Fn,
                 llvm::DenseMap<const clang::FunctionDecl *, int> &State) const;

  clang::CallGraph CG;
};

/// Reports, for each callee LoopAnalysis recorded on LI, everything that callee
/// transitively reaches. Lives here rather than in LoopAnalysis.cpp because it
/// is call-resolution reporting rather than loop-shape reporting, and here
/// rather than in main.cpp so the driver stays wiring-only.
void printCallChains(llvm::raw_ostream &OS, const LoopInfo &LI,
                     const CallResolver &Resolver);

} // namespace p05

#endif // P05_ANALYSIS_CALLRESOLVER_H
