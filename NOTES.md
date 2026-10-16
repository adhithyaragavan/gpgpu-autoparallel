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

### Day 11-13 — GPU-profitability heuristic: a cost model, not a score

**Decision:** Added `ProfitabilityAnalyzer` (`src/analysis/Profitability.h/.cpp`), which turns a
SAFE loop's shape, data-access, and interprocedural op-count facts into an `OffloadTarget`
(`GpuOffload` / `CpuParallel` / `Sequential` — three-valued for the same reason `SafetyVerdict` is:
"safe but eight iterations" is a real, distinct case from "safe and worth threading", not a
degenerate corner of the binary GPU/CPU split the roadmap originally sketched). The decision is a
roofline-style cost model — `t_seq`, `t_cpu`, `t_gpu` computed from a documented `MachineModel`
struct (launch overhead, PCIe/DRAM/GPU bandwidth, CPU/GPU throughput, core count), target =
argmin — rather than the weighted feature score ROADMAP.md's original wording suggested. The
reported gate cascade (work, transfer, intensity, verdict) is a *rendering* of that same
computation: every threshold printed is derived from `MachineModel`, not a second, independently
hand-picked set of numbers to defend. All six `MachineModel` fields are `llvm::cl::opt` overrides
(`--pcie-bandwidth`, `--gpu-throughput`, etc.), so "is this just tuned to your benchmarks?" is
answered by re-running with different hardware, not by argument — confirmed empirically:
`--pcie-bandwidth=64` (an NVLink-class link) flips `saxpy.c` from `CPU_PARALLEL` to `GPU_OFFLOAD`
on the same loop, same analysis.

**The finding that shaped the plan:** working the model's arithmetic *before* writing benchmarks
around it (the adversarial-review habit applied one step earlier than usual) surfaced that the
default `MachineModel` implies a break-even arithmetic intensity of **~2.85 flop/byte** — the flop
count divided by bytes moved across PCIe, below which the transfer cost always dominates no matter
how large the loop. All three existing demo benchmarks sit 10-50x below that wall:
`example.c`/`process` (0.06), `saxpy.c` (0.08 — despite its own header comment, written in Week 1,
calling it "the headline GPU-profitable case"; SAXPY being memory-bound is a textbook result, not
an artifact of this model, and that comment was wrong and has been corrected). `small_update.c`
never gets the chance to be memory-bound — its trip count of 8 loses to `Sequential` outright.
**No benchmark existing before this pass was GPU-profitable under an honest model.** Two
consequences, both acted on: `benchmarks/compute_heavy.c` was added — a straight-line 40-FMA
helper (80 flops against 16 bytes, 5.0 flop/byte) that clears the wall and verifies an
unconditional `GPU_OFFLOAD` — so Days 14-16's codegen has a real input for its GPU path; and
`saxpy.c`'s role in the demo story flips from "the GPU case" to "the case that proves the tool
doesn't just offload everything safe", which is a *better* fit for the brief's actual success
criterion than the original framing was.

**Reasoning — symbolic bounds get a solved threshold, not a guess or a demotion:** most SAFE loops
in the existing benchmark/fixture set have a parameter bound (`example.c`'s `process`, most of
`safety_cases.c`), so treating "trip count unknown" as an automatic CPU fallback would silently
punish the project's own headline cross-function demo case for having a symbolic bound rather than
for being unprofitable. Instead: because flops(n) and bytes(n) are both proportional to n at a
fixed per-element ratio, whichever term wins each target's `max(compute, memory)` is the same for
every n > 0 — so each side collapses to one linear function of n, and the GPU/CPU crossover has an
exact closed form (`n* = (KernelLaunchUs - ThreadStartUs) / (rate_cpu - rate_gpu)`) rather than
needing a numeric search. Rounded up to a power of two, that becomes `GPU_OFFLOAD if (n >= T)`,
rendered in Week 3 as OpenMP's own `if()` clause — a spec feature, not new machinery.
`tests/profitability_cases.c`'s `gpu_conditional_case` (the same 80-flop kernel as
`compute_heavy.c`, but with a parameter bound) verifies `T = 8192`, solved from `n* ≈ 4966.9`.

**Reasoning — stride needs no gate of its own:** a loop with step `s` touches `T` elements but
*spans* `T·s` of index space, and a `map()` clause covering a strided access has to map the whole
span. Charging `TransferBytes` on the span while `TouchedBytes` (the DRAM-traffic term used for
`t_seq`/`t_cpu`) stays on the true touched count means poor coalescing shows up automatically as
transfer cost the model already prices, with no separate hand-set stride penalty to defend.
`tests/profitability_cases.c`'s `strided_case` (step 16, 65536 touched elements spanning
1,048,576) verifies `CPU_PARALLEL` with the GPU path dominated by a 1.4ms transfer term — 16x the
unstrided cost on the same touched-element count.

**Reasoning — scatter access needs no gate either, for a different reason:** it cannot reach this
pass at all. `SafetyAnalyzer` already requires every array access to be indexed by exactly the
induction variable, so `data[idx[i]]`-style scatter is already `UNSAFE` before profitability runs.
The "access pattern" feature ROADMAP.md's Week 2 note asked for is therefore mostly pre-filtered by
the safety pass; what is left of it here is stride, handled above.

**Reasoning — the interprocedural op count is the genuinely load-bearing half, same shape of
argument as Days 8-10's transitive safety walk:** `example.c`'s loop body is
`data[i] = scale(data[i], factor)` — zero arithmetic operators visible without crossing the call
boundary into `scale`. A profitability pass that stopped at the loop body would price every
call-wrapped kernel at 0 flops/iteration and always decline it, for the wrong reason. `getOpCount`
is memoized per function (mirroring `SafetyAnalyzer::getEffects`) and folded across
`CallResolver::getReachable`'s transitive set the same way `FunctionEffects` is.

**Known limitations, documented rather than hidden (full list in `Profitability.h`):** op counts
are static — a loop or branch inside a callee is counted once, not trip-count times, so
`compute_heavy.c`'s `heavy_elem` had to be written straight-line rather than as a loop, and this is
a real lower-bound risk for any future control-flow-heavy benchmark. All operations are weighted
equally (a divide costs the same as an add). Cache reuse across iterations isn't modelled. Mapped
regions always start at index 0, so a loop over `[1000, 2000)` would map 2000 elements, not 1000 —
none of the current benchmarks have a nonzero start, so this hasn't bitten yet. And the six
`MachineModel` defaults are order-of-magnitude estimates for a generic discrete PCIe GPU, not
measurements of any real device — the CLI overrides are the honest answer to that, not a claim the
defaults are correct.

**Verification:** all four demo benchmarks now report a profitability verdict —
`example.c`/`process` and both `saxpy.c` loops and `small_update.c` all correctly decline GPU
(`CPU_PARALLEL` or `SEQUENTIAL`), `compute_heavy.c`/`heavy_transform` lands unconditional
`GPU_OFFLOAD`. New fixture `tests/profitability_cases.c` (six loops: unconditional `GPU_OFFLOAD`,
conditional `GPU_OFFLOAD if (n >= 8192)`, two `CPU_PARALLEL` cases — low-intensity and strided —
`SEQUENTIAL` at trip 8, and a known-`UNSAFE` loop confirming the safety gate blocks profitability
outright rather than pricing and declining it) matches every expected verdict exactly, numbers
hand-derived before running the tool and confirmed to match to three decimal places. No regression
in `benchmarks/loop_shapes.c`/`loop_unrecognized.c`'s `LoopKind` classifications or
`tests/safety_cases.c`/`tests/call_chains.c`'s `SafetyVerdict`s — all unchanged from Days 8-10.

**Alternative considered:** a weighted 0-1 feature score (trip count, data volume, intensity,
pattern each normalized and summed against a cutoff), the option ROADMAP.md's original wording most
directly suggested. Rejected because the weights would be the least defensible numbers in the
entire project — "why is trip count weighted 0.35?" has no better answer than "it seemed
reasonable" — whereas every number the cost model produces traces back to one of six named,
sourced, overridable machine parameters.

---

### Day 14 — GPU pragma insertion: sibling file, declare target folded in, if(target:) guard

**Decision:** New `src/codegen/OmpRewriter`, wired behind a `-rewrite` flag, acts only on
`Evaluated && GpuOffload` verdicts. It writes to a sibling `<name>.omp.c` rather than in place, and
in the same pass wraps every transitively reachable callee of an offloaded loop in
`#pragma omp declare target` / `end declare target` — pulled forward from Days 15-16's scope. The
guard clause on a conditional verdict is emitted as `if(target: <GuardExpr>)`, not a bare `if(...)`.

**Reasoning:** The map clauses and guard condition are read straight off
`ProfitabilityVerdict::Regions` / `GuardExpr` — no second walk of the loop body, which is exactly
what Days 11-13 built those fields to make possible. Sibling-file output keeps the sequential
baseline diffable per this file's repo-layout convention, and means `-rewrite`'s absence leaves
every existing report byte-identical (verified against all nine benchmark/test inputs). Folding
`declare target` in now — rather than deferring it to Day 15 — means Day 15 starts from an actual
compile attempt instead of a guaranteed failure on the first call inside a target region: a target
construct cannot call a function without a device-side compilation, and `CallResolver::getReachable`
already produces the exact transitive set needed, the same walk `ProfitabilityAnalyzer::getOpCount`
makes. The `target:` modifier on `if` is required rather than stylistic — `target teams distribute
parallel for` is a combined construct where more than one constituent directive accepts `if`, so an
unqualified `if()` is ambiguous about which one it gates. That said, since the guarded construct
runs entirely on the host when the condition is false, this doubles as today's only taste of the
CPU-parallel path, without pre-empting Days 17-18's own pragma for genuinely `CPU_PARALLEL` verdicts.

Four cases are declined and recorded in `RewriteSummary::Skipped` rather than silently skipped or
mishandled, matching how `HasIndirectCall` and `SafetyVerdict::Unknown` are surfaced upstream: a
loop not written in the main file (LoopCollector has no such filter), a loop whose location is a
macro expansion (Rewriter's source-location model doesn't support rewriting there), a loop
lexically nested inside one already annotated (nested `target teams` is invalid OpenMP — untested
by current benchmarks, but free to guard against), and a callee with no main-file definition
(cannot be wrapped, and Day 15 will need another way to make it available to the device build).

**Verification:** all nine benchmark/test inputs produce byte-identical reports with `-rewrite`
omitted. `benchmarks/compute_heavy.c -rewrite` emits exactly the pragma the analysis report already
printed (`map(from: out[0:65536]) map(to: in[0:65536])`) with `heavy_elem` wrapped in
`declare target`. `tests/profitability_cases.c -rewrite` emits both the unconditional case and
`if(target: n >= 8192)` on the parameter-bound case, matching `GPU_OFFLOAD if (n >= 8192)` in the
report exactly; its known-`UNSAFE` loop is correctly left untouched. `saxpy.c`, `example.c`,
`small_update.c`, and `tests/safety_cases.c` all correctly annotate zero loops and write no file.
Both rewritten files pass `clang -fsyntax-only -fopenmp` cleanly.

**Known gap, deferred to Day 15 on purpose:** this machine has `libomp.dylib` (host OpenMP) but no
`libomptarget` and no discrete GPU, so a `-fsyntax-only` host-side parse is the strongest check
available locally — actually linking and running a device binary needs either different hardware or
an explicit "host-fallback build only" scope cut, and that decision belongs to Day 15, not today.

**Alternative considered:** in-place rewriting via `Rewriter::overwriteChangedFiles()`. Rejected —
it destroys the sequential baseline the repo layout is built around keeping alongside the tool's
output, and complicates the exact diff-based verification this pass depends on for correctness.

---

### Day 14 (cont.) — adversarial review of the pragma-insertion commit found eight real bugs; all fixed

**Decision:** Ran an adversarial review agent against the just-committed `OmpRewriter` diff before
calling Day 14 done, per standing practice. It found eight reproducible correctness bugs — two of
them capable of emitting a pragma whose `map()` clause describes the wrong memory region, which
`-fsyntax-only` cannot catch because the C is syntactically valid either way. All eight are fixed
in a follow-up commit; a ninth item (`-rewrite`/`-o` and the nine `MachineModel` flags all hidden
from `--help`) was fixed alongside them since the one-line cause was the same category mismatch.

**The two data-correctness bugs, both in `Profitability.cpp`'s extent computation, not the new
codegen file — the review is what made them observable as wrong emitted code rather than latent:**
- A loop bounded by `<=` (`for (i = 0; i <= 99; i++)`, 100 iterations) mapped only 99 elements —
  the extent text was the bound's literal value, silently one short of the trip count whenever the
  comparison was inclusive. Fixed: `Extent` is bumped by one when `LI.CmpOp == BO_LE`, on both the
  constant- and parameter-bound paths.
- A descending loop (`for (i = N-1; i >= 0; i--)`) mapped `[0:0]` — its "bound" is the *lower*
  limit, not an element count, and the extent model has no representation for that. Rather than
  extend the model under review pressure, `OmpRewriter::rewriteLoop` now declines any loop whose
  `CmpOp` is `BO_GT`/`BO_GE` and records why; reversed-traversal mapping is future work.

**The six codegen-file bugs, all in `OmpRewriter.cpp`:**
- The nested-loop guard used a strict `<` on end locations, so brace-less nesting
  (`for (...) for (...) body;`, where both `ForStmt`s share an end location) went undetected and
  produced invalid doubly-nested `target teams`. Fixed to an inclusive `contains()` test, shared
  now between the lexical-nesting check and the new dynamic-nesting check below.
- The pragma was inserted assuming `for` starts its own line. `if (c) for (...)` and a single-line
  function body both put a `#pragma` mid-line, which does not parse. Fixed with a `startsOwnLine`
  check (walks backward from the loop's `SourceLocation` to the nearest `\n`, failing on any
  non-whitespace) that only prepends a newline when the assumption doesn't hold — the common
  already-own-line case stays exactly as compact as before (verified byte-identical against the
  original `compute_heavy.omp.c`).
- Nothing stopped a function whose *own body contains* an offloaded loop from also being wrapped in
  `declare target` when called from a second offloaded loop — a target region invoked from inside
  another target region, invisible to `-fsyntax-only` because the nesting is dynamic (through a
  call), not lexical. `finalize()` now checks each `declare target` candidate's source range against
  every already-annotated loop's location and declines with a named reason.
- `InsertTextBefore`/`InsertTextAfterToken`'s `bool` returns (true on failure) were ignored at every
  call site. A macro-generated closing brace (`#define ENDF }`) reproduced this concretely: the
  opening `declare target` pragma was inserted, the closing one silently wasn't, and everything to
  EOF ended up inside an unclosed device region. Both declare-target inserts are now pre-validated
  with `Rewriter::isRewritable` before either runs (avoiding a half-applied pair), and the loop
  pragma's own insert is checked and skipped-with-reason on failure rather than assumed to succeed.
- `-o` with more than one input file silently let each translation unit overwrite the last one's
  output while printing a success line for both. `main` now rejects `-o` outright when the tool's
  source path list has more than one entry.
- `PendingDeclareTargets` is a `DenseSet<FunctionDecl*>`, so `finalize()`'s iteration order — and
  therefore the declare-target report lines — depended on pointer values rather than the program.
  Fixed by sorting into source order before processing; verified identical across five consecutive
  runs on a fixture with five declare-target candidates spanning two offloaded loops.

**Verification:** every fix reproduced from a failing case first (kept under
`/private/tmp/.../scratchpad/rev/`, not committed — throwaway fixtures, not benchmarks), then
re-checked passing after the fix, then the full nine-input regression sweep re-run end to end: the
*only* line that changed from the pre-review baseline is the intended `a[0:99]` → `a[0:100]` fix:
identical everywhere else, `compute_heavy.omp.c` regenerates byte-for-byte identical to the original
commit's version, and all four correct-refusal benchmarks still refuse.

**Reasoning for fixing all eight rather than triaging to "good enough for a demo":** every one of
them is silent — wrong output or an invalid file with no diagnostic pointing at the cause, which is
the worst failure mode for something that is about to be handed to an actual OpenMP offload compiler
on Day 15. A loop that gets skipped-with-a-reason is a known gap; a loop that gets a `map(from:
out[0:0])` and compiles clean is a bug someone finds by getting wrong numbers out of a device run,
possibly well into Day 15 or later, with much less signal about where it came from.

<!-- Add new entries below as you build. -->

---

### Days 15-16 — Three-tier verification without a GPU, and instrumenting the benchmarks

**Decision:** Build a three-tier evidence ladder (`scripts/build_and_run.sh` +
`scripts/check_maps.py`) that gets as close as this machine allows to DAY_BY_DAY.md's "compile
rewritten source with your OpenMP offload toolchain, get it to actually build" / "get it running and
producing output" — rather than declaring the day done on `-fsyntax-only` alone, or blocked outright
by the missing device.

**What's actually available on this machine, checked directly, not assumed:** Homebrew LLVM 22.1.8
ships `libomp.dylib` (host OpenMP runtime) but no `libomptarget`; `omp_get_num_devices()` returns 0
at runtime. Both device-arch probes fail explicitly — `nvptx-arch` returns nothing (no NVIDIA GPU),
`amdgpu-arch` reports "No AMD GPU detected". Forcing `--offload-arch=sm_70` fails at "cannot find
libdevice" (no CUDA toolkit installed); with `-nocudalib` it instead fails inside the NVPTX backend
("PTX version 4.2 does not support target 'sm_70'... minimum required PTX version is 6.0" — an
`ptxas`-version mismatch, not a codegen bug); `--offload-arch=gfx90a -nogpulib` crashes inside the
AMDGPU backend on a Mach-O section-specifier error (ROCm's ELF-oriented offloading sections aren't
meaningful on this Mach-O host, unrelated to the tool's own output). So real device execution is
correctly out of reach here, exactly as CLAUDE.md predicted going into Day 15 — but three narrower
checks all turned out to be possible and are what the ladder is built on:

- **Tier 1 (host build & run).** `clang -fopenmp -O2` on a rewritten `.omp.c` links against
  `libomp.dylib` alone and runs — `__kmpc_fork_teams`/`__kmpc_fork_call`/`__kmpc_for_static_init_4`
  are present in `nm`. With no `-fopenmp-targets`, OpenMP's own semantics make the host the initial
  device, so `target teams distribute parallel for` legitimately executes as 1 team × N host threads
  (confirmed via `omp_get_num_teams()`/`omp_get_num_threads()` printed from inside a probe region —
  1 and 10 respectively on this 10-core machine, following `OMP_NUM_THREADS`). This is conformant
  fallback behavior, not a trick.
- **Tier 2 (device codegen).** `-fopenmp-targets=nvptx64-nvidia-cuda --offload-arch=sm_52
  -nocudalib --offload-device-only -S` emits real NVPTX for a rewritten file, without needing a CUDA
  toolkit at all (`-nocudalib` skips only the libdevice math-intrinsics link, not codegen). This
  compiles the *device* side of the target region — the part `-fsyntax-only` never touches.
- **Tier 3 (map-clause verification).** `--offload-host-only -S -emit-llvm` lowers each `map()`
  clause to entries in `@.offload_sizes`/`@.offload_maptypes` — e.g. `compute_heavy.omp.c`'s
  `map(from: out[0:65536]) map(to: in[0:65536])` lowers to `[524288, 8, 524288, 8]` /
  `[0x22, 0x4000, 0x21, 0x4000]`, decoded per `llvm/Frontend/OpenMP/OMPConstants.h` as `from`+
  `TARGET_PARAM`/`ATTACH`/`to`+`TARGET_PARAM`/`ATTACH`, with 524288 = 65536 × 8 bytes exactly.
  `scripts/check_maps.py` automates this decode-and-compare against the pragma text the tool itself
  reported, so the two are checked against each other, not eyeballed.

**What each tier does not prove, stated explicitly because it's the load-bearing caveat:** Tier 1's
host-fallback execution does not exercise `map()` at all — on the host-as-device, map clauses
degenerate to no-ops over already-shared memory, so a completely wrong `map()` clause would still run
and still produce the right answer here. That is exactly why Tier 3 exists as a separate check rather
than being inferred from Tier 1 passing. Tier 2 proves the device side *compiles*, not that a real
device *executes* it correctly. Tier 3 proves what Clang's host-side runtime call will tell a real
offload runtime to move — not that the runtime moves it correctly, or that the values landing at the
device addresses are the right ones. None of the three is a substitute for running on an actual GPU;
that gap is inherited forward, not closed.

**A real, checked assumption that turned out false — declare-target's necessity on this compiler:**
CLAUDE.md's Day 14 entry states "a target region cannot call a function without a device-side
compilation, so without it Day 15 would open on a guaranteed compile failure." I tried to build a
negative control on exactly that claim: stripped `#pragma omp declare target` /
`#pragma omp end declare target` from a copy of `compute_heavy.omp.c` and re-ran Tier 2. It compiled
clean anyway, with `heavy_elem` still present as a device `.func` in the PTX. Pushed further with a
synthetic two-hop case (`heavy_elem` calling a second, also-unmarked `inner_helper`) — same result,
`inner_helper` also present in the device PTX. Homebrew Clang 22.1.8 implicitly promotes any
same-translation-unit function transitively reachable from a `target` region to device compilation,
with no `declare target` pragma required at all, contradicting the assumption as stated for this
specific toolchain version.

This does **not** make the tool's `declare target` wrapping pass wrong or dead code — it's still the
behavior the OpenMP spec requires generally (separate compilation, other compilers/older Clang
versions, indirect reachability the implicit analysis can't see through), and being explicit rather
than relying on one compiler's inference is the more defensible, portable design. But the specific
claim "without it, compilation guaranteed to fail" is now corrected to "without it, compilation is
not guaranteed to fail on Clang 22's implicit target-region analysis, but is not portable and is not
what the standard's separate-compilation model assumes" — and Tier 2's real, demonstrated negative
control turned out to be different from the one first attempted: stripping the pragma *line itself*
(as if a rewriter bug silently dropped an insertion the report claimed happened) does correctly FAIL
Tier 2's entry-count assertion — 0 `.entry __omp_offloading_` symbols in the PTX against the tool's
reported 1 pragma line — which is the assertion that actually has teeth on this toolchain.

**Making the benchmarks runnable at all.** None of the ten `.c` files in `benchmarks/`/`tests/`
contained a single `#include`, `printf`, or use of a computed result before this — `clang -O2 -S
benchmarks/compute_heavy.c` compiled `main` to `mov w0, #0; ret`, since nothing consumed
`heavy_transform`'s output. "Producing output" was structurally impossible. Fixed by adding
`#include <stdio.h>` plus a checksum-and-print to each of the four benchmarks that has a `main()`
(`compute_heavy`, `saxpy`, `example`, `small_update`) — `tests/*.c` stay analyzer-only fixtures with
no `main`, unchanged, since that was never their role.

Checked, not assumed, that this doesn't quietly change the safety/profitability analysis under test:
macOS SDK headers are declaration-only, so `#include <stdio.h>` adds zero loops to any report (only
requires the tool be invoked with `-isysroot $(xcrun --show-sdk-path)` so `stdio.h` resolves). The
checksum loop itself (`for (i) sum += arr[i]`) is a new loop each report gains exactly one of, and it
is correctly flagged `UNSAFE` — `loop-carried dependence: writes to 'sum', which is not declared
inside the loop body` — which is `SafetyAnalysis.h`'s documented reduction over-approximation working
exactly as designed, not a new gap. Verified with a full nine-input regression sweep: the four
untouched `tests/`/other `benchmarks/` fixtures are byte-identical to the Day 14 baseline; the four
instrumented benchmarks differ from baseline by exactly the expected one new `UNSAFE` loop block plus
shifted line numbers, nothing else moved. `benchmarks/compute_heavy.omp.c` was regenerated after the
edit; its pragma text (`map(from: out[0:65536]) map(to: in[0:65536])`) is unchanged — only the
`#include` and the checksum block were added around it.

No timing instrumentation added — deferred to Day 20, once the CPU-fallback path (Days 17-18) exists
to be measured against.

**Alternative considered:** Separate harness files (`benchmarks/harness/<name>_main.c`) instead of
editing the benchmarks in place, keeping the analyzed `.c` files pristine. Rejected: it would require
removing `main()` from each benchmark, which changes the analyzed report anyway (fewer loops, one
fewer function), so it doesn't actually avoid report churn — it just moves where the churn is, while
adding a second file and a second build step per benchmark for no offsetting benefit.

**Adversarial review found and fixed one real bug:** `has_offload` in `build_and_run.sh` was
determined by `[[ -f "$omp_src" ]]` alone. `p05tool --rewrite -o <path>` never deletes or truncates
`<path>` when nothing gets annotated, and `build/bench/` was never cleaned between runs — so a
`.omp.c` left over from an earlier invocation (different machine-model flags tried by hand, an older
commit, a benchmark that used to annotate) would be silently picked up and tested as if it were this
run's output. Reproduced directly: seeded a mismatched `.omp.c` at `build/bench/example.omp.c`
(`example.c` currently annotates zero loops) and confirmed the pre-fix script built and ran it under
`example`'s stage names regardless. Fixed with `rm -f "$omp_src"` immediately before regenerating,
plus deriving `has_offload` from the tool's own `nothing annotated` report text rather than file
presence — so a stale file surviving some other way still can't be mistaken for this run's output.
Re-verified against the same reproduction (file now absent after regenerate; the four-benchmark
sweep and `compute_heavy`'s full-tier pass are otherwise identical to before the fix).

### Days 17-18 — CPU-threaded fallback path: a solved guard, a targeted degrade, and a real verdict bug

**Decision:** Extend `OmpRewriter` to emit `#pragma omp parallel for` for `CPU_PARALLEL` loops, not
just `#pragma omp target teams distribute parallel for` for `GPU_OFFLOAD` ones. Three sub-decisions
made along the way:

1. Close a gap Days 11-13 had left open by design: a symbolic-bound `CPU_PARALLEL` verdict previously
   ended in "trip count is a runtime parameter and assumed not pathologically small" — an honest
   caveat, but an avoidable one. `Profitability.cpp` already solves the GPU-vs-CPU crossover in closed
   form for a symbolic bound; the CPU-vs-sequential crossover is the same algebra against a different
   pair of rates (`t_seq(n) = RateSeq * n`, `t_cpu(n) = RateCpu * n + ThreadStartUs`, solve for `n*`),
   so it costs nothing new to derive and removes a documented weak point. `ProfitabilityVerdict` gained
   `CpuBeatsSeq` (bool, computed for every evaluated loop) and `CpuGuardExpr` (the symbolic-bound
   threshold, mirroring `GuardExpr`), and the rewrite pass renders it as `if(parallel: n >= T)`.
2. Only one of `OmpRewriter`'s three existing GPU declines degrades to the CPU pragma instead of
   emitting nothing: a descending loop. The other two (macro-expansion location, lexically nested
   inside an already-annotated loop) are objections about the rewrite mechanism itself and apply
   identically to `parallel for`. The descending case is different in kind: it's declined specifically
   because `ArrayRegion`'s mapped-region model reads the loop's bound text as an element count, which
   a descending loop's lower limit is not (see Day 14's entry and `Profitability.h`'s `ExtentText`
   comment) — and `parallel for` has no `map()` clause at all, so that specific objection evaporates.
   Degrading is gated on the verdict's own `CpuBeatsSeq`, so a degraded loop is still a target the cost
   model endorses, not just "whatever's left when GPU doesn't work." The substitution is recorded as an
   explicit `Skipped` line (`"degraded to CPU-threaded ... instead of the GPU_OFFLOAD verdict"`), never
   silent — `RewriteSite` gained a `Kind` field so the report and `finalize()` can always tell which
   pragma actually landed at a given site, independent of what the original verdict said.
3. No `private()`/`reduction()` clauses on the CPU pragma. Not an oversight: `SafetyAnalyzer` already
   requires every scalar written in a loop body to be declared fresh inside it, and OpenMP predetermines
   the loop control variable private on its own, so there is no shared-scalar hazard left to clause
   against. A real reduction (`sum += a[i]`) is already `UNSAFE` under the existing body-dependence rule
   and never reaches this pass. Documented in `OmpRewriter.h` rather than left as an unexplained gap.

**A real bug found and fixed while extending this, not introduced by it:** the symbolic-bound branch's
final `else` — reached whenever the GPU rate didn't beat the CPU rate — chose `CPU_PARALLEL`
unconditionally, without ever checking whether host threading beat running the loop sequentially at
all. Every benchmark and fixture in this repo has `RateCpu < RateSeq` under the default machine model
(8 cores, 25% single-core DRAM share), so no verdict anyone had actually seen was wrong — but
`--cpu-cores=1 --single-core-dram-share=1.0` makes `RateCpu == RateSeq`, which both produces a verdict
the model's own numbers don't support *and* would have divided by zero in the new CPU-vs-sequential
crossover. Fixed by computing `CpuBeatsSeq` independently before the final decision and routing to
`SEQUENTIAL`, with a named reason, when it's false. Verified with the negative control the fix was
built for: `p05tool --cpu-cores=1 --single-core-dram-share=1.0 benchmarks/example.c` now reports
`SEQUENTIAL` for the loop that previously reported `CPU_PARALLEL` unconditionally.

**`small_update.c` still emits nothing, and that's correct, not a shortfall.** The Day 15-16 entry's
"Next" line predicted all three CPU-eligible benchmarks would gain pragmas once this landed. Two did
(`example.c`, `saxpy.c`); `small_update.c`'s only `SAFE` loop has trip count 8, and the cost model
correctly rules it `SEQUENTIAL` — no target recovers a 5μs thread-start cost over eight iterations.
Bending that benchmark to force a pragma would erase the one thing it exists to demonstrate (safe and
profitable are different questions), so `scripts/build_and_run.sh`'s SKIP reason for it was corrected
instead of the benchmark: "no parallelizable loop... correctly ruled SEQUENTIAL" rather than "CPU
fallback is Days 17-18."

**`scripts/build_and_run.sh` needed to tell GPU and CPU pragmas apart, not just count them.** Tier 1
(host build & run) doesn't care which pragma kind got a loop there, but Tiers 2-3 (device codegen,
map-clause verification) are meaningless for a `parallel for` with no `map()` clause and no device
offloading entry. The report's `^  line ` lines are split into `gpu_pragmas` (containing `omp target`)
and `cpu_pragmas` (the rest); Tier 1 gates on their sum, Tiers 2-3 gate on `gpu_pragmas` alone with a
SKIP reason naming why CPU pragmas don't apply, rather than the old single `has_offload` flag that
would have made Tier 2's entry-count assertion fail the moment `saxpy.c` gained CPU pragmas but no GPU
ones.

**Result:** `compute_heavy` is an unchanged full-tier PASS (regression check on the GPU path — it also
picked up a second, previously-unannotated `CPU_PARALLEL` loop, the array-init loop in `main`, since
that loop was never `GpuOffload` and so was invisible to Day 14's pass). `example` and `saxpy` go from
full SKIP to a genuine Tier 1 PASS: built, run, and checksum-diffed against the sequential baseline
(bit-identical — every rewritten loop here is element-independent with no reduction, so a mismatch
would have meant a real safety-analysis miss, not a rounding difference). `small_update` SKIPs for the
reason above. Two new fixtures added to `tests/profitability_cases.c`
(`cpu_conditional_case`, `gpu_offload_descending_case`) exercise the symbolic CPU guard and the
degrade path respectively — the descending case was additionally hand-verified end to end (compiled,
run, checksum-diffed against its own sequential form outside the fixture set) before being committed
as a regression fixture, the same discipline Day 14's review applied.

Full nine-input regression sweep (without `--rewrite`) confirmed byte-identical to the pre-Days-17-18
baseline except the new `cpu-vs-seq` reason lines and `if(...)` guards this change adds to existing
`CPU_PARALLEL` verdicts, and the two new fixture cases — no `GPU_OFFLOAD`, `SEQUENTIAL`, or `UNSAFE`
verdict anywhere flipped.

### Days 17-18 (cont.) — adversarial review of the CPU-fallback commit found one real, silent, compile-breaking bug

**Bug:** `LoopAnalysis.cpp` only populates `LoopInfo::BoundText` for `Kind == VariableBound`. A
`ConstantBound` loop with an unknown `TripCount` — a shape `CLAUDE.md` explicitly documents as
expected ("a `CONSTANT_BOUND` loop can still have an unknown trip count (variable start value)") —
reads `BoundText` as an empty string, since there's nothing symbolic about its *bound* to print; what's
unknown is its *start* value (e.g. `for (int i = start; i < 8192; i++)` with `start` a parameter), and
`LoopInfo` never captured that expression's source text. Both `Profitability.cpp`'s symbolic branch
(pre-existing `GuardExpr`, and this commit's new `CpuGuardExpr`) built guard text as
`LI.BoundText + " >= " + T` with no check that `BoundText` was actually populated, producing
`if(target:  >= 8192)` or `if(parallel:  >= 512)` — text Clang correctly rejects with `error: expected
expression`, pointing at the empty left operand.

This defect predates Days 17-18 (the GPU `GuardExpr` path was already exposed to it), but had **zero
prior blast radius**: no existing benchmark or fixture has a symbolic-start loop, so it never fired.
Days 17-18's new code opened two *new* paths into it — `CpuGuardExpr` on a direct `CPU_PARALLEL`
verdict, and, more importantly, the new descending-loop degrade: every descending loop was previously
declined outright by `OmpRewriter` regardless of this defect, so a descending symbolic-start loop had
literally never reached codegen before; the new degrade path is what turned a latent, un-triggerable
bug into one that actually wrote a broken `.omp.c` file and reported success while doing it.

**Caught by:** the adversarial-review subagent this repo's working conventions call for after any
non-trivial implementation — it re-derived the crossover algebra by hand, then built a synthetic
descending/symbolic-start input specifically to probe the interaction between the new degrade path and
the pre-existing `BoundText` handling, compiled the tool's own output, and got a real compiler error.
Exactly the kind of thing a `-fsyntax-only` check on the *tool itself* can't catch, since the bug is in
what the tool writes, not in the tool's own source.

**Fix:** rather than reconstruct the missing trip-count expression textually (`(8192) - start`) — a
real fix, but one that requires capturing the start expression's source text in `LoopInfo` and handling
`CmpOp`/off-by-one correctly for a formula that mixes a symbolic start with a constant bound, which is
more surface than this bug warrants fixing under right now — `ProfitabilityAnalyzer::analyzeLoop` now
declines to price a loop any further once it's established there's no printable trip-count expression to
guard on (`!TripCount && Kind != VariableBound`, checked once, ahead of both branches that would
otherwise build guard text from it). Matches the tri-state philosophy already used elsewhere in this
codebase — `SafetyAnalyzer` returns `Unknown` rather than guess when it "cannot see this far"; this loop
returns `SEQUENTIAL` with a named reason rather than fabricate a guard neither branch can actually
render. The ops/byte facts already computed by that point (they don't depend on `Start`) stay in the
report; only the trip-count-dependent decision is declined.

**Verified:** the exact failing repro (`for (int i = start; i >= 0; i--)` calling a high-intensity
kernel, `start` a parameter) now reports `SEQUENTIAL` with the new reason and emits no pragma at all,
for both the descending/CPU-degrade path and the pre-existing ascending/GPU-guard path tried against the
same underlying defect. A permanent regression fixture,
`tests/profitability_cases.c`'s `unguardable_bound_case`, pins this. Full nine-input regression sweep
confirmed byte-identical classification to before the fix on every existing benchmark and fixture — the
defect had no prior exposure, so nothing else could have moved. `scripts/build_and_run.sh`'s full ladder
re-run clean after the fix.

### Day 19 — Exact per-element output comparison, replacing the checksum as the authoritative check

**Decision:** `scripts/build_and_run.sh`'s `tier1-diff` stage only ever diffed each benchmark's
printed stdout, which is a single scalar: `sum += arr[i]` over the whole output array, printed as
`printf("%.12e")`. That's a real check, but a weak one — a plain additive sum cannot distinguish a
correct run from one with a swapped element pair, a permutation, or a pair of compensating errors
(`+δ` at index `i`, `-δ` at index `j`); all three leave the sum unchanged. This matters most for the
two 65536-element benchmarks (`saxpy.c`, `compute_heavy.c`), where a subtle per-element bug is far
more plausible to hide behind an unchanged sum than in `small_update.c`'s 8-element case. Added a
genuine per-element comparison (`scripts/compare_outputs.py`) and a new `tier1-exact` stage that
runs it, kept `tier1-diff` alongside it (demoted to a coarse smoke check in the script's header
comment, not removed — it's cheap, exercises a different code path, and Days 15-18's NOTES.md
entries already cite "checksums matched" as part of the recorded evidence chain).

**How the benchmarks dump their output, and why this specific mechanism:** each benchmark's `main()`
now optionally writes its raw output array (`fwrite`, no formatting) to a path given as `argv[1]`.
Two constraints shaped this over the more obvious alternatives:

1. It must not perturb what the tool has already verified about these files. `LoopCollector`
   (`src/analysis/LoopAnalysis.h:24-35`) only overrides `VisitForStmt`, and never looks at a
   `CallExpr` outside a loop body — so a single `fwrite` call placed after every existing `for` loop
   in a benchmark is structurally invisible to the loop finder and everything downstream of it
   (safety, profitability). It cannot become a new candidate loop in the tool's report, which is a
   *stronger* inertness guarantee than Days 15-16's checksum loop, which did add a new (correctly
   `UNSAFE`) loop to every benchmark's report. A per-element `fprintf` dump was considered and
   rejected on ordinary engineering grounds — it needs either a new `for` loop (a direct violation)
   or one unrolled call per element, which is absurd at 65536 elements; raw `fwrite` of the array's
   own memory is the only "single call, no loop" mechanism that captures the whole array, and since
   both binaries run on the same machine there's no endianness/format concern to handle.
2. The trigger mechanism went through one real revision during implementation: an env var
   (`P05_DUMP_PATH`, read via `getenv`) was the original plan, gated behind a new
   `#include <stdlib.h>` next to the existing `<stdio.h>`. That fails the actual bar this change was
   held to — a before/after diff of the tool's own `--rewrite` report on each benchmark had to be
   *empty*, not "the same modulo shifted line numbers" — because inserting any new line above the
   analyzed loops shifts every subsequent loop's reported `line:col` by one, even though nothing about
   loop classification changed. Caught immediately by actually running that diff (not assumed) before
   moving on. Fixed by switching to a CLI argument instead: `fopen`/`fwrite`/`fclose` are already
   declared via each benchmark's existing `<stdio.h>` include, so the only change needed was editing
   `int main()` to `int main(int argc, char **argv)` **in place** on its existing line — no line
   insertion anywhere above the loops, hence a genuinely empty report diff. `getenv`/env-var-gating was
   also considered as a fix (via a local `extern char *getenv(const char *);` declaration to avoid the
   `<stdlib.h>` include) but rejected as needlessly hacky next to the argv approach, which is more
   idiomatic C and reads naturally as "pass a dump path as an optional argument."

**Why exact/bit-identical comparison, not a tolerance:** every rewritten loop in this benchmark set
is element-independent (a map, no reduction), and the pre-existing checksum-based `tier1-diff` had
already shown bit-identical results for every runnable benchmark. There's no known legitimate source
of floating-point nondeterminism between the `-O2` and `-O2 -fopenmp` builds here (no FMA-contraction
or reduction-ordering difference applies to a loop with no cross-iteration accumulation) to tolerate.
If a real mismatch ever shows up, that's new information to investigate, not something to
pre-emptively paper over with a tolerance chosen without evidence.

**Why Python (`compare_outputs.py`) over `cmp`:** `cmp -l` reports byte offsets in octal, not an
element index or the actual mismatched values — useless for debugging a real failure, which the task
explicitly needs (first differing index, both values). Python3 is already a `build_and_run.sh`
dependency via `scripts/check_maps.py` (Tier 3), so this adds no new toolchain dependency. Comparison
uses `struct.pack(...) != struct.pack(...)` rather than `==`, so `NaN`/`-0.0` compare bit-for-bit
rather than by IEEE-754 comparison semantics (`NaN != NaN`, `-0.0 == 0.0`), which would silently
under- or over-report a mismatch for either value if it ever occurred.

**Verified, not assumed:** before/after `p05tool` report diff on all four benchmark `.c` files
(`example`, `saxpy`, `small_update`, `compute_heavy`) confirmed byte-for-byte empty after the argv
fix, and a full regression run (no `--rewrite`) across every `benchmarks/`+`tests/` input confirmed
no verdict anywhere moved. `compare_outputs.py` was sanity-checked directly against a deliberately
corrupted dump (a single flipped double) before being wired into the script, to confirm it actually
reports FAIL with the correct index/values rather than passing everything by construction.

**Result:** `scripts/build_and_run.sh`'s full ladder, re-run end to end: `tier1-exact` PASSes for
`example` (1000/1000 doubles exact), `saxpy` (65536/65536), and `compute_heavy` (65536/65536) — the
two large benchmarks are exactly the ones this check adds genuinely new evidence for, since a
checksum collision was only ever plausible at that size. `small_update` still has no `.omp` binary
(unchanged fact from Days 17-18: its only `SAFE` loop is trip-count 8, correctly ruled `SEQUENTIAL`),
so `tier1-exact` SKIPs for it with the same reason `tier1-omp` already gave — brought into line with
the script's own stated philosophy ("every unavailable check prints an explicit SKIP with a reason"),
which `tier1-diff`/the new `tier1-exact` weren't actually following for that branch before this
change. No real correctness bug was found in the *analyzed pipeline* — expected, given the prior
bit-identical checksums and the element-independent loop shapes, but this had to be run for real to
say so, per CLAUDE.md's own "fix correctness bugs here, this is non-negotiable."

### Day 19 (cont.) — adversarial review of the validation-tooling commit found one real bug, in the harness itself

**Bug:** `scripts/build_and_run.sh`'s new `tier1-exact` block ran `"$seq_bin"`/`"$omp_bin"` with a dump
path as their sole argument but never checked either invocation's exit status, and — unlike `$omp_src`
a few lines above it (`rm -f "$omp_src"`, with a comment explaining exactly this class of problem for
that file) — never removed a pre-existing `.seq.dump`/`.omp.dump` before running them. So a binary that
crashes or otherwise fails to write its dump on a given run leaves the *previous* run's (valid) dump
file sitting at the same path, and `compare_outputs.py` would then silently compare that stale file
against the other binary's fresh output and report `PASS`, never noticing the current run produced
nothing at all.

**Caught by:** the adversarial-review subagent this repo's working conventions call for after any
non-trivial implementation (same practice as Day 14 and Days 17-18). It reproduced the failure
concretely rather than reasoning about it in the abstract: ran the full ladder once to populate a real
`compute_heavy.omp.dump`, then substituted a fake binary that exits 139 (simulating a crash) in place
of `omp_bin` and re-ran the script's exact `tier1-exact` logic — the stale dump from the earlier good
run was compared against the sequential output and reported `PASS: 65536 doubles match exactly`, even
though the "current" OpenMP run produced nothing.

**Why this had no prior analogue to warn from:** the exact `rm -f "$omp_src"` pattern this bug
recreates was already fixed once in this file, for a different file, during Days 15-16 (see that
entry above) — but the fix wasn't generalized into a rule ("always clear a stage's output path before
regenerating it, don't rely on it being absent") at the time, so the same shape of bug reappeared here
when a second stage was added that also writes to a path across runs.

**Fix:** `rm -f "$OUTDIR/$name.seq.dump" "$OUTDIR/$name.omp.dump"` immediately before both invocations
(mirroring the existing `$omp_src` pattern directly), plus capturing each binary's exit status
explicitly and recording a named `tier1-exact` `FAIL` ("dump run failed -- seq exit N, omp exit M")
ahead of ever invoking `compare_outputs.py` when either is nonzero, rather than only relying on
`compare_outputs.py`'s wrong-byte-length check to eventually notice a missing dump (that fallback also
works, from a clean `$OUTDIR`, but produces a confusing Python traceback instead of a clear message,
and doesn't fire at all against a stale file from a dirty `$OUTDIR` — which is exactly the failure mode
that made this bug possible).

**Verified:** re-ran the same reproduction (fake-crash `omp_bin` against a pre-existing valid dump)
against the fixed script logic directly — now correctly reports `tier1-exact FAIL (dump run failed --
seq exit 0, omp exit 139)` instead of a false `PASS`. Full ladder re-run clean afterward (all four
benchmarks, same PASS/SKIP pattern as before the fix — the fix only changes behavior on a failure path
that the normal run never exercises).

### Day 20 — Real wall-clock timing, sequential vs. gated-policy, measured against the model's own predictions

**What was added:** each benchmark's `main()` now brackets its one headline call — the call that
enters the loop crossing a function boundary (`process`/`saxpy`/`normalize`/`heavy_transform`) — with
`clock_gettime(CLOCK_MONOTONIC, ...)` and prints `TIMING <name> <us>` to stderr. `scripts/
build_and_run.sh` gained a `measure_timing_us()` helper that runs a built binary `TIMING_ITERS`
(default 7) times, discards stdout, parses the stderr TIMING line each time, and reports the **min**
across runs — the standard way to see past scheduler-jitter noise at microsecond scale without
touching the benchmark's own logic (each repeat is a fresh process, so there's no in-process
re-application of a non-idempotent transform like `clamp_unit` to worry about, unlike repeating the
call *within* one run would create). New `timing-seq`/`timing-omp`/`timing-speedup` stages record the
result per benchmark, `timing-omp` correctly `SKIP`s for `small_update` (no `.omp` binary exists for
it, same fact as every other Day-19-era `SKIP` there).

**Design decision, and a course-correction caught before it shipped (same practice as Day 19's
getenv→argv pivot):** the first approach tried was `-include time.h` as an extra clang arg (fed to
`p05tool`'s own parse and to every build in `build_and_run.sh`) rather than editing any benchmark's
`#include` list — this would have kept every analyzed loop's `line:col` in the report byte-for-byte
unchanged, the same bar Day 19 held itself to. It worked mechanically (verified: `p05tool --rewrite`,
the host `-O2`/`-O2 -fopenmp` builds, all succeeded with it). But it was rejected anyway, for a reason
the line-count bar doesn't capture: it makes the benchmark **not standalone-compilable** — running
`clang benchmarks/saxpy.c` directly, without the special flag, now fails with `undeclared identifier
'CLOCK_MONOTONIC'` (confirmed directly, and independently by the editor's own diagnostics flagging
exactly that on the instrumented file). Every benchmark this project claims to run on is supposed to
be "unmodified, ordinary sequential C" — a benchmark that only compiles with a project-specific flag
quietly contradicts that framing, and would confuse anyone (a judge included) who compiled it by hand.
Switched to a real `#include <time.h>` at the top of each file instead, accepting the consequence:
every loop's reported `line:col` shifts by a uniform **+1** in that file (the include is the first
new line, nothing else moves). Verified this is genuinely benign, not assumed: a before/after
`p05tool` report diff on all four benchmarks shows *only* the line-number lines changing (always by
exactly +1, always consistently) — every verdict, pragma, reason string, and byte of report text
otherwise identical. The bar this change is held to is therefore "verdicts unchanged," not "line
numbers unchanged" — a deliberate, narrower redefinition of Day 19's bar for this specific case,
not an abandonment of it.

**Mandatory fix alongside this, not optional polish:** `build_and_run.sh`'s existing `tier1-seq`/
`tier1-omp` run lines merged stdout+stderr (`2>&1`) into the files `tier1-diff` compares. Once the
binaries print a TIMING line to stderr on every invocation, that merge would make `tier1-diff` FAIL
permanently, on every run, regardless of correctness (the omp binary's measured microseconds always
differ from the seq binary's). Changed both to `2>/dev/null` — the checksum-comparison run was never
meant to carry timing data anyway; `measure_timing_us`'s own dedicated repeat-loop invocations are
where timing is actually captured, separately.

**Measured, not modelled — the actual numbers (median of 3 confirmation runs, each already a min of
7):**

| benchmark | measured seq | measured omp (gated policy) | speedup | modelled t_seq | modelled t_cpu | modelled t_gpu |
|---|---|---|---|---|---|---|
| example (n=1000, guarded) | ~0us | ~220us | n/a (see below) | guard-only | — | — |
| saxpy | ~12us | ~31us | **0.4x — slower** | 314.573us | 83.643us | 143.319us |
| small_update | ~0us | no `.omp` binary | n/a | 0.026us | 5.006us | 10.011us |
| compute_heavy | ~215us | ~82us | **2.6x — faster** | 1310.720us | 168.840us | 107.867us |

Three things the raw table doesn't say on its own, each checked against the actual report/build
output rather than assumed:

1. **`example`'s ~220us is not thread-dispatch cost — it's OpenMP runtime cold start.** `process`'s
   pragma is guarded (`if(parallel: n >= 4096)`), and the call site here passes `n=1000`, below the
   threshold: the guard correctly keeps this loop's execution serial at runtime, exactly as designed.
   The measured ~220us instead reflects that this is the *only* OpenMP construct anywhere in
   `example.omp.c` (1 CPU-threaded pragma, confirmed from the report) — so it's the first (and only)
   time this process ever touches libomp, paying that library's one-time thread-pool/runtime setup
   cost once, at that call, regardless of the guard. This is a genuine measurement of a real cost
   this project's cost model has no term for at all (`MachineModel::ThreadStartUs` prices *entering a
   parallel region*, not *initializing the runtime for the first time in the process*) — worth stating
   plainly rather than reading it as "the guard didn't help": it did exactly what it was supposed to
   (kept 1000 elements off a thread team), and this number would not reappear on a second call in the
   same process.

2. **`saxpy`'s CPU-threaded pragma measures genuinely slower than sequential, contradicting the
   model's own prediction that it should win (`t_cpu=83.643us < t_seq=314.573us`).** Unlike `example`,
   `saxpy.omp.c` has *two* pragmas (the array-init loop in `main()` and the bracketed `saxpy()` call),
   so by the time the bracketed call runs, libomp is already warm (the init loop's own pragma paid
   that cost moments earlier in the same process) — this is measuring steady-state per-region dispatch
   overhead, not cold start, and it's still ~2.5x slower than running the 65536-iteration loop
   sequentially. At 2 flops/iteration and only 24 bytes touched per iteration, the actual work is
   trivial enough that thread-team wake/distribute/join overhead dominates outright — real evidence
   that `MachineModel::ThreadStartUs = 5.0` us understates real per-region overhead on this machine by
   roughly an order of magnitude for a region this cheap, not just at the margin. This is the single
   most important finding of the day: the model's *decision* (CPU_PARALLEL) is not confirmed by
   measurement here — it's contradicted. Nothing in this pass changes the model (out of scope,
   confirmed with the user — recalibrating ~9 parameters off 3-4 data points is not defensible), but
   the disagreement itself is now real, checked evidence, not a guess, and belongs in the finale
   write-up as an honest known limitation rather than something to paper over.

3. **`compute_heavy`'s ~82us omp measurement is not a GPU number and must not be compared to the
   model's `t_gpu`.** `build_and_run.sh`'s Tier 1 builds `omp_bin` with plain `-fopenmp` (no
   `-fopenmp-targets=nvptx64-nvidia-cuda` — confirmed by reading that build line directly), so the
   `target teams distribute parallel for` region runs on "the host device" per the OpenMP spec:
   genuinely multi-threaded, but the `map()` clauses' data motion is a no-op on the host, so none of
   the modelled `GpuTransferUs` (~87 of the modelled 107.867us `t_gpu`) is actually paid here. The
   honest comparison is against the model's `t_cpu` (168.840us) — and the measured ~82us beats even
   that, meaning real vectorized/threaded execution of this compute-bound kernel outperforms the
   model's own CPU estimate by roughly 2x. Read together with finding 2, this says something coherent
   about the model as a whole: its *decisions* (which target wins) are not obviously wrong — the
   compute-bound loop really is the one that benefits from parallelism, exactly as `GPU_OFFLOAD`
   claims — but its *absolute magnitudes* are order-of-magnitude estimates exactly as
   `Profitability.h`'s own header comment already says ("not measurements of any particular
   machine"), and this is the first time that gap has actually been measured and quantified rather
   than just disclaimed.

**`small_update`'s ~0us seq measurement** is expected, not a harness bug: the model's own `t_seq`
estimate for this loop is 0.026us (26 nanoseconds) — below `clock_gettime`'s practically observable
resolution on this machine at the `%.3f`-microsecond precision printed here. This is itself the
correct data point: the cost model declined to parallelize an 8-element loop, and real measurement
confirms there's essentially nothing there to parallelize.

**Verified, not assumed:** full ladder (`scripts/build_and_run.sh`) re-run five times back to back
across this change — the *qualitative* result is stable every time (`saxpy`'s omp path is always
slower than seq, `compute_heavy`'s is always ~2.5-3x faster, `example`'s guard always keeps it near
`tier1-seq`'s own time floor), and `saxpy`/`compute_heavy` land within a few microseconds run to run
(the table above reports the median of three of those runs). `example`'s ~200-270us cold-start number
carries wider absolute variance (system-dependent OS-thread/runtime-init noise), which is itself
consistent with what finding 1 above claims it's actually measuring — a one-time setup cost, not a
steady-state one. Every pre-existing PASS/SKIP unchanged from before this change, no new FAILs
anywhere. Before/after `p05tool --rewrite` report diff on all four benchmarks confirmed the only
differences are the uniform `+1` line-number shift described above — no verdict, pragma, or reason
string moved.

**Adversarial review of this commit** (same practice as Days 14, 17-18, and 19) found one real bug in
`measure_timing_us`'s first version, in its exit-status check rather than in the benchmarks
themselves: it read `$bin`'s exit code back out of `${PIPESTATUS[0]:-$?}` after
`us=$("$bin" 2>&1 >/dev/null | grep ... | awk ...)` — but with `set -o pipefail` and three pipeline
stages, that only ever reports the *rightmost* non-zero exit code among them, not `$bin`'s specifically.
Reproduced concretely: a fake binary built to `exit(139)` with no stdout/stderr output at all (no
TIMING line) still correctly triggered the hard-fail (so this was never a silent-pass bug, unlike both
of Day 19's), but reported `"exited 1"` — that's `grep`'s own "no match found" exit code, not the
binary's real 139, because `grep` matching nothing was the rightmost non-zero stage once the crash
happened before any output was written. Fixed by running `$bin` directly (not through a pipe) with its
stderr captured to a temp file, checking `$?` straight from that invocation, then separately grepping
the temp file for the TIMING line — removes the pipefail ambiguity entirely, and each failure mode now
gets its own accurate message. Verified against the same fake-crash binary: now correctly reports
`"exited 139"`. The min-of-N tracking logic (a small `awk -v a=... -v b=... 'BEGIN{exit !(a<b)}'`
comparison per iteration) was checked separately against a fixed, non-noisy input sequence
(`11 10 12 9 15 → 9`) to confirm it actually tracks a minimum rather than trusting real (noisy) binary
timings to happen to exercise every branch.

**Explicitly out of scope for this pass (confirmed with the user before starting):** no change to
`ProfitabilityAnalyzer`/`MachineModel`'s constants or decision logic. This adds *measured evidence
alongside* the model, it does not recalibrate the model from it — three data points is not enough to
responsibly refit nine parameters, and `DAY_BY_DAY.md`'s Day 20 line asks to "measure timing... record
real numbers," not rebuild the cost model.

Next: Day 21 (if time allows, per `DAY_BY_DAY.md` — not started) — a naive "offload everything safe"
comparison, for a three-way story (baseline vs. naive vs. gated policy) rather than today's two-way
one. Not designed yet; would need its own decision about what "naive" means concretely for this tool
(e.g. GPU target for every SAFE loop regardless of the profitability verdict) before it could be
measured the same way today's numbers were.

---

### Day 21 — the naive "offload everything safe" comparison, three-way instead of two

**Decision: what "naive" means for this tool.** Every loop the *safety* pass cleared
(`SafetyVerdict::Safe`) is GPU-offloaded unconditionally — `#pragma omp target teams
distribute parallel for` + `map()` clauses, with the runtime `if(target:)`/
`if(parallel:)` guards dropped, because a guard is itself a profitability judgement
naive is defined to skip making. `ProfitabilityAnalyzer` gained a `Policy` enum
(`Gated`/`Naive`, `Profitability.h`) and a `-policy` flag; the analyzer's old
`analyzeLoop` body was renamed `priceLoop` (unchanged) and a new `analyzeLoop` runs
`priceLoop` then `applyPolicy`. Under `Naive`, `applyPolicy` overrides `V.Target` to
`GpuOffload` and clears `GuardExpr`/`CpuGuardExpr` — but only *after* `priceLoop` has
already computed the full gated cascade, so the report always shows both the
cost-model verdict it's overriding and the override itself, not just the naive
outcome. `Policy` defaults to `Gated`, threaded through the same
factory→action→consumer chain `MachineModel` already uses, so every call site
without `-policy` — every run before today — is unaffected.

Two things naive still respects, both correctness limits rather than profitability
opinions: the safety gate (`!V.Evaluated` returns immediately — an UNSAFE or UNKNOWN
loop is never forced) and loops with no printable `map()` extent (the
`ConstantBound`-with-unknown-start case documented in the Days 15-16 entry — nothing
valid to write, so naive declines exactly like gated does, with a named reason).
`OmpRewriter` needed zero changes: it consumes only a `ProfitabilityVerdict`, and a
naive verdict is shaped exactly like a gated one. `CpuBeatsSeq` is deliberately left
as `priceLoop` computed it — `OmpRewriter.cpp` consults it independently (e.g.
whether a descending GPU loop may degrade to a CPU pragma), and forcing it true under
naive would let this pass emit a pragma the cost model itself contradicts, exactly
the invariant the gated path already enforces.

**Harness:** `scripts/build_and_run.sh` gained a second loop over `BENCHMARKS`,
run only after the existing gated ladder finishes for every benchmark, so it cannot
shift a pre-existing gated PASS/SKIP line. New stages: `naive-regenerate`
(`p05tool --rewrite --policy=naive`), `naive-build` (`-O2 -fopenmp`, same flags as
the gated `tier1-omp` build), `naive-exact` (per-element vs. the sequential dump,
same rm-first/exit-status-checked-before-compare discipline Day 19's adversarial
review established for `tier1-exact`), `timing-naive`, and `policy-compare` (the
three-way `seq / naive / gated` line, with an explicit `n/a` when the gated policy
never built an `.omp` binary at all rather than reproducing `timing-speedup`'s silent
`0.00x` from a `0/0` divide).

**Adversarial review of this commit caught one real, deterministic bug.** The
existing gated loop only ever wrote `$name.seq.dump` inside its `total_pragmas != 0`
branch (as part of `tier1-exact`). `small_update` never enters that branch — the
gated policy correctly declines its only SAFE loop — so on a genuinely fresh
`build/bench` the naive lane's `naive-exact` stage FAILed with "no sequential dump,"
taking the whole script's exit code nonzero. Reproduced directly:
`rm -rf build/bench && ./scripts/build_and_run.sh` → `small_update naive-exact FAIL
(no sequential dump ...)`. A second, worse symptom hid behind the first: on a *dirty*
`build/bench` (the normal case — re-running the script twice), a stale
`$name.seq.dump` left over from some earlier run silently satisfied the `-f` check
and let `naive-exact` PASS against stale data — exactly the anti-pattern Day 19's own
`tier1-exact` fix was written to prevent, just reintroduced one level up by the new
lane trusting whatever the gated loop happened to leave behind instead of owning its
own dump. Fixed by adding an unconditional `seq-dump` stage right after `tier1-seq`
(before the `total_pragmas == 0` early-out), with its own `rm -f` + direct
exit-status check — every benchmark now gets a fresh sequential dump regardless of
what the gated policy decides to do with it; `tier1-exact` still does its own
independent `rm -f` + regenerate of the same file for benchmarks that reach it,
harmless duplicate work that keeps that stage's existing discipline untouched.
Verified fixed the same way it was reproduced: `rm -rf build/bench &&
./scripts/build_and_run.sh` now reports `small_update naive-exact PASS (8 doubles
match exactly)` and exits 0. Everything else the review checked — `applyPolicy`'s
UNSAFE/UNKNOWN passthrough, the `--policy` flag's default-Gated byte-identical
output, `--policy=bogus` failing loudly rather than silently defaulting, the
`gated_status`/`extract_us` shell helpers, no state leaking across the two separate
`for` loops — came back clean, verified by actually running the tool rather than
just reading the diff.

**Verified, not assumed:**
- Regression sweep: `p05tool` (no `-policy`, no `-rewrite`) against every
  `benchmarks/`+`tests/` input, diffed against the pre-Day-21 baseline captured
  before any code changed — byte-identical, all nine inputs.
- Discrimination, both directions: `--policy=naive` on `benchmarks/small_update.c`
  flips its trip-count-8 loop from `SEQUENTIAL` to `GPU_OFFLOAD` with the override
  named in `Reasons`; the same flag on `tests/safety_cases.c` leaves every
  `UNSAFE`/`UNKNOWN` loop exactly as `SEQUENTIAL`/"not evaluated" as under `Gated` —
  a policy that only ever says yes would prove nothing, same rule this project has
  applied to every prior pass.
- All four naive-rewritten benchmarks compile clean under `-O2 -fopenmp` and pass
  `naive-exact` (per-element bit match against the sequential dump) — naive is
  correct, not merely different, which is what lets the timing numbers below mean
  anything.
- Full ladder (`rm -rf build/bench && ./scripts/build_and_run.sh`) run 6 times
  across this change (including the pre-fix reproduction and its fix); every
  pre-existing gated PASS/SKIP unchanged, no new FAILs after the seq-dump fix.

**Measured, not modelled — median of 3 confirmation runs (each already a min of 7,
same discipline as Day 20):**

| benchmark | seq | naive | gated | result |
|---|---|---|---|---|
| example (n=1000) | ~0us | ~30us | ~228us | naive **~7.6x faster** than gated |
| saxpy | ~11us | ~29us | ~32us | tie (~1.1x, noise-level) |
| small_update | ~0us | ~346us | n/a (declined) | gated **correctly pays nothing** |
| compute_heavy | ~211us | ~83us | ~80us | tie (~1.0x, noise-level) |

Three things this table doesn't say on its own:

1. **`small_update` is the clean, intended case, and it lands exactly as designed.**
   An 8-element loop: naive pays ~346us to offload it for no benefit, gated correctly
   recognizes there's nothing to gain and emits no pragma at all, costing ~0us. This
   is the one benchmark built specifically to be "safe but not profitable"
   (`benchmarks/small_update.c`'s own header comment), and it's the one where the
   three-way comparison says exactly what the brief wants gating to prove.

2. **`saxpy` and `compute_heavy` tie because this machine has no real GPU, not
   because the policies agree.** Naive forces both benchmarks' loops to
   `GPU_OFFLOAD`; gated already chose `GPU_OFFLOAD` for `compute_heavy`'s headline
   loop (compute-bound, clears the break-even intensity) and `CPU_PARALLEL` for
   `saxpy`'s (memory-bound, declined for GPU — see Days 11-13). But Tier 1's `.omp`
   binary is built with plain `-fopenmp`, no `-fopenmp-targets` (same fact Day 20's
   finding 3 already established): a `target teams distribute parallel for` region
   runs on "the host device" per the OpenMP spec, genuinely threaded but with no real
   PCIe transfer to pay. On this host-fallback build, `GPU_OFFLOAD` and
   `CPU_PARALLEL` pragmas dispatch through the same libomp fork-join path, so their
   measured cost is nearly indistinguishable — confirmed directly: `compute_heavy`'s
   headline-loop pragma text is byte-identical between `compute_heavy.omp.c` and
   `compute_heavy.naive.c` (it was already unconditional `GPU_OFFLOAD` under gated),
   and the two runs' timings track within noise. This is a real limitation of what
   this evidence can show, not a wash for the policy design — a real GPU would make
   naive's `saxpy` decision (offload a memory-bound loop with 2 flops/24 bytes) pay
   real PCIe transfer cost the model already prices at 143.319us (`t_gpu`, Day 20's
   table), which this host-fallback measurement structurally cannot reproduce.

3. **`example` is the day's genuinely surprising, checked result, and it needs to be
   read carefully, not at a glance.** Naive measures *faster* than gated here — by a
   wide, reproducible margin (6.5x-14x across 6 runs) — which looks backwards for a
   comparison meant to vindicate gating. The explanation is not that gating made a
   bad call: `example.c`'s call site passes `n=1000`, below the gated pragma's own
   `if(parallel: n >= 4096)` guard, so the guard is doing exactly its job (keeping
   1000 elements off a thread team). The explanation is Day 20 finding 1, now shown
   to have a sharper and more costly consequence than that entry described. Under
   gated, `process()`'s pragma is the *only* OpenMP construct anywhere in
   `example.omp.c` (confirmed: `grep pragma example.omp.c` shows one line) — so the
   guard evaluating false still pays libomp's one-time runtime cold-start cost,
   because that cost is a property of *touching* an OpenMP construct at all, not of
   the guard's outcome. Under naive, `example.naive.c` has *two* target pragmas (the
   array-init loop and the bracketed `process()` call, confirmed: `grep pragma
   example.naive.c`) — the init loop pays the cold start first, so by the time the
   bracketed, timed call runs, libomp is already warm and the measurement is just
   per-region dispatch (~30us, the same order as `saxpy`'s warm-dispatch number from
   Day 20). The guard did not fail; the *file shape* did — a single guarded
   OpenMP construct is the worst case for this cost, because there is no second
   construct anywhere to absorb the one-time setup cost the guard's own branch still
   triggers. `MachineModel::ThreadStartUs` has no term for this at all (same gap Day
   20 identified), and this result is the sharpest demonstration of it yet: gating a
   program's *only* OpenMP construct can make real wall-clock time strictly worse
   than never gating, on a machine where cold start dominates. This belongs in the
   finale write-up as an honest, measured limitation, in the same spirit as Day 20's
   `saxpy` finding — not something to paper over because it complicates the headline
   "gating helps" story. `small_update`, above, is the cleaner demonstration of that
   headline story; `example` is the honest asterisk on it.

**Explicitly out of scope for this pass (same line as Day 20):** no change to
`MachineModel`'s constants or `ProfitabilityAnalyzer`'s cost-model arithmetic — this
adds a second, measured decision procedure alongside the existing one, it does not
recalibrate either from three benchmarks' worth of data.

Next: writeup and demo-prep, per `CLAUDE_CODE_WORKFLOW.md` and the remaining
`DAY_BY_DAY.md` days (23-24, and Block B) — consolidate this running `NOTES.md` log
into the finale write-up, rehearse the walkthrough (call graph → safety → gated
profitability → naive contrast → codegen → measured result) cold, and record a demo
recording as backup.

---

### Days 22-24 — the narrated demo, a real writeup, and closing the small blemishes

**Where this sits on the calendar.** Day 21 landed Sept 12. Block B (Sept 15-18,
post-midsem) starts tomorrow and evaluation is Sept 19-20. `DAY_BY_DAY.md`'s Days
22-24 are buffer + writeup; Day 23's deliverable (this file) was already well ahead
of schedule. Day 24's was not started at all: a repo-wide search for
`demo*`/`present*`/`slides*`/`*.gif`/`*.cast` turned up nothing, and
`DAY_BY_DAY.md:51,59` both call for a recorded demo backup before evaluation.

**Three things done, in priority order:**

1. **`scripts/demo.sh`** — a new script running the exact five-stage walkthrough
   `DAY_BY_DAY.md:60` names for the finale (call graph → safety → profitability →
   codegen → measured result), narrated between stages, real `p05tool` invocations
   rather than canned output. `DEMO_PAUSE=0` runs it straight through for a recorded
   backup capture; default mode waits on Enter between acts for a live, rehearsed
   walkthrough. Act 2 is deliberately `tests/safety_cases.c`, not another SAFE
   benchmark — it is the only place in the whole repo that shows the safety check
   saying no, and until this script existed nothing ever ran it in front of anyone.
   Verified both modes end to end, and against a deliberately cold `build/bench/`
   (Act 5 prints a clear "run build_and_run.sh first" instead of silently having
   nothing to show, rather than failing). `DEMO_PAUSE=0` runs in well under a second
   of actual execution time — pacing in a live demo is entirely narration, not
   waiting on the tool.

2. **`README.md` restructured from a changelog into a writeup.** The prior version
   put 140 of 169 lines under one `## Status` heading as append-only day-range
   entries — ordered by when things were built, not how the pipeline runs, and
   internally out of order (a "Days 17-18" banner sat above content actually
   describing Days 2-13, then Day 14, then 15-16, then a *second* Days 17-18 entry).
   Replaced with: what this is, the five-pass pipeline in execution order (one
   paragraph each, naming the implementing file), quick start including the new
   demo script, a Results section with real tables (correctness: exact per-element
   match across every benchmark and both policies; timing: the three-way
   seq/naive/gated numbers with the two counterintuitive findings called out by
   name, not buried mid-paragraph), a consolidated "What it doesn't do" section
   (the aliasing assumption, 2D always-UNSAFE, static op counts as a lower bound,
   no cache-reuse modelling, map extents always from index 0, no real GPU in the
   ladder — six items that were previously scattered across four different
   Status paragraphs), and "How it's verified" naming the three tiers and the five
   discrimination fixtures explicitly. `NOTES.md` (this file) is unchanged in
   spirit — the raw chronological log — and README now points at it instead of
   half-duplicating it.

3. **Small, deliberate blemishes closed** rather than left for an evaluator to find
   first:
   - `scripts/build_and_run.sh`'s `timing-speedup` stage divided `seq_us / omp_us`
     with no zero-numerator guard, so `example` (sequential baseline below
     `clock_gettime`'s practical resolution) printed `0.00x (seq 0.000us / omp
     243.000us)` — reading as "the tool made it infinitely slower," and sitting
     *first* in the summary table, above `saxpy`'s real `0.39x` and
     `compute_heavy`'s real `2.54x`. The correct guard already existed 170 lines
     below it, in Day 21's `policy-compare` stage, whose own comment named
     `timing-speedup` as producing this exact misleading output without fixing it
     there too. Fixed with the same guard shape `policy-compare` uses: an explicit
     `n/a -- sequential baseline (...) is below clock_gettime's practical
     resolution at this scale` line instead of a bare `0.00x`.
   - `.DS_Store` (both root and `src/`) and seven `.idea/*` files were tracked in a
     repo whose commit history is being evaluated. Untracked with `git rm --cached`
     (files remain on disk) and added to `.gitignore` alongside `cmake-build-debug/`
     (an untracked CLion build directory that was one accidental `git add .` away
     from being tracked too).
   - Checked whether the 2D-access limitation (`m[i][j]` always `UNSAFE`) was
     documented anywhere other than README — it already is, in detail, in
     `SafetyAnalysis.h`'s own over-approximation list (added during the Days 8-10
     adversarial review). No change needed; confirmed rather than assumed.

**Verified, not assumed:**
- Regression sweep across every `benchmarks/`+`tests/` input, no `-rewrite`, no
  `-policy` — byte-identical to the pre-change baseline. None of this touched
  analysis or codegen logic.
- Full ladder (`rm -rf build/bench && ./scripts/build_and_run.sh`) — no FAILs,
  `timing-speedup` now reads correctly for `example` with `saxpy`'s and
  `compute_heavy`'s real numbers unchanged.
- `scripts/demo.sh` run in both modes (paced, with piped `Enter` presses consumed
  correctly at each of the five pause points; `DEMO_PAUSE=0` straight through) and
  against a cold `build/bench/`, confirming the documented degrade path.

**Adversarial review of this commit** (same practice as Days 14, 17-18, 19, 20, 21)
found two real issues, both fixed:

1. **A real crash, in `scripts/demo.sh` itself.** Every `p05tool` invocation expanded
   `"${SYSROOT_ARGS[@]}"` under `set -u`. When `xcrun`/an SDK isn't found (any
   non-macOS machine, or macOS without Xcode's command-line tools), `SYSROOT_ARGS`
   is a genuinely empty array — and macOS's own default `/bin/bash` (3.2.57, the
   interpreter `#!/usr/bin/env bash` actually resolves to here, and the one this
   whole repo's scripts already target) treats expanding an empty array under
   `set -u` as an unbound-variable reference, not an empty expansion. Reproduced
   directly: forcing `SDK=""` crashed the script mid-Act-1 with `SYSROOT_ARGS[@]:
   unbound variable`, exit 1, live on stage — exactly the failure mode a demo script
   cannot afford. This same pattern already exists, unfixed, in
   `scripts/build_and_run.sh` (`SYSROOT_ARGS` there, lines 66-67 and every
   `"${SYSROOT_ARGS[@]}"` after) — latent there too, just never triggered on this
   development machine, where an SDK is always found. Out of scope to fix
   `build_and_run.sh`'s pre-existing instance in this pass (not touched by this
   commit otherwise, and it isn't the file being newly shipped as a live-demo
   script), but worth a `NOTES.md` flag rather than leaving it undiscovered. Fixed
   in `demo.sh` with the portable bash idiom for this exact case —
   `"${SYSROOT_ARGS[@]+"${SYSROOT_ARGS[@]}"}"` — verified with a fixed-input repro
   (`arr=(); f(){ :; }; f "${arr[@]}"` crashes under `set -u` in bash 3.2; the
   `${arr[@]+"${arr[@]}"}` form does not) and by re-running the script with `SDK`
   forced empty end to end: exits 0, no crash, every act runs (just without
   `-isysroot`, which is the whole point on a machine that doesn't need one).
2. **An internal inconsistency in the new README table.** The correctness table's
   `gated` column claimed "exact match" for `small_update` — but the gated policy
   correctly declines that loop entirely (trip count 8, ruled `SEQUENTIAL`), so no
   `.omp` binary exists for it and no comparison ever ran. The claim wasn't
   overstated evidence so much as evidence that was never gathered, stated as if it
   had been — caught by cross-checking the correctness table against the timing
   table two sections below, which already correctly said `gated n/a (declined)`
   for the same row. Fixed to `n/a — gated declines, no .omp binary to compare`,
   matching the timing table's own treatment. Also added an explicit run-to-run
   noise caveat to the timing table's intro after the review reran the full ladder
   fresh and got `compute_heavy`'s naive/gated ordering to flip relative to the
   numbers first written down (both are within ~10% of each other — genuinely
   noise-dominated, not a fixed fact, and the table now says so rather than
   presenting one run's ordering as settled).

Everything else the review checked — every file/path reference in the new README,
the `SafetyAnalysis.h` claims in "What it doesn't do" against the actual header
text, the `timing-speedup` guard's fire/no-fire behavior, `src/` being genuinely
untouched — came back clean, verified by actually running things rather than only
reading the diff.

Next: Block B (Sept 15-18) — re-familiarize with the pipeline cold, live-rehearse
`scripts/demo.sh`, and do a final dry run of the walkthrough without notes before
Sept 19-20 evaluation. `scripts/build_and_run.sh`'s own latent `SYSROOT_ARGS`
unbound-variable risk (found above, not fixed there) is a candidate for Block B's
Day 2 ("fix anything left rough") if there's real time to spare — it has never
actually fired on this development machine and isn't blocking anything today.

---

# Research phase (from 2026-10-01)

The goal is an ISC 2027 Research Poster (deadline 2027-01-20). The plan is in RESEARCH_PLAN.md
(local). The entries below follow the same Decision/Reasoning format.

### 2026-10-01 — Verdict regression suite: lit + FileCheck, CHECK lines next to each loop

**Decision:** Every fixture's expected verdicts (kind, trip count, UNRECOGNIZED reason, safety,
profitability) are now FileCheck lines directly above the loop they describe. They run under lit
(`tests/lit.cfg.py`) and in CI. Every loop across the 5 fixture files is covered. The expected
values were taken from the tool's current output, then checked against each fixture's prose comment.
**Correction (2026-10-02):** this entry first claimed "all 44 agreed". That was false. The adversarial
review below found `for (;;)` pinned to "no init clause" while its comment said "no condition clause".
The first check was by eye; the redone check printed each spec beside each snapshot.
**Reasoning:** The Phase 1 rewrite replaces the safety pass wholesale. Until now "both directions"
was checked by hand. It must become automatic before the code it protects is rewritten. Guard
thresholds are matched as `{{[0-9]+}}` rather than literals: Phase 2 recalibrates the cost model,
which legitimately moves every threshold, while a guarded↔unguarded flip still fails. The suite was
mutation-checked against fed-in tool output. A flipped verdict, a dropped loop and a dropped guard
each fail it. `CHECK-LABEL` can't use `[[@LINE]]`, so loop locations use plain `CHECK`. Sequential
matching still pins each field to its own loop: a field missing from loop k would match in loop
k+1's block, and loop k+1's location check would then fail.
**Alternative considered:** a custom Python checker that parses the prose comments. Rejected because
lit/FileCheck is the LLVM-standard harness that reviewers recognise, and prose is too irregular to
parse reliably.

### 2026-10-02 — GPU offload spike: real T4 offload works via NVIDIA HPC SDK, not apt.llvm.org clang

**Decision:** GPU evaluation runs on Colab/Kaggle T4, compiling P05's output with NVIDIA HPC SDK
`nvc -mp=gpu`. Results: `eval/colab/results/spike_t4_2026-10-02.json`, notebook
`eval/colab/offload_spike.ipynb`.
**Reasoning:** Under `OMP_TARGET_OFFLOAD=MANDATORY`, a probe confirmed the target region executed
on a non-host device. Then `benchmarks/compute_heavy.omp.c`, the tool's unmodified output, ran on
the T4.
- Output against the sequential baseline: max relative error 6.6e-16, with 45505/65536 elements
  bitwise equal. The remainder is the expected FMA-contraction rounding difference (`r*x + c`
  fused on device), not a correctness bug.
- Correctness for GPU results is therefore an ε-comparison with the tolerance stated, not bitwise.
  This revises the hackathon's bit-for-bit bar, which only held because the "device" was the host.
- The measured 158 ms (vs 3.55 ms sequential on Colab's CPU) is dominated by one-time CUDA context
  creation in a single-kernel program. It is the same class of cold-start cost Day 20/21 found for
  libomp, much larger, and still absent from the cost model. Phase 2 calibration must measure it as
  its own term and time steady-state kernels after a warm-up.
**Alternative considered:** apt.llvm.org clang with libomptarget (Path A). Its probe failed. The
cause is not yet diagnosed and is worth a single follow-up cell. A working clang offload path would
give a second compiler for cross-checking. It is not blocking, since P05 emits standard OpenMP and
`nvc` is a production OpenMP-offload compiler.

### 2026-10-02 — Adversarial review of Phase 0: suite tightened, one tool bug found

**Decision:** Phase 0 got a fresh-context adversarial review before committing, per the project's
review-after-implement convention. Every finding inside Phase 0's scope was fixed. The one tool bug
it found is recorded and scheduled, not fixed here.

**Reasoning (what it found → what changed):**
- **Spec mismatch baked into a snapshot.** `for (;;)` hits the no-init reject first, so the
  "no condition clause" path (`LoopAnalysis.cpp`) had no coverage. Kept `for (;;)` as the no-init
  case with a corrected comment, and added `no_condition` (`for (int i = 0; ; i++)`) so the
  no-condition path is really exercised. `loop_unrecognized.c` now has 9 loops. The generator now
  prints every loop's prose spec beside the snapshotted values; all rows were compared, and agree.
- **Unanchored fields.** Trip counts 10→100, 8→8192, 65536→655360, `SAFE`→`SAFEX`, a reason or
  guard with a suffix: all were *accepted*. Every field is now anchored with `{{$}}`.
- **Only the verdict was checked, never the reason.** A 3-level chain collapsed to depth 1, or an
  UNSAFE verdict reached for an unrelated reason, passed. The suite now also pins every
  `reaches … max depth K` line and the first reason line under each UNSAFE/UNKNOWN verdict. This
  matters most for Phase 1: a rewritten safety pass must reach the same verdicts *for the same
  reasons*. All 13 of the review's accepted mutations were re-run and are now rejected.
- **lit wrote `Output/` and timing files into `tests/`.** `test_exec_root` is now `build/tests`.
- **CI would likely have failed.** Ubuntu 24.04's Python is externally managed (PEP 668), so a bare
  `pip install lit` fails; it's now `pipx install lit`. Also added the optional
  `clang-tools`/`libpolly`/`libclang-cpp` packages that Debian's ClangConfig.cmake checks exist.
  Still unverified until the first push.
- **Spike notebook.**
  - It now records *which stage* Path A failed at, with stderr.
  - It no longer hides the `libomp` install output.
  - It installs `clang-tools` and lists the NVPTX device-runtime dir (the review's top hypothesis
    for the Path A failure).
  - Output goes to a log instead of `| head`, which could SIGPIPE the run before its dump is
    written.
  - Every rerun starts fresh.
  - Two caveats are now explicit: the probe proves "a non-host device", and only NOTIFY/INFO output
    proves "GPU". And `seq_us` (gcc) vs the nvc GPU time is cold-start data, not a speedup.
- **`docs/related_work.md` overclaimed.** The P05 row described Phase 1/2 targets (call-site-aware
  summaries, calibration) as present-tense facts. It also omitted the no-alias assumption. It now
  states current status, with targets labelled as targets. Unverified claims about Cetus/OpenMPC,
  Polly-ACC, COMPOFF, Cerberus and DiscoPoP are now tagged [verify].

**Tool bug found, deliberately not fixed in Phase 0.** `gpu_offload_descending_case` maps
`out[0:0]`/`in[0:0]`, because a descending loop's region can't be sized. Its transfer is therefore
priced at 0 µs (t_gpu 20.5 µs) against 87.4 µs for the identical ascending loop (t_gpu 107.9 µs).
The verdict is the same either way, which is why no verdict check could see it. It is the same root
cause as the README's "mapped regions always start at index 0" limitation. So it is fixed with Phase
2's map-extent rework (RESEARCH_PLAN.md Phase 2.1): an unsizeable region must be priced
conservatively or declined, never priced at zero. The fixture's comment now says so.

### 2026-10-02 — Phase 1.1: the loop-nest model

**Decision:** Every `LoopInfo` now knows where it sits among the other loops:
- `Parent`, `Children` and `Depth` (indices into the collector's flat vector).
- `NestIVs`, the trusted induction variables down to this loop.
- `EnclosedByUnrecognized`, `PerfectlyNested`, `BoundsDependOnOuterIV` and `InsideWhileOrDo` (renamed
  `InsideRepeatingContext` after the review, below).

`LoopCollector` overrides `TraverseForStmt` so that only a loop's *body* is inside it. The report
gained two lines per loop, `nest` and `nest ivs`. No verdict, report line or rewrite output changed.

**Reasoning:**
- **Why indices, not pointers:** the vector reallocates while it is being filled, so pointers would
  dangle.
- **Why the flat vector stays:** the driver, codegen, the report and every existing CHECK block keep
  working untouched.
- **Why only the body counts:** `TraverseForStmt` records the loop, traverses init/cond/inc with the
  stack unchanged, then pushes the loop for the body only. So a loop in an init clause (possible via a
  GNU statement expression) gets the *enclosing* loop as its parent.
- **Why the order is unchanged:** recording happens before any child is visited, so the order is
  exactly what `VisitForStmt` produced: pre-order, parents before children.
- **Why the queue is dropped:** the `while`/`do` overrides pass a null `DataRecursionQueue` to the base
  traversal. That makes the whole subtree finish before the counter is decremented. Forwarding the
  queue would let children be visited after the decrement.
- **Unrecognized enclosing loops:** their variables are deliberately left out of `NestIVs` and the
  triangular check, and `EnclosedByUnrecognized` is set instead, so later phases treat those
  variables as unknown symbols.
- **"Purely additive" was verified directly, not just inferred from tests:** the pre-change tool was
  built from `HEAD` in a scratch worktree. Its full report and `-rewrite` output were diffed against
  the new tool's, with the two new lines deleted, for every C file in the repo (9 files, 56 loops).
  All were byte-identical.

**Test tooling:**
- New fixture `tests/nests.c`: both directions per fact. It had 27 loops, not the 26 this entry
  first said (the review caught that); it has 43 after the review additions below.
- The CHECK generator is now committed as `scripts/gen_filecheck.py`, and prints each loop's prose spec
  beside the snapshot.
- **A flaw it hit:** the child's `parent L:C` was first pinned as a literal line, but inserting the
  CHECK lines moves the parent loop. Each loop's location check now captures its line into a FileCheck
  numeric variable (`[[#LOOP3:@LINE+8]]`), and the child checks `parent [[#LOOP3]]:5`. Confirmed that a
  wrong parent and a wrong line are both rejected.
- All fixtures were regenerated in this format. The diff was verified to only rewrite location lines
  and add the nest lines.
- Eight nest mutations (wrong parent, depth, perfect/triangular/unrecognized/while flags, child
  count, IV order) are all rejected.

**Requirements this hands to Phase 1.3, which the fixture already exposes:**
- **`PerfectlyNested` alone must not license `collapse(2)`.** In `loop_in_init_clause` the outer loop
  is "perfect" yet has 2 children, because a loop hides in the inner loop's init clause. `collapse`
  also needs both loops recognized, a rectangular nest (`!BoundsDependOnOuterIV`, unless the OpenMP 5.0
  non-rectangular forms are targeted on purpose), and inner bounds invariant in the outer loop.
- **`caller` in `nests.c` is a pinned false UNSAFE.** `callee_with_loop(&a[i*n], n)` writes a distinct
  row per iteration. This is the Phase 1.5 call-site-aware-summaries target. When 1.5 lands, this
  CHECK is allowed to flip to SAFE, and only in that direction.

### 2026-10-02 — Phase 1.1 adversarial review: generator hardened, nest facts sharpened, two older bugs fixed

**Decision:** A fresh-context review found no bug in the nest model's C++. It re-verified "purely
additive" on the 56 tracked loops plus 303 throwaway loops of tricky shapes. Every finding was then
fixed. Two of the fixes correct bugs that **predate** Phase 1.1; the new fixture is what exposed them.

**Reasoning (finding → fix):**
- **The generator silently dropped a loop's checks when two loops share a line.** Blocks were keyed by
  line alone. It now exits with an error instead.
- **The generator wiped a fixture's CHECK lines if the tool failed.** It wrote the stripped file before
  running the tool. It now runs the tool on a stripped temp copy and rewrites the fixture only on
  success. Verified: 340 CHECK lines survive a missing tool, and no temp file is left behind.
- **The generator mishandled loops from `#include`d files.** It keyed by line only, so it could hit a
  KeyError or bind the wrong parent. Locations are now keyed by (file, line), loops from other files
  aren't pinned, and the report prints `parent FILE:L:C` when the parent is in another file. The RUN
  line is now decided by the directory name, so regenerating outside the repo still adds it.
- **"Triangular" false positive:** `j < sizeof(a[i])` mentions `i` but folds to a constant. A start or
  bound that folds to a constant is no longer checked. This is sound, since a constant can't depend on
  `i`.
- **"Perfect" misses, in the safe direction:**
  - Now perfect: `#pragma clang loop` (an `AttributedStmt` wrapper) and `{ { for } }`.
  - Still imperfect, deliberately: a label in front of the inner loop, because a `goto` could enter
    it.
- **`InsideWhileOrDo` was incomplete, and is renamed `InsideRepeatingContext`:** a loop in a `for`'s
  condition or increment clause re-runs every iteration but was reported as depth 0 and not
  repeated. Those clauses now count. The init clause runs once, so it doesn't.
- **Block literals:** a loop inside `^{ ... }` in a loop body was counted as a child. It runs when the
  block is invoked, like a called function, so it now starts a fresh nest.
- **Pre-existing bug 1, soundness:** a statement expression in a bound (`j < ({ ...; s + n; })`) was
  accepted, and the loop was marked SAFE. Like a call, a statement expression can run arbitrary code
  every iteration. It is now rejected: UNRECOGNIZED, "bound expression contains a statement
  expression". This is a verdict change in the conservative direction only, and it reaches no
  previously existing fixture.
- **Pre-existing bug 2, codegen produced uncompilable output:** a bound spanning two source lines was
  pasted raw into the one-line `#pragma omp parallel for if(parallel: n\n - 1 >= 2048)`. Reproduced
  with the pre-change tool: clang rejected the rewritten file ("expected ')'"). Bound text that spans
  lines is now taken from Clang's single-line pretty-printer. Fixture `multiline_bound_case` pins the
  guard as `n - 1 >= T`, and the rewritten file compiles.
- **Re-verified:** after all the fixes, every tracked C file at `HEAD` still produces byte-identical
  report and `-rewrite` output, with the nest lines removed.

**Carried into Phase 1.2/1.3 (recorded, not built):**
- **A reused induction variable** (`int i; for (i…) for (i…)`) gives `nest ivs : i, i` over the
  *same* variable. That is distinct from shadowing, which is two different `i`s and is pinned in
  `nests.c` as correct. Phase 1.2 must reject a nest whose inner loop rewrites an outer loop's
  variable.
- **`collapse(k)`** needs more than `PerfectlyNested`; see the 1.1 entry above.

### 2026-10-02 — Phase 1.2: affine subscript extraction

**Decision:** New `src/analysis/AffineAccess.{h,cpp}`. For every loop, it lists every array/pointer
access in the body, inner loops included. For each access it records:
- its base;
- its kind: R, W, RW (compound assignment or `++`), or `&` (address taken);
- one affine subscript per dimension, `Σ c·IV + Σ d·symbol + const`, *or* "non-affine" plus a reason.

It makes no verdict. Phase 1.3's dependence tests consume it, and the report only gains access lines.
Re-verified against the pre-change tool: all 11 C files (136 loops) produce byte-identical report and
`-rewrite` output apart from the new access lines.

**Reasoning, design choices that keep it sound:**
- **A symbol must be provably invariant.** A non-induction variable counts as a symbol only if
  nothing in the loop (body, condition or increment) can write it:
  - It must not be written by name or address-taken in the loop.
  - If it's a global, or a local whose address escapes anywhere in the function, there must be no
    call in the loop and no write through a pointer of an aliasing type (same type modulo
    signedness, or a character type).
  - Volatile and non-integer variables are never symbols.
- **Inner loops' header updates are not "body writes".** The first build had a real bug here: for
  the *outer* loop of every nest, `m[i][j]` came out non-affine, because the inner loop's own `j++`
  sits inside the outer body. Writes from a recognized inner loop's own init/increment to its
  induction variable are now excluded. Any *other* write to it still makes it non-affine at every
  level (fixture `inner_iv_modified`).
- **Forward substitution** of `int k = 2*i + 1; a[k]`. The initializer is evaluated with the
  induction variables that were valid where `k` was *declared*, not where it's used. Otherwise a
  variable declared before an inner loop could pick up that loop's IV.
- **True multi-dimensional arrays** (static, VLA, `double (*)[4]`) are detected by following
  array-to-pointer decays down the subscript chain. `double **a; a[i][j]` loads a pointer, so it's
  opaque. The inner `a[i]` is reported as its own read.
- **Delinearization** of `X*N + J` (any operand order, recursive in X): `a[i*n + j]` becomes
  `[i][j]`, and `a[(i*n + j)*n + k]` becomes `[i][j][k]`. Applied **only** when J is the induction
  variable of an enclosing loop running exactly `0..N-1` by 1, so `0 ≤ J < N` is proven, and only
  when N is a genuine invariant symbol, never an IV. Otherwise the access stays non-affine. Fixtures
  `wrong_bound` and `wrong_start` cover the cases that must not delinearize.
- **Every non-affine shape gives a specific reason**, so 1.3 can say *why* a loop is UNKNOWN:
  - `idx[i]` in a subscript, `i*i`, `/`, `%`, calls
  - a moving base pointer
  - struct-member bases
  - symbols changed by writes, calls or aliasing

**Test-suite hardening found while building it:**
- **Extra lines slipped through.** A mutation adding a spurious access line was *accepted*, because
  plain `CHECK` skips lines. The access block, and the `safety` line right after it, now use
  `CHECK-NEXT`, so the block is pinned exactly. Re-checked: a spurious line and a dropped line both
  fail, along with the 9 other access mutations, including a non-affine access reported as affine.
- **My first escaping helper was wrong.** Source text containing `[[`/`{{` must be escaped for
  FileCheck, and the first version re-escaped its own output. It is now single-pass, and was tested
  through FileCheck itself.

**For Phase 1.3:**
- An access whose base was delinearized must not be compared dimension-by-dimension with a
  non-delinearized access to the same base (different shapes). Compare those conservatively.
- Overflow in subscript arithmetic is assumed not to happen, the same assumption the trip counts
  make.

### 2026-10-02 — Phase 1.2 adversarial review: six soundness bugs, fixed at the root

**Decision:** The fresh-context review of Phase 1.2 found **six HIGH-severity soundness bugs**. In each,
an access was reported with a *wrong* affine form, or never reported at all. Phase 1.3 will use these
forms to prove loops safe, so each one would have become a false SAFE. Rather than patching six
symptoms, the core rule was rebuilt:
- **Every write to a variable is recorded with its exact statement.** An induction variable is
  "clean" at an access only if a recognized loop enclosing *that access* binds it and nothing else
  writes it. A symbol is a variable that *nothing* in the loop writes.
- **If the analyzed loop's own variable (or an enclosing one) is written inside it, every access is
  non-affine**, because the iteration space itself is unknown.

**Reasoning (bug → fix), each with a fixture in `tests/affine.c` ([R1a]…[R6''']):**
- **R1: an inner loop's header write was invisible everywhere in the outer loop.** The first fix for
  "outer `m[i][j]` non-affine" dropped those writes globally. As a result:
  - `j` used *after* its loop became a "symbol";
  - a reused `for (i…) for (i…)` went unnoticed;
  - a parameter reused as an inner counter became a symbol;
  - forward substitution and delinearization inherited the problem.

  Now a header write is legitimate only for accesses lexically inside that loop. An enclosing binding
  shadowed by the same variable is skipped, so the inner loop of a reused-IV pair stays an ordinary
  loop. The outer one is poisoned.
- **R2: delinearization never checked that J's body leaves J alone**, so `if (j == n-1) j = n;` still
  gave `[i][j]`. It also checked N only within the analyzed loop, even when J's loop encloses it. It
  now requires J clean and N fixed over J's whole loop. Facts for an enclosing loop are computed on
  demand.
- **R3: narrowing casts passed through as identity** (`a[(unsigned char)i]`, the ring-buffer idiom).
  Integral casts are now accepted only when they preserve every value: widening that doesn't turn
  signed into unsigned, or same width and same signedness.
- **R4: `s[i].x = …` was recorded as a READ of `s[i]`.** Kinds now propagate through `.` members, and
  through array members (`s[i].v[k]`, `p->v[k]`), to the element that holds them.
- **R5: `->` accesses were never recorded** (`s->y += i` was invisible). `p->f` and `(p + e)->f` are
  now pointer accesses to `p[0]` and `p[e]`.
- **R6: a pointer declared in the body was treated as fixed.** `int *q = a + n - i; q[i] += 1` was
  reported as distinct per iteration, but it is always `a[n]`. Such pointers are now substituted when
  provable (`q[i]` → `[n] of 'a'`, via a recursive split of `P ± e ± …` and `&P[e]`) and opaque
  otherwise. Pointer bases get the same escape/call/alias checks as symbols. That catches
  `int **pp = &p; *pp = *pp - 1` and a global pointer base with a call in the loop.
  - **This bug predates Phase 1.2 and is a live false SAFE today.** The hackathon `SafetyAnalysis`
    says SAFE for the `r6_body_pointer` loop, both on `HEAD` and now, and would emit a pragma for a
    race. Phase 1.3 replaces that rule; until then it's recorded here.
- **Medium:**
  - `sizeof(b[i+7])` is no longer recorded as an access, since `sizeof` doesn't evaluate its operand.
  - `*(arr + i)` on a named array is now affine.
  - The strict-aliasing assumption and "pointers passed to calls are Phase 1.5" are now stated in
    the header.

**Process notes:**
- The reviewer accidentally ran `-rewrite` on the repo's own fixtures, and said so. The tracked
  `.omp.c` files came out byte-identical. The stray untracked files were deleted. But
  `tests/*.omp.c` (gitignored) carry a copy of the fixture's RUN line, so lit started running them as
  tests (11 instead of 7). `tests/lit.cfg.py` now excludes every `*.omp.c` it finds.
- The header previously claimed "never a wrong affine form". That was false, and the review proved
  it. The header now lists the two assumptions that *can* make a form wrong if violated (no signed
  overflow; strict aliasing), separately from limits that only lose precision.
- **Re-verified after the fixes:**
  - all 11 C files (164 loops) still give byte-identical report and `-rewrite` output apart from
    access lines;
  - every R-fixture's result was compared by hand with its prose spec;
  - all 7 lit tests pass.

### 2026-10-02 — Phase 1.2, second review round: two more wrong-form bugs fixed

**Decision:** The rebuilt core got its own fresh-context review. The core rule held: variants of all six
first-round bugs, three-level IV reuse, IVs written through pointers or calls, and more all came back
correct. Two more wrong-form bugs and three small gaps were found and fixed, each with a fixture
([R7]–[R11] in `tests/affine.c`).

**Reasoning (bug → fix):**
- **HIGH: a `void *` round-trip changed the element size unnoticed.** `void *v = a; char *c = v;
  c[i]` was reported as `[i] of 'a'` (byte i vs int i). The cause was `IgnoreParenImpCasts`, which
  also strips implicit pointer bitcasts. Pointer splitting now looks through only no-op,
  lvalue-to-rvalue and array-decay casts. A substituted pointer must step over the same element type
  as its base, and GNU `void *` arithmetic is rejected.
- **MEDIUM: unsigned constants that wrap.** `a[u + -1]` gave `[u + 4294967295]`, which a dependence
  test would read as a distance of about 4e9, so it would miss the real distance-1 dependence. In
  unsigned arithmetic narrower than 64 bits, an operand constant ≥ 2^(w-1) is now non-affine, and so
  is negating an unsigned value. A bare constant subscript like `a[4294967295u]` is still taken at
  face value, because there it means exactly that.
- **LOW:**
  - Delinearization now requires `j` and `n` to have the same type. An unsigned `j` against an `int
    n` compares as unsigned, and a narrower `j` can wrap.
  - Inline asm counts as writing its outputs and as a call (clobbers).
  - `sizeof` on a VLA-typed operand *is* evaluated, so it is now traversed. Each expression is
    recorded once, since VLA sizes are reachable twice.
  - A self-referencing initializer (`int k = k + 1;`) is no longer substituted.
- **Not acted on, with reasons:**
  - `mayAlias` ignores aggregates that contain an `int`. A struct assignment through a pointer can
    only overwrite a standalone `int` variable by violating strict aliasing, which is already an
    assumption.
  - Poison-reason wording differs slightly between accesses (cosmetic).
  - `*m` in `(*m)[i]` is recorded as a read (it's non-affine, so harmless).

**Re-verified:**
- 172 loops across all 11 C files give byte-identical report and `-rewrite` output apart from access
  lines.
- The round-2 fixes changed no pinned value in any other fixture.
- All 7 lit tests pass.

### 2026-10-02 — Phase 1.3: dependence tests replace "every subscript must be exactly i"

**Decision:** New `src/analysis/DependenceTest.{h,cpp}`. For every pair of accesses to the same base
where at least one writes (each writer is also paired with itself), the subscripts are equated per
dimension:
- the analyzed loop's variable becomes two distinct unknowns, i1 ≠ i2;
- each inner loop's variable becomes two independent unknowns;
- symbols and enclosing loops' variables are shared.

Tests run cheapest first: ZIV, then GCD (symbols included), then strong SIV (exact distance; must be a
nonzero multiple of the step and fit in the iteration span), then Banerjee per iteration order. The
Banerjee ranges are affine in symbols (`AffineAccess` now records each loop variable's range).

The sign of a symbolic extreme is decided with the facts "the analyzed loop runs ≥ 2 iterations" and
"an inner loop that hosts the access runs ≥ 1", via a single-fact λ-multiplier argument. That proves,
for example, `a[i]` vs `a[i+n]` with `i < n` independent.

Outcome per pair:
- independent;
- **dependent**, proven: an exact distance, or "the same element on every iteration". This gives
  UNSAFE.
- **not disproved**: UNKNOWN.

`SafetyAnalysis` drops its array/deref/arrow "exactly i" checks in favor of this. It keeps the scalar
rules (Phase 1.4) and the callee-effect rules (Phase 1.5).

**Reasoning, and how each verdict change was justified:**
- **Every flip was listed and read against its code before being accepted**, not just regenerated.
  - **About 40 loops went UNSAFE → SAFE.** They include every 2-D/3-D nest level with distinct
    rows/elements (`m[i][j]`, `a[i*n + j]`, column-major, VLA, `double (*)[4]`), shifted
    reads of other arrays, `a[n-i-1]`, `a[2i+1]`, and `a[4i + j]` with `j < 4`. Each was
    confirmed parallel.
  - **Fixes in the conservative direction:**
    - `r6_body_pointer` (`q = a + n - i; q[i] += 1`) went **SAFE → UNSAFE**: "every iteration
      touches the same element". This closes the live false SAFE found in the Phase 1.2 review.
    - The `void*` views and the `asm` case went SAFE → UNKNOWN.
    - Indirect writes like `a[idx[i]]` were reclassified UNSAFE → UNKNOWN. No race is proven; both
      verdicts block parallelization.
- **The map interlock** (found while designing, before any SAFE flip could reach codegen):
  - **The problem:** every `map()` clause is `base[0:bound]`. That was only correct because "SAFE"
    used to imply every subscript is exactly `i`. Offloading the newly-SAFE `a[i] = b[i+1]` or
    `m[i][j]` would leave touched memory unmapped on the device.
  - **The fix:** `LoopSafety::GpuMappable`. GPU offload is declined unless every access is exactly
    `base[i]` on a base declared outside the loop, under every policy. Such loops degrade to CPU
    `parallel for` if the cost model says threading beats sequential, else to sequential.
  - **Verified:** the four demo benchmarks' rewrites are byte-identical to their tracked `.omp.c`
    files. Phase 2.1 lifts the interlock by sizing maps from the affine ranges.
- **Delinearization generalized**, because the fixture exposed the gap. `a[i*n + j - 1]` (row
  prefix) and `a[(i-1)*n + j + 1]` (wavefront) were UNKNOWN. Now `X*N + j + c` is accepted when j's
  actual range shifted by c provably stays in `[0, N-1]`:
  - j counts up from a constant s to `j < N + e` (or `<=`), with s + c ≥ 0 and top + c ≤ N - 1;
  - constants peeled from the outside are folded into c;
  - unsigned arithmetic is never peeled.
- **Honest UNKNOWN over claimed UNSAFE.** The wavefront's outer loop (`a[i][j]` vs `a[i-1][j+1]`) is
  only dependent when the inner loop has room for the +1 shift (n ≥ 3). With a symbolic n that can't
  be proven, so it is UNKNOWN, and the fixture's spec was corrected to say why.

**Runtime evidence, not just verdicts:**
- `tests/dependence.c`'s value-dependent functions are *run* by `scripts/check_parallel.sh`:
  - the fixture is rewritten with every SAFE loop forced to `parallel for` (new testing policy
    `-policy all-cpu`);
  - it and the original are built against `eval/runtime/dependence_main.c` (pseudo-random inputs,
    every output dumped);
  - the outputs must be bit-identical over 20 runs at 8 threads.
- 12 forced pragmas pass.
- **The check discriminates:** injecting a `parallel for` on the UNSAFE `prefix_sum` produced a
  mismatch on the first runs.
- It now runs in CI (Linux needs `libomp-dev`).

**Remaining gaps, honest:**
- Triangular rows (`a[i*n + j]` with `j < i`) are not delinearized, so they come out UNKNOWN.
- Reductions and scalar temporaries declared outside the body are UNSAFE (Phase 1.4).
- The no-alias assumption between distinct pointer parameters stands (Phase 1.6).
- The cost model's per-iteration bytes/ops still ignore inner loops' trip counts (Phase 2), so
  CPU_PARALLEL guards for nests are rough.

### 2026-10-02 — Phase 1.3 adversarial review: four new false SAFE verdicts, all fixed and run

**Decision:** The fresh-context review produced **runtime evidence**, not just verdicts.
- **What it found:** four loops that Phase 1.3 newly marked SAFE gave different results in parallel
  (`scripts/check_parallel.sh`, mismatches mostly on run 1). A random-loop fuzzer, brute-forced over
  small n and m, found the first one independently.
- **Also found:** five older false-SAFE / bad-map paths through function calls, and three overclaimed
  UNSAFE verdicts.
- **Status:** all are fixed. Each is a fixture in `tests/dependence.c` ([D1]–[D7], [L1]), and the
  value-dependent ones are added to the runtime driver. That driver now forces 16 pragmas and passes
  30/30 runs.

**Reasoning (bug → fix):**
- **D1, HIGH: facts about unrelated inner loops.** "An inner loop runs" was used as a fact for every
  pair. It is only true when one of the accesses is inside that loop: with `m = 0` a sibling loop is
  empty, and `a[i]` vs `a[-m]` collide. Each access now records its enclosing inner loops
  (`ArrayAccess::InnerIVs`), and only those loops' facts are used for a pair. The root's "≥ 2
  iterations" fact is always valid.
- **D2, HIGH: different row lengths compared dimension-wise.** `a[i*n + j]` and `a[i*m + k]` were
  both treated as `[i][·]`. Each delinearized access now records its row-length variables
  (`RowLengths`), and different ones mean different shapes, so the result is UNKNOWN.
- **D3, HIGH: `static`/`extern` in the body treated as private.** They are one object shared by all
  iterations. Only `hasLocalStorage()` variables are private now: in `DeclaredInLoop`, and in the
  scalar rule's `LocalVars`. The scalar half of this predates Phase 1.3.
- **D4, MEDIUM: local pointer aliases.** `double *q = a;` then `q[i+1]` vs `a[i]`. The hackathon "exactly
  i" rule used to hide this. A local pointer set outside the loop is now resolved to what it points
  into when it is initialized once, never reassigned in the function, and has a constant offset.
  Otherwise it is `BaseMayAlias`, and pairs against any other base are UNKNOWN. Parameters and
  globals keep the documented no-alias assumption.
- **D5–D7, older, false SAFE through calls:**
  - a callee writing through a pointer *derived* from its parameter (`q = p + 1; *q = …`) now
    counts as possibly writing caller memory (UNKNOWN);
  - a callee *reading* a global the loop writes is UNKNOWN;
  - a callee reading or writing through a pointer argument into an array the loop writes is UNKNOWN.
  This is coarse, conservative, and call-site independent. Phase 1.5 refines it.
- **GPU maps:**
  - The interlock now also declines offload when any call passes a pointer, when a callee uses a
    global, when the loop's start is negative or unknown, or when a base may alias.
  - The new `tests/interlock.c` runs under `-policy naive` and pins this: only the exactly-`a[i]`
    loop reaches the GPU.
  - The generator now keeps a fixture's own RUN flags.
- **LOW, overclaimed UNSAFE:**
  - A loop with at most one iteration has no carried dependence. Its summary is now "at most one
    iteration".
  - "Same element every iteration" through an inner loop is now only claimed when that inner
    loop's range doesn't depend on the analyzed loop's variable (a triangular range can be empty).
  - Accesses through `.field` are never a *proven* dependence, because the analysis is
    field-insensitive.
- **Stated assumption added:** subscripts stay within the declared bounds of the object. `m[i][j+1]`
  past a row's end is ISO-C undefined behaviour and is treated as unable to reach the next row.

**Re-verified:**
- all 9 lit tests pass;
- `check_parallel.sh` gives 30/30;
- the four demo benchmarks' rewrites are byte-identical to their tracked `.omp.c` files.

The reviewer's checks of the Banerjee formulas, the λ argument, strong SIV with negative steps, GCD,
ranges and delinearization offsets all came back clean, plus ~9,000 fuzzed loops with only D1 as a
false SAFE.

### 2026-10-02 — Phase 1.3, second review round: the edges of what the analysis sees

**Decision:** The second fresh-context review used two kinds of evidence. One was runtime: every
false SAFE was confirmed by `check_parallel.sh` with a mismatch on the first run. The other was a new
fuzzer that logs actual addresses per iteration. It brute-forces small parameter values and flags a
SAFE verdict whenever two iterations touch the same address with a write.

The core math held: **6,000 fuzzed loops gave 0 false SAFE** from the dependence tests, ranges,
delinearization and facts. It found nine families of false SAFE *at the edges of what the analysis
sees*, plus three bad GPU `map()` clauses under the **default** policy. All are fixed with broad
conservative rules rather than per-case patches. Each has a fixture ([E1]–[E9] in
`tests/dependence.c`; three map cases in `tests/interlock.c`). The value-dependent ones are in the
runtime driver.

**Reasoning (family → rule):**
- **Pointers into callees by any route** (E1, E2):
  - a local alias `q = a` passed to a reader;
  - a pointer inside a struct passed by value;
  - a pointer cast to `long`;
  - `qs[0]` / `s.p` loaded from memory.

  **Rule:** if a callee (transitively) touches memory through any pointer, every argument that is
  not a plain number (`carriesNoPointer`: arithmetic type, no pointer-to-integer cast inside) is
  checked. A named object counts: a named array, or a pointer parameter its function never reassigns
  or lets escape. That conflicts only if the loop writes it. Anything else may point anywhere, so it
  conflicts with *any* loop write. `pointerRoot` now crosses a subscript or member only under `&`
  (address arithmetic), never through a load.
- **Callee writes through a local that holds a pointer** (E3: struct, pointer array, integer). This
  is now an unknown write unless the local is a plain array.
- **Invisible effects in callees** (E4, E5): calls through function pointers, atomic builtins and
  inline asm inside a callee now count as unknown writes.
- **Invisible effects and early exits in the loop body** (E5, E9): atomics, asm, `break` (not inside a
  nested loop/switch), `return` and `goto` make the loop UNKNOWN. For the exits, the pragma clang
  would have been given doesn't even compile.
- **No-alias assumption narrowed** (E6, E6'):
  - A pointer parameter keeps it only while it still holds the caller's value. A parameter the
    function reassigns or lets escape may alias.
  - **Every global pointer may alias**, since it can be set anywhere. This costs some precision on
    code that uses global pointers, in exchange for soundness.
  - A callee using a global *pointer* conflicts with any loop write.
  - A callee using a global conflicts with may-alias writes too (E7).
- **The single-iteration shortcut** (E8) is skipped when the loop's variable is modified inside it.
  The header range is then not the iteration space.
- **GPU maps under the default policy:**
  - `a[i]++` and `s[i].x = …` are now writes in `RegionVisitor`. A member store maps the element
    `tofrom`, since the rest of the element must survive.
  - Offload requires subscript syntax, so `*(b + i)` never gets an unmapped `b`.
  - Any callee that touches memory through a pointer blocks offload.
  - The generator now pins the `map()` line for every GPU verdict.

**Re-verified:**
- all 9 lit tests pass;
- `check_parallel.sh` forces 16 pragmas and passes 30/30;
- the demo benchmarks' rewrites are byte-identical to their tracked `.omp.c` files;
- no existing fixture changed verdict, only reason wording.

**Open, recorded:** the reviewer's address-logging fuzzer was the most effective tool this project
has had, and it lived only in `/tmp`. It should become a committed tool (`eval/fuzz/`) and run in CI
before Phase 1.4, so every later phase is fuzzed, not just reviewed.

### 2026-10-02 — Address-logging fuzzer committed (`eval/fuzz/`), in CI

**Decision:** the reviewer's `/tmp` fuzzer was gone, so `eval/fuzz/` was written fresh to the same
idea and now runs in CI (`fuzz.py --seed 1 --files 15`, about 45 s). It has two oracles:

- **Address oracle (primary).** An instrumented copy of each generated function routes every access
  through `fuzzrt.h`, which records the iteration of every enclosing loop. A loop instance carries a
  dependence iff two of its iterations touch one address and one of them writes. A SAFE verdict on
  such a loop fails the run, and the failure names a witness (`iterations 0 and 1 both touch A+2`
  at `(n, m, k)`). It does not depend on thread timing.
- **Differential run.** The `-policy all-cpu` rewrite is built with `-fopenmp` and run against the
  sequential build at 8 threads. The output hashes must match. This also catches emitted pragmas
  that don't compile.

**Respecting the stated assumptions, not testing them:**
- Distinct pointer parameters get disjoint buffers.
- A loop instance during which any access leaves its object (an out-of-range declared dimension,
  or a pointer leaving its buffer) is discarded. This is per instance, not per run: a bad subscript
  elsewhere in the function does not throw away a clean loop's evidence.
- The differential run executes only runs that never left an object, so it never runs UB.

**The generator:**
- Two thirds of the functions are "clean": one layout per array, and reads are small shifts of
  the writes. This puts many loops right at the SAFE/UNSAFE boundary.
- Boundary shapes target the dependence tests: shifts up to ±4 next to constant bounds 3–5,
  symbolic terms with coefficient 2 (`2*i + n` vs `2*i`), ZIV reads (`A[n]`, `A[k]`), step 2 with
  odd distances, triangular nests, descending loops, linearized rows, a local alias `q`, and
  helpers called with `&A[i]`.
- The rest of the functions are unconstrained, to probe the edges.

**Evidence that it discriminates:**
- **A tool that flips UNSAFE to SAFE:** caught, and the witnesses were checked by hand.
- **Pragmas forced onto every loop:** caught by the differential run.
- **The review's five p05tool mutants** (SIV span `<`→`<=`, GCD ignoring symbolic terms, ZIV, Banerjee
  bound −1, `Dist % Step`):
  - four are caught within 40 seeds, and SIV, GCD and Banerjee are caught within CI's 15;
  - the ZIV mutant changes no verdict on the generated loops;
  - before the coverage fixes below, three of these passed a CI-sized run.

**Adversarial review of the fuzzer (fresh context):** no soundness bug in the oracle.
- Plain and instrumented builds were hashed identically over 100 seeds.
- The conflict test was unit-tested.
- The `lvl` (declaration level) rule for privates is right.

Fixed:
- **HIGH:** a file with no SAFE loop has no rewrite, which was reported as a compile failure. A stale
  `prog.omp.c` from a `--keep` directory could also be compiled silently.
- **MEDIUM (a run could pass while checking nothing):**
  - The run now fails if fewer than half the runs are valid or no SAFE loop ran ≥ 2 iterations.
  - The sequential run's exit status and line count are now checked.
- **MEDIUM (coverage):** two changes.
  - Per-instance validity replaced whole-run invalidation; the reviewer's prototype of it found
    84 more real conflict witnesses over 60 seeds.
  - Declared-dimension subscripts are now non-negative.
  - About 45% of loops used to die on calls or scalar writes before reaching the dependence test.
    Unconstrained functions now emit fewer of those, and the boundary shapes above were added.
- **LOW, kept as is:** the differential run is weak at 2 threads (1 of 10 injected races seen) and
  strong at 8, so CI stays at 8. The address oracle is the primary check anyway.

**Results:** three campaigns of 150 files each found **0 false SAFE, with every parallel output
identical**:
- before the coverage fixes: 3,341 loops, 866 SAFE;
- the reviewer's run: 3,356 loops, 821 SAFE;
- after the fixes (seeds 2000–2149): 3,269 loops, 839 SAFE, of which 805 ran ≥ 2 iterations
  in a clean instance; 95% of runs stayed inside their objects.

**Found while designing Phase 1.4 (open, runtime-confirmed):** `int i; for (i = 0; …) …; return i;`
is SAFE and gets `parallel for`. OpenMP makes `i` private, so `ivout` returned garbage (1829037980)
instead of 100. `lastprivate(i)` does not fix it: on a zero-trip loop it leaves `i` unchanged, where
sequential code has run `i = 0`. The generator only writes `for (int i …)`, which is why it was
missed. Phase 1.4 fixes the bug and teaches the generator this shape.

**Information only:** UNSAFE loops that ran but never conflicted are mostly `h_set(&A[i], …)` /
`h_add(A, i, …)`. Those are UNSAFE through the call-site-independent "writes through pointer
parameter" rule, an overclaim Phase 1.5 removes. A few need `m ≥ 5`, which is outside the
brute-forced range.

### 2026-10-12 — Phase 1.4: scalar privatization and reductions

**Decision:** New `src/analysis/ScalarAnalysis.{h,cpp}` classifies every scalar a loop writes by name,
using **liveness computed over Clang's CFG of the enclosing function**:
- **private:** dead at the start of the body (written before read in every iteration) and dead
  after the loop.
- **lastprivate:** private, but read after the loop, and written unconditionally at the body's top
  level with no `continue` that could skip it.
- **reduction:** every mention in the loop is one associative update with a single operator: `+`
  (`-=` combines with `+`), `* & | ^`, or min/max, either `x = x < e ? e : x` or
  `if (e > x) x = e;`.
- **otherwise UNSAFE**, a genuine loop-carried scalar.

Never privatized: globals and statics, address-taken variables, variables the loop's own
condition/increment uses, and non-arithmetic variables. `-fp-reductions` (off by default, the user's
decision on 2026-10-12) gates floating-point reductions.

The loop's own counter, declared outside and read after the loop, gets `lastprivate(i)` only when a
constant trip count ≥ 1 proves the loop runs. On a zero-trip loop `lastprivate` leaves `i` untouched,
where the sequential code ran `i = start`, so otherwise the loop is UNKNOWN. **This fixes the open
bug** from the fuzzer entry: `ivout` and two existing fixtures (`r1a`, `r1c`) got a plain `parallel
for` and an undefined counter afterwards.

**Reasoning:**
- **Why Clang's CFG, not `LiveVariables`:** `LiveVariables` answers "live at the end of a block".
  What's needed is liveness at the start of the loop body and right after the loop, which are two
  different successors of the same condition block. A small backward dataflow, over only the
  function's automatic, non-address-taken scalar variables, answers exactly those two points.
  Building the CFG with `setAllAlwaysAdd()` makes every read (`LValueToRValue` of a `DeclRefExpr`)
  and every plain assignment visible.
- **Why this shape matters:** PolyBench-style C declares `int i, j; double t;` at the top. Under the
  hackathon rule, every loop using an inner counter or temporary was UNSAFE. Now the outer loop of
  such a nest gets `private(j, t)`.
- **GPU:** loops with lastprivate or reduction clauses stay CPU-only (`GpuMappable` false). On a
  device, a reduction variable needs its own `map(tofrom:)`, which is Phase 2.1. `private` is fine on
  `target`.
- **Codegen:** the clauses are appended to both the CPU and GPU pragmas. The report gains
  `scalars : private(j, t) reduction(+: s)` for SAFE loops.
- **Existing fixtures:** six pinned verdicts changed, each read against its code:
  - three `for (k…) s += k;` loops inside statement expressions are now SAFE (`reduction`);
  - `r1a` and `r1c` inner loops went SAFE → UNKNOWN (the counter-after-loop bug, live in old
    fixtures);
  - the `r1a` outer loop went UNSAFE → UNKNOWN (its `j` is now correctly private; what remains is
    the uncomparable `a[j]` vs `a[j+1]`).

**Evidence:**
- **New fixtures:**
  - `tests/scalars.c`: 26 loops covering every clause and each refusal, among them a conditional
    write read after the loop, `continue`, mixed operators, a running value read elsewhere,
    `e - s`, an FP sum, a global, an address-taken variable, a written bound, and a zero-trip
    counter;
  - `tests/fp_reductions.c`, run with `-fp-reductions`.
- **Runtime:**
  - `scripts/check_parallel.sh tests/scalars.c eval/runtime/scalars_main.c` forces 12 pragmas, all
    with clauses, and their outputs and every return value match;
  - it discriminates: dropping all `reduction` or all `lastprivate` clauses fails on run 1;
  - **dropping `private` was invisible at `-O2`**, because the compiler keeps the shared scalar in a
    register. `check_parallel.sh` now builds and runs at **both `-O2` and `-O0`**, and at `-O0` the
    missing `private` fails on run 1;
  - both runtime checks are in CI.
- **Fuzzer:**
  - Generator: a third of the loops now use a counter declared at the top of the function
    (`int i, j, l;`), half of those read it after the loop. A new statement shape writes `s0`/`s1`
    and then uses them in the same iteration. Reductions are more frequent.
  - Oracle (`fuzzrt.h`): it reads each loop's clauses from the report (`clauses.h`). A private or
    lastprivate `s0`/`s1` is **checked**: the first touch in every iteration must be a write.
    Reductions are verified by the exact differential run (u64).
  - CI's 15 seeds: 24 SAFE loops with clauses (20 on `s0`/`s1`), 0 false SAFE.
  - **Four tool mutants are all caught within CI's 15 seeds:**
    1. privatize even when read first: caught by the per-iteration check ("iteration 0 reads
       private s1 before writing it");
    2. lastprivate emitted as private;
    3. every reduction as `+`;
    4. no zero-trip check on the counter.

    Mutants 2–4 are caught by the differential run.

### 2026-10-12 — Phase 1.4 adversarial review: five wrong-clause bugs, fixed

**Decision:** The fresh-context review confirmed five ways to get a SAFE loop with a wrong clause,
each by running sequential vs parallel. All are fixed, with fixtures [S1]–[S4] in
`tests/scalars.c` (also in the runtime driver):
1. **CRITICAL, a reduction whose running value is used:** `a[i] = (s += b[i])`, or
   `if ((s += x) > 100)`. That is a prefix sum, not a reduction. An update now counts as a
   reduction step only in a **discarded-value position**: a statement, an if/else branch, a for
   init/increment, a loop body, or the left side of a comma.
2. **min/max with mismatched types:** compared in `unsigned`, stored in `int`, or narrowed. Now the
   comparison's operands and the stored value must all have V's own type.
3. **An integer accumulator with a floating-point operand** (`s += b[i] * 0.5`): it truncates every
   step, and it also slipped past the `-fp-reductions` gate. Now rejected.
4. **`_Bool`:** every step normalizes to 0/1, so `-=`/`^=` aren't the reductions they look like.
   Now excluded.
5. **`_Complex` floating point** slipped past the FP gate. Complex types are now excluded, and the
   gate uses `isFloatingType()`.

Also fixed: a variable with a `cleanup` attribute is read invisibly at scope exit, so it is never
privatized.

**Test gap closed:** no lit test pinned the emitted clauses. The generator now pins the
`scalars :` line.

**Evidence:**
- The reviewer's repo-fuzzer campaign (`--seed 5000 --files 150`): 3,357 loops, 238 with clauses,
  0 false SAFE.
- Their scalar-focused fuzzer (290 files, mixed types, `_Bool`, value-used updates) found bugs 1, 2
  and 4 independently.
- After the fixes: 11 lit tests pass, both runtime checks pass at -O2 and -O0, and the CI-size fuzz
  run is clean.

**Caveat recorded:** in 2 of the reviewer's seeds, the *unmodified sequential* program gave
different output under Homebrew clang 22.1.8 at -O2 than at -O0. No undefined behaviour was found,
though it wasn't ruled out either, so this is a possible miscompile. `check_parallel.sh`'s -O2
comparison could therefore raise a false alarm under this compiler. The -O0 comparison is
unaffected.

### 2026-10-12 — Phase 1.5: call-site-aware interprocedural summaries

**Decision:** New `src/analysis/Summaries.{h,cpp}` replaces the call-site-independent callee rule
("a callee that writes through *some* pointer parameter rejects every loop that calls it"). Each
function gets a summary: the list of accesses its body makes, including those of everything it calls,
in AffineAccess's form but in the function's own terms:
- the base is a pointer parameter, a global/static object (globals are implicit parameters), or unknown;
- subscripts and loop ranges are affine in the function's integer parameters and its own loop
  variables.

At each call the arguments are substituted for the parameters. The resulting accesses are appended
to the loop's own, and **the unchanged Phase 1.3 dependence tests decide**. `helper(&a[i])` writing
`p[0]` becomes `W a[i]`. `row_fill(&a[i*m], m)` looping `row[j]`, `0 <= j < m`, becomes `W a[i][j]`
of rows of length m.

- **AffineAccess changes:**
  - It gained `analyzeFunction()`: a whole body with no root loop, where every loop is an inner loop
    and loop-variable cleanliness is checked per loop.
  - It records every call's arguments (`CallSite`/`CallArg`) in the same affine terms. A pointer
    argument may be a named pointer or 1-D array plus an offset, a row start `&a[X*N]`, or `&v` for a
    whole non-array, non-pointer `v`.
- **Construction:** summaries are built bottom-up on demand and memoized. A call back into a summary
  in progress (recursion), or a reachable function with no visible body, makes a summary
  *incomplete*. The existing call-chain walk then reports it, and the loop is UNKNOWN.
- **What is dropped:** only a callee's own automatic objects, which are fresh on every call. `&a[..]`
  accesses that are passed directly as an argument are also dropped, because the composed accesses
  replace them. Static locals stay, as shared objects.
- **What stays name-level:** writes to globals by name (UNSAFE, as before), atomics and asm
  (UNKNOWN), and calls through a function pointer (UNKNOWN).
- **Ablation:** `-ipa=coarse` keeps the old rule. On all 15 fixtures and benchmarks its output is
  byte-identical to the pre-1.5 binary, which was checked by building HEAD in a worktree.

**Limits, all in the conservative direction:**
- A callee subscript or loop range that mentions a global is non-affine at the loop, because the
  loop could change the global between calls.
- A pointer parameter the callee reassigns or lets escape, a global pointer, or an unresolved local
  pointer is an unknown base.
- A row-start argument composes only with a 1-D callee subscript `j + c`, where j is a callee loop
  variable or one of the caller's inner loop variables whose range provably stays inside the row.
- An *integer* argument like `h_add(C, i*n + j)` is not delinearized. That is a possible follow-up.
- A reduction through a call (`accumulate(total, x)`) stays UNSAFE.
- Distinct pointer parameters are still assumed not to alias; that is Phase 1.6.

**Fixture changes**, under the 2026-10-12 extension of the CHECK-change rule: false-UNSAFE → SAFE,
proven UNKNOWN → UNSAFE, and new reason text on loops that stay rejected.
- `nests.c` `caller`: UNSAFE → **SAFE**. This was the false UNSAFE pinned in Phase 1.1
  (`callee_with_loop(&a[i*n], n)`); it is now `W a[i][j]`.
- `dependence.c` D5, D6, D7 and E1: UNKNOWN → **UNSAFE**. Each callee reads `x[i+1]` (D5 also writes
  it) while the loop writes `x[i]`. The distance-1 dependence is real, and the summary now proves it.
- `safety_cases.c` `accumulate`: still UNSAFE. The reason is now "RW `total[0]`: every iteration
  touches the same element" instead of "writes through pointer parameter".
- `dependence.c` e_rds, e_rdl, e_wr_struct: still UNKNOWN, with the reason now naming the unnamable
  access.
- `interlock.c`: one composed read line was added. The verdict is unchanged.

**New fixture `tests/summaries.c`:** 23 labelled loops in matched pairs:
- `p[0]` vs `p[1]` through a pointer to one element;
- `p[k]` vs `p[k+1]` through an index argument;
- two calls, disjoint vs colliding;
- rows vs a row overrun (UNKNOWN, since the row model can't express the overrun);
- a transitive chain without and with a back-read;
- a global element vs `g_tab[0]` vs a global scalar;
- `&t` with t private vs shared;
- a callee-local scratch array;
- UNKNOWN for recursion, an indirect subscript, a reassigned parameter, a global offset and an opaque
  argument.

All 23 match their labels. The fixture runs under `check_parallel.sh` with
`eval/runtime/summaries_main.c`, in CI too.

**Evidence:**
- 12 lit tests pass.
- `check_parallel.sh` reports bit-identical output at -O2 and -O0 for summaries.c (30 runs),
  dependence.c (30) and scalars.c (20).
- Oracle mutation: dropping the composed accesses forces 24 pragmas instead of 13, and the runtime
  check fails on run 1.
- Fuzzer, seeds 1–15, before → after: SAFE 76 → 87; UNSAFE loops that never conflicted 12 → 3; still
  no false SAFE.
  - Ten loops went UNSAFE → UNKNOWN. In each, the coarse rule was the only evidence for UNSAFE, and
    the real subscripts (`i*n + j`, `j <= n`) are non-affine. Both verdicts block parallelization.
- A fresh fuzz run (seeds 100–159, 1,356 loops, 319 SAFE) has no false SAFE.

### 2026-10-12 — Phase 1.5 adversarial review: two false SAFE through K&R calls, fixed

**Decision:** A fresh-context review found two false SAFE verdicts, each confirmed at runtime with
`check_parallel.sh`. Both are calls to unprototyped (K&R) functions. With no prototype, nothing is
converted at the call site, so the argument's value is not the parameter's value:
- `kr(p, k) long *p; signed char k;` called as `kr(a, i)`. Here `k` is really `(signed char)i`, which
  repeats every 256 iterations, but the summary composed `a[i + 128]`.
- `kr3(p) char *p; { p[8] = 1; }` called as `kr3(&a[2*i])`. The callee steps over bytes, but the
  offset was composed in `long` elements as `a[2i + 8]`.

Both loops were UNSAFE under `-ipa=coarse`, so Phase 1.5 introduced them.

**Fix:** `usableArg()` in Summaries.cpp is now the only place a parameter's argument is looked up.
An argument is substituted only when both hold:
- the callee is prototyped, both at the definition and at the call;
- the argument's type equals the parameter's type, up to qualifiers.

Otherwise every access through that parameter, and every subscript or row length using it, is
non-affine (UNKNOWN). Both reproducers are pinned in `tests/summaries.c` (`k_kr_narrow`, `k_kr_bytes`)
and run in its runtime driver.

**Also from the review:**
- Precision losses, recorded for later:
  - One callee reached at two sites with different loop lengths loses its loop variable's range
    (`mergeRange`). Renaming callee variables per site would fix it.
  - A callee range bounded by the caller's inner loop variable is UNKNOWN.
  - `&s[i].x` arguments are rejected.
  - Some overlapping `<=`, `!=` and descending callee loops are UNKNOWN rather than UNSAFE, which
    still blocks.
- Code quality: the "indexes with global" reason now says "not one of its parameters", because the
  symbol may also be a callee's static. The recursion stub is set once, in the constructor.
- Checked correct by the reviewer: static locals are never privatized; global arrays; a reassigned
  pointer parameter; `&scalar` and `&struct` arguments; returned or loaded pointers; `_Bool`, enum and
  unsigned row lengths; mixed 1-D/2-D shapes; transitive remapping; overlapping same-callee sites;
  triangular ranges; mutual recursion.

**Evidence after the fix:**
- 12 lit tests pass.
- `check_parallel.sh` passes for summaries.c (30 runs), dependence.c (30) and scalars.c, at -O2
  and -O0.
- Both reproducers are UNKNOWN.
- Fuzz seeds 1–15 and 300–339: no false SAFE.

### 2026-10-13 — Phase 1.6: aliasing made explicit instead of assumed

**Decision:** New `src/analysis/AliasAnalysis.{h,cpp}` removes the soundness hole documented in
`SafetyAnalysis.h` since Week 2. Accesses through two differently named bases used to be assumed
disjoint. `f(long *a, const long *b)` writing `a[i] = b[i + 1]` was SAFE, yet with `b == a` it is
`a[i] = a[i + 1]`. Now DependenceTest asks an oracle about every pair of differently named bases
where one is written:
- **Distinct:** two named objects; a pointer parameter against this call's own automatic objects; a
  `const` object; or a static/global object whose address never escapes, meaning every use reads,
  writes or directly subscripts it by name. This counts only when all uses are visible: a static
  local, a `static` global, or any global under `-whole-program`.
- **Types:** element types that C's strict-aliasing rule keeps apart (never char, void or records).
  This is the same assumption AffineAccess already makes.
- **Restrict:** either base is a `restrict` pointer parameter.
- **Callers:** every call of the function passes disjoint objects. Callers' own parameters are
  proven the same way, up to 3 levels. This needs all calls visible: the function is `static` (or
  `-whole-program` is given), its address is never taken, and the calls are prototyped.
- **Unproven:** the tool builds a run-time check that the byte ranges the loop touches through the
  two bases don't overlap.
  - Each subscript's extremes come from the loop's and inner loops' ranges (`subscriptBounds`, the
    same arithmetic as Banerjee). Multi-dimensional and delinearized accesses are linearized with
    their row lengths.
  - The test is integer arithmetic on `__INTPTR_TYPE__`, so it forms no out-of-bounds pointer and
    compares no pointers across objects.
  - It is conjoined into `if(parallel: ...)` under every policy. A false check runs the loop on one
    thread.
  - If no range can be computed, the loop is UNKNOWN.

A pointer parameter can also point at a global the loop reads by name, so non-array, non-const
globals read in the loop or in a reachable callee join the accesses as one-element reads.

**User decisions (2026-10-13):**
- GPU offload needs a static proof. A runtime-checked loop stays on the host: a false `if(target:)`
  still runs teams/parallel on the host, and overlapping arrays can't be mapped as separate regions.
- The caller proof covers only `static` functions, unless `-whole-program` says no other TU calls
  this file's functions.
  - `scripts/build_and_run.sh` passes `-whole-program`, because the benchmarks are complete programs.
  - So do the RUN lines of `profitability_cases.c` and `interlock.c`, to keep their GPU coverage.
- `-alias=assume` keeps the old assumption as the ablation baseline. It is byte-identical to `70764bd`
  on all 16 fixtures and benchmarks.

**Fixture changes:**
- Lines added only. The generator now pins `alias :` and `alias guard:` (the field regex needed
  `\s*`).
- Across the old fixtures: 35 pairs checked at run time, 14 proven by callers, 11 by element types.
- With `-whole-program`, every verdict in the benchmarks and GPU fixtures equals HEAD's.
- One loop was briefly SAFE → UNKNOWN (`dependence.c`, a `static double t[64]` declared in the loop
  body). It is SAFE again through the escape rule, because `t` is only ever subscripted.

**New fixture `tests/alias.c`:** run-time checked (two parameters, delinearized rows, a parameter
vs a global array, a parameter vs a global scalar, a static function with one overlapping call);
proven (restrict, element types, callers); UNKNOWN (prefix sum: the inner range depends on `i`).
`eval/runtime/alias_main.c` calls every checked loop with **overlapping** arguments too: `b == a`,
rows shifted by one, `p == g_arr`.

**Evidence:**
- 13 lit tests pass.
- `check_parallel.sh` passes on alias.c, dependence.c, summaries.c (30 runs each) and scalars.c, at
  -O2 and -O0.
- Mutation checks: with `-alias=assume` (no guards), alias.c mismatches on run 1, and the fuzzer
  finds 8 mismatches in seeds 1–15.
- The fuzzer now repeats its differential run with `B == A`, `B == A + 1` and `C == B - 1`. Seeds
  1–15 and 100–159 are clean.

**Precision cost:** fuzz seeds 1–15 go from SAFE 87 to 79. The lost loops read a non-affine subscript
(`B[i % 3]`, `A[i * n + j]`, `h_get(p, o)`) of a pointer that could be the written one. No range
means no check, and UNKNOWN is the honest answer. Possible follow-ups: bound non-affine subscripts
by the array's known size; check triangular ranges at their outer extremes.

### 2026-10-13 — Phase 1.6 adversarial review: four wrong guards, one contract hole, fixed

**Decision:** A fresh-context review found these, confirmed by runtime mismatch or compile error:
1. **A shadowed parameter.** A base reached through a resolved local pointer (`long *x = a;` then a
   block with its own `long *a`) was named `a` in the guard, which meant the inner local at the
   pragma. Runtime mismatch.
2. **A global reached only through a callee:**
   - declared after the loop, the emitted pragma didn't compile (`use of undeclared identifier`);
   - shadowed by an enumerator, the guard silently compared against 0. Runtime mismatch.
3. **A global pointer read by name was sized as its pointee** (1 byte for a `char *`), not as the
   8-byte variable. `c` legally pointing into it raced. Runtime mismatch.
4. **`-whole-program` also made external globals "never escaping".** That is stronger than the flag's
   contract: another TU can take `&g`.
5. **Types proof:** `void *` against `long *`, and `const long *` against `long *`, were called
   incompatible element types. Compilers' TBAA treats these as aliasing. Runtime mismatch.
   `long` against `long long` overlapping is ISO undefined, but the overlap is real in practice.

**Fixes:**
- Every name a guard writes must mean its declaration just before the loop. That covers bases, the
  symbols and enclosing-loop variables in their subscripts, row lengths, and the symbols in the used
  ranges. Nothing else in the function (a variable, enumerator, typedef or function, in any block)
  may share the name, and a global must have a declaration before the loop. Otherwise there is no
  check and the loop is UNKNOWN.
- A by-name access (no expression) is sized by the variable's own type.
- The never-escapes proof is limited to static locals and internal-linkage globals.
- Any two pointer element types, and integers of the same width, may alias.
- Quality: identical non-affine guard pieces are deduplicated, and `+1` is applied after the cast to
  `__INTPTR_TYPE__`.

All review cases are pinned in `tests/alias.c` (`k_shadowed_param`, `k_late_global`,
`k_enum_shadow`, `rt_global_ptr`, `rt_void_ptrs`, `rt_same_width`). The driver calls the
checked ones with overlapping arguments.

**Fuzzer harness bug found on the way:** the aliased differential variants reused the disjoint run's
valid parameter combos. But aliasing changes control flow: seed 531, `C == B - 1`, took a branch
that wrote `H[10][0]` past `H`, and that UB differed at -O2. The instrumented build now also runs
each variant, with the aliased parameter's region set to the buffer it points into, and each variant
is differential-tested only on its own valid runs.

**Evidence:**
- 13 lit tests pass.
- `check_parallel.sh` passes on alias.c, dependence.c, summaries.c (30 runs) and scalars.c.
- Without guards (`-alias=assume`), alias.c and fuzz seeds 1–15 still mismatch, so the mutation
  checks still bite.
- Fuzz seeds 1–15 and 500–539 are clean.
- `-alias=assume` is byte-identical to `70764bd` on all 17 fixtures and benchmarks.

**Precision losses the review noted, not fixed:**
- A parameter reassigned before the loop is BaseMayAlias.
- A static-local buffer can't be named in a guard.
- `long (*a)[m]` VLA rows aren't linearized.
- A callee loop bound by a global is unbounded.

### 2026-10-16 — Phase 2.1: map() sections from the subscripts' ranges, collapse(2), scalar maps

**Decision:** New `src/analysis/DataRegions.{h,cpp}` (`RegionPlanner`) sizes every GPU `map()` from
the index ranges the loop's accesses actually reach. The Phase 1.3 interlock (GPU only for loops
whose accesses are all `base[i]`) is lifted: a loop goes to the GPU when every base gets a section
and no callee reaches memory a `map()` can't name. `src/analysis/SourceText.{h,cpp}` holds the
expression printing and the "can this name be written before the loop" check, moved out of
AliasAnalysis (byte-identical refactor).

**A latent bug this fixes.** The old regions mapped a never-read array `from` even when the loop
writes only some of its elements (`if (c[i]) y[i] = v`). On a device with its own memory that
copies uninitialized device memory over every element the loop left alone. It never showed because
every run so far was host-only. Confirmed: hand-editing the new `tofrom` back to `from` in
`tests/maps.c` (`cond_write_g`) gives a mismatch on the host-offload device. The descending-loop bug
from the Phase 0 review is fixed too: `benchmarks/loop_shapes.c`'s countdown maps `out[0:65536]`,
not `out[0:0]`, and the degrade-to-CPU path only remains under `-maps=bound`.

**How a section is built** (header comment of `DataRegions.h` has the full rule):
- Each subscript's smallest and largest value over the loop's and inner loops' ranges
  (`subscriptBounds`, as for the Phase 1.6 overlap checks), on the outermost dimension.
- **Object** (array with constant extents): clamped to `[0, extent)`, no further conditions.
- **Pointer** (`p[lo:len]`, a linear `a[lo:len]` for delinearized `a[i*n + j]`, or `m[lo:len][0:M]`
  for a pointer to rows): mapping copies every element of the section, so both ends must be
  elements the program surely touches. Only *exact* accesses define them: on every iteration (no
  `if`, `?:`, `&&`/`||`, `switch` on the way; callees without early exits) with every loop variable
  stepping ±1 over its whole range (`IVRange::Exact`). Every other access must lie inside, by a
  constant offset. The ends are reached only if the loops run, and the lowest index must be >= 0:
  those conditions are the **map guard**, conjoined into `if(target:)` under every policy (false
  runs the region on the host, where nothing is copied).
- **Direction:** `to` if never written; `from` only if never read and one exact plain write spans
  the whole section; otherwise `tofrom`.
- Reduction and lastprivate scalars get `map(tofrom: s)`, so reductions can now go to the GPU.

**collapse(2)** (`markCollapsible`, GPU pragma only): a perfect two-level nest, both loops
recognized and SAFE on their own (the outer verdict excludes dependences between outer iterations,
the inner one those within one), the inner range exact and rectangular with bounds fixed during the
nest (AffineAccess's symbols), no inner run-time alias check, and the inner variable not
lastprivate. The CPU pragma doesn't collapse: it can cost the inner loop its vectorization, and the
model has no term for parallel width. The driver now computes every loop's safety before reporting
any, since the outer loop's decision needs the inner verdict.

**Cost model:** constant-trip inner loops' operations are multiplied by their trip count, and
callees are counted at their call sites (memoized, recursion-guarded); `compute_heavy`-style helpers
are no longer 4 ops. Bytes per iteration and transfer size come from the section sizes when they are
constant (fixes the descending pricing). Per-iteration units are no longer floored at one: an inner
loop over `g[i][j]` touches one row per run, not per iteration (that floor briefly flipped
`dependence.c`'s inner `grid2` loop to CPU_PARALLEL for the wrong reason).

**Fixture changes** (all of the allowed Phase 2 kinds; safety verdicts unchanged):
- `collapse :` lines and `map guard:` lines added (the generator now pins both).
- Loops the interlock declined now show their section and go GPU where the model says so
  (`interlock.c` shifted/delinearized/2-D cases), or a sharper decline reason (`a[i * 8]` passed on,
  a VLA row, a conditional access, index -4 below the pointer, a callee reaching a global).
- `dependence.c` `scale_grid` outer loop: SEQUENTIAL → CPU_PARALLEL, from the inner-trip costing
  (192 ops per row, not 3).
- `loop_shapes.c` countdown: `out[0:0]`/`in[0:0]` → `[0:65536]`.

**New fixture `tests/maps.c`** + `eval/runtime/maps_main.c`: conditional writes (pointer: no GPU;
global: `tofrom`), full / strided / shifted writes, a conditional shifted read (declined), a
countdown, a clamped global part, rows, delinearized, inner dependence / triangle / `j` read after
(no collapse), one column of each row (`tofrom`), integer reduction, lastprivate, and a callee
writing a row through `&a[i*m]`. The driver fills every output with random values first and runs
each symbolic loop with n = 5000, 1 and 0 (guard false).

**Separate-memory runtime check:** `OFFLOAD=1 scripts/check_parallel.sh` rewrites with `-policy
naive` and builds with `-fopenmp-targets=<host>-unknown-linux-gnu`, run with
`OMP_TARGET_OFFLOAD=MANDATORY`. LLVM's host plugin gives target regions memory of their own, so a
short section or a wrong `from` changes the output (confirmed by hand mutations: `from` for
`tofrom` mismatches; a section one short corrupts the heap). Linux only (`liboffload-22-dev`); CI
runs it on all five runtime fixtures, and `eval/docker/Dockerfile` runs it on a Mac.

**Bug found on the way:** a loop whose whole body is one call without braces (`for (...) f(...);`)
never counted the call as unconditional (the parent walk started above its stop node), so every
callee access was declined. Fixed in `Unconditional`.

**Evidence:**
- 14 lit tests pass; `check_parallel.sh` passes on all five runtime fixtures (CPU, 10 runs, -O2
  and -O0) and with `OFFLOAD=1` (47 target regions on the host device, 18 of them in maps.c).
- Fuzz seeds 1–15 clean.
- NVPTX device compile (`--offload-device-only -S`) of every fixture and benchmark rewritten with
  `-policy naive`: about 100 target regions, all compile.
- `-maps=bound` is identical to `eeb8734` on 22 files under four flag sets, except for where the
  casts sit in run-time check text (the review fix below changes Phase 1.6's guards too).
- GPU-eligible loops across fixtures and benchmarks (`-policy naive`): 36 → 122.

**Precision limits, not fixed:**
- A pointer written or read only under a condition gets no section (common: `if (x[i] > 0) y[i] =`).
  Declared array sizes (Phase 2.5 item 3) would bound it.
- Strides other than ±1 are not exact, so `a[2*i]` written with `i += 2` is declined.
- An inner loop's per-iteration bytes, when its section isn't constant, are charged a whole row.
- Object sections with symbolic ends carry `?:` clamps; correct, but long.

### 2026-10-16 — Phase 2.1 adversarial review: four wrong sections, fixed

**Decision:** A fresh-context review wrote about 30 adversarial probes and ran them on the host-offload
device (Docker, `OMP_TARGET_OFFLOAD=MANDATORY`) against sequential builds. It found four bugs, each
a wrong result or a crash:
1. **`from` over an Object when the covering write's inner loop runs zero times.** In
   `for (i < n) for (j < m) g[i] = ...`, the section `g[0:n]` doesn't depend on j, so `m = 0`
   copied device garbage over `g[0..n)`. The planner only required the loops around an end-defining
   access to run for *pointer* layouts. Now a write that proves `from` puts the analyzed loop and
   every loop around it into the map guard (`n >= 1 && m >= 1`), for every layout. A loop with a
   constant empty range now declines instead of being skipped.
2. **Unsigned arithmetic in sections and guards.** `asIntPtr` cast the finished expression,
   `(intptr)(n - k)`, so the subtraction happened in `unsigned` and wrapped for `n < k`: the guard
   came out true and the section was ~2^32 elements (abort). Now each variable is cast before any
   arithmetic, `((intptr)n - (intptr)k)`. The same function prints Phase 1.6's overlap checks, so
   they change text too (fixture lines changed only in cast placement).
3. **Unsigned clamps on an Object.** `(lo < 0 ? 0 : lo)` and `hi - lo + 1` stayed unsigned: a
   zero-trip loop over an unsigned `n` mapped all of `G` `from`. A bare variable is now written as
   itself only when it is a signed integer; otherwise it is cast.
4. **`_Generic`'s controlling expression counted as evaluated.** `b[i + 1000]` inside it widened
   `b`'s section past the object (SIGSEGV against a guard page). `Unconditional` now accepts only the
   selected association of a `_Generic` and the chosen side of `__builtin_choose_expr`.

Also: the `map guard:` report line is now printed only for SAFE loops.

All four are pinned in `tests/maps.c` (`cover_inner`, `unsigned_ptr`, `unsigned_obj`, `generic`), and
the driver runs them with the failing arguments (`m = 0`, `n < k`, `n = 0`). Dropping `m >= 1` from
`cover_inner`'s guard by hand gives a mismatch. All 22 runnable review probes now match the sequential
build.

**Not fixed (noted):**
- A descending loop with a symbolic bound is still priced with the bound as its trip count. That is
  the Phase 0 issue; it now reaches the GPU path, where the map guard keeps it correct. The cost
  guard (`lo >= 256`) is meaningless there. This belongs to the Phase 2.3 calibration work.
- Guards can be redundant (`lo >= 0 && lo >= -1`), and with unsigned symbols some are constant.
- Object and Rows sections rely on C's rule that a subscript stays inside its own dimension, as
  the dependence test does.

