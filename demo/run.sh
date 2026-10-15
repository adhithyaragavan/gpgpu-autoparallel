#!/usr/bin/env bash
# Meeting demo: three short acts, Enter between each.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; TOOL="$ROOT/build/p05tool"
SDK=$(xcrun --show-sdk-path 2>/dev/null || true); ARGS=(); [[ -n "$SDK" ]] && ARGS=(-isysroot "$SDK")
pause() { read -r -p $'\n[Enter] ' _; clear; }
short() { grep -E "\.c:[0-9]+:|callees    :|accesses|^    [RW]+ |safety|^    - [^-a-z]|^    - [A-Z]|alias|profitability :|-> " | grep -v "ops/iteration\|data/iteration\|intensity\|per-element\|crossover"; }

clear; echo "== Act 1: safety, through function calls and pointers (demo/meeting.c) =="
sed -n '5,46p' "$ROOT/demo/meeting.c"; pause
"$TOOL" "$ROOT/demo/meeting.c" -- "${ARGS[@]}" 2>&1 | short; pause

echo "== Act 2: the generated code =="
"$TOOL" --rewrite "$ROOT/demo/meeting.c" -- "${ARGS[@]}" >/dev/null 2>&1
grep -n -A1 "#pragma" "$ROOT/demo/meeting.omp.c"; pause

echo "== Act 3: a compute-heavy loop the cost model sends to the GPU (benchmarks/compute_heavy.c) =="
"$TOOL" --whole-program "$ROOT/benchmarks/compute_heavy.c" -- "${ARGS[@]}" 2>&1 | sed -n '/compute_heavy.c:43/,/compute_heavy.c:50/p' | grep -v "compute_heavy.c:50"
"$TOOL" --whole-program --rewrite "$ROOT/benchmarks/compute_heavy.c" -- "${ARGS[@]}" >/dev/null 2>&1
echo; grep -n -A1 "#pragma omp target" "$ROOT/benchmarks/compute_heavy.omp.c"
