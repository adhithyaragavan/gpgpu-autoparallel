# P05 — Automatic Parallelizing Compiler for GPGPU with Interprocedural Analysis

SegFault 2026 hackathon project. See `ROADMAP.md` for the build plan and `NOTES.md` for the
running design-decision log.

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

**Aug 18 (Days 5-6 of `DAY_BY_DAY.md`, done together) — interprocedural call resolution.**

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

**Known gap:** nothing yet checks that a loop body leaves the induction variable alone, so
`for (int i = 0; i < n; i++) { i = 0; }` currently classifies as a clean loop. That is body
analysis, deferred to the Week 2 safety pass and marked `TODO` in `src/analysis/LoopInfo.h`. It
must land before codegen emits any pragma.

Next: Day 7 buffer, then Days 8-10 — coarse side-effect check (does a reachable function write
through a pointer arg or touch a global) on top of the reachability layer above.
