// Runtime check for tests/bounds.c (scripts/check_parallel.sh tests/bounds.c
// eval/runtime/bounds_main.c). Every loop whose alias guard comes from a
// value or size bound is called with disjoint arguments (the guard holds,
// the loop runs in parallel) and with OVERLAPPING ones (the guard must fail:
// in parallel these would race and differ from the sequential build).
// vb_unsigned_wrap is never called: its subscript wraps to ~4e9.
#include <stdio.h>
#include <stdlib.h>

#include SRC

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static unsigned long long next(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return rng;
}
static long rnd(void) { return (long)(next() % 100); }
static long *arr(size_t n) {
  long *p = malloc(n * sizeof(long));
  for (size_t i = 0; i < n; i++) p[i] = rnd();
  return p;
}
static int *idxs(size_t n, int mod) {
  int *p = malloc(n * sizeof(int));
  for (size_t i = 0; i < n; i++) p[i] = (int)(next() % mod);
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 4000;
  // (a) value bounds, disjoint then overlapping
  { long *a = arr(n), *b = arr(3); vb_mod(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); vb_mod(a, a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + 2); vb_mod(a + 2, a, n); dump(a, sizeof(long) * (n + 2)); }
  { long *a = arr(n), *b = arr(n); vb_mod_sym(a, b, 7, n); dump(a, sizeof(long) * n); }
  { long *a = arr(2 * n), *b = arr(7); vb_mod_neg(a, b + 3, n); dump(a, sizeof(long) * 2 * n); }
  { long *a = arr(2 * n); vb_mod_neg(a, a + 3, n); dump(a, sizeof(long) * 2 * n); }
  { long *a = arr(n), *b = arr(8); int *k = idxs(n, 100); vb_mask(a, b, k, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); int *k = idxs(n, 100); vb_mask(a, a, k, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + 6); int *k = idxs(n, 100); vb_mask(a + 6, a, k, n); dump(a, sizeof(long) * (n + 6)); }
  { long *a = arr(n), *b = arr(100); int *k = idxs(n, 100); vb_mask_neg(a, b, k, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n), *b = arr(n), *c = arr(n); vb_div(a, b, c, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); vb_div(a, a, a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + n / 4), *c = arr(n); vb_div(a + n / 4, a, c, n); dump(a, sizeof(long) * (n + n / 4)); }
  { long *a = arr(n), *b = arr(10); vb_square(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); vb_square(a, a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + 9); vb_square(a + 9, a, n); dump(a, sizeof(long) * (n + 9)); }
  { long *a = arr(n + 8); vb_apart(a, n); dump(a, sizeof(long) * (n + 8)); }
  { long *a = arr(n + 8); vb_apart_neg(a, n); dump(a, sizeof(long) * (n + 8)); }
  // (b) size bounds, disjoint then overlapping
  { long *p = arr(n); int *k = idxs(n, NT); for (int i = 0; i < NT; i++) tab[i] = rnd();
    sz_global(p, k, n); dump(p, sizeof(long) * n); }
  { int *k = idxs(NT, NT); for (int i = 0; i < NT; i++) tab[i] = rnd();
    sz_global(tab, k, NT); dump(tab, sizeof tab); }
  // Overlapping only the upper half of tab: a bound short of 4095 misses it.
  { int *k = idxs(NT, NT); for (int i = 0; i < NT; i++) tab[i] = rnd();
    sz_global(tab + NT / 2 + 8, k, NT / 2 - 8); dump(tab, sizeof tab); }
  { long *p = arr(NR); int *k = idxs(NR, NC);
    for (int i = 0; i < NR; i++) for (int j = 0; j < NC; j++) grid[i][j] = rnd();
    sz_grid(p, k, NR); dump(p, sizeof(long) * NR); }
  { int *k = idxs(NR, NC);
    for (int i = 0; i < NR; i++) for (int j = 0; j < NC; j++) grid[i][j] = rnd();
    sz_grid(&grid[0][0], k, NR); dump(grid, sizeof grid); }
  { long *p = arr(n), *q = arr(100); int *k = idxs(n, 100); sz_pointer(p, q, k, n); dump(p, sizeof(long) * n); }
  { int *k = idxs(NS, NS); for (int i = 0; i < NS; i++) s_src[i] = rnd();
    sz_static_caller(k); dump(s_dst, sizeof s_dst); }
  { int *k = idxs(NS, NS); long *o = arr(NS); for (int i = 0; i < NS; i++) m_src[i] = rnd();
    sz_static_mixed_caller(k, o); dump(m_dst, sizeof m_dst); }
  { double (*pa)[NR][NR] = malloc(sizeof *pa);
    for (int i = 0; i < NR; i++) {
      t_x[i] = (double)rnd();
      for (int j = 0; j < NR; j++) t_a[i][j] = (*pa)[i][j] = (double)rnd();
    }
    sz_tri_caller(NR, pa); dump(t_a, sizeof t_a); dump(pa, sizeof *pa); }
  { double (*A)[NR] = malloc(sizeof(double) * NR * NR); double *x = malloc(sizeof(double) * NR);
    for (int i = 0; i < NR; i++) { x[i] = (double)rnd(); for (int j = 0; j < NR; j++) A[i][j] = (double)rnd(); }
    sz_tri_extern(NR, A, x); dump(A, sizeof(double) * NR * NR); }
  { for (int i = 0; i < NR; i++) {
      t2_x[i] = (double)rnd();
      for (int j = 0; j < NR; j++) t2_a[i][j] = (double)rnd();
    }
    sz_tri_gpu_caller(NR); dump(t2_a, sizeof t2_a); }
  { int *k = idxs(NS, NT); for (int i = 0; i < NT; i++) o_tab[i] = rnd();
    sz_object(k); dump(o_out, sizeof o_out); }
  { int *ks = idxs(NS, NS / 2), *kb = idxs(NS, NS);
    for (int i = 0; i < NS; i++) z_big[i] = rnd();
    for (int i = 0; i < NS / 2; i++) z_small[i] = rnd();
    sz_sizes_caller(ks, kb); dump(z_dst, sizeof z_dst); }
  { int *k = malloc(sizeof(int) * 2 * NS);
    for (int i = 0; i < 2 * NS; i++) k[i] = NS + i - 1 < 0 ? 0 : NS + i - 1;
    for (int i = 0; i < 4; i++) for (int j = 0; j < NS; j++) r_rows[i][j] = rnd();
    sz_row_caller(k, 2 * NS); dump(r_rows, sizeof r_rows); }
  { struct hack *h = malloc(sizeof(struct hack) + sizeof(long) * NS); long *o = arr(NS);
    int *k = idxs(NS, NS); h->n = NS; for (int i = 0; i < NS; i++) h->d[i] = rnd();
    sz_member_caller(h, o, k); dump(o, sizeof(long) * NS); }
  { int *k = idxs(4, 100); sz_past_caller(k); dump(w_obj, sizeof w_obj); }
  fclose(out);
  return 0;
}
