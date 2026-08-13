# Using Claude Code to Build P05, Day by Day

This is the practical layer on top of the day-by-day checklist: how to actually structure your Claude Code sessions so it accelerates you without producing code you can't defend at the finale.

## 1. Set up the project so Claude Code has context automatically

Before Day 1 coding starts, create a `CLAUDE.md` in your repo root (Claude Code reads this automatically at the start of every session). Put in it:
- One paragraph on what the project is and does (interprocedural loop-safety + GPU-profitability analysis + OpenMP codegen for sequential C/C++, via Clang LibTooling).
- The current phase you're in (update this daily — "currently on: call graph resolution, Day 5-7 of the roadmap").
- Key architectural decisions already made (e.g., "safety check is coarse: flags any pointer-arg or global write as unsafe, no points-to analysis").
- Where things live (`src/analysis/`, `src/codegen/`, `benchmarks/`, `NOTES.md`).

This means you don't have to re-explain the project in every prompt, and it keeps Claude Code's suggestions consistent with decisions you made three days ago instead of re-litigating them.

Also start `NOTES.md` on Day 1 (empty is fine) — the roadmap already has you filling it in as you go. Point Claude Code at it when you want help drafting an entry.

## 2. One Claude Code session per checklist item, not one session for the whole project

Open a fresh, focused session (or at least a fresh conversation) for each day's goal rather than running one long continuous session for the whole week. Compiler code is local-reasoning-heavy — a scoped prompt like "implement a `RecursiveASTVisitor` that finds `ForStmt` nodes with statically-determinable constant bounds and records their source range" gets far more reliable output than "keep building the loop analyzer" carried over from three unrelated tasks ago.

Practical pattern for each day:
1. State the day's single goal from the checklist as your first message, plus 2-3 sentences of context (what exists already, what this connects to).
2. Let Claude Code propose an approach before writing code, if the task is non-trivial (call graph resolution, the safety check, the profitability heuristic) — ask "how would you approach X given Y constraint" first, sanity-check the approach yourself, then say "go ahead."
3. For pure API/boilerplate (CMake setup, basic AST visitor scaffolding, `Rewriter` insertion calls), just ask directly — no need for a design discussion.

## 3. Where to slow down and read carefully vs. where to move fast

**Move fast, trust more:** environment/build setup, CMake, basic AST traversal scaffolding, printing/debugging utilities, benchmark program boilerplate.

**Slow down, read every line:** the safety-analysis logic (Days 8-10), the profitability heuristic (Days 11-13), and the codegen pragma/map-clause generation (Days 14-18). These are exactly the parts that fail silently — wrong output, not a crash — and exactly the parts judges will ask you to explain. For these:
- After Claude Code writes the logic, ask it to walk you through it line by line before you accept it.
- Write the test cases yourself (the "one safe case, one unsafe case" pairs the roadmap calls for) — don't let it write both the logic and the tests, or bugs and blind spots can hide in both.
- If you can't re-explain what a chunk of generated code does without looking at it, that's a signal to stop and actually understand it before moving to the next day — this is also literally your "learn as you go" mechanism from the earlier plan.

## 4. Use it for the specific friction points in this project

- **Clang API discovery**: "what's the right ASTMatcher/visitor method to find X" — this is Claude Code's strongest use case here, since Clang's API surface is large and you're new to it.
- **Debugging AST dumps**: when something isn't matching or resolving right, paste the actual `-ast-dump` output or your visitor's print output, not a description of the symptom. Precise input → precise fixes.
- **OpenMP pragma/map-clause syntax**: getting `map(to:)`/`map(from:)`/`map(tofrom:)` right is fiddly — good use of AI lookup, but verify against the OpenMP spec section directly since this is a place small mistakes cause silent wrong-answer bugs.
- **Drafting `NOTES.md` entries**: after you make a real decision (e.g., capping interprocedural depth, choosing the coarse alias check), describe the decision and ask it to help you write a clear paragraph — you're the one who made the call, it's just helping you write it up fast.

## 5. Daily loop that keeps you honest

At the end of each day:
1. Run whatever you built against your benchmarks — confirm it actually does today's goal, not just "the code compiles."
2. Commit to git with a message describing what actually works (small commits per day make it easy to see real progress and roll back if a later day's changes break something).
3. Update `CLAUDE.md`'s "currently on" line and jot a one-liner in `NOTES.md` if you made a decision worth remembering.
4. Write your 2-3 sentence "what is this and why does it work" note (from the learning plan) before you close the session — this is what makes you able to defend the project cold at the finale instead of just having working code you can't fully explain.

## 6. When something isn't working after 2-3 tries

Stop iterating blindly. Step back and ask Claude Code to help you understand *why* it's failing (dump the AST, check the actual vs. expected pragma output, check the actual vs. expected map clause) rather than trying a fourth variation. If you're still stuck, it's a good moment to simplify the day's scope per the roadmap's rule — get a narrower version working today rather than burning the day chasing the full version.
