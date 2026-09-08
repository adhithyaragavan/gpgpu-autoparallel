#include <stdio.h>

// Minimal Day 1 sanity benchmark: a loop that calls a helper function.
// This is exactly the shape naive, intraprocedural-only parallelizers miss —
// the loop's safety depends on knowing that `scale` has no side effects.

double scale(double x, double factor) {
  return x * factor;
}

void process(double *data, int n, double factor) {
  #pragma omp parallel for if(parallel: n >= 4096)
  for (int i = 0; i < n; i++) {
    data[i] = scale(data[i], factor);
  }
}

int main(int argc, char **argv) {
  double data[1000];
  for (int i = 0; i < 1000; i++) {
    data[i] = (double)i;
  }
  process(data, 1000, 2.0);

  double sum = 0.0;
  for (int i = 0; i < 1000; i++)
    sum += data[i];
  printf("example checksum %.12e\n", sum);

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
      fwrite(data, sizeof(double), 1000, f);
      fclose(f);
    }
  }
  return 0;
}
