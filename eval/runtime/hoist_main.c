// Runtime check for tests/hoist.c (OFFLOAD=1 scripts/check_parallel.sh
// tests/hoist.c eval/runtime/hoist_main.c). On the host device with memory
// of its own, data kept in a `target data` region is only right on the host
// where a `target update` (or the region's end) copied it back: a missing
// update, or a loop left on the host inside the region, shows up as output
// different from the sequential build. Each function runs at a size where
// the region is active and at small ones (its condition false, or empty
// loops).
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
  const int ns[] = {20000, 9000, 3, 1, 0};
  for (int k = 0; k < 5; k++) {
    const int n = ns[k], steps = 4;
    const size_t m = n ? n : 1;
    { double *u = arr(m), *v = arr(m); jacobi(u, v, n, steps); dump(u, 8 * m); dump(v, 8 * m); }
    { double *a = arr(m), *b = arr(m), *c = arr(m); pipeline(a, b, c, n); dump(b, 8 * m); dump(c, 8 * m); }
    { double *u = arr(m), *r = arr(m); dumpd(residual(u, r, n, steps)); dump(u, 8 * m); dump(r, 8 * m); }
    { double *u = arr(m + 1), *w = arr(m + 1); boundary(u, w, n, steps); dump(u, 8 * m); dump(w, 8 * m); }
    { double *a = arr(m), *b = arr(m); shrink(a, b, n, steps); dump(a, 8 * m); dump(b, 8 * m); }
    { double *a = arr(m), *b = arr(m); swap(a, b, n, steps); dump(a, 8 * m); dump(b, 8 * m); }
    { double *a = arr(m), *b = arr(m); dumpd(escape(a, b, n, steps)); dump(a, 8 * m); dump(b, 8 * m); }
    if (n > 0) { double *a = arr(m), *b = arr(m); cond_reads(a, b, n); dump(a, 8 * m); dump(b, 8 * m); }
    { double *a = arr(m), *b = arr(m); maybe_alias(a, b, n, steps); dump(a, 8 * m); dump(b, 8 * m);
      maybe_alias(a, a, n, steps); dump(a, 8 * m); }
    { double *a = arr(m), *b = arr(m); once(a, b, n); dump(a, 8 * m); }
    { double *a = arr(m + 4), *b = arr(m); dumpd(loaded_ptr(a, b, n, steps)); dump(a, 8 * m); }
    { double *a = arr(m + 4), *b = arr(m); dumpd(local_ptr(a, b, n, steps)); dump(a, 8 * m); }
    { double *a = arr(m), *b = arr(m); zero_steps(a, b, n, steps); dump(a, 8 * m); dump(b, 8 * m);
      zero_steps(0, 0, n, 0); }
    { double *a = arr(m), *b = arr(m); under_if(a, b, n, 1 << 28, steps); dump(a, 8 * m); dump(b, 8 * m);
      under_if(a, b, n, n / 2, steps); dump(a, 8 * m); dump(b, 8 * m); }
    if (n > 0) { double *a = arr(m), *b = arr(m); dumpd(typedef_run(a, b, n)); dump(b, 8 * m); }
    { double *a = arr(m), *b = arr(m); hinted(a, b, n, steps); dump(a, 8 * m); dump(b, 8 * m); }
  }
  for (int i = 0; i < H; i++) { h_x[i] = rnd(); h_y[i] = rnd(); }
  calls_offloader(3); dump(h_x, sizeof h_x); dump(h_y, sizeof h_y); dumpd(h_acc);
  { double *a = arr(64), *b = arr(64); h_n = 64; escaped_bound(a, b, 4); dump(a, 8 * 64); dump(b, 8 * 64); }
  for (int i = 0; i < G; i++) g_x[i] = rnd();
  global_steps(3); dump(g_x, sizeof g_x); dump(g_y, sizeof g_y);
  fclose(out);
  return 0;
}
