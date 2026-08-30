# P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

SegFault 2026 hackathon project. See `NOTES.md` for the running design-decision log.

## Quick start

```bash
mkdir build && cd build
cmake -G Ninja .. -DCMAKE_PREFIX_PATH=<path to your LLVM/Clang install's cmake config dir>
ninja
./p05tool ../benchmarks/example.c --
```

If `find_package(Clang REQUIRED CONFIG)` can't find Clang, point `CMAKE_PREFIX_PATH` at the `lib/cmake`
directory of your LLVM/Clang installation (e.g. wherever `ClangConfig.cmake` lives).

## Status

**Days 11-13 — GPU-profitability heuristic.**

The tool classifies every `for` loop it finds as one of:

- `CONSTANT_BOUND` — bound folds to an integer constant; trip count extracted where the start
  value is also known.
- `VARIABLE_BOUND` — canonical loop shape, bound not statically known, recorded symbolically
  (e.g. "bound is parameter `n`").
- `UNRECOGNIZED` — not a simple canonical counted loop, with a specific reason. Flagged rather
  than silently mishandled.

It also records the direct callees of each loop body (`LoopInfo::Callees`) and, on top of that,
resolves what those callees transitively call across function boundaries — `CallResolver`
(`src/analysis/CallResolver.h/.cpp`) wraps Clang's `CallGraph` utility, keyed by `FunctionDecl*`
rather than by loop, with a breadth-first walk and a cycle guard. It reports three reachability
facts per call chain: how many functions are reachable and how deep, whether any of them has no
visible body in this translation unit (`HasOpaqueCallee` — a stand-in for a real library call), and
whether the chain recurses (`HasRecursion`, detected with a dedicated cycle search so that two
functions sharing a helper — a diamond, not a cycle — is never mistaken for recursion). This layer
reports reachability only; judging whether a reachable function's side effects make the loop unsafe
is the Week 2 safety pass. Analysis lives in `src/analysis/`; `src/driver/main.cpp` is wiring only.

Verified with a matched benchmark pair — `benchmarks/loop_shapes.c` (10 loops, all recognized)
and `benchmarks/loop_unrecognized.c` (8 loops, all flagged, distinct reasons) — plus a dedicated
`tests/call_chains.c` fixture (a 2-level chain, a 3-level chain, a call with no visible body, and
mutual recursion). Both directions matter: a classifier that only ever says yes proves nothing.

On top of that, `SafetyAnalyzer` (`src/analysis/SafetyAnalysis.h/.cpp`) turns those reachability
facts into a per-loop verdict — `SAFE` / `UNKNOWN` / `UNSAFE`, a tri-state rather than a bool, so
"the analysis cannot see this far" (an opaque callee, an indirect call, recursion) is never
conflated with "a hazard was actually found." Two independent hazard sources feed it: whether any
function reachable from the loop's callees writes through a pointer parameter or touches a
global/`static` (coarse and call-site-independent — flagged regardless of which argument a given
call site passed), and whether the loop's own body reassigns its induction variable or carries a
dependence across iterations (every array/pointer access, read or write, must be indexed by exactly
the induction variable — this closes the two `TODO`s that were previously open in
`src/analysis/LoopInfo.h`).

Verified against a new `tests/safety_cases.c` fixture (twelve loops, every safe/unsafe/unknown
category, matched pairs in both directions) plus all three demo benchmarks landing `SAFE`. An
adversarial pass done directly against the built tool (not a subagent — the review channel hit
repeated infrastructure failures) found and fixed one real overclaim: the body-dependence rule is
sound for two references to the *same* array, but not across *distinct* pointer parameters that
might alias with a nonzero shift (`a[i] = b[i] + 1.0` verifies `SAFE` even though a caller passing
`b == a + 1` makes it a real cross-iteration hazard). That is now documented as the layer's real,
unverified assumption — see `SafetyAnalysis.h` and the Day 8-10 entry in `NOTES.md` — rather than
silently left as an overclaimed guarantee. A second, non-dangerous finding from the same pass: 2D
access (`m[i][j]`) is currently always flagged `UNSAFE`, a completeness gap rather than a soundness
one, left undone since no current benchmark is 2D.

On top of that, `ProfitabilityAnalyzer` (`src/analysis/Profitability.h/.cpp`) turns a `SAFE` loop's
shape, its array accesses, and its interprocedural arithmetic-operation count into an
`OffloadTarget` — `GPU_OFFLOAD`, `CPU_PARALLEL`, or `SEQUENTIAL`, a three-way split rather than the
binary profitable/not-profitable the roadmap originally sketched, because "safe but only 8
iterations" is a real, distinct case from "safe and worth threading". The decision is a
roofline-style cost model (`t_seq`/`t_cpu`/`t_gpu`, argmin) driven by a documented `MachineModel`
struct — launch overhead, PCIe/DRAM/GPU bandwidth, CPU/GPU throughput, core count — with every one
of its six parameters overridable from the command line (`--pcie-bandwidth`, `--gpu-throughput`,
etc.), so the decision can be re-run against different hardware assumptions rather than trusted on
faith. The reported gate cascade (ops/iteration, data/iteration, intensity vs. this machine's
break-even, the three modelled times) is a rendering of that same computation, not a second,
independently hand-set set of thresholds.

Working the model's own arithmetic before building around it surfaced that the default
`MachineModel` implies a break-even arithmetic intensity of ~2.85 flop/byte, and **all three
existing demo benchmarks sit 10-50x below it** — `saxpy.c` included, despite an earlier version of
its own header comment calling it "the headline GPU-profitable case" (SAXPY is textbook
memory-bound; that comment was wrong and has been corrected). A fourth benchmark,
`benchmarks/compute_heavy.c` (40 fused multiply-adds per element, 5.0 flop/byte), was added so the
tool has at least one loop that clears the bar and verifies unconditional `GPU_OFFLOAD` — Days
14-16's GPU codegen path needs a real input. For loops with a parameter (not compile-time-constant)
bound — most of the existing SAFE benchmark set, including `example.c`'s headline cross-function
loop — the model still gives a real answer via a solved crossover trip count rather than a guess or
an automatic demotion: `GPU_OFFLOAD if (n >= T)`, with `T` derived algebraically from the same six
machine parameters (`tests/profitability_cases.c`'s `gpu_conditional_case` verifies `T = 8192`),
rendered in Week 3 as OpenMP's own `if()` clause.

Verified against a new `tests/profitability_cases.c` fixture (six loops — unconditional and
conditional `GPU_OFFLOAD`, two `CPU_PARALLEL` cases, `SEQUENTIAL`, and a known-`UNSAFE` loop
confirming profitability never runs on anything the safety pass didn't clear) plus all four demo
benchmarks, every verdict matching hand-derived numbers to three decimal places. No regression in
any Days 2-10 classification or safety verdict.

Next: Days 14-16 — `Rewriter`-based pragma insertion for the GPU-offload path
(`#pragma omp target teams distribute parallel for` with `map()` clauses already built by
`ProfitabilityAnalyzer::analyzeLoop`'s `ArrayRegion` list).
