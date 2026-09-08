// Demo benchmark 4: the one kernel in this benchmark set that actually
// clears the GPU-profitability bar. example.c, saxpy.c and small_update.c
// are all SAFE, all cross a function boundary, and all correctly verify NOT
// GPU-profitable under Days 11-13's cost model (see NOTES.md) — every one of
// them sits far below this machine model's ~2.85 flop/byte break-even
// intensity, because SAXPY-shaped kernels (a multiply-add or two per element
// against 16-24 bytes moved) are the textbook memory-bound case. Without a
// benchmark on the other side of the wall, the profitability heuristic would
// only ever be demonstrated saying "no" — this is the benchmark that proves
// it can also say "yes", and proves that yes is derived, not hardcoded.
//
// heavy_elem is 40 fused multiply-adds against one read and one write of 8
// bytes each: 80 flops / 16 bytes = 5.0 flop/byte, comfortably above the
// break-even. Deliberately straight-line, same reason as
// tests/profitability_cases.c's heavy_kernel: op counting is static, so a
// loop inside the helper would be counted once rather than trip-count times.

#include <stdio.h>

double heavy_elem(double x) {
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

#define N 65536

// Bound is the literal N directly (not a parameter, unlike example.c/
// process's `n`) so the trip count folds to a compile-time constant and the
// cost model can give an unconditional verdict rather than a guarded one —
// the strongest possible contrast against the three benchmarks that decline.
void heavy_transform(double *out, const double *in) {
  for (int i = 0; i < N; i++) {
    out[i] = heavy_elem(in[i]);
  }
}

int main() {
  static double in[N], out[N];
  for (int i = 0; i < N; i++) {
    in[i] = (double)i / N;
  }
  heavy_transform(out, in);

  double sum = 0.0;
  for (int i = 0; i < N; i++)
    sum += out[i];
  printf("compute_heavy checksum %.12e\n", sum);
  return 0;
}
