// Days 11-13 fixture for ProfitabilityAnalyzer. Expected target is written
// above each loop, matching the safety_cases.c / loop_shapes.c convention: a
// single wrong verdict here is a bug. Every loop but the last is SAFE, so the
// discrimination being proven here is entirely profitability's own -- both
// directions of GPU_OFFLOAD (unconditional and the symbolic-bound GPU_IF),
// CPU_PARALLEL, and SEQUENTIAL all need a real example, per DAY_BY_DAY.md
// Day 13's "confirm it correctly says not profitable sometimes" applied one
// layer deeper than the safety pass already did.

// ====================================================== GPU_OFFLOAD ========

// A straight-line, high-arithmetic-intensity kernel: 40 fused multiply-adds
// (80 flops) against one read and one write of 8 bytes each -- 5.0 flop/byte,
// comfortably above this machine model's ~2.85 flop/byte break-even. This is
// deliberately the one kernel in the whole benchmark/fixture set worth a GPU
// at all -- example.c, saxpy.c and small_update.c all sit 10-50x below the
// wall, which is the point of running the model rather than assuming "safe
// and big" implies "offload it". See NOTES.md's Day 11-13 entry.
//
// Must stay straight-line: op counting is static (Profitability.h), so a
// loop inside this helper would be counted once rather than trip-count
// times, undercounting the real cost.
static double heavy_kernel(double x) {
  double r = 0.1;
  r = r * x + 0.01; r = r * x + 0.02; r = r * x + 0.03; r = r * x + 0.04;
  r = r * x + 0.05; r = r * x + 0.06; r = r * x + 0.07; r = r * x + 0.08;
  r = r * x + 0.09; r = r * x + 0.10; r = r * x + 0.11; r = r * x + 0.12;
  r = r * x + 0.13; r = r * x + 0.14; r = r * x + 0.15; r = r * x + 0.16;
  r = r * x + 0.17; r = r * x + 0.18; r = r * x + 0.19; r = r * x + 0.20;
  r = r * x + 0.21; r = r * x + 0.22; r = r * x + 0.23; r = r * x + 0.24;
  r = r * x + 0.25; r = r * x + 0.26; r = r * x + 0.27; r = r * x + 0.28;
  r = r * x + 0.29; r = r * x + 0.30; r = r * x + 0.31; r = r * x + 0.32;
  r = r * x + 0.33; r = r * x + 0.34; r = r * x + 0.35; r = r * x + 0.36;
  r = r * x + 0.37; r = r * x + 0.38; r = r * x + 0.39; r = r * x + 0.40;
  return r;
}

// GPU_OFFLOAD -- constant trip 65536, intensity 5.0 flop/byte.
void gpu_offload_case(double *out, const double *in) {
  for (int i = 0; i < 65536; i++) {
    out[i] = heavy_kernel(in[i]);
  }
}

// =========================================================== GPU_IF =======

// GPU_OFFLOAD if (n >= T) -- the same kernel, but the bound is a parameter,
// so the trip count is not known until run time. Proves the conditional
// path: this loop is worth a GPU only above a threshold the cost model
// solves for (not one that was picked), not unconditionally.
void gpu_conditional_case(double *out, const double *in, int n) {
  for (int i = 0; i < n; i++) {
    out[i] = heavy_kernel(in[i]);
  }
}

// ====================================================== CPU_PARALLEL ======

// CPU_PARALLEL -- large constant trip, low intensity. The SAXPY shape: one
// add, two array touches, transfer-bound on any real PCIe link.
void cpu_low_intensity_case(double *y, const double *x) {
  for (int i = 0; i < 65536; i++) {
    y[i] = y[i] + x[i];
  }
}

// CPU_PARALLEL -- strided. Touches 65536 elements, but the induction
// variable spans indices 0..1048575 (step 16), so the mapped region a real
// map() clause would need is 16x wider than the useful work. Transfer cost
// is charged on that span, not the touched count -- no separate stride gate
// exists, see the header comment in Profitability.h for why none is needed.
void strided_case(double *a) {
  for (int i = 0; i < 1048576; i += 16) {
    a[i] = a[i] * 2.0;
  }
}

// ======================================================== SEQUENTIAL ======

// SEQUENTIAL -- trip count 8. No target recovers a kernel-launch or
// thread-start cost over eight iterations; matches small_update.c's real
// verdict, restated here so this file alone proves the discrimination.
void sequential_case(double *a) {
  for (int i = 0; i < 8; i++) {
    a[i] = a[i] * 2.0;
  }
}

// CPU_PARALLEL -- edge case, not a discrimination pair: no array access at
// all in the body, so BytesPerIter is 0 and Intensity is a division by zero
// bytes. Regression fixture for a bug an adversarial pass caught before it
// ever shipped: the report formatter cast that infinite double straight to
// int64_t (undefined behaviour) rather than checking isfinite() first.
void zero_bytes_case(void) {
  for (int i = 0; i < 65536; i++) {
    double tmp = (double)i * 2.0;
    (void)tmp;
  }
}

// ===================================================== not evaluated ======

// Not evaluated -- safety verdict is UNSAFE (a direct callee writes a
// global), so profitability never runs at all. This is not "priced and
// declined"; it was never priced. Confirms the safety gate holds.
static int touch_count = 0;
static double touches(double x) {
  touch_count++;
  return x;
}
void unsafe_gate_case(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = touches(data[i]);
  }
}
