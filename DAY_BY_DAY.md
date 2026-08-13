# P05 Day-by-Day Checklist (Clang LibTooling)

Working backward from the calendar: hacking starts Aug 15, final evaluation is Sept 19-20, and midsems eat roughly Sept 8-14. That leaves two real work blocks — **Block A: Aug 15 – Sept 7 (about 3.5 weeks, your main build window)** and **Block B: Sept 15-18 (a short polish/recovery window right before evaluation)**. Treat Block A as if it's your entire runway; Block B is a buffer for whatever slipped, not a place to plan new work.

Each day below has a single primary goal. If a day's goal isn't done by end of day, the fix is to cut scope on that goal, not to let it bleed into tomorrow's — tomorrow's goal depends on today's being real, even if smaller than planned.

---

## Block A — Aug 15 to Sept 7

### Days 1-2 (Aug 15-16): Environment + trivial pass
- Day 1: Install Clang/LLVM dev libs, get a CMake project linking `clangTooling`/`clangASTMatchers`/`clangRewrite` building.
- Day 2: `ASTFrontendAction` + `RecursiveASTVisitor` that parses a `.c` file and prints every `ForStmt` and `CallExpr`. If this isn't done by end of Day 2, ask for help rather than losing Day 3 to it.

### Days 3-4 (Aug 17-18): Benchmarks + loop detection
- Day 3: Finalize 2-3 benchmark programs (write at least one yourself — a loop calling a helper function doing array math). Get trip-count extraction working for simple constant/variable bounds.
- Day 4: Loop finder catches all target loops across your benchmarks. Sanity-print them.

### Days 5-7 (Aug 19-21): Call graph + one-level interprocedural resolution
- Day 5: Use Clang's `CallGraph` utility to resolve calls inside loop bodies to their `FunctionDecl`.
- Day 6: Extend resolution to walk multi-level call chains (with a recursion guard) — this is what makes it genuinely interprocedural, not just one hop.
- Day 7: Buffer/catch-up day. If you're on schedule, start the coarse side-effect check (does callee write through a pointer arg or global).

### Days 8-10 (Aug 22-24): Safety analysis
- Day 8: Coarse alias/side-effect check working across the call chain.
- Day 9: Safety check correctly flags at least one "safe, crosses function boundary" case and one "unsafe" case (e.g., callee writes a global) — you need both to prove the check discriminates, not just always says yes.
- Day 10: Buffer/catch-up.

### Days 11-13 (Aug 25-27): GPU-profitability heuristic
- Day 11: Define the feature set on paper first (trip count, data volume moved, access pattern, compute intensity) and the scoring rule — write this down before coding it.
- Day 12: Implement the heuristic, producing a profitable/not-profitable decision with a logged plain-English reason per loop.
- Day 13: Test it against your benchmarks and at least one deliberately low-trip-count or scattered-access case to confirm it correctly says "not profitable" sometimes — same discrimination principle as the safety check.

### Days 14-16 (Aug 28-30): Codegen — profitable path
- Day 14: `Rewriter`-based pragma insertion for the GPU-offload path (`#pragma omp target teams distribute parallel for` + `map()` clauses).
- Day 15: Compile rewritten source with your OpenMP offload toolchain, get it to actually build.
- Day 16: Get it running and producing output (correctness not yet verified — just "it runs").

### Days 17-18 (Aug 31 – Sept 1): Codegen — CPU fallback path
- Day 17: `Rewriter`-based pragma insertion for the CPU-threaded path (`#pragma omp parallel for`) for safe-but-not-profitable loops.
- Day 18: Both paths compiling and running.

### Days 19-21 (Sept 2-4): Validation
- Day 19: Numerically compare parallelized output vs. sequential baseline on each benchmark — fix correctness bugs here, this is non-negotiable.
- Day 20: Measure timing: sequential vs. your gated-policy output. Record real numbers.
- Day 21: If time allows, add the naive "offload everything safe" comparison for a three-way story. If not, skip it — two-way (baseline vs. gated) is still a complete, valid result.

### Days 22-24 (Sept 5-7): Buffer + writeup
- Day 22: Catch-up day for whatever slipped in validation.
- Day 23: Write `NOTES.md` — ROSE→LibTooling call, one-level-vs-deeper interprocedural scope, coarse aliasing, benchmark choices, all framed as reasoned tradeoffs.
- Day 24: Record a demo video/GIF as backup, and do a first dry-run explaining the pipeline out loud end to end. Then it's midsem time — everything from here should be "done, minus polish."

---

## Block B — Sept 15-18 (post-midsem, pre-evaluation)

- Day 1: Re-familiarize yourself with your own code (a week+ away is a lot after exams) — re-read `NOTES.md` and walk the pipeline yourself before touching anything.
- Day 2: Fix anything that broke or was left rough; do not add new scope here unless Block A finished with real time to spare.
- Day 3: Finalize demo (live rehearsal + recorded backup), polish the writeup.
- Day 4: Final rehearsal of the finale walkthrough — call graph → safety check → profitability decision → codegen → measured result — cold, no notes.

---

## The one rule that matters most

If any day in Block A runs long, the fix is always **cut that day's scope down to something real and move on** — never let a slip cascade into eating tomorrow's day too. A safety check that only handles the exact shape of your one benchmark, shipped and working, beats a general one that's still half-built when midsems hit.
