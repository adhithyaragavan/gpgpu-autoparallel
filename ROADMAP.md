# P05 Roadmap — Automatic Parallelizing Compiler for GPGPU (Clang LibTooling-based)

Assumptions: solo builder, systems/CS background but new to compiler internals, ~5 weeks (Aug 15 – Sept 19), building with an AI coding assistant as a pairing partner while genuinely learning the concepts. Adjust the daily load to whatever hours/day you actually have — the week boundaries matter more than the day boundaries.

**Tooling change from the original brief:** the brief suggests building on ROSE, but ROSE's value there is mainly its built-in parsing support for programs that *already* contain CUDA/OpenMP/UPC constructs. Since P05's actual input is plain sequential C/C++ (you're *generating* OpenMP pragmas, not parsing existing GPU code), that advantage doesn't apply here, and ROSE's build overhead is a disproportionate risk for a solo 5-week build. This plan uses **Clang LibTooling** instead: same source-level AST analysis and source-to-source rewriting capability, dramatically easier setup, much better docs. Note this substitution and the reasoning explicitly in your writeup — it's a defensible engineering call, not a corner cut.

## The core risk, stated up front

This brief has four hard subsystems: interprocedural whole-program analysis, GPU-profitability reasoning, OpenMP 4.5+ target-offload codegen, and CPU-threaded fallback — validated on real benchmarks with correct data movement. Doing all four well, solo, in five weeks, starting new to compilers, is genuinely ambitious. The plan is built around one rule: **narrow the benchmark set hard, and get a thin end-to-end pipeline working before you go deep on any one piece.** A complete pipeline on 2 benchmarks beats a half-built pipeline aimed at 10.

---

## Week 0 (now – Aug 15): Setup

**Goal:** Clang/LLVM dev environment ready, and you can run a trivial `ASTFrontendAction` that walks a C file and prints something.

- Day 1: Install Clang + LLVM dev headers/libs (prebuilt release binaries are fine — no from-source LLVM build needed). Set up a CMake project linking against `clangTooling`, `clangASTMatchers`, `clangRewrite`.
- Day 1-2: Get a minimal `ASTFrontendAction` + `RecursiveASTVisitor` running: parse a `.c` file, print every `ForStmt` and `CallExpr` found. This is your "hello world" — once it runs, setup risk is basically gone (this is the step ROSE made painful; LibTooling should take an afternoon, not days).
- Day 2-3: Pick your benchmark set now, not later. 2-3 small C programs with a hot loop that calls a helper function (this is the case naive parallelizers miss, and it's your headline demo). Good sources: PolyBench/C, Rodinia (pick CPU-only kernels you can strip down), or hand-write 1-2 synthetic examples if real benchmarks are too tangled to instrument quickly.
- Day 3-4: Confirm you have a real OpenMP GPU offload toolchain to compile/run against later (Clang with `-fopenmp -fopenmp-targets=...`, or an emulation/host-fallback target if you don't have GPU hardware). Sort this out now, not in Week 3 — it's the other environment-setup risk in this project.

**Learn this week:** what Clang's AST looks like (`ASTContext`, `Decl`/`Stmt` hierarchy) and how `RecursiveASTVisitor`/`ASTMatchers` traverse it; what a call graph is; skim Clang's LibTooling tutorial (`clang.llvm.org/docs/LibASTMatchersTutorial.html` and the LibTooling docs) — you'll use `ForStmt`, `CallExpr`, `FunctionDecl`, and the `Rewriter` API constantly.

---

## Week 1 (Aug 15-22): Interprocedural call graph + basic loop detection

**Goal:** Given a C program, produce a list of candidate loops, including ones whose body calls a helper function, with the callee resolved.

- Build the loop finder: visit `ForStmt` nodes, record trip-count expressions if statically visible (constant bounds, simple induction variable).
- Build a simple call graph: for each loop body, find `CallExpr`s inside it, resolve to the `FunctionDecl` definition (intraprocedural first — module-local functions only, skip external library calls for now). Clang actually ships a `CallGraph` utility class you can build on directly instead of writing this from scratch.
- Get side-effect / alias info for the callee at a coarse level to start: does it write through any pointer parameter, does it touch globals, does it call anything else. Full points-to analysis is a research-grade problem — a conservative, coarse "does this function write through any pointer arg or global" check is a legitimate and defensible simplification for a hackathon; state that simplification explicitly in your writeup.

**Learn this week:** call graphs and how they're built from an AST; the concept of "may-alias" vs "must-alias" and why aliasing is *the* reason interprocedural analysis is hard; what makes a loop "parallel-safe" (no loop-carried dependence) at a basic level — read up on loop-carried dependence before you write the safety check.

**AI-pairing usage note:** lean on it hardest this week for Clang AST API discovery (matcher syntax, visitor method names, `CallGraph` usage) — ask it to find the right node types and traversal patterns, but write the actual safety-check *logic* yourself or in tight collaboration, since that logic is the intellectual core of the project and you need to be able to explain it cold in the finale Q&A.

---

## Week 2 (Aug 22-29): Full interprocedural safety analysis + GPU-profitability heuristic

**Goal:** Correctly flag which loops (including cross-function ones) are safe to parallelize at all, and score which of those are GPU-profitable vs. merely safe.

- Extend the call graph work to be properly interprocedural: if a loop calls `f`, and `f` calls `g`, safety analysis needs to walk that chain (with a recursion/cycle guard).
- Build the GPU-profitability heuristic: no ML needed here — a transparent, feature-based scoring function using trip count, estimated data volume moved (in vs. out), memory access pattern (strided/coalesced-friendly vs. scattered, inferable from array-subscript expressions in the AST), and computational intensity (ops per byte moved). This is your explainability story: log *why* each decision was made in plain terms, since the brief explicitly wants a suitability decision that beats "offload everything safe."
- Write this heuristic down as an explicit, documented rule set before coding it — it's the part judges will ask you to defend.

**Learn this week:** roofline-model-style reasoning (compute intensity vs. memory bandwidth) — you don't need the full model, just the intuition; why kernel launch overhead and host-device transfer cost matter enough to make "safe" parallelism unprofitable.

---

## Week 3 (Aug 29 – Sept 5): Codegen — OpenMP target offload + CPU fallback

**Goal:** For loops flagged GPU-profitable, insert `#pragma omp target teams distribute parallel for` with correct `map()` clauses directly into the source above the loop; for safe-but-not-profitable loops, insert the CPU-threaded `#pragma omp parallel for` instead.

- Use Clang's `Rewriter` API to insert the pragma text into the original source buffer right before the loop, then emit the rewritten source. This is simpler and more robust than generating IR or object code directly — you get human-readable, diffable output for your demo too.
- Start with straight-line loops with simple array accesses — get correct data-mapping clauses (`map(to:)`, `map(from:)`, `map(tofrom:)`) right for the simple case before handling anything irregular.
- Recompile the rewritten source with your OpenMP offload toolchain and confirm it actually runs on the target (GPU or emulated) — this is where "correct host-device data movement" (an explicit success criterion in the brief) gets validated, not just "it compiles."

**Learn this week:** OpenMP target/teams/distribute/parallel-for semantics and the map clause data-sharing model — read the OpenMP 4.5+ spec sections on target offload directly, it's shorter and clearer than most tutorials.

---

## Week 4 (Sept 5-12): End-to-end validation + the headline demo case

**Goal:** Full pipeline works on your 2-3 chosen benchmarks, with numerically validated output and measured speedup, including at least one case with a function call inside a hot loop that a naive (intraprocedural-only) parallelizer would have skipped.

- Run baseline (sequential), naive "offload everything safe" policy, and your profitability-gated policy, and measure all three. This three-way comparison is your strongest evidence for the "measurably outperforms naive" claim in the brief.
- Numerically validate outputs (compare against sequential baseline results) — a "correct" mark matters more to judges than a fast-but-wrong result.
- Buffer time here for the inevitable: AST edge cases (macros, complex loop bounds), OpenMP toolchain quirks, off-by-one data-mapping bugs.

---

## Sept 12-19: Polish, writeup, demo prep

- Cut anything half-working rather than demo it live — a clean 2-benchmark story beats a shaky 5-benchmark one.
- Prepare the "why we simplified X" explanations (ROSE→LibTooling substitution, coarse aliasing, whichever corners you cut) — reviewers respect explicit, reasoned scope cuts far more than an over-claimed feature that breaks under questioning.
- Record a demo video/GIF as backup in case live demo hits environment issues at the venue.

---

## How to use an AI coding assistant well for this (not just "vibe" it)

1. **Treat it as a fast API/docs lookup + boilerplate writer, not a design authority.** Clang's AST API is large — this is exactly where an AI pairing tool earns its keep (finding the right matcher, the right visitor method, the right `Rewriter` call). But the *decisions* — what counts as parallel-safe, how the profitability heuristic is weighted, what to simplify — should be ones you make and can explain, because that's what gets probed in judging and what you'll actually claim on your resume.

2. **Read every diff before accepting it, especially in the safety-analysis and codegen code.** A subtly wrong "safe to parallelize" check or a wrong `map()` clause doesn't crash — it silently produces wrong numbers. Always validate output correctness, not just "it ran."

3. **Use it to explain unfamiliar code back to you, not just write code.** After it generates an AST-visitor or `Rewriter`-based codegen block, ask it to explain *why* it structured it that way and what would break if you changed X. That turns "vibe coding" into actual learning, and it's cheap to do — a few extra prompts per session.

4. **Keep a running "decisions and simplifications" log as you go**, ideally in a `NOTES.md` in the repo — including the ROSE→LibTooling call. Ask your AI pairing tool to help you draft entries whenever you make a real design call. This becomes your writeup almost for free, and it's the artifact that makes the project defensible rather than just "AI wrote a compiler."

5. **Use separate, scoped sessions/prompts per subsystem** (call-graph analysis, profitability heuristic, codegen) rather than one giant "build the whole compiler" prompt. Compiler correctness is local-reasoning-heavy; smaller, well-specified asks produce more reliable code and are much easier for you to review.

6. **When you hit a wall, paste the actual error/AST dump, not a paraphrase.** Precise input gets precise help; vague "it's not working" prompts are where AI-assisted debugging goes sideways fastest.

## How to learn alongside this (concrete, minimal-overhead)

- **Just-in-time, not upfront.** Don't try to read a compilers textbook cover to cover before Week 0 ends — read the specific concept right before the week that needs it (the "Learn this week" notes above are ordered for this).
- **Primary sources over tutorials for the two specs that matter most:** the OpenMP 4.5+ specification (target/teams/distribute sections) and Clang's own LibTooling/AST Matchers docs and tutorial. Secondary blog posts about "how OpenMP offload works" are often stale or oversimplified; the spec itself is more reliable and not actually that long for the sections you need.
- **For the CS fundamentals** (call graphs, alias analysis, loop-carried dependence, roofline reasoning), the relevant chapters of the "Dragon Book" (Aho et al., *Compilers: Principles, Techniques, and Tools*) or Muchnick's *Advanced Compiler Design and Implementation* cover this well — you don't need to read them front to back, just the specific chapters on interprocedural analysis and dependence analysis when you get there.
- **Explain it back.** After implementing each piece, write 2-3 sentences in your own words (in that `NOTES.md`) on what the concept is and why the code does what it does. If you can't, that's the signal to slow down on that piece before moving on — and it directly produces your finale talking points.
