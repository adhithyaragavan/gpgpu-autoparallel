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

<!-- Add new entries below as you build. -->
