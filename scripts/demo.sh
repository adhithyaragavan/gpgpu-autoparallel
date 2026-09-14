#!/usr/bin/env bash
# The finale walkthrough, narrated: the same five-stage story DAY_BY_DAY.md's
# Block B Day 4 names -- call graph -> safety check -> profitability decision
# -> codegen -> measured result -- as one runnable script instead of a set of
# ad hoc commands to remember on stage.
#
# Two modes:
#   ./scripts/demo.sh              -- paced, waits for Enter between acts
#   DEMO_PAUSE=0 ./scripts/demo.sh -- runs straight through, for recording a
#                                     backup capture (asciinema, screen
#                                     recorder, etc.)
#
# Never rebuilds or reruns the sweep on stage: p05tool is expected to exist
# already (same guard build_and_run.sh uses), and Act 5 reads timing out of
# build/bench/results.log, produced ahead of time by
# ./scripts/build_and_run.sh -- if that file is missing, Act 5 says so
# plainly and tells you what to run first, rather than silently having
# nothing to show.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
TOOL="$ROOT/build/p05tool"
OUTDIR="$ROOT/build/bench"
RESULTS="$OUTDIR/results.log"
PAUSE="${DEMO_PAUSE:-1}"

if [[ ! -x "$TOOL" ]]; then
  echo "demo.sh: $TOOL not found -- build it first (cmake --build $ROOT/build)" >&2
  exit 1
fi

SDK=""
if command -v xcrun >/dev/null 2>&1; then
  SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi
SYSROOT_ARGS=()
[[ -n "$SDK" ]] && SYSROOT_ARGS=(-isysroot "$SDK")

# Bold section headers, degrading to plain text if the terminal has no tput.
bold() { tput bold 2>/dev/null; printf '%s' "$1"; tput sgr0 2>/dev/null; echo; }

act() {
  echo
  echo "================================================================"
  bold "$1"
  echo "================================================================"
}

say() {
  echo "-- $1"
}

pause() {
  [[ "$PAUSE" == "0" ]] && return 0
  echo
  read -r -p "-- [press Enter to continue] " _
}

run() {
  echo "\$ $*"
  "$@"
}

# ---------------------------------------------------------------------------
act "Act 0 -- the input: ordinary, unmodified sequential C"
say "benchmarks/example.c. Nothing here is annotated. A loop whose body calls"
say "a helper function -- the shape a naive, intraprocedural-only"
say "parallelizer misses, because it can't see past the function call."
echo
run cat "$ROOT/benchmarks/example.c"
pause

# ---------------------------------------------------------------------------
act "Act 1 -- interprocedural resolution"
say "process()'s loop body is 'data[i] = scale(data[i], factor)' -- one call,"
say "zero visible arithmetic at the loop itself. The tool resolves the call"
say "across the function boundary and reasons about what scale() actually does."
echo
run "$TOOL" "$ROOT/benchmarks/example.c" -- "${SYSROOT_ARGS[@]+"${SYSROOT_ARGS[@]}"}"
pause

# ---------------------------------------------------------------------------
act "Act 2 -- safety, both directions"
say "example.c's loop verifies SAFE. That alone proves nothing -- a checker"
say "that only ever says yes hasn't checked anything. tests/safety_cases.c is"
say "the matched negative set: loops that write a global, write through a"
say "pointer parameter, or call something with no visible body, each"
say "correctly flagged UNSAFE or UNKNOWN with a specific, named reason."
echo
run "$TOOL" "$ROOT/tests/safety_cases.c" -- "${SYSROOT_ARGS[@]+"${SYSROOT_ARGS[@]}"}"
pause

# ---------------------------------------------------------------------------
act "Act 3 -- profitability: safe is not the same question as worth it"
say "compute_heavy.c: compute-bound (5.0 flop/byte), clears this machine's"
say "break-even intensity -- unconditional GPU_OFFLOAD."
echo
run "$TOOL" "$ROOT/benchmarks/compute_heavy.c" -- "${SYSROOT_ARGS[@]+"${SYSROOT_ARGS[@]}"}"
echo
say "small_update.c: same shape (a loop calling a helper), SAFE -- but only"
say "8 iterations. Thread-start and kernel-launch overhead would dwarf the"
say "actual work, so the cost model declines it: SEQUENTIAL, correctly."
echo
run "$TOOL" "$ROOT/benchmarks/small_update.c" -- "${SYSROOT_ARGS[@]+"${SYSROOT_ARGS[@]}"}"
pause

# ---------------------------------------------------------------------------
act "Act 4 -- codegen: the pragma, declare target, and map() clauses"
say "compute_heavy.c (unmodified) against compute_heavy.omp.c (rewritten by"
say "this tool's -rewrite pass), diffed. Every line added here is a decision"
say "already justified in Act 3's report -- the map() clause's direction and"
say "extent are read straight off the loop's own array-region analysis, not"
say "picked separately."
echo
run diff -u "$ROOT/benchmarks/compute_heavy.c" "$ROOT/benchmarks/compute_heavy.omp.c"
pause

# ---------------------------------------------------------------------------
act "Act 5 -- the measured result: sequential vs. naive vs. gated"
if [[ ! -f "$RESULTS" ]]; then
  echo
  echo "No $RESULTS found -- run ./scripts/build_and_run.sh first to produce"
  echo "real measured numbers (it builds and times every benchmark under all"
  echo "three policies). Skipping this act."
else
  say "Real wall-clock numbers (min of 7 repeats each), not modelled"
  say "estimates -- three policies per benchmark: sequential baseline, naive"
  say "'offload everything safe' (no guards), and this tool's cost-model-gated"
  say "policy. See NOTES.md's Day 21 entry for the full reasoning; two results"
  say "below are deliberately counterintuitive and reported as measured"
  say "rather than smoothed over:"
  echo
  grep '|policy-compare|' "$RESULTS" | while IFS='|' read -r name _stage status; do
    printf '  %-16s %s\n' "$name" "$status"
  done
  echo
  say "small_update is the clean case gating exists for: naive pays real cost"
  say "to offload 8 elements for nothing; gated pays none."
  say "example is the sharpest finding: naive measures FASTER than gated here"
  say "-- not because the gated guard was wrong (it correctly keeps n=1000"
  say "serial, below its own n>=4096 threshold), but because gating a file's"
  say "*only* OpenMP construct still pays libomp's one-time runtime cold"
  say "start regardless of the guard's outcome, with no second construct in"
  say "the file to absorb it. A real, measured gap the cost model has no term"
  say "for -- reported honestly, not hidden."
fi

echo
act "End -- the finale walkthrough in one line"
say "loop found -> call resolved across the function boundary -> safety"
say "verdict (both directions proven) -> profitability decision (safe != worth"
say "it) -> pragma generated from that same analysis -> measured, not just"
say "modelled, and the honest disagreements reported alongside the wins."
