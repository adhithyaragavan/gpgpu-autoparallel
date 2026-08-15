# P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

SegFault 2026 hackathon project. See `CLAUDE.md` for project context (read automatically by Claude
Code), `ROADMAP.md` for the build plan, and `NOTES.md` for the running design-decision log.

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

**Aug 16 (Day 2 of `DAY_BY_DAY.md`) — Day 3's trip-count goal is done, a day ahead.**

The tool classifies every `for` loop it finds as one of:

- `CONSTANT_BOUND` — bound folds to an integer constant; trip count extracted where the start
  value is also known.
- `VARIABLE_BOUND` — canonical loop shape, bound not statically known, recorded symbolically
  (e.g. "bound is parameter `n`").
- `UNRECOGNIZED` — not a simple canonical counted loop, with a specific reason. Flagged rather
  than silently mishandled.

It also records the direct callees of each loop body, which is the hook the interprocedural pass
builds on. Analysis lives in `src/analysis/`; `src/driver/main.cpp` is wiring only.

Verified with a matched benchmark pair — `benchmarks/loop_shapes.c` (10 loops, all recognized)
and `benchmarks/loop_unrecognized.c` (8 loops, all flagged, distinct reasons). Both directions
matter: a classifier that only ever says yes proves nothing.

**Known gap:** nothing yet checks that a loop body leaves the induction variable alone, so
`for (int i = 0; i < n; i++) { i = 0; }` currently classifies as a clean loop. That is body
analysis, deferred to the Week 2 safety pass and marked `TODO` in `src/analysis/LoopInfo.h`. It
must land before codegen emits any pragma.

Next: Day 5 — Clang's `CallGraph` to resolve loop-body calls across function boundaries.
