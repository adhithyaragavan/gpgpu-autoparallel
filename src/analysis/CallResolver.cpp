#include "analysis/CallResolver.h"

#include "llvm/ADT/DenseSet.h"

#include <deque>

using namespace clang;

namespace p05 {

CallResolver::CallResolver(ASTContext &Ctx) {
  // Recursively visits the whole TU once; every FunctionDecl *definition* it
  // finds gets a node with edges to whatever it calls. Built eagerly here so
  // each later query is proportional to the subgraph it walks, not to TU size.
  CG.addToCallGraph(Ctx.getTranslationUnitDecl());
}

std::vector<ResolvedCallee>
CallResolver::getDirectCallees(const FunctionDecl *Caller) const {
  std::vector<ResolvedCallee> Result;
  if (!Caller)
    return Result;

  // See the header comment on this function: canonicalize regardless of
  // whether the underlying lookup already does, since a mismatch here fails
  // silently (an empty result instead of an error).
  CallGraphNode *Node = CG.getNode(Caller->getCanonicalDecl());

  // No node means Caller was never visited while building the graph: it has
  // no body in this TU (only definitions get visited), and nothing else in
  // the TU happened to call it either. Both cases mean "nothing to report",
  // same as an empty result.
  if (!Node)
    return Result;

  llvm::DenseSet<const FunctionDecl *> Seen;
  for (const CallGraphNode::CallRecord &Edge : Node->callees()) {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(Edge.Callee->getDecl());
    if (!FD)
      continue; // Blocks/ObjC methods: out of scope, this project targets C.
    FD = FD->getCanonicalDecl();

    if (!Seen.insert(FD).second)
      continue; // Dedup, mirrors the LoopInfo::Callees convention.

    ResolvedCallee RC;
    RC.Callee = FD;
    // hasBody() walks the whole redeclaration chain, so this is correct
    // regardless of which redecl the call graph happened to store, and is
    // unambiguous where "the CallGraphNode has no outgoing edges" would not
    // be: that could also mean "a real leaf function" rather than "opaque".
    RC.HasVisibleBody = FD->hasBody();
    RC.Depth = 1;
    Result.push_back(RC);
  }
  return Result;
}

CallChainSummary CallResolver::getReachable(const FunctionDecl *Root) const {
  CallChainSummary Summary;
  if (!Root)
    return Summary;
  const FunctionDecl *CanonRoot = Root->getCanonicalDecl();

  // Breadth-first walk. Depth doubles as the "discovered" set: a function
  // present here has already been assigned its (shortest) distance from the
  // root and must not be re-enqueued, which is what keeps a cycle back to an
  // already-discovered function from looping the walk forever.
  llvm::DenseMap<const FunctionDecl *, unsigned> Depth;
  Depth[CanonRoot] = 0;
  std::deque<const FunctionDecl *> Queue{CanonRoot};

  while (!Queue.empty()) {
    const FunctionDecl *Cur = Queue.front();
    Queue.pop_front();
    unsigned CurDepth = Depth[Cur];

    for (const ResolvedCallee &RC : getDirectCallees(Cur)) {
      if (Depth.count(RC.Callee))
        continue; // Already discovered via a path of equal-or-shorter length.

      unsigned NewDepth = CurDepth + 1;
      Depth[RC.Callee] = NewDepth;
      Summary.Reachable.push_back({RC.Callee, RC.HasVisibleBody, NewDepth});
      Summary.HasOpaqueCallee |= !RC.HasVisibleBody;
      Summary.MaxDepth = std::max(Summary.MaxDepth, NewDepth);

      // A function with no visible body has no edges we can see, so there is
      // nothing to gain by enqueuing it -- CG.getNode would find it, at best,
      // as the empty node created for the call edge we just followed.
      if (RC.HasVisibleBody)
        Queue.push_back(RC.Callee);
    }
  }

  llvm::DenseMap<const FunctionDecl *, int> State;
  Summary.HasRecursion = findCycle(CanonRoot, State);

  return Summary;
}

bool CallResolver::findCycle(
    const FunctionDecl *Fn,
    llvm::DenseMap<const FunctionDecl *, int> &State) const {
  // Three-state DFS coloring, not a plain visited set: a plain "have I seen
  // this node before" check cannot tell a genuine cycle (an edge back to an
  // ancestor still on the current path) apart from a diamond (an edge into a
  // function already fully explored via a *different*, non-overlapping path).
  // Only the former is recursion. Conflating the two would flag perfectly
  // ordinary shared helpers -- `a` and `b` both calling `c` -- as recursive,
  // which is exactly the kind of false "unsafe" signal the Week 2 safety pass
  // must not be handed.
  if (auto It = State.find(Fn); It != State.end())
    return It->second == 0; // 0 = still on the DFS stack => a back edge.

  State[Fn] = 0; // On stack.
  bool Found = false;
  for (const ResolvedCallee &RC : getDirectCallees(Fn)) {
    if (!RC.HasVisibleBody)
      continue; // No visible edges out, so it cannot close a cycle.
    if (findCycle(RC.Callee, State)) {
      Found = true;
      break; // One confirmed cycle is enough; this is a yes/no flag.
    }
  }
  State[Fn] = 1; // Fully explored.
  return Found;
}

void printCallChains(llvm::raw_ostream &OS, const LoopInfo &LI,
                     const CallResolver &Resolver) {
  if (LI.Callees.empty())
    return;

  OS << "  call chains (beyond the loop body's direct callees):\n";
  for (const FunctionDecl *Callee : LI.Callees) {
    CallChainSummary Summary = Resolver.getReachable(Callee);

    OS << "    " << Callee->getNameAsString() << " reaches "
       << Summary.Reachable.size() << " function(s), max depth "
       << Summary.MaxDepth << ": ";
    if (Summary.Reachable.empty()) {
      // Reachable excludes the root itself by design (see the header comment
      // on CallChainSummary::Reachable), so a self-recursive root -- one whose
      // only outgoing edge points back to itself -- also lands here with an
      // empty set. "(leaf)" would contradict the "contains recursion" line
      // printed right below for that case, so it is reserved for functions
      // that are genuinely calling nothing.
      OS << (Summary.HasRecursion ? "<nothing further>" : "<nothing> (leaf)")
         << "\n";
    } else {
      for (size_t I = 0; I < Summary.Reachable.size(); ++I) {
        if (I)
          OS << ", ";
        const ResolvedCallee &RC = Summary.Reachable[I];
        OS << RC.Callee->getNameAsString() << "@" << RC.Depth;
        if (!RC.HasVisibleBody)
          OS << "<no visible body>";
      }
      OS << "\n";
    }

    if (Summary.HasOpaqueCallee)
      OS << "      contains a call whose body is not visible in this TU\n";
    if (Summary.HasRecursion)
      OS << "      contains recursion\n";
  }
  OS << "\n";
}

} // namespace p05
