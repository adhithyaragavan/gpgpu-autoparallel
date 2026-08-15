# Project: P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

SegFault 2026 hackathon project. Solo build, tight timeline (see ROADMAP.md).

## What this is

A Clang LibTooling-based compiler tool that takes unmodified sequential C programs and:
1. Finds candidate loops via AST analysis, including loops whose bodies call helper functions.
2. Performs interprocedural safety analysis — resolves calls across function boundaries and checks
   (coarsely) whether the called function has side effects that make the loop unsafe to parallelize.
3. Scores GPU-offload profitability for safe loops (trip count, data volume, access pattern) —
   "safe" does not automatically mean "should offload."
4. Rewrites the source (via Clang's Rewriter API) to insert OpenMP 4.5+ target-offload pragmas for
   GPU-profitable loops, or CPU-threaded `#pragma omp parallel for` as a fallback otherwise.

## Why Clang LibTooling instead of ROSE (the brief's suggested base)

ROSE's main advantage for this brief is built-in parsing support for programs that already contain
CUDA/OpenMP/UPC constructs. Our input is plain sequential C — we generate OpenMP directives, we don't
parse existing GPU code — so that advantage doesn't apply. ROSE's build overhead was judged a
disproportionate risk for a solo, time-boxed build. Clang LibTooling gives equivalent source-level AST
analysis and source-to-source rewriting with much lower setup risk and far better documentation.

## Current phase

<!-- UPDATE THIS LINE EVERY DAY -->
Aug 16 (Day 2 of DAY_BY_DAY.md) — Day 3's trip-count goal is done a day early. Loop detection
classifies every `ForStmt` as CONSTANT_BOUND / VARIABLE_BOUND / UNRECOGNIZED, extracts trip counts
where statically determinable, and records per-loop body callees. Analysis lives in `src/analysis/`;
`main.cpp` is wiring only. Remaining for Day 3-4: finalize the 2-3 benchmark set (only `example.c`
plus the two loop-shape test files exist so far). Next: Day 5, Clang `CallGraph` to resolve
loop-body calls across function boundaries.

## Key decisions made so far

<!-- Add one line per real decision as you make it. Mirror these into NOTES.md with more detail. -->
- Using Clang LibTooling, not ROSE (see above).
- Constant bounds are detected by constant *folding* (`Expr::EvaluateAsInt`), not by matching integer
  literals — so `const int N = 512` counts as a constant bound.
- Loop classification and trip count are independent: a CONSTANT_BOUND loop can still have an unknown
  trip count (variable start value). Consumers check `LoopInfo::TripCount`, never infer it from the kind.
- A function call in the bound expression makes a loop UNRECOGNIZED, not variable-bound.
- Unresolvable indirect calls in a loop body are tracked explicitly (`HasIndirectCall`), since they must
  block parallelization later.

## Repo layout

- `src/analysis/` — loop detection, call graph resolution, interprocedural safety analysis, GPU-profitability heuristic.
- `src/codegen/` — Rewriter-based pragma insertion (OpenMP target-offload and CPU-threaded fallback).
- `src/driver/` — the ClangTool entry point / CLI that wires analysis + codegen together.
- `benchmarks/` — input C programs used to test and demo the pipeline. Keep the sequential baseline
  version of each benchmark alongside the tool's output for easy diffing.
- `tests/` — small test-case C snippets, especially matched safe/unsafe and profitable/not-profitable
  pairs (see ROADMAP.md — you need both directions to prove the checks discriminate, not just say yes).
- `NOTES.md` — running log of design decisions and scope simplifications. This becomes the writeup.
- `ROADMAP.md` — the week-by-week / day-by-day build plan.

## Working conventions

- One focused task per session. State the day's goal + relevant existing context as the first message.
- For non-trivial logic (safety analysis, profitability heuristic, codegen correctness), propose an
  approach before writing code, and read every line before accepting.
- For boilerplate (CMake, AST visitor scaffolding, Rewriter calls), move fast.
- Every day: run against benchmarks, commit, update the "Current phase" line above, add a NOTES.md
  entry for any real decision made.
- When debugging AST/matching issues, use `clang -Xclang -ast-dump -fsyntax-only` on the relevant
  benchmark file and paste the actual output — not a description of the symptom.
