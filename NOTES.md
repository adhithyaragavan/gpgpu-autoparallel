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
