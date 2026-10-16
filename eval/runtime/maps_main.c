// Runtime check for tests/maps.c (OFFLOAD=1 scripts/check_parallel.sh
// tests/maps.c eval/runtime/maps_main.c). The offload build runs every
// target region on a host device with memory of its own, so every element a
// region needs must be copied in by its map() section, and every element it
// must not change must be outside a `from`. Each output array is filled with
// random values first: an element a wrong `from` overwrites, or one a too
// short section never copies back, differs from the sequential build. Each
// symbolic loop also runs with an empty range (its map guard is false).
#include <stdio.h>
#include <stdlib.h>

#include SRC

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (long)(rng % 1000);
}
static long *arr(size_t n) {
  long *p = malloc((n ? n : 1) * sizeof(long));
  for (size_t i = 0; i < n; i++) p[i] = rnd();
  return p;
}
static void fill(long *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = rnd(); }
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }
static void dumpl(long v) { dump(&v, sizeof v); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int ns[] = {5000, 1, 0};
  for (int k = 0; k < 3; k++) {
    const int n = ns[k], m = 37, r = 23;
    { long *y = arr(n), *c = arr(n); cond_write(y, c, 7, n); dump(y, sizeof(long) * n); }
    { long *y = arr(n), *x = arr(n); full_write(y, x, n); dump(y, sizeof(long) * n); }
    { long *y = arr(2 * n), *x = arr(n); stride_write(y, x, n); dump(y, sizeof(long) * 2 * n); }
    { long *y = arr(n + 1), *b = arr(n + 1); shift(y, b, n); dump(y, sizeof(long) * (n + 1)); }
    { long *y = arr(n), *b = arr(n + 1), *c = arr(n); cond_shift(y, b, c, n); dump(y, sizeof(long) * n); }
    { long (*mm)[C] = malloc(sizeof(long[C]) * (r + 1)); fill(&mm[0][0], (size_t)C * (r + 1));
      rows(mm, k == 2 ? 0 : r); dump(mm, sizeof(long[C]) * (r + 1));
      triangle(mm, k == 2 ? 0 : r); dump(mm, sizeof(long[C]) * (r + 1));
      column(mm, k == 2 ? 0 : r); dump(mm, sizeof(long[C]) * (r + 1)); free(mm); }
    { int rr = k == 2 ? 0 : r; long *a = arr((size_t)rr * m + 1), *b = arr((size_t)rr * m);
      linear(a, b, rr, m); dump(a, sizeof(long) * ((size_t)rr * m + 1));
      callee_rows(a, rr, m); dump(a, sizeof(long) * ((size_t)rr * m + 1)); }
    { long *a = arr(n); dumpl(sum(a, n)); dumpl(last_t(a, n)); dump(a, sizeof(long) * n); }
  }
  fill(g_a, N); fill(g_b, N);
  cond_write_g(3); dump(g_a, sizeof g_a);
  descending(); dump(g_a, sizeof g_a);
  fill(g_a, N);
  global_part(100, 3000); dump(g_a, sizeof g_a);
  global_part(3000, 10); dump(g_a, sizeof g_a);
  fill(&g_m[0][0], (size_t)R * C);
  inner_dep(); dump(g_m, sizeof g_m);
  dumpl(last_j()); dump(g_m, sizeof g_m);
  // review cases: zero-trip inner loop, unsigned n < k, unsigned n = 0
  fill(g_a, N); cover_inner(50, 0); dump(g_a, sizeof g_a);
  cover_inner(50, 3); dump(g_a, sizeof g_a);
  { long *a = arr(100), *b = arr(100); unsigned_ptr(a + 50, b, 30, 10); dump(a, sizeof(long) * 100);
    unsigned_ptr(a, b, 10, 90); dump(a, sizeof(long) * 100); }
  fill(g_a, N); unsigned_obj(0); dump(g_a, sizeof g_a);
  unsigned_obj(77); dump(g_a, sizeof g_a);
  { long *a = arr(300), *b = arr(300); generic(a, b, 300); dump(a, sizeof(long) * 300); }
  { long *x = arr(301), *c = arr(300); read_pair(c, x, x + 1, 300); dump(c, sizeof(long) * 300); }
  fclose(out);
  return 0;
}
