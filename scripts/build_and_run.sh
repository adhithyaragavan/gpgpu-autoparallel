#!/usr/bin/env bash
# Days 15-16: regenerate each benchmark's OpenMP pragmas, then run the
# three-tier evidence ladder documented in NOTES.md:
#
#   Tier 1 - host build & run: does the rewritten source compile against a
#            real OpenMP runtime (libomp) and produce output identical to
#            the sequential baseline? (On this machine, with no
#            libomptarget and no GPU, the "device" the target region runs
#            on is the host itself -- conformant OpenMP, not a hack.)
#   Tier 2 - device codegen: does Clang's NVPTX backend accept the rewritten
#            source as a *device* compilation unit, with every callee this
#            file wraps in `declare target` actually present as a device
#            function? (Catches declare-target bugs that -fsyntax-only
#            cannot see.)
#   Tier 3 - map-clause verification: do the map() clauses the tool wrote
#            lower to the byte counts and to/from directions Clang's
#            offload runtime call actually receives? (scripts/check_maps.py)
#
# No real GPU is available here, so no tier proves a device actually ran
# correctly -- see NOTES.md for exactly what each tier does and does not
# prove. Every unavailable check prints an explicit SKIP with a reason
# rather than being silently omitted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CLANG="${CLANG:-clang}"
TOOL="$ROOT/build/p05tool"
OUTDIR="$ROOT/build/bench"
NVPTX_ARCH="${NVPTX_ARCH:-sm_52}"

mkdir -p "$OUTDIR"

if [[ ! -x "$TOOL" ]]; then
  echo "build_and_run.sh: $TOOL not found -- build it first (cmake --build $ROOT/build)" >&2
  exit 1
fi

SDK=""
if command -v xcrun >/dev/null 2>&1; then
  SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi
SYSROOT_ARGS=()
[[ -n "$SDK" ]] && SYSROOT_ARGS=(-isysroot "$SDK")

BENCHMARKS=(example saxpy small_update compute_heavy)

# macOS ships bash 3.2 (no associative arrays), so results are logged as
# "name|stage|status" lines to a plain file instead of a -A array.
RESULTS_LOG="$OUTDIR/results.log"
: >"$RESULTS_LOG"
FAILED=0

record() {
  local name="$1" stage="$2" status="$3"
  echo "$name|$stage|$status" >>"$RESULTS_LOG"
  echo "  [$name:$stage] $status"
  [[ "$status" == FAIL* ]] && FAILED=1
}

for name in "${BENCHMARKS[@]}"; do
  src="$ROOT/benchmarks/$name.c"
  omp_src="$OUTDIR/$name.omp.c"
  report="$OUTDIR/$name.report.txt"
  echo "=== $name ==="

  # --- Regenerate ---
  # Remove any stale <name>.omp.c left in build/bench/ from an earlier run
  # first: p05tool only writes -o's target when it has something to
  # annotate, it never deletes/truncates a pre-existing file at that path,
  # so without this a leftover file from a previous invocation (different
  # flags, an older commit, a benchmark that used to annotate) would be
  # silently treated as this run's output below.
  rm -f "$omp_src"
  if "$TOOL" --rewrite -o "$omp_src" "$src" -- "${SYSROOT_ARGS[@]}" >"$report" 2>&1; then
    record "$name" "regenerate" "PASS"
  else
    record "$name" "regenerate" "FAIL (p05tool exited nonzero; see $report)"
    continue
  fi

  # Derived from the tool's own report, not just file presence, so a stale
  # file surviving some other way still can't be mistaken for this run's
  # output.
  has_offload=0
  if [[ -f "$omp_src" ]] && ! grep -q 'nothing annotated' "$report"; then
    has_offload=1
  fi

  # --- Tier 1: host build & run ---
  seq_bin="$OUTDIR/$name.seq"
  if "$CLANG" -O2 "${SYSROOT_ARGS[@]}" -o "$seq_bin" "$src" 2>"$OUTDIR/$name.seq.build.log"; then
    "$seq_bin" >"$OUTDIR/$name.seq.out" 2>&1
    record "$name" "tier1-seq" "PASS"
  else
    record "$name" "tier1-seq" "FAIL (sequential build; see $OUTDIR/$name.seq.build.log)"
    continue
  fi

  if [[ "$has_offload" -eq 0 ]]; then
    record "$name" "tier1-omp" "SKIP: no GPU-offload loop in this benchmark; CPU-parallel fallback is Days 17-18"
    record "$name" "tier2" "SKIP: no rewritten source to compile for device"
    record "$name" "tier3" "SKIP: no map() clauses to verify"
    continue
  fi

  omp_bin="$OUTDIR/$name.omp"
  if "$CLANG" -O2 -fopenmp "${SYSROOT_ARGS[@]}" -o "$omp_bin" "$omp_src" 2>"$OUTDIR/$name.omp.build.log"; then
    "$omp_bin" >"$OUTDIR/$name.omp.out" 2>&1
    record "$name" "tier1-omp" "PASS"
    if diff -q "$OUTDIR/$name.seq.out" "$OUTDIR/$name.omp.out" >/dev/null; then
      record "$name" "tier1-diff" "PASS (checksums match)"
    else
      record "$name" "tier1-diff" "FAIL (checksums differ -- $(cat "$OUTDIR/$name.seq.out") vs $(cat "$OUTDIR/$name.omp.out"); correctness is Day 19's job, but note it now)"
    fi
  else
    record "$name" "tier1-omp" "FAIL (OpenMP build; see $OUTDIR/$name.omp.build.log)"
    continue
  fi

  # --- Tier 2: device codegen ---
  ptx="$OUTDIR/$name.ptx"
  if "$CLANG" -fopenmp -fopenmp-targets=nvptx64-nvidia-cuda --offload-arch="$NVPTX_ARCH" \
       -nocudalib --offload-device-only -S "${SYSROOT_ARGS[@]}" "$omp_src" -o "$ptx" \
       2>"$OUTDIR/$name.ptx.build.log"; then
    entry_count=$(grep -c '\.entry __omp_offloading_' "$ptx" || true)
    pragma_count=$(grep -c '^  line ' "$report" || true)
    if [[ "$entry_count" -eq "$pragma_count" ]]; then
      record "$name" "tier2-entries" "PASS ($entry_count offloading entr$([ "$entry_count" -eq 1 ] && echo y || echo ies) == $pragma_count pragma(s))"
    else
      record "$name" "tier2-entries" "FAIL ($entry_count offloading entries != $pragma_count pragma(s))"
    fi

    dt_ok=1
    while IFS= read -r fn; do
      [[ -z "$fn" ]] && continue
      if grep -q "\.func.*[^A-Za-z0-9_]${fn}(" "$ptx"; then
        :
      else
        dt_ok=0
        record "$name" "tier2-declare-target" "FAIL ($fn is declare-target'd but has no device .func in $ptx)"
      fi
    done < <(grep '^  declare target: ' "$report" | sed 's/^  declare target: //')
    [[ "$dt_ok" -eq 1 ]] && record "$name" "tier2-declare-target" "PASS (every declare-target callee present as a device .func)"
  else
    record "$name" "tier2-entries" "FAIL (device codegen; see $OUTDIR/$name.ptx.build.log)"
    continue
  fi

  # --- Tier 3: map-clause verification ---
  # Only meaningful for a single-region file: Clang gives each additional
  # offload region its own @.offload_sizes.N / @.offload_maptypes.N pair
  # with independent (and not co-numbered) suffixes -- see check_maps.py's
  # docstring. Every benchmark this script drives has exactly one pragma.
  region_count=$(grep -c '^  line ' "$report" || true)
  if [[ "$region_count" -ne 1 ]]; then
    record "$name" "tier3" "SKIP: $region_count offload regions in this file; check_maps.py's default globals only resolve a single region (run it by hand with --sizes-global/--maptypes-global, see NOTES.md)"
    continue
  fi

  host_ll="$OUTDIR/$name.host.ll"
  if "$CLANG" -fopenmp -fopenmp-targets=nvptx64-nvidia-cuda --offload-host-only -S -emit-llvm \
       "${SYSROOT_ARGS[@]}" "$omp_src" -o "$host_ll" 2>"$OUTDIR/$name.host.build.log"; then
    pragma_text=$(grep '^  line ' "$report" | head -1 | grep -oE 'map\([^)]*\)( map\([^)]*\))*')
    if [[ -z "$pragma_text" ]]; then
      record "$name" "tier3" "SKIP: no map() clause found on the reported pragma line"
      continue
    fi
    if python3 "$SCRIPT_DIR/check_maps.py" --ll "$host_ll" --pragma "$pragma_text" >"$OUTDIR/$name.maps.txt" 2>&1; then
      record "$name" "tier3" "PASS (see $OUTDIR/$name.maps.txt)"
    else
      record "$name" "tier3" "FAIL (see $OUTDIR/$name.maps.txt)"
    fi
  else
    record "$name" "tier3" "FAIL (host-side IR emission; see $OUTDIR/$name.host.build.log)"
  fi
done

echo
echo "=== summary ==="
while IFS='|' read -r rname rstage rstatus; do
  printf '%-16s %-22s %s\n' "$rname" "$rstage" "$rstatus"
done <"$RESULTS_LOG"

if [[ "$FAILED" -eq 1 ]]; then
  echo
  echo "build_and_run.sh: at least one stage FAILed"
  exit 1
fi
echo
echo "build_and_run.sh: no FAILs (SKIPs are expected for benchmarks with no GPU-offload loop)"
exit 0
