# Design Decisions & Scope Log

Running log of real decisions and simplifications, in the order made. This becomes the basis of the
finale writeup — each entry should be a decision plus the reasoning, not just a fact.

## Format for entries

```
### <date> — <short title>
**Decision:** what was decided.
**Reasoning:** why, including what tradeoff was accepted.
**Alternative considered:** what else was possible and why it was rejected (if relevant).
```

---

### Day 0 — Clang LibTooling instead of ROSE

**Decision:** Build on Clang LibTooling rather than the ROSE framework suggested in the original brief.

**Reasoning:** ROSE's main relevant strength — native parsing of programs that already contain
CUDA/OpenMP/UPC syntax — doesn't apply to this project, since the input is plain sequential C/C++ and
the tool *generates* OpenMP directives rather than parsing existing GPU code. ROSE's build setup is a
known, disproportionate time sink, which is an unacceptable risk on a solo, time-boxed build. Clang
LibTooling provides equivalent source-level AST traversal and source-to-source rewriting (via
`RecursiveASTVisitor`/`ASTMatchers` + `Rewriter`), is far better documented, and has a much lower setup
cost.

**Alternative considered:** Sticking with ROSE as specified in the brief. Rejected due to build-time risk
outweighing the (largely inapplicable) CUDA-awareness benefit for this specific input domain.

---

### Day 2 — Constant bounds folded, not literal-matched

**Decision:** A loop counts as `CONSTANT_BOUND` when its bound expression folds via Clang's
`Expr::EvaluateAsInt`, not only when it is a literal `IntegerLiteral` node.

**Reasoning:** `for (int i = 0; i < N; i++)` with `const int N = 512` has a trip count that is
every bit as known at compile time as `i < 512`. Matching only literals would classify it
`VARIABLE_BOUND` and hand the profitability heuristic "trip count unknown" for a loop whose size
we demonstrably know — the heuristic would then decline to offload a 512-iteration loop for no
real reason. `EvaluateAsInt` also picks up `8 * 8`, `sizeof(a)/sizeof(a[0])` and enum constants.
It defaults to `SE_NoSideEffects`, so it will not fold anything that has to run code.

**Alternative considered:** Strict `dyn_cast<IntegerLiteral>`. Simpler to describe, but it
understates what the analysis actually knows.

---

### Day 2 — Classification and trip count are separate facts

**Decision:** `LoopKind` is decided by the *bound* alone; the exact iteration count lives in a
separate `std::optional<int64_t> TripCount` that is only filled when start, bound and step are
all known.

**Reasoning:** They genuinely come apart. `for (int i = start; i < 1000; i++)` has a constant
bound but an unknown trip count. Collapsing the two would force a choice between lying about the
kind and lying about the count. Keeping them separate means consumers must check the optional
rather than infer a number from the kind — slightly more work at each call site, but no wrong
answers.

---

### Day 2 — A call in the bound expression is UNRECOGNIZED, not variable-bound

**Decision:** `for (int i = 0; i < f(n); i++)` is rejected outright rather than treated as a
loop with symbolic bound `f(n)`.

**Reasoning:** The condition is re-evaluated on every iteration. If `f` has side effects or
returns a different value each call, there is no fixed bound to reason about, and the loop may
not even be a counted loop. Recording it as "bound is `f(n)`" would look like ordinary symbolic
information while quietly being unsound.

---

### Day 2 — Per-loop callee recording replaces the Day 0 global call print

**Decision:** Calls are collected per loop body into `LoopInfo::Callees`, and unresolvable
indirect calls set a separate `HasIndirectCall` flag rather than being dropped.

**Reasoning:** The Day 0 visitor printed every `CallExpr` in the file indented as though it sat
inside a loop — in `example.c` the call to `process` at line 20 is at top level but printed as
if nested. That is the wrong data shape to build interprocedural analysis on. Tracking indirect
calls separately matters because an unresolvable callee must *block* parallelization later;
silently dropping it would leave a soundness hole exactly where the analysis is weakest.

**Scope note:** Verifying that the loop body does not reassign the induction variable is *not*
done yet — it is body analysis, deferred to the Week 2 safety pass, and flagged as a TODO in
`src/analysis/LoopInfo.h`. It is a hard prerequisite before any pragma is emitted.

---

### Day 4 — Benchmark set finalized: three demo programs, not just test fixtures

**Decision:** The 2-3 program demo set is `example.c`, `saxpy.c`, `small_update.c`.
`loop_shapes.c`/`loop_unrecognized.c` stay as classifier unit-test fixtures, not
part of the demo set — they exist to exercise loop-shape edge cases, not to tell
an end-to-end story.

**Reasoning:** The three demo programs are deliberately spread across the axis
Week 2's profitability heuristic needs to discriminate on, while keeping the
"call inside the hot loop" shape constant across all three (that's the headline
case a naive intraprocedural parallelizer misses):
- `example.c` — moderate size (1000), the original Day 0 sanity case.
- `saxpy.c` — classic BLAS kernel, large trip count (65536), dense/coalesced
  access. Expected to land clearly GPU-profitable.
- `small_update.c` — trip count 8. Safe to parallelize (pure helper, no
  aliasing), but expected to land NOT profitable — kernel-launch/transfer
  overhead would dwarf the work. Without this case, the profitability heuristic
  has nothing to say no to, and "safe implies offload" would go unfalsified.

**Alternative considered:** Pulling from PolyBench/C or Rodinia. Rejected for
now — hand-written kernels are small enough to fully understand and modify
(needed once codegen and profitability land), and SAXPY is a real, recognizable
kernel rather than a toy, so it doesn't cost the demo credibility.

---

### Day 5-6 — Interprocedural resolution via clang::CallGraph, keyed by FunctionDecl, not by loop

**Decision:** Added `CallResolver` (`src/analysis/CallResolver.h/.cpp`), wrapping
`clang::CallGraph`. It exposes `getDirectCallees(FunctionDecl*)` (one hop, deduplicated) and
`getReachable(FunctionDecl*)` (transitive BFS closure, with `HasOpaqueCallee` and `HasRecursion`
flags). Queried by `FunctionDecl*`, not stored as a new field on `LoopInfo`. Days 5 and 6 were done
together rather than split, since every demo benchmark's loop-body callee (`scale`, `axpy_elem`,
`clamp_unit`) is a leaf — one-hop resolution alone would have produced nothing distinguishable
from the `LoopInfo::Callees` list Day 2 already built, making it unverifiable on its own.

**Reasoning:** "What does function X call" is a fact about X, not about any particular loop —
several loops can share a callee, and once the walk is transitive, several other functions can
reach it too. Keying by `FunctionDecl*` lets one `clang::CallGraph`, built once per translation
unit, answer every query, and mirrors the `LoopKind`/`TripCount` precedent from Day 2: two facts
that are genuinely separate should not be collapsed into one record just because they're usually
asked about together.

**Implementation detail worth recording:** `clang::CallGraph::getOrInsertNode` canonicalizes the
`Decl*` it stores nodes under, but the paired `getNode` lookup does a raw map find with no
canonicalization (confirmed against upstream `clang/lib/Analysis/CallGraph.cpp`). Since
`CallExpr::getDirectCallee()` — what `LoopInfo::Callees` is built from — can return a
non-canonical redeclaration, a naive `CG.getNode(someCallee)` can silently return null for a
function that plainly has calls, indistinguishable from "calls nothing." `CallResolver`
canonicalizes both the query input and every returned callee explicitly.

**Cycle detection is a separate 3-color DFS, not a BFS "already visited" flag.** The original plan
sketch described marking `HasRecursion` whenever the BFS walk re-encountered an already-discovered
function. That is wrong: two independent functions calling a shared helper (a diamond — `a` and
`b` both call `c`, no cycle) revisits `c` the same way a genuine cycle would, so the naive check
would flag ordinary shared helpers as recursive. Caught during the adversarial self-review, not
during initial implementation. Fixed by running a dedicated DFS with three-state coloring
(on-stack / done / unvisited) that only flags an edge back to a function *still on the current
path* — a true back-edge — never a re-visit of a function already fully explored via a different
path.

**Alternative considered:** A `TransitiveCallees` field on `LoopInfo`. Rejected for the same reason
given above — it duplicates data whenever two loops share a callee and gives the Day 6 walker
nowhere natural to live independent of any one loop.

**Scope note:** This layer produces reachability facts only (how many functions, how deep, any
opaque body, any recursion) — no side-effect or safety judgment. That is Week 2 (Days 8-10).
`tests/call_chains.c` was added as a dedicated fixture (2-level chain, 3-level chain, opaque
callee, mutual recursion) since no benchmark in the 3-program demo set exercises more than one
call hop.

---

### Day 8-10 — Safety analysis: tri-state verdict, coarse callee effects, and a strict body-dependence rule

**Decision:** Added `SafetyAnalyzer` (`src/analysis/SafetyAnalysis.h/.cpp`), which turns
`CallResolver`'s reachability facts into a per-loop `SafetyVerdict` (`Safe` / `Unknown` / `Unsafe`,
not a bool). Two independent hazard sources feed it:

1. **Interprocedural** — `FunctionEffects`, computed per function from its own body only and
   memoized: does it write through a pointer parameter, or touch a global/`static`? Folded across
   every callee `LoopInfo::Callees` records and everything `CallResolver::getReachable` says is
   transitively reachable from them, so a global write three call-hops down is caught, not just a
   direct callee's own writes.
2. **Intraprocedural** — the loop body itself: does it reassign its own induction variable, and is
   every array/pointer access in the body (read *or* write) indexed by exactly the induction
   variable? This closes the two TODOs left open in `LoopInfo.h` since Day 2.

**Reasoning — tri-state, not bool:** `Unknown` (opaque callee body, indirect call, recursion, an
unresolvable write target) and `Unsafe` (a hazard actually *found*) block parallelization
identically downstream, so the split costs nothing in soundness — but collapsing them would repeat
the exact mistake `HasIndirectCall` and `HasOpaqueCallee` were already careful not to make:
conflating "cannot see" with "looked and found nothing wrong."

**Reasoning — call-site-independent effects:** a function is flagged for writing through *some*
pointer parameter regardless of which argument any particular call site passed for it. This is what
lets the interprocedural half stay coarse with zero alias analysis between call sites, at the cost of
flagging some genuinely safe calls (see over-approximations below) — a precision-for-soundness trade
in the conservative direction only, matching ROADMAP.md's explicit sanctioning of a coarse
"writes through a pointer arg or touches a global" check over real points-to analysis.

**Reasoning — the body rule requires *every* access, not just writes, to equal the induction
variable:** this is what makes it sound without alias analysis for two references to the *same*
object — element i and element i' of one array can only be the same location when i == i', full
stop, regardless of what else in the program aliases that array. A weaker rule checking only write
targets would miss `y[i] = x[i - 1]` when x and y alias.

**Adversarial review finding (self-run, not a subagent — see below):** that same soundness argument
does *not* extend to two *distinct* pointer/array parameters that might alias each other with a
nonzero shift. Constructed and empirically confirmed:

```c
void alias_shift_hazard(double *a, double *b, int n) {
  for (int i = 0; i < n; i++) a[i] = b[i] + 1.0;   // verifies SAFE
}
```

If a caller ever passes `b == a + 1`, this is `a[i] = a[i+1] + 1.0` in disguise: iteration `k` reads
`a[k+1]`, iteration `k+1` later writes `a[k+1]` — a real cross-iteration RAW hazard, invisible to
this checker because it never reasons about whether two *different* parameters could alias. The
original header comment claimed unconditional soundness; it was wrong and has been corrected to
state the real, unverified assumption this whole layer rests on: distinct pointer/array parameters
don't alias (the same promise C's `restrict` keyword makes explicit, here made implicitly). Verifying
it for real is exactly the points-to analysis ROADMAP.md already scoped out — not fixed, only
honestly documented.

A second finding from the same pass, in the safe direction (a completeness gap, not a soundness
one): any 2D-style access (`m[i][j]`, `a[i*cols+j]`) inside a loop properly nested over `i` then `j`
verifies `UNSAFE` for both loops, because the outer subscript's index is `j` (or an affine
expression), never the bare induction variable of the loop being judged — this layer has no notion
of "belongs to a properly nested inner loop." No 2D loop nest can currently verify SAFE. Not fixed:
none of the three demo benchmarks are 2D, and doing it properly is a real scope increase, not a bug
fix — logged here so it isn't forgotten if a future benchmark needs it.

**Process note:** the background review subagent failed three times in a row on transient
infrastructure errors (a sleep interrupt, then a persistent self-signed-certificate/connectivity
error) before producing any output. Rather than keep retrying a broken channel, the adversarial pass
was done directly — construct hostile snippets by hand, run them through `./build/p05tool`, compare
actual output against hand-derived expected behavior. Both findings above came from that pass, which
is the same standard the Day 5-6 review applied to catch the diamond-vs-cycle bug in `CallResolver`.

**Verification:** all three demo benchmarks (`example.c`, `saxpy.c`, `small_update.c`) verify SAFE,
including their `main()` init loops. `tests/safety_cases.c` — a new fixture, twelve loops covering
every category (safe pure/transitive-pure/body-local-temp; unsafe direct/transitive global write,
pointer-param write, induction-variable reassignment, neighbor access, scatter write, reduction;
unknown opaque callee, unknown indirect call) — matches its expected-verdict comments exactly.
`tests/call_chains.c`'s opaque-callee and mutual-recursion cases land `UNKNOWN`; its two- and
three-level pure chains land `SAFE`. `benchmarks/loop_shapes.c`'s existing `LoopKind` classifications
are unchanged; its one genuinely loop-carried case (`a[i] = a[i + 1]`, reading a neighbor) now
correctly reports `UNSAFE` as new information layered on top, not a regression of the shape check.

**Alternative considered:** a plain `bool IsSafe`. Rejected for the tri-state reasoning above — the
distinction is free to keep and answers "how do you know it's safe?" far better in the finale Q&A
than a verdict that can't tell "checked and it's clean" from "couldn't check."

---

<!-- Add new entries below as you build. -->
