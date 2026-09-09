// Demo benchmark 3: safe but NOT GPU-profitable. Same shape as example.c and
// saxpy.c (loop body calls a pure helper), but the trip count is tiny (8) —
// kernel-launch and host-device transfer overhead would dwarf the actual work.
// This is the case the Week 2 profitability heuristic must learn to decline,
// so the demo can show "safe" and "profitable" are different questions.

#include <stdio.h>
#include <time.h>

double clamp_unit(double v) {
  if (v < 0.0)
    return 0.0;
  if (v > 1.0)
    return 1.0;
  return v;
}

void normalize(double *values) {
  for (int i = 0; i < 8; i++) {
    values[i] = clamp_unit(values[i]);
  }
}

int main(int argc, char **argv) {
  double values[8] = {-0.5, 0.2, 1.5, 0.7, -1.0, 0.9, 0.0, 2.0};
  { struct timespec _p05_t0, _p05_t1; clock_gettime(CLOCK_MONOTONIC, &_p05_t0); normalize(values); clock_gettime(CLOCK_MONOTONIC, &_p05_t1); fprintf(stderr, "TIMING small_update %.3f\n", (_p05_t1.tv_sec - _p05_t0.tv_sec) * 1e6 + (_p05_t1.tv_nsec - _p05_t0.tv_nsec) / 1e3); }

  double sum = 0.0;
  for (int i = 0; i < 8; i++)
    sum += values[i];
  printf("small_update checksum %.12e\n", sum);

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
      fwrite(values, sizeof(double), 8, f);
      fclose(f);
    }
  }
  return 0;
}
