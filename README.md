# P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

A Clang LibTooling source-to-source tool. Point it at unmodified, ordinary
sequential C and it finds loops that are safe to parallelize, including ones whose
bodies call helper functions, decides which of those are worth offloading to a GPU
versus threading on the CPU versus leaving alone, and rewrites the source with the
corresponding OpenMP pragmas.

It started as a SegFault 2026 hackathon project (solo build). Since October 2026 it
has been developed as a research project, and the analysis has been rebuilt:

- **Dependence tests** (ZIV, GCD, strong SIV, Banerjee) on affine subscripts over
  whole loop nests, replacing the hackathon rule "every subscript must be exactly
  `i`".
- **Call-site-aware interprocedural summaries.** A called function's array accesses
  are rewritten in terms of each call's arguments and tested like the loop's own.
  For example, `scale_row(&a[i*n], n)` is recognised as touching only row `i`.
- **Scalar privatization and reductions**, emitted as `private`, `lastprivate` and
  `reduction` clauses.
- **Aliasing made explicit.** Two pointers the loop uses are either proven not to
  overlap (distinct objects, incompatible element types, `restrict`, or every caller
  passing separate arrays), or the pragma gets a run-time overlap check in its
  `if()`. Failing both, the loop stays sequential. GPU offload requires a proof.

Each of these has a flag that restores the previous, simpler rule (`-ipa=coarse`,
`-alias=assume`), so its effect can be measured. A five-minute walkthrough is
`demo/run.sh`.

The brief's suggested base was ROSE. ROSE's main advantage is built-in parsing
support for programs that already contain CUDA/OpenMP/UPC constructs, but this
project's input is plain sequential C: it generates OpenMP directives rather than
parsing existing GPU code, so that advantage doesn't apply. ROSE's build overhead was
also a disproportionate risk for a solo, time-boxed build. Clang LibTooling gives the
same source-level AST analysis and source-to-source rewriting for much less setup
cost.

## The pipeline

Five passes, in this order, for every candidate loop:

1. **Loop detection** (`src/analysis/LoopAnalysis.{h,cpp}`). Finds every `for` loop,
   classifies its bound (`CONSTANT_BOUND` / `VARIABLE_BOUND` / `UNRECOGNIZED`, the
   last with a specific reason), and extracts a trip count where one is statically
   visible. Constant bounds are found by constant folding (`Expr::EvaluateAsInt`),
   not by matching integer literals, so `const int N = 512` counts.
2. **Interprocedural call resolution** (`src/analysis/CallResolver.{h,cpp}`). Wraps
   Clang's `CallGraph` utility, keyed by `FunctionDecl*`, and walks each loop's
   callees transitively (breadth-first, with a cycle guard) to find every function
   reachable from the loop body, not just the direct callees.
3. **Safety analysis** (`src/analysis/SafetyAnalysis.{h,cpp}`, using
   `AffineAccess`, `DependenceTest`, `Summaries`, `ScalarAnalysis` and
   `AliasAnalysis`). A tri-state verdict — `SAFE` / `UNKNOWN` / `UNSAFE`, not a
   bool, since "cannot see this far" and "found a real hazard" are different
   findings. Every array access in the loop, and in the functions it calls
   (substituted per call site), becomes an affine subscript, and every pair that
   could conflict goes through the dependence tests. Scalars the loop writes are
   classified as private, lastprivate, reduction or shared. Pairs of pointers are
   proven disjoint or given a run-time check. The report names the access pair and
   test behind every verdict. A recursive or opaque callee, or a subscript that
   isn't affine, makes the loop `UNKNOWN`, never `SAFE`.
4. **Profitability** (`src/analysis/Profitability.{h,cpp}`). A roofline-style cost
   model (`t_seq`/`t_cpu`/`t_gpu`, argmin), driven by a `MachineModel` struct whose
   parameters are all overridable from the command line. Turns a `SAFE` loop's trip
   count, array-access pattern, and interprocedural op count into `GPU_OFFLOAD` /
   `CPU_PARALLEL` / `SEQUENTIAL` — three-way rather than binary, since "safe but only
   8 iterations" is a different case from "safe and worth threading." A
   parameter-bound loop gets a solved runtime guard (`GPU_OFFLOAD if (n >= T)`) with
   `T` derived algebraically from the machine parameters, rather than a guess. A
   second policy, `--policy=naive`, offloads every `SAFE` loop unconditionally
   instead, used for comparison — see Results below.
5. **Codegen** (`src/codegen/OmpRewriter.{h,cpp}`). Clang's `Rewriter` API inserts
   `#pragma omp target teams distribute parallel for` with `map()` clauses for
   `GPU_OFFLOAD` loops (wrapping their transitive callees in `declare target`), or
   `#pragma omp parallel for` for `CPU_PARALLEL` loops, into a sibling `<name>.omp.c`.
   Never in place. `map()` direction and extent come straight from the profitability
   pass's own array-region analysis rather than being recomputed. Scalar clauses
   (`private`, `reduction`, ...) come from the safety pass, and a run-time alias
   check is joined into the pragma's `if(parallel: ...)`.

`src/driver/main.cpp` is wiring only; it holds no analysis logic.

## Quick start

```bash
mkdir build && cd build
cmake -G Ninja .. -DCMAKE_PREFIX_PATH=<path to your LLVM/Clang install's cmake config dir>
ninja
./p05tool ../benchmarks/example.c -- -isysroot "$(xcrun --show-sdk-path)"
```

If `find_package(Clang REQUIRED CONFIG)` can't find Clang, point `CMAKE_PREFIX_PATH`
at the `lib/cmake` directory of your LLVM/Clang installation (wherever
`ClangConfig.cmake` lives). `-isysroot` (macOS only, drop it elsewhere) is needed
because the benchmarks `#include <stdio.h>`/`<time.h>` to print a checksum and
measure timing.

For a short walkthrough of the current analysis (safety through function calls,
pointer overlap, the generated pragmas and a GPU verdict):

```bash
./demo/run.sh
```

The original hackathon walkthrough, all five stages narrated:

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

**GPU offload on real hardware.** A spike on a Colab T4 (`eval/colab/`) confirmed
that the generated OpenMP `target` code runs on the device with NVIDIA HPC SDK
`nvc`, with output matching the host to 6.6e-16 relative error. That run was
dominated by CUDA context start-up. A performance evaluation on real applications
is the next stage of the project and has not been done yet.

The results below are from the hackathon phase, measured on the host only.

**Correctness.** Every rewritten benchmark's output is compared element by element
against the sequential baseline, bit for bit — not just by a summed checksum, which
can't catch a swapped pair or compensating errors. Every benchmark that produces a
pragma passes exactly, under both policies:

| benchmark | elements | gated | naive |
|---|---|---|---|
| example | 1000 | exact match | exact match |
| saxpy | 65536 | exact match | exact match |
| small_update | 8 | n/a — gated declines, no `.omp` binary to compare | exact match |
| compute_heavy | 65536 | exact match | exact match |

**Timing.** `clock_gettime` brackets each benchmark's headline call. Numbers below
are the min of 7 repeats, median of 3 confirmation runs, on an unloaded development
machine with no real GPU (every `.omp` binary here runs its target regions on the
host — see "What it doesn't do" below). The `saxpy`/`compute_heavy` rows are
noise-dominated: both are within ~10% between naive and gated, close enough that
re-running the ladder can flip which one comes out faster. `example`'s gap is not
noise — it's reproducibly ~6.5-8x across repeated runs.

| benchmark | sequential | naive (offload everything safe) | gated (this tool's policy) | result |
|---|---|---|---|---|
| example (n=1000) | ~0us | ~30us | ~228us | naive ~7x faster (reproducible) — see below |
| saxpy | ~11us | ~29us | ~32us | tie, noise-level |
| small_update | ~0us | ~346us | n/a (declined) | gated pays nothing |
| compute_heavy | ~211us | ~83us | ~80us | tie, noise-level |

Two of these are worth explaining:

- `small_update` is the clean case the gated policy exists for. Eight iterations:
  naive pays real cost (~346us) to offload it for no benefit, while the cost model
  correctly recognizes there's nothing to gain and declines, costing nothing.
- `example` is the most interesting result, and it runs backwards from what "gating
  helps" would predict. Naive measures ~6.5-8x faster here, reproducibly. This isn't
  because the gated policy's guard made a bad call — it correctly keeps `n=1000`
  serial, below its own `n >= 4096` threshold. It's because `example.omp.c` has
  exactly one OpenMP construct in the whole file: libomp's one-time runtime
  cold-start cost is paid the moment that construct is touched, regardless of
  whether its guard evaluates true. With no second OpenMP construct in the file to
  absorb that cost, gating a file's only construct is the worst case for it, not the
  best. The cost model has no term for this at all. See `NOTES.md`'s Day 20 and Day
  21 entries for the full derivation.
- `saxpy` and `compute_heavy` tie between naive and gated, but not because the two
  policies agree. With no real GPU hardware, a `target teams distribute parallel
  for` region and a `parallel for` region dispatch through the same libomp
  fork-join path on this host — a real limit on what this evidence can show, not a
  wash for the policy design.

Every measured number, and the harness bug an adversarial review found and fixed
while building this comparison, is in `NOTES.md`, Days 19-21.

## What it doesn't do

- **No points-to analysis.** Pointer overlap is decided per pair of names, from
  declarations, types, `restrict` and the visible call sites (see `AliasAnalysis.h`).
  A pair it can't prove separate gets a run-time check, and that loop is never
  offloaded to the GPU. A loop whose accesses can't be bounded (for example
  `b[i % 3]` read while writing through a pointer that might be `b`) is `UNKNOWN`.
  The caller-based proof applies only to `static` functions, unless
  `-whole-program` declares that no other file calls this one's functions.
- **Irregular code is mostly out of reach.** Indirect subscripts (`a[idx[i]]`),
  pointer chasing and data-dependent control flow make a loop `UNKNOWN`.
- **Loop-carried dependences are decided by classic dependence tests**
  (ZIV, GCD, strong SIV, Banerjee with symbolic bounds) over affine subscripts,
  including 2-D/3-D nests and linearized `a[i*n + j]` indexing. Subscripts they
  can't express (indirect `a[idx[i]]`, triangular rows) make a loop `UNKNOWN`, not
  `SAFE`. Scalars a loop writes are classified by liveness over Clang's CFG:
  `private`, `lastprivate`, or `reduction` (`+ * & | ^ min max`), so PolyBench-style
  code with counters and temporaries declared at the top of a function parallelizes.
  Floating-point reductions need `-fp-reductions` (off by default: they change the
  last bits of the result).
- **GPU offload is only chosen when every access is exactly `base[i]`**, the one
  shape today's `map(...[0:bound])` clauses size correctly. Other `SAFE` loops get a
  CPU `parallel for` until map regions are computed from the subscript ranges.
- **`WhileStmt`/`DoStmt` are not classified at all** — only canonical `for` loops are
  candidates.
- **Arithmetic-operation counts are static, a lower bound.** A loop inside a called
  function is counted once per call site, not trip-count times, and every operation
  is weighted equally (a divide costs the same as an add).
- **No cache-reuse modelling.** Every iteration is priced as if it touches cold DRAM.
- **Mapped array regions always start at index 0**, regardless of the loop's actual
  start value — a loop over `[1000, 2000)` maps 2000 elements, not 1000.
- **No real GPU in the verification ladder.** Every `.omp` binary here builds with
  plain `-fopenmp`, no `-fopenmp-targets`, so a `target` region runs on "the host
  device" per the OpenMP spec — conformant, but it never exercises real PCIe
  transfer or an actual device compute unit. `MachineModel`'s GPU-side constants are
  order-of-magnitude estimates, as stated in `Profitability.h`'s own header comment,
  not measurements of a particular machine.

## How it's verified

`scripts/build_and_run.sh` runs a three-tier evidence ladder in place of a real
device run, for every benchmark, under every policy:

1. **Tier 1 — host build & run.** Compiles the rewritten source against real
   `libomp` and runs it; `tier1-exact` compares its output element by element
   against the sequential baseline (see Results, above).
2. **Tier 2 — device codegen.** Does Clang's NVPTX backend accept the rewritten
   source as a device compilation unit, with the offloading-entry count and
   `declare target` wrapping matching the tool's own report?
3. **Tier 3 — map-clause verification.** Decodes the `map()` clauses' actual Clang
   lowering (`@.offload_sizes`/`@.offload_maptypes`) and cross-checks direction and
   byte count against the reported pragma text.

No tier proves a device actually ran correctly — see `NOTES.md` for what each tier
does and doesn't prove.

**Safety verdicts against ground truth.** `eval/fuzz/fuzz.py` generates random C
loop nests, runs the tool on them, and checks every `SAFE` verdict two ways. First,
an instrumented build logs the memory address and iteration of every access, and a
`SAFE` loop fails if two iterations touch the same address and one of them writes.
Second, the parallel rewrite must produce the same output as the sequential build,
including when the pointer arguments overlap. It runs in CI.

**Parallel vs sequential.** `scripts/check_parallel.sh` rewrites a fixture with every
`SAFE` loop forced parallel, runs it against a driver in `eval/runtime/`, and requires
bit-identical output to the sequential build over repeated runs, at `-O2` and `-O0`.
The alias driver also calls every guarded loop with overlapping arguments.

**Both directions.** `scripts/build_and_run.sh` exercises the four demo benchmarks
(`example`, `saxpy`, `small_update`, `compute_heavy`) end to end, but the core claim —
that the safety and profitability checks actually discriminate, rather than always
saying yes — rests on a separate set of fixtures built to fail. Each loop's expected
verdict is a FileCheck line directly above it, run automatically by lit (locally and in
CI):

```bash
python3 -m venv .venv && .venv/bin/pip install lit   # once
.venv/bin/lit -v tests                                # after building
```

The CHECK lines are generated by `scripts/gen_filecheck.py`, which snapshots the
tool's current output. It prints every loop's prose comment beside the values it
pins, so each snapshot is checked against its spec before it is committed.

The fixtures:

- `benchmarks/loop_shapes.c` / `loop_unrecognized.c` — matched positive/negative pair
  for loop classification (10 recognized, 9 flagged, each with a distinct reason).
- `tests/call_chains.c` — 2-level and 3-level call chains, an opaque callee, mutual
  recursion.
- `tests/safety_cases.c` — twelve loops covering every `SAFE`/`UNSAFE`/`UNKNOWN`
  category, matched pairs in both directions.
- `tests/scalars.c` / `tests/fp_reductions.c` — scalar privatization and
  reductions in both directions (also run, at `-O2` and `-O0`, by
  `scripts/check_parallel.sh tests/scalars.c eval/runtime/scalars_main.c`).
- `tests/dependence.c` — the dependence tests in both directions (independent by
  ZIV/GCD/SIV/Banerjee, proven dependent, undecidable). It is also *run*:
  `scripts/check_parallel.sh` rewrites it with every `SAFE` loop forced parallel
  (`-policy all-cpu`), builds it and the original against
  `eval/runtime/dependence_main.c`, and requires bit-identical output over 20 runs.
  It is in CI, and was checked to fail on an injected wrong pragma.
- `tests/affine.c` — affine subscript extraction: shifted, scaled and symbolic
  subscripts, true 2D arrays vs `double **`, delinearized `a[i*n + j]` (and the cases
  that must *not* delinearize), pointer arithmetic, forward-substituted locals, and
  every non-affine shape that must never be reported as affine.
- `tests/nests.c` — the loop-nest model: perfect/imperfect, rectangular/triangular,
  sibling loops, loops inside unrecognized loops and inside `while`/`do`, and loops
  that are *not* children (in another loop's init clause, or in a called function).
- `tests/profitability_cases.c` — unconditional and conditional `GPU_OFFLOAD`,
  `CPU_PARALLEL`, `SEQUENTIAL`, and a known-`UNSAFE` loop confirming profitability
  never runs on a loop the safety pass didn't clear.
- `tests/summaries.c` — interprocedural summaries: the same callee or call shape on
  the safe and the unsafe side of a call boundary, row-start arguments, recursion and
  opaque callees.
- `tests/alias.c` — every way a pointer pair is proven disjoint, every run-time
  check, and the cases that must stay `UNKNOWN`.
- `tests/interlock.c` — loops kept off the GPU until map extents are computed from
  subscript ranges.

`scripts/demo.sh`'s Act 2 runs `tests/safety_cases.c` live, since it's the evidence
most likely to go unseen otherwise.

## Repo layout

- `src/analysis/` — loop detection, call graph resolution, interprocedural safety
  analysis, GPU-profitability heuristic.
- `src/codegen/` — Rewriter-based pragma insertion (OpenMP target-offload and
  CPU-threaded fallback).
- `src/driver/` — the ClangTool entry point / CLI, wiring only.
- `benchmarks/` — input C programs, sequential baseline alongside the tool's
  rewritten output for easy diffing.
- `tests/` — matched safe/unsafe and profitable/not-profitable fixture pairs, see
  "How it's verified" above.
- `scripts/` — `build_and_run.sh` (the evidence ladder), `demo.sh` (the narrated
  walkthrough), `check_parallel.sh` (parallel vs sequential), `gen_filecheck.py`
  (CHECK-line snapshots), `check_maps.py` / `compare_outputs.py` (Tier 3 and
  correctness verification).
- `eval/` — `fuzz/` (the safety fuzzer), `runtime/` (drivers for
  `check_parallel.sh`), `colab/` (the GPU spike notebook and its results).
- `demo/` — a short walkthrough of the current analysis.
- `docs/related_work.md` — comparison with prior work.
- `NOTES.md` — the full, chronological design-decision log this README is
  consolidated from: every real decision, every bug an adversarial review caught,
  and the exact reasoning and numbers behind everything summarized above.
