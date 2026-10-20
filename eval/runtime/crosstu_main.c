// Runtime check for tests/crosstu.c (EXTRA=tests/Inputs/crosstu_other.c
// scripts/check_parallel.sh tests/crosstu.c eval/runtime/crosstu_main.c).
// The tool sees both files; both builds link the other file unchanged. Every
// SAFE loop is called with disjoint arguments (its overlap check holds, it
// runs in parallel) and with OVERLAPPING ones, including the other file's
// global XG (the check must fail: in parallel these would race).
#include <stdio.h>
#include <stdlib.h>

#include SRC

double x_nowhere(double v) { return v - 1.0; }
double *x_hidden(void);

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (long)(rng % 100);
}
static double *arr(size_t n) {
  double *p = malloc(n * sizeof(double));
  for (size_t i = 0; i < n; i++) p[i] = (double)rnd();
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 4000;
  for (int i = 0; i < 4096; i++) XG[i] = (double)rnd();
  for (int ov = 0; ov < 2; ov++) {
    double *m = arr(5 * n + 8);
    double *a = m, *b = ov ? m + 1 : m + n + 1;
    c_pure(a, b, n); dump(m, sizeof(double) * (5 * n + 8));
    c_pure(b, a, n); dump(m, sizeof(double) * (5 * n + 8));
    c_ptr(a, n); dump(m, sizeof(double) * (5 * n + 8));
    c_chain(a, b, n); dump(m, sizeof(double) * (5 * n + 8));
    c_chain(b, a, n); dump(m, sizeof(double) * (5 * n + 8));
    // Rows of 4 from b: disjoint, then starting inside a's range.
    c_rows(a, ov ? m + 2 : m + n + 1, n); dump(m, sizeof(double) * (5 * n + 8));
  }
  {
    double *a = arr(n);
    c_global(a, n); dump(a, sizeof(double) * n);
    c_global(XG + 1, n); dump(XG, sizeof XG);
    c_global(XG, n); dump(XG, sizeof XG);
  }
  {
    double *a = arr(n);
    g_global(n); dump(XG, sizeof XG);
    g_dup(a, n); dump(a, sizeof(double) * n);
    g_nowhere(a, n); dump(a, sizeof(double) * n);
    g_static(a, n); dump(a, sizeof(double) * n);
    g_static(x_hidden() + 1, 7); dump(x_hidden(), sizeof(double) * 8);
  }
  fclose(out);
  return 0;
}
