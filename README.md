# P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

SegFault 2026 hackathon project (solo build). A Clang LibTooling-based source-to-source
tool: point it at unmodified, ordinary sequential C, and it finds loops safe to
parallelize — including ones whose bodies call helper functions — decides which of
those are actually worth offloading to a GPU versus threading on the CPU versus
leaving alone, and rewrites the source with the corresponding OpenMP pragmas.

The brief's suggested base was ROSE, whose main advantage is built-in parsing support
for programs that already *contain* CUDA/OpenMP/UPC constructs. This project's input
is plain sequential C — it *generates* OpenMP directives, it doesn't parse existing
GPU code — so that advantage doesn't apply, and ROSE's build overhead was judged a
disproportionate risk for a solo, time-boxed build. Clang LibTooling gives equivalent
source-level AST analysis and source-to-source rewriting with a much lower setup cost.

## The pipeline

Five passes, run in this order for every candidate loop:

1. **Loop detection** (`src/analysis/LoopAnalysis.{h,cpp}`) — finds every `for` loop,
   classifies its bound (`CONSTANT_BOUND` / `VARIABLE_BOUND` / `UNRECOGNIZED`, the last
   with a specific reason), and extracts a trip count where one is statically visible.
   Constant bounds are detected by constant *folding* (`Expr::EvaluateAsInt`), not by
   matching integer literals, so `const int N = 512` counts.
2. **Interprocedural call resolution** (`src/analysis/CallResolver.{h,cpp}`) — wraps
   Clang's `CallGraph` utility, keyed by `FunctionDecl*`, and walks each loop's callees
   transitively (breadth-first, with a cycle guard) to find every function reachable
   from the loop body, not just the ones it calls directly.
3. **Safety analysis** (`src/analysis/SafetyAnalysis.{h,cpp}`) — a tri-state verdict
   (`SAFE` / `UNKNOWN` / `UNSAFE`, not a bool, because "cannot see this far" and "found
   a real hazard" are different findings that should stay distinguishable) from two
   coarse, conservative checks: does anything reachable from the loop's callees write
   through a pointer parameter or touch a global, and does the loop body itself carry
   a dependence across iterations (every array/pointer access must be indexed by
   exactly the induction variable).
4. **Profitability** (`src/analysis/Profitability.{h,cpp}`) — a roofline-style cost
   model (`t_seq`/`t_cpu`/`t_gpu`, argmin), driven by a `MachineModel` struct whose
   parameters are all overridable from the command line. Turns a `SAFE` loop's trip
   count, array-access pattern, and interprocedural arithmetic-operation count into one
   of `GPU_OFFLOAD` / `CPU_PARALLEL` / `SEQUENTIAL` — three-way rather than a binary
   profitable/not, because "safe but only 8 iterations" is a real, distinct case from
   "safe and worth threading." A parameter-bound loop gets a solved runtime guard
   (`GPU_OFFLOAD if (n >= T)`) rather than a guess or an automatic demotion — `T` is
   derived algebraically from the same machine parameters. A second policy,
   `--policy=naive`, offloads every `SAFE` loop unconditionally instead, for measuring
   against — see Results below.
5. **Codegen** (`src/codegen/OmpRewriter.{h,cpp}`) — Clang's `Rewriter` API inserts
   `#pragma omp target teams distribute parallel for` with `map()` clauses for
   `GPU_OFFLOAD` loops (wrapping their transitive callees in `declare target`), or
   `#pragma omp parallel for` for `CPU_PARALLEL` loops, into a sibling `<name>.omp.c` —
   never in place. `map()` direction and extent are read straight off the
   profitability pass's own array-region analysis, not recomputed.

`src/driver/main.cpp` is wiring only — it owns no analysis logic.

## Quick start

```bash
mkdir build && cd build
cmake -G Ninja .. -DCMAKE_PREFIX_PATH=<path to your LLVM/Clang install's cmake config dir>
ninja
./p05tool ../benchmarks/example.c -- -isysroot "$(xcrun --show-sdk-path)"
```

If `find_package(Clang REQUIRED CONFIG)` can't find Clang, point `CMAKE_PREFIX_PATH` at
the `lib/cmake` directory of your LLVM/Clang installation (e.g. wherever
`ClangConfig.cmake` lives). `-isysroot` (macOS only; drop it elsewhere) is needed
because the benchmarks `#include <stdio.h>`/`<time.h>` to print a checksum and measure
timing.

To see the whole pipeline narrated end to end, one stage at a time:

```bash
./scripts/demo.sh                # paced, waits for Enter between stages
DEMO_PAUSE=0 ./scripts/demo.sh   # runs straight through (for a recorded backup)
```

To regenerate every benchmark's OpenMP pragmas and run the full build/run/timing/
device-codegen/map-clause verification sweep, all three policies, in one step:

```bash
./scripts/build_and_run.sh
```

To emit a rewritten `.omp.c` yourself: `./p05tool -rewrite <file.c> -- <clang args>`
(add `-policy naive` for the unconditional-offload comparison policy).

## Results

**Correctness: exact, not approximate.** Every rewritten benchmark's output is
compared element-by-element (not just by a summed checksum, which can't catch a
swapped pair or compensating errors) against the sequential baseline, bit-for-bit.
Every benchmark that produces a pragma at all — under both the gated and the naive
policy — passes exactly:

| benchmark | elements | gated | naive |
|---|---|---|---|
| example | 1000 | exact match | exact match |
| saxpy | 65536 | exact match | exact match |
| small_update | 8 | n/a — gated declines, no `.omp` binary to compare | exact match |
| compute_heavy | 65536 | exact match | exact match |

**Timing: measured, not modelled — a three-way comparison.** `clock_gettime` brackets
each benchmark's headline call; numbers below are the min of 7 repeats, median of 3
confirmation runs, on an unloaded development machine with no real GPU (every `.omp`
binary here runs its target regions on the host — see "What it doesn't do" below for
what that does and doesn't prove). The `saxpy`/`compute_heavy` "tie" rows are
noise-dominated by construction — both are within ~10% between naive and gated, small
enough that re-running the ladder can flip which one reports faster; that instability
*is* the finding for those two rows, not a caveat on it (see the explanation below).
`example`'s gap is not noise: it is reproducibly ~6.5-8x across repeated runs.

| benchmark | sequential | naive (offload everything safe) | gated (this tool's policy) | result |
|---|---|---|---|---|
| example (n=1000) | ~0us | ~30us | ~228us | naive **~7x faster** (reproducible) — see below |
| saxpy | ~11us | ~29us | ~32us | tie (noise-level) |
| small_update | ~0us | ~346us | n/a (declined) | gated **correctly pays nothing** |
| compute_heavy | ~211us | ~83us | ~80us | tie (noise-level) |

Two of these four results are not what they look like at a glance, and both are
reported here deliberately rather than smoothed over:

- **`small_update` is the clean case gated policy exists for.** An 8-element loop:
  naive pays real cost (~346us) to offload it for no benefit; the cost model
  correctly recognizes there's nothing to gain and declines, costing nothing.
- **`example` is this project's most important finding, and it runs backwards from
  what "gating helps" would predict.** Naive measures ~6.5-8x *faster* here,
  reproducibly across repeated runs — not
  because the gated policy's guard made a bad call (it correctly keeps `n=1000`
  serial, below its own `n >= 4096` threshold) but because `example.omp.c` has
  exactly one OpenMP construct in the whole file: libomp's one-time runtime
  cold-start cost is paid the moment that construct is touched, *regardless of
  whether its guard evaluates true* — and with no second OpenMP construct anywhere
  in the file to absorb that cost, gating a file's only construct is the worst case
  for this cost, not the best. The cost model has no term for it at all. This is an
  honest, checked limitation, not a bug to paper over — see `NOTES.md`'s Day 20 and
  Day 21 entries for the full derivation.
- `saxpy` and `compute_heavy` tie between naive and gated not because the two
  policies agree, but because with no real GPU hardware, a `target teams distribute
  parallel for` region and a `parallel for` region dispatch through the same libomp
  fork-join path on this host — a real limit on what this evidence can show, not a
  wash for the policy design.

Full reasoning, every measured number, and the harness bug an adversarial review
caught and fixed while building this comparison: `NOTES.md`, Days 19-21.

## What it doesn't do

Stated up front rather than left for someone else to find:

- **Aliasing between distinct pointer parameters is assumed away, not verified.**
  `void f(double *a, double *b, int n) { for (i) a[i] = b[i] + 1.0; }` verifies
  `SAFE`, but if a caller passes `b == a + 1`, the loop is really `a[i] = a[i+1] +
  1.0` in disguise — a genuine cross-iteration hazard this check cannot see, because
  it never reasons about whether two *different* array parameters might alias. This
  is the same promise C's `restrict` keyword makes explicit, made here implicitly
  and unverified — full points-to analysis is a research-grade problem, out of scope
  for a hackathon build. Documented in `SafetyAnalysis.h`; found by this project's
  own adversarial review, not by an outside reviewer.
- **No 2D loop nest can currently verify `SAFE`.** `m[i][j]`-style access is always
  flagged `UNSAFE`: the safety check validates every access against a single loop's
  own induction variable, with no notion of "this index belongs to a loop nested
  inside the one being judged." A completeness gap, not a soundness one — real
  matrix/stencil kernels would need this closed first.
- **`WhileStmt`/`DoStmt` are not classified at all** — only canonical `for` loops are
  candidates. A deliberate scope cut, not an oversight.
- **Arithmetic-operation counts are static, a lower bound.** A loop inside a called
  function is counted once per call site, not trip-count times; all operations are
  weighted equally (a divide costs the same as an add in this model).
- **No cache-reuse modelling.** Every iteration is priced as if it touches cold DRAM;
  a loop that re-reads the same cache line repeatedly is priced identically to one
  that doesn't.
- **Mapped array regions always start at index 0**, regardless of the loop's actual
  start value — a loop over `[1000, 2000)` maps 2000 elements, not 1000.
- **No real GPU in the verification ladder.** Every `.omp` binary this project builds
  and runs uses plain `-fopenmp`, no `-fopenmp-targets` — on this development
  machine, a `target` region runs on "the host device" per the OpenMP spec, which is
  conformant but never exercises real PCIe transfer or an actual device compute
  unit. `MachineModel`'s GPU-side constants are order-of-magnitude estimates, stated
  as such in `Profitability.h`'s own header comment, never claimed as measurements
  of a particular machine.

## How it's verified

`scripts/build_and_run.sh` runs a three-tier evidence ladder in place of a real
device run, for every benchmark, under every policy:

1. **Tier 1 — host build & run.** Compiles the rewritten source against real
   `libomp` and runs it; `tier1-exact` compares its output element-by-element
   against the sequential baseline (see Results, above).
2. **Tier 2 — device codegen.** Does Clang's NVPTX backend accept the rewritten
   source as a *device* compilation unit, with the offloading-entry count and
   `declare target` wrapping matching the tool's own report?
3. **Tier 3 — map-clause verification.** Decodes the `map()` clauses' actual Clang
   lowering (`@.offload_sizes`/`@.offload_maptypes`) and cross-checks direction and
   byte count against the reported pragma text.

No tier proves a device actually ran correctly — see `NOTES.md` for exactly what each
tier does and doesn't prove.

**Discrimination, in both directions.** `scripts/build_and_run.sh` exercises the four
demo benchmarks (`example`, `saxpy`, `small_update`, `compute_heavy`) end to end, but
the project's core claim — that the safety and profitability checks actually
discriminate, rather than always saying yes — is proven by a separate set of fixtures
built specifically to fail, run manually against the tool and checked against the
comment above each loop stating the expected verdict:

- `benchmarks/loop_shapes.c` / `loop_unrecognized.c` — the matched positive/negative
  pair for loop classification (10 recognized, 8 flagged, each with a distinct reason).
- `tests/call_chains.c` — 2-level and 3-level call chains, an opaque callee, mutual
  recursion.
- `tests/safety_cases.c` — twelve loops covering every `SAFE`/`UNSAFE`/`UNKNOWN`
  category, matched pairs in both directions.
- `tests/profitability_cases.c` — unconditional and conditional `GPU_OFFLOAD`,
  `CPU_PARALLEL`, `SEQUENTIAL`, and a known-`UNSAFE` loop confirming profitability
  never runs on a loop the safety pass didn't clear.

`scripts/demo.sh`'s Act 2 runs `tests/safety_cases.c` live, specifically because this
is the evidence most likely to go unseen otherwise.

## Repo layout

- `src/analysis/` — loop detection, call graph resolution, interprocedural safety
  analysis, GPU-profitability heuristic.
- `src/codegen/` — Rewriter-based pragma insertion (OpenMP target-offload and
  CPU-threaded fallback).
- `src/driver/` — the ClangTool entry point / CLI, wiring only.
- `benchmarks/` — input C programs, sequential baseline alongside the tool's rewritten
  output for easy diffing.
- `tests/` — matched safe/unsafe and profitable/not-profitable fixture pairs — see
  "How it's verified," above.
- `scripts/` — `build_and_run.sh` (the evidence ladder), `demo.sh` (the narrated
  walkthrough), `check_maps.py` / `compare_outputs.py` (Tier 3 and correctness
  verification, respectively).
- `NOTES.md` — the full, chronological design-decision log this README is
  consolidated from: every real decision, every bug an adversarial review caught, and
  the exact reasoning and numbers behind everything summarized above.
