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
#include <time.h>

#pragma omp declare target
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
#pragma omp end declare target

#define N 65536

// Bound is the literal N directly (not a parameter, unlike example.c/
// process's `n`) so the trip count folds to a compile-time constant and the
// cost model can give an unconditional verdict rather than a guarded one —
// the strongest possible contrast against the three benchmarks that decline.
void heavy_transform(double *out, const double *in) {
  #pragma omp target teams distribute parallel for map(from: out[0:65536]) map(to: in[0:65536])
  for (int i = 0; i < N; i++) {
    out[i] = heavy_elem(in[i]);
  }
}

int main(int argc, char **argv) {
  static double in[N], out[N];
  #pragma omp parallel for
  for (int i = 0; i < N; i++) {
    in[i] = (double)i / N;
  }
  { struct timespec _p05_t0, _p05_t1; clock_gettime(CLOCK_MONOTONIC, &_p05_t0); heavy_transform(out, in); clock_gettime(CLOCK_MONOTONIC, &_p05_t1); fprintf(stderr, "TIMING compute_heavy %.3f\n", (_p05_t1.tv_sec - _p05_t0.tv_sec) * 1e6 + (_p05_t1.tv_nsec - _p05_t0.tv_nsec) / 1e3); }

  double sum = 0.0;
  for (int i = 0; i < N; i++)
    sum += out[i];
  printf("compute_heavy checksum %.12e\n", sum);

  // Day 19: optional raw dump of the final output array, when a dump path
  // is given as argv[1] -- not an env var, and main()'s existing signature
  // line is edited in place rather than a new #include being inserted
  // above the loops, so this addition cannot shift any loop's reported
  // line:col in the tool's report (getenv would need <stdlib.h>; fopen/
  // fwrite/fclose are already declared via the existing <stdio.h>). A
  // single fwrite -- not a loop -- so LoopCollector (which only visits
  // ForStmt) never sees it; this cannot become a new candidate loop in the
  // tool's report either. Lets scripts/compare_outputs.py do an exact
  // per-element comparison against the sequential baseline, which the
  // checksum above cannot: a swapped pair of elements, or two compensating
  // errors, sums to the same total either way. See NOTES.md (Day 19).
  if (argc > 1) {
    FILE *f = fopen(argv[1], "wb");
    if (f) {
      fwrite(out, sizeof(double), N, f);
      fclose(f);
    }
  }
  return 0;
}
