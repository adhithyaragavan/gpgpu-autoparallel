// Runtime check for tests/known.c (scripts/check_parallel.sh tests/known.c
// eval/runtime/known_main.c). Every loop whose safety rests on a known
// function is called with disjoint arguments (the overlap check holds, the
// loop runs in parallel) and with OVERLAPPING ones (the check must fail). The
// asserts never fire. The opaque twins run too, sequentially.
#include <stdio.h>
#include <stdlib.h>

#include SRC

double fast_sqrt(double x) { return x / 2.0; }

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (long)(rng % 100);
}
static double *arr(size_t n) {
  double *p = malloc(n * sizeof(double));
  for (size_t i = 0; i < n; i++) p[i] = (double)(rnd() - 30) / 7.0;
  return p;
}
static int *iarr(size_t n) {
  int *p = malloc(n * sizeof(int));
  for (size_t i = 0; i < n; i++) p[i] = (int)rnd() - 50;
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 4000;
  for (int ov = 0; ov < 2; ov++) {
    double *m = arr(2 * n + 1);
    double *a = m, *b = ov ? m + 1 : m + n + 1;
    k_libm(a, b, n); dump(m, sizeof(double) * (2 * n + 1));
    k_assert(a, b, n); dump(m, sizeof(double) * (2 * n + 1));
    k_assert(b, a, n); dump(m, sizeof(double) * (2 * n + 1));
    koff = ov ? 0 : 1;
    k_global(a, b, n - 1); dump(m, sizeof(double) * (2 * n + 1));
    int *im = iarr(2 * n + 1);
    k_expect(im, ov ? im + 1 : im + n + 1, n); dump(im, sizeof(int) * (2 * n + 1));
  }
  {
    double *a = arr(n), *b = arr(n);
    int *e = iarr(n);
    g_frexp(a, b, e, n); dump(a, sizeof(double) * n); dump(e, sizeof(int) * n);
    g_user(a, b, n); dump(a, sizeof(double) * n);
    for (int i = 0; i < n; i++) a[i] = (double)(i % 50);
    g_carried(a, n); dump(a, sizeof(double) * n);
    char *strs[4] = {"a", "bb", "ccc", ""};
    char **s = malloc(sizeof(char *) * n);
    for (int i = 0; i < n; i++) s[i] = strs[i % 4];
    int *ia = iarr(n);
    g_strlen(ia, s, n); dump(ia, sizeof(int) * n);
    srand(7);
    g_rand(ia, n); dump(ia, sizeof(int) * n);
    g_thread(ia, __builtin_thread_pointer(), n); dump(ia, sizeof(int) * n);
  }
  fclose(out);
  return 0;
}
