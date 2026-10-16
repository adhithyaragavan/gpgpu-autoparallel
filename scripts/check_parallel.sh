#!/usr/bin/env bash
# Runtime check of the safety verdicts: rewrite a fixture with every SAFE
# loop forced to a CPU `parallel for` (-policy all-cpu), build it and the
# untouched fixture against the same driver, run both, and require
# bit-identical output. A loop wrongly verdicted SAFE shows up as a mismatch
# (races are timing-dependent, so it runs RUNS times with many threads).
#
#   scripts/check_parallel.sh [fixture] [driver]
# defaults: tests/dependence.c eval/runtime/dependence_main.c
#
# OFFLOAD=1 (Linux, needs liboffload): rewrite with -policy naive instead, so
# every SAFE loop the map() sections allow goes to a target region, and run
# those on the host device (-fopenmp-targets=<host>-unknown-linux-gnu,
# OMP_TARGET_OFFLOAD=MANDATORY). That device has memory of its own, so a
# map() section that misses an element, or a `from` over an element the loop
# doesn't write, shows up as a mismatch too.
# P05_FLAGS adds tool flags (e.g. -maps=bound, to see the old sections fail).
# POLICY overrides the policy (e.g. POLICY=gated OFFLOAD=1 runs the cost
# model's own choices, guards and data regions included).
set -euo pipefail
cd "$(dirname "$0")/.."
FIXTURE=${1:-tests/dependence.c}
DRIVER=${2:-eval/runtime/dependence_main.c}
TOOL=${P05_TOOL:-build/p05tool}
CC=${CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang || command -v clang)}
RUNS=${RUNS:-20}
SYSROOT=()
[[ "$(uname)" == Darwin ]] && SYSROOT=(-isysroot "$(xcrun --show-sdk-path)")
OMPFLAGS=(-fopenmp)
DEFAULT_POLICY=all-cpu
if [[ "${OFFLOAD:-0}" == 1 ]]; then
  [[ "$(uname)" == Linux ]] || { echo "OFFLOAD=1 needs Linux (host-offload plugin)"; exit 2; }
  OMPFLAGS+=(-fopenmp-targets="$(uname -m)-unknown-linux-gnu")
  DEFAULT_POLICY=naive
  export OMP_TARGET_OFFLOAD=MANDATORY
fi
POLICY=${POLICY:-$DEFAULT_POLICY}
[[ "$(uname)" == Darwin ]] && OMPFLAGS+=(-L/opt/homebrew/opt/libomp/lib -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
base=$(basename "$FIXTURE" .c)
# Strip the FileCheck lines (they are comments, but keep the copy clean).
grep -v -E '^\s*// (CHECK|CHECK-NEXT|RUN):' "$FIXTURE" > "$WORK/$base.c"

"$TOOL" -rewrite -policy "$POLICY" ${P05_FLAGS:-} "$WORK/$base.c" -- "${SYSROOT[@]}" > "$WORK/report.txt"
pragmas=$(grep -c '#pragma omp' "$WORK/$base.omp.c" || true)
targets=$(grep -c '#pragma omp target teams' "$WORK/$base.omp.c" || true)
echo "rewrote $FIXTURE (-policy $POLICY): $pragmas pragma(s), $targets target region(s)"

# Both optimization levels: at -O2 a race on a scalar the compiler keeps in a
# register is invisible (a missing private() goes unseen); at -O0 every
# variable lives in memory and the race shows.
fail=0
for OPT in -O2 -O0; do
  "$CC" $OPT -w "${SYSROOT[@]}" -DSRC="\"$WORK/$base.c\"" "$DRIVER" -o "$WORK/seq"
  "$CC" $OPT -w "${SYSROOT[@]}" "${OMPFLAGS[@]}" -DSRC="\"$WORK/$base.omp.c\"" "$DRIVER" -o "$WORK/par"
  "$WORK/seq" "$WORK/seq.bin"
  for ((r = 1; r <= RUNS; r++)); do
    OMP_NUM_THREADS=${OMP_NUM_THREADS:-8} "$WORK/par" "$WORK/par.bin"
    if ! cmp -s "$WORK/seq.bin" "$WORK/par.bin"; then
      echo "MISMATCH at $OPT on run $r: a loop verdicted SAFE produced different results in parallel"
      fail=1
      break 2
    fi
  done
done
[[ $fail == 0 ]] && echo "OK: parallel output bit-identical to sequential over $RUNS run(s) at -O2 and -O0"
exit $fail
