#!/usr/bin/env bash
# Runtime check of the safety verdicts: rewrite a fixture with every SAFE
# loop forced to a CPU `parallel for` (-policy all-cpu), build it and the
# untouched fixture against the same driver, run both, and require
# bit-identical output. A loop wrongly verdicted SAFE shows up as a mismatch
# (races are timing-dependent, so it runs RUNS times with many threads).
#
#   scripts/check_parallel.sh [fixture] [driver]
# defaults: tests/dependence.c eval/runtime/dependence_main.c
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
[[ "$(uname)" == Darwin ]] && OMPFLAGS+=(-L/opt/homebrew/opt/libomp/lib -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
base=$(basename "$FIXTURE" .c)
# Strip the FileCheck lines (they are comments, but keep the copy clean).
grep -v -E '^\s*// (CHECK|CHECK-NEXT|RUN):' "$FIXTURE" > "$WORK/$base.c"

"$TOOL" -rewrite -policy all-cpu "$WORK/$base.c" -- "${SYSROOT[@]}" > "$WORK/report.txt"
pragmas=$(grep -c '#pragma omp' "$WORK/$base.omp.c" || true)
echo "rewrote $FIXTURE: $pragmas pragma(s) forced on (every SAFE, non-nested loop)"

"$CC" -O2 -w "${SYSROOT[@]}" -DSRC="\"$WORK/$base.c\"" "$DRIVER" -o "$WORK/seq"
"$CC" -O2 -w "${SYSROOT[@]}" "${OMPFLAGS[@]}" -DSRC="\"$WORK/$base.omp.c\"" "$DRIVER" -o "$WORK/par"

"$WORK/seq" "$WORK/seq.bin"
fail=0
for ((r = 1; r <= RUNS; r++)); do
  OMP_NUM_THREADS=${OMP_NUM_THREADS:-8} "$WORK/par" "$WORK/par.bin"
  if ! cmp -s "$WORK/seq.bin" "$WORK/par.bin"; then
    echo "MISMATCH on run $r: a loop verdicted SAFE produced different results in parallel"
    fail=1
    break
  fi
done
[[ $fail == 0 ]] && echo "OK: parallel output bit-identical to sequential over $RUNS run(s)"
exit $fail
