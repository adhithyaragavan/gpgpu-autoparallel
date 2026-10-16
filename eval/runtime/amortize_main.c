// Runtime check for tests/amortize.c, run with the cost model's own choices:
// POLICY=gated OFFLOAD=1 scripts/check_parallel.sh tests/amortize.c
// eval/runtime/amortize_main.c. The regions it hoists carry cost guards in
// their condition, so each function runs at a size above them (the region
// and its GPU loops active, `target update`s around host code) and below
// (everything on the host); both must match the sequential build.
#include <stdio.h>
#include <stdlib.h>

#include SRC

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static double rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (double)(rng % 1000) / 100.0;
}
static double *arr(size_t n) {
  double *p = malloc((n ? n : 1) * sizeof(double));
  for (size_t i = 0; i < n; i++) p[i] = rnd();
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }
static void dumpd(double v) { dump(&v, sizeof v); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int ns[] = {40000, 20000, 1000, 2, 0};
  for (int k = 0; k < 5; k++) {
    const int n = ns[k];
    const size_t m = n ? n : 1;
    { double *u = arr(m), *v = arr(m); const_steps(u, v, n); dump(u, 8 * m); dump(v, 8 * m); }
    { double *u = arr(m), *v = arr(m); param_steps(u, v, n, 3); dump(u, 8 * m); dump(v, 8 * m); }
    { double *u = arr(m), *r = arr(m); dumpd(host_reads(u, r, n)); dump(u, 8 * m); dump(r, 8 * m); }
    if (n >= 2) { double *a = arr(m), *b = arr(m); host_rw(a, b, n); dump(a, 8 * m); dump(b, 8 * m); }
    { double *a = arr(m), *b = arr(m), *c = arr(m); pipeline(a, b, c, n); dump(b, 8 * m); dump(c, 8 * m); }
    { double *u = arr(m), *v = arr(m); converge(u, v, n, 0.0); dump(u, 8 * m); dump(v, 8 * m);
      converge(u, v, n, 1.0); dump(u, 8 * m); dump(v, 8 * m); }
    { double *x = arr(m), *c = arr(m); drag(x, c, n); dump(x, 8 * m); dump(c, 8 * m); }
    if (n >= 1) { double *a = arr(m); peak(a, n, 3); dump(a, 8 * m); peak(a, n, 0); dump(a, 8 * m); }
    { double *y = arr(m), *z = arr(m); own_gpu(y, z, n); dump(y, 8 * m); dump(z, 8 * m); }
  }
  for (int i = 0; i < N2; i++) g_a[i] = rnd();
  nested_steps(); dump(g_a, sizeof g_a);
  fclose(out);
  return 0;
}
