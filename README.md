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

Day 0 — environment scaffolding only. `main.cpp` is the trivial AST-walking checkpoint from the
roadmap: it should print every `ForStmt` and `CallExpr` in `benchmarks/example.c`. Once this builds and
runs cleanly, move on to Day 1 of `ROADMAP.md`.
