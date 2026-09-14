#!/usr/bin/env bash
# Days 15-16: regenerate each benchmark's OpenMP pragmas, then run the
# three-tier evidence ladder documented in NOTES.md:
#
#   Tier 1 - host build & run: does the rewritten source compile against a
#            real OpenMP runtime (libomp) and produce output identical to
#            the sequential baseline? (On this machine, with no
#            libomptarget and no GPU, the "device" the target region runs
#            on is the host itself -- conformant OpenMP, not a hack.)
#            Two sub-checks: tier1-diff is a coarse smoke check (the
#            printed checksum, sum(arr), matches) and tier1-exact (Day 19)
#            is the authoritative one -- every element of the output array,
#            dumped raw by passing a path as argv[1] to the benchmark,
#            compared bit-for-bit (scripts/compare_outputs.py). A checksum
#            alone can't catch a swapped element pair or a pair of
#            compensating errors; see NOTES.md (Day 19).
#   Timing (Day 20) - real wall-clock time, seq vs. this benchmark's
#            gated-policy omp binary, bracketing just the headline loop's
#            call site internally (clock_gettime, printed to stderr so it
#            can't perturb tier1-diff's stdout checksum). Reports the min
#            of TIMING_ITERS process-level repeats per binary. See NOTES.md
#            (Day 20) for how to read these against the model's own
#            estimates -- some of them measure something other than what
#            they look like at a glance (example.c's guard, compute_heavy's
#            host-fallback omp binary).
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
#
# Day 21 - naive policy comparison: a second pass, after the gated ladder
# above completes for every benchmark, builds and measures each benchmark
# under `p05tool --policy naive` (offload every SAFE loop unconditionally,
# no guards) and reports a three-way seq/naive/gated timing comparison
# (naive-regenerate/naive-build/naive-exact/timing-naive/policy-compare).
# See NOTES.md's Day 21 entry for what "naive" means concretely and why.
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

# Day 19: element count of the raw double-array dump each benchmark's
# main() optionally writes when given a path as argv[1] (see
# benchmarks/*.c). A case statement, not an associative array -- macOS
# ships bash 3.2, same reason results are logged to a plain file below
# instead of a -A array.
dump_count_for() {
  case "$1" in
    example) echo 1000 ;;
    saxpy) echo 65536 ;;
    small_update) echo 8 ;;
    compute_heavy) echo 65536 ;;
    *) echo 0 ;;
  esac
}

# Day 20: number of process-level repeats measure_timing_us runs per binary,
# reporting the min across them. Wall-clock timing at microsecond scale is
# noisy (scheduler jitter, etc.); min is the standard way to see past that
# without touching the benchmark source (each repeat is a fresh process, so
# there's no in-process re-application of a non-idempotent transform like
# clamp_unit/scale to worry about -- unlike repeating the call *within* one
# run would be).
TIMING_ITERS="${TIMING_ITERS:-7}"

# Runs $1 (a built benchmark binary, no dump-path argv) TIMING_ITERS times,
# discarding stdout and parsing the "TIMING <name> <us>" line each benchmark
# prints to stderr (see benchmarks/*.c), and prints the minimum microsecond
# value seen. Mirrors this project's own established lesson (both of Day
# 19's adversarial-review bugs were an unchecked invocation silently treated
# as valid data): a nonzero exit or a missing TIMING match aborts the whole
# script with a named error rather than being folded into "just no output".
#
# $bin's own exit status is captured directly from running it (not read back
# out of a `... | grep | awk` pipe's combined pipefail status): with three
# pipeline stages, pipefail only ever reports the *rightmost* non-zero exit
# code, so a binary that crashes *before* ever printing its TIMING line would
# have its real exit code masked by grep's unrelated "no match" exit (1) --
# caught by actually reproducing that exact case (a fake binary that exits
# 139 with no output) against an earlier version of this function, which
# still correctly hard-failed but misreported "exited 1" instead of 139.
measure_timing_us() {
  local bin="$1" i rc us best="" tmp
  tmp="$(mktemp)"
  for ((i = 0; i < TIMING_ITERS; i++)); do
    "$bin" >/dev/null 2>"$tmp"
    rc=$?
    if [[ "$rc" -ne 0 ]]; then
      echo "measure_timing_us: $bin exited $rc on iteration $i" >&2
      rm -f "$tmp"
      exit 1
    fi
    us=$(grep '^TIMING ' "$tmp" | awk '{print $3}')
    if [[ -z "$us" ]]; then
      echo "measure_timing_us: $bin exited 0 but produced no TIMING line on iteration $i (stderr: $(cat "$tmp"))" >&2
      rm -f "$tmp"
      exit 1
    fi
    if [[ -z "$best" ]] || awk -v a="$us" -v b="$best" 'BEGIN{exit !(a<b)}'; then
      best="$us"
    fi
  done
  rm -f "$tmp"
  echo "$best"
}

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
  # output. Split into GPU vs. CPU pragmas: a `^  line ` report line is
  # either kind (see OmpRewriter's report format), and only "omp target"
  # lines carry map() clauses / produce a device offloading entry — Tiers
  # 2-3 need at least one of those, Tier 1 (build & run, either pragma)
  # doesn't care which kind got the loop there.
  gpu_pragmas=$(grep -c '^  line .*omp target' "$report" || true)
  total_pragmas=$(grep -c '^  line ' "$report" || true)
  cpu_pragmas=$((total_pragmas - gpu_pragmas))

  # --- Tier 1: host build & run ---
  seq_bin="$OUTDIR/$name.seq"
  if "$CLANG" -O2 "${SYSROOT_ARGS[@]}" -o "$seq_bin" "$src" 2>"$OUTDIR/$name.seq.build.log"; then
    # stdout (the checksum, tier1-diff's input) and stderr (Day 20's TIMING
    # line) must stay separate now: merging them here would make tier1-diff
    # compare "checksum + TIMING" text, which differs from the omp binary's
    # own run by construction (the measured microseconds), and would FAIL
    # every single run regardless of correctness.
    "$seq_bin" >"$OUTDIR/$name.seq.out" 2>/dev/null
    record "$name" "tier1-seq" "PASS"
    seq_us="$(measure_timing_us "$seq_bin")"
    record "$name" "timing-seq" "PASS (min of $TIMING_ITERS runs: ${seq_us}us)"
  else
    record "$name" "tier1-seq" "FAIL (sequential build; see $OUTDIR/$name.seq.build.log)"
    continue
  fi

  # --- Day 21: unconditional sequential dump -----------------------------
  # Generated here, right after tier1-seq, rather than only inside
  # tier1-exact below (which is itself skipped whenever total_pragmas == 0
  # -- e.g. small_update, where the gated policy correctly declines its
  # only SAFE loop). The naive lane further down needs a sequential dump
  # to compare against for *every* benchmark, including ones the gated
  # policy offloads nothing for; without this, naive-exact FAILed on a
  # fresh build/bench with "no sequential dump" for exactly that case
  # (caught by adversarial review of this commit). tier1-exact below still
  # does its own rm -f + regenerate of this same file for benchmarks that
  # reach it -- harmless duplicate work, and it keeps that stage's existing
  # self-contained rm-then-regenerate discipline untouched.
  dump_count="$(dump_count_for "$name")"
  rm -f "$OUTDIR/$name.seq.dump"
  seq_dump_rc=0
  "$seq_bin" "$OUTDIR/$name.seq.dump" >/dev/null 2>&1 || seq_dump_rc=$?
  if [[ "$seq_dump_rc" -ne 0 ]]; then
    record "$name" "seq-dump" "FAIL (dump run failed -- seq exit $seq_dump_rc)"
    continue
  fi
  record "$name" "seq-dump" "PASS ($dump_count doubles written to $OUTDIR/$name.seq.dump)"

  if [[ "$total_pragmas" -eq 0 ]]; then
    record "$name" "tier1-omp" "SKIP: no parallelizable loop in this benchmark (every SAFE loop here was correctly ruled SEQUENTIAL by the cost model)"
    record "$name" "tier1-diff" "SKIP: no parallelizable loop in this benchmark (every SAFE loop here was correctly ruled SEQUENTIAL by the cost model)"
    record "$name" "tier1-exact" "SKIP: no parallelizable loop in this benchmark (every SAFE loop here was correctly ruled SEQUENTIAL by the cost model)"
    record "$name" "timing-omp" "SKIP: no parallelizable loop in this benchmark (every SAFE loop here was correctly ruled SEQUENTIAL by the cost model)"
    record "$name" "tier2" "SKIP: no rewritten source to compile for device"
    record "$name" "tier3" "SKIP: no map() clauses to verify"
    continue
  fi

  omp_bin="$OUTDIR/$name.omp"
  if "$CLANG" -O2 -fopenmp "${SYSROOT_ARGS[@]}" -o "$omp_bin" "$omp_src" 2>"$OUTDIR/$name.omp.build.log"; then
    "$omp_bin" >"$OUTDIR/$name.omp.out" 2>/dev/null
    record "$name" "tier1-omp" "PASS"
    if diff -q "$OUTDIR/$name.seq.out" "$OUTDIR/$name.omp.out" >/dev/null; then
      record "$name" "tier1-diff" "PASS (checksums match)"
    else
      record "$name" "tier1-diff" "FAIL (checksums differ -- $(cat "$OUTDIR/$name.seq.out") vs $(cat "$OUTDIR/$name.omp.out"))"
    fi

    # --- Tier 1 (cont.): exact per-element comparison (Day 19) ---
    # tier1-diff above only proves the printed checksum (sum(arr)) matches --
    # a plain additive sum can't distinguish a correct run from one with a
    # swapped element pair or a pair of compensating errors. Rerun both
    # binaries with a dump path as argv[1] and compare every element exactly.
    dump_count="$(dump_count_for "$name")"
    # Remove any stale dump left over from an earlier run first, same reason
    # as the `rm -f "$omp_src"` above: neither binary invocation below has
    # its exit status checked before this point in a way that would stop a
    # stale file from being compared, so without this a binary that crashes
    # here would silently leave the previous run's (valid) dump in place,
    # and the compare would then falsely PASS against stale data instead of
    # catching the failure.
    rm -f "$OUTDIR/$name.seq.dump" "$OUTDIR/$name.omp.dump"
    seq_dump_rc=0
    "$seq_bin" "$OUTDIR/$name.seq.dump" >/dev/null 2>&1 || seq_dump_rc=$?
    omp_dump_rc=0
    "$omp_bin" "$OUTDIR/$name.omp.dump" >/dev/null 2>&1 || omp_dump_rc=$?
    if [[ "$seq_dump_rc" -ne 0 || "$omp_dump_rc" -ne 0 ]]; then
      record "$name" "tier1-exact" "FAIL (dump run failed -- seq exit $seq_dump_rc, omp exit $omp_dump_rc)"
    elif python3 "$SCRIPT_DIR/compare_outputs.py" \
         --seq "$OUTDIR/$name.seq.dump" --omp "$OUTDIR/$name.omp.dump" \
         --count "$dump_count" >"$OUTDIR/$name.exact.txt" 2>&1; then
      record "$name" "tier1-exact" "PASS ($(cat "$OUTDIR/$name.exact.txt"))"
    else
      record "$name" "tier1-exact" "FAIL ($(cat "$OUTDIR/$name.exact.txt"))"
    fi

    # --- Timing (Day 20): real wall-clock time, seq vs. this benchmark's
    # gated-policy omp binary, replacing reliance on ProfitabilityAnalyzer's
    # modelled microsecond estimates as the timing evidence. See NOTES.md
    # for how to read these numbers against the model's own predictions --
    # in particular, example.c's n=1000 call site sits below its pragma's
    # own if(parallel: n>=4096) guard (expect near-parity with tier1-seq by
    # construction), and compute_heavy's omp binary here is plain -fopenmp
    # (no -fopenmp-targets), so it measures host-fallback target-region
    # execution, not a discrete GPU -- compare it against the model's t_cpu,
    # not t_gpu.
    omp_us="$(measure_timing_us "$omp_bin")"
    record "$name" "timing-omp" "PASS (min of $TIMING_ITERS runs: ${omp_us}us)"
    # Guarded the same way Day 21's policy-compare (below) already guards
    # this same divide: seq_us can be 0.000 (example, small_update -- below
    # clock_gettime's practical resolution at this scale, see NOTES.md Day
    # 20), and an unguarded s/o then reports "0.00x", reading as "the tool
    # made it infinitely slower" rather than what actually happened. This
    # line was the one Day 21's own comment (see policy-compare below)
    # named as producing that misleading output without fixing it here too.
    if awk -v s="$seq_us" 'BEGIN{exit !(s == 0)}'; then
      record "$name" "timing-speedup" "n/a -- sequential baseline (${seq_us}us) is below clock_gettime's practical resolution at this scale (omp ${omp_us}us)"
    else
      record "$name" "timing-speedup" "$(awk -v s="$seq_us" -v o="$omp_us" 'BEGIN{printf "%.2fx (seq %.3fus / omp %.3fus)", s/o, s, o}')"
    fi
  else
    record "$name" "tier1-omp" "FAIL (OpenMP build; see $OUTDIR/$name.omp.build.log)"
    continue
  fi

  if [[ "$gpu_pragmas" -eq 0 ]]; then
    record "$name" "tier2" "SKIP: no GPU-offload loop; the $cpu_pragmas CPU-threaded pragma(s) here have no device entry / no map() clauses by construction"
    record "$name" "tier3" "SKIP: no GPU-offload loop; the $cpu_pragmas CPU-threaded pragma(s) here have no device entry / no map() clauses by construction"
    continue
  fi

  # --- Tier 2: device codegen ---
  ptx="$OUTDIR/$name.ptx"
  if "$CLANG" -fopenmp -fopenmp-targets=nvptx64-nvidia-cuda --offload-arch="$NVPTX_ARCH" \
       -nocudalib --offload-device-only -S "${SYSROOT_ARGS[@]}" "$omp_src" -o "$ptx" \
       2>"$OUTDIR/$name.ptx.build.log"; then
    entry_count=$(grep -c '\.entry __omp_offloading_' "$ptx" || true)
    if [[ "$entry_count" -eq "$gpu_pragmas" ]]; then
      record "$name" "tier2-entries" "PASS ($entry_count offloading entr$([ "$entry_count" -eq 1 ] && echo y || echo ies) == $gpu_pragmas GPU pragma(s))"
    else
      record "$name" "tier2-entries" "FAIL ($entry_count offloading entries != $gpu_pragmas GPU pragma(s))"
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
  # docstring. Every benchmark this script drives has at most one GPU
  # pragma; CPU-threaded pragmas in the same file carry no map() clauses and
  # don't count toward this.
  if [[ "$gpu_pragmas" -ne 1 ]]; then
    record "$name" "tier3" "SKIP: $gpu_pragmas GPU-offload regions in this file; check_maps.py's default globals only resolve a single region (run it by hand with --sizes-global/--maptypes-global, see NOTES.md)"
    continue
  fi

  host_ll="$OUTDIR/$name.host.ll"
  if "$CLANG" -fopenmp -fopenmp-targets=nvptx64-nvidia-cuda --offload-host-only -S -emit-llvm \
       "${SYSROOT_ARGS[@]}" "$omp_src" -o "$host_ll" 2>"$OUTDIR/$name.host.build.log"; then
    pragma_text=$(grep '^  line .*omp target' "$report" | head -1 | grep -oE 'map\([^)]*\)( map\([^)]*\))*')
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


# --- Day 21: naive "offload everything safe" policy, second pass ---------
# A *separate* loop over BENCHMARKS, run only after every gated-policy stage
# above has finished, so nothing here can shift a pre-existing gated PASS/
# SKIP line (see NOTES.md, Day 21 -- the ladder above is untouched by this
# addition, on purpose). "Naive" means: every loop the safety pass cleared
# is GPU-offloaded unconditionally (guards dropped), via `p05tool --policy
# naive`; see Profitability.h's Policy enum comment for the exact rule and
# why it still declines a loop for a correctness reason (descending, macro
# location, nested, opaque callee) exactly like the gated policy does.
#
# Reuses measure_timing_us/dump_count_for/record/CLANG/SYSROOT_ARGS from
# above rather than redefining them. Also reuses the tier1-exact discipline
# Day 19's adversarial review established: remove a stale dump before
# regenerating it, and check the run's exit status directly (not through a
# pipe) before ever trusting compare_outputs.py's verdict.

# $1=name $2=stage -> that row's status field from $RESULTS_LOG (last match;
# bash 3.2 on macOS has no associative arrays, so this greps the plain log
# already written by record() above instead of a lookup table).
gated_status() {
  grep "^$1|$2|" "$RESULTS_LOG" | tail -1 | cut -d'|' -f3-
}

# Pulls the "N.NNN" out of a record() status string of the shape
# "PASS (min of 7 runs: N.NNNus)" (timing-seq/timing-omp's own format).
# Empty output means the stage never PASSed (e.g. timing-omp SKIPs for
# small_update, which has no .omp binary at all).
extract_us() {
  echo "$1" | grep -oE '[0-9.]+us\)$' | sed 's/us)$//'
}

for name in "${BENCHMARKS[@]}"; do
  src="$ROOT/benchmarks/$name.c"
  naive_src="$OUTDIR/$name.naive.c"
  naive_report="$OUTDIR/$name.naive.report.txt"
  echo "=== $name (naive policy) ==="

  seq_bin="$OUTDIR/$name.seq"
  seq_status="$(gated_status "$name" tier1-seq)"
  if [[ "$seq_status" != PASS* ]]; then
    record "$name" "naive-regenerate" "SKIP: sequential build did not PASS above, nothing to compare against"
    record "$name" "naive-build" "SKIP: sequential build did not PASS above"
    record "$name" "naive-exact" "SKIP: sequential build did not PASS above"
    record "$name" "timing-naive" "SKIP: sequential build did not PASS above"
    record "$name" "policy-compare" "SKIP: sequential build did not PASS above"
    continue
  fi

  # Same stale-output guard as the gated regenerate stage above: p05tool
  # only writes -o's target when it has something to annotate, it never
  # truncates a pre-existing file there.
  rm -f "$naive_src"
  if "$TOOL" --rewrite --policy=naive -o "$naive_src" "$src" -- "${SYSROOT_ARGS[@]}" >"$naive_report" 2>&1; then
    record "$name" "naive-regenerate" "PASS"
  else
    record "$name" "naive-regenerate" "FAIL (p05tool exited nonzero; see $naive_report)"
    continue
  fi

  naive_total=$(grep -c '^  line ' "$naive_report" || true)
  if [[ "$naive_total" -eq 0 ]]; then
    record "$name" "naive-build" "SKIP: naive policy produced no pragma (every SAFE loop here was declined for a correctness reason, not a profitability one -- see $naive_report)"
    record "$name" "naive-exact" "SKIP: no naive binary to compare"
    record "$name" "timing-naive" "SKIP: no naive binary to compare"
    record "$name" "policy-compare" "SKIP: no naive binary to compare"
    continue
  fi

  naive_bin="$OUTDIR/$name.naive"
  if "$CLANG" -O2 -fopenmp "${SYSROOT_ARGS[@]}" -o "$naive_bin" "$naive_src" 2>"$OUTDIR/$name.naive.build.log"; then
    record "$name" "naive-build" "PASS"
  else
    record "$name" "naive-build" "FAIL (naive OpenMP build; see $OUTDIR/$name.naive.build.log)"
    continue
  fi

  # --- naive-exact: per-element vs. the sequential dump, proving naive is
  # correct-but-slower (or correct-and-faster) rather than merely different.
  dump_count="$(dump_count_for "$name")"
  rm -f "$OUTDIR/$name.naive.dump"
  naive_dump_rc=0
  "$naive_bin" "$OUTDIR/$name.naive.dump" >/dev/null 2>&1 || naive_dump_rc=$?
  if [[ "$naive_dump_rc" -ne 0 ]]; then
    record "$name" "naive-exact" "FAIL (dump run failed -- naive exit $naive_dump_rc)"
  elif [[ ! -f "$OUTDIR/$name.seq.dump" ]]; then
    record "$name" "naive-exact" "FAIL (no sequential dump at $OUTDIR/$name.seq.dump -- the seq-dump stage above should have produced one)"
  elif python3 "$SCRIPT_DIR/compare_outputs.py" \
       --seq "$OUTDIR/$name.seq.dump" --omp "$OUTDIR/$name.naive.dump" \
       --count "$dump_count" >"$OUTDIR/$name.naive.exact.txt" 2>&1; then
    record "$name" "naive-exact" "PASS ($(cat "$OUTDIR/$name.naive.exact.txt"))"
  else
    record "$name" "naive-exact" "FAIL ($(cat "$OUTDIR/$name.naive.exact.txt"))"
  fi

  naive_us="$(measure_timing_us "$naive_bin")"
  record "$name" "timing-naive" "PASS (min of $TIMING_ITERS runs: ${naive_us}us)"

  # --- policy-compare: the three-way number the whole day exists for.
  # Guards a zero/empty denominator explicitly, the same way timing-speedup
  # above now also does (example/small_update both measure ~0us sequential
  # -- see NOTES.md, Day 20).
  seq_us_val="$(extract_us "$(gated_status "$name" timing-seq)")"
  gated_us_val="$(extract_us "$(gated_status "$name" timing-omp)")"
  if [[ -n "$gated_us_val" ]]; then
    compare_msg="$(awk -v s="${seq_us_val:-0}" -v n="$naive_us" -v g="$gated_us_val" \
      'BEGIN{
        printf "seq %.3fus / naive %.3fus / gated %.3fus -- ", s, n, g
        if (g < n) printf "gated wins (naive %.2fx slower than gated)", n/g
        else if (n < g) printf "naive wins (gated %.2fx slower than naive)", g/n
        else printf "tie"
      }')"
  else
    compare_msg="$(awk -v s="${seq_us_val:-0}" -v n="$naive_us" \
      'BEGIN{printf "seq %.3fus / naive %.3fus / gated n/a (no CPU/GPU pragma at all -- cost model declined this loop entirely)", s, n}')"
  fi
  record "$name" "policy-compare" "$compare_msg"
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
