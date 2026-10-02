// Runtime check for tests/dependence.c, used by scripts/check_parallel.sh.
// Compiled twice: once with SRC = the fixture as written (sequential), once
// with SRC = the tool's rewrite under -policy all-cpu (every SAFE loop gets a
// parallel for). Each function runs on pseudo-random data and every output
// array is written to argv[1]; the two dumps must be bit-identical.
#include <stdio.h>
#include <stdlib.h>

#include SRC

static unsigned long long rng = 88172645463325252ULL;
static double rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (double)(rng % 100000) / 1000.0;
}
static double *arr(size_t n) {
  double *p = malloc(n * sizeof(double));
  for (size_t i = 0; i < n; i++) p[i] = rnd();
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 257, n3 = 41;
  { double *a = arr(n * n), *b = arr(n * n); transpose(b, a, n); dump(b, sizeof(double) * n * n); }
  { double *m = arr(n * n), *x = arr(n), *y = arr(n); matvec(y, m, x, n); dump(y, sizeof(double) * n); }
  { double *a = arr(2 * n); halves(a, n); dump(a, sizeof(double) * 2 * n); }
  { double *a = arr(2 * n + 1); even_odd(a, n); dump(a, sizeof(double) * (2 * n + 1)); }
  { double *a = arr(n + 2); stride_two(a, n); dump(a, sizeof(double) * (n + 2)); }
  { double *a = arr(20); distance_beyond_span(a); dump(a, sizeof(double) * 20); }
  { double *a = arr(n * n); row_prefix(a, n); dump(a, sizeof(double) * n * n); }
  { double *a = arr(n * n); column_prefix(a, n); dump(a, sizeof(double) * n * n); }
  { double *a = arr(n3 * n3 * n3), *c = arr(n3 * n3 * n3); scale3(c, a, n3); dump(c, sizeof(double) * n3 * n3 * n3); }
  { for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++) grid2[i][j] = rnd();
    scale_grid(); dump(grid2, sizeof grid2); }
  { double *a = arr(n); prefix_sum(a, n); dump(a, sizeof(double) * n); }
  { double *a = arr(n), acc = 0; accumulate_into_array(&acc, a, n); dump(&acc, sizeof acc); }
  { double *a = arr(n * n); wavefront(a, n); dump(a, sizeof(double) * n * n); }
  { double *a = arr(n), *b = arr(n * n); same_row_each_time(a, b, n); dump(a, sizeof(double) * n); }
  { double *a = arr(2 * n + 1); halves_inclusive(a, n); dump(a, sizeof(double) * (2 * n + 1)); }
  { double *a = arr(n + 3); unknown_offset(a, n, 3); dump(a, sizeof(double) * (n + 3)); }
  { double *a = arr(n); int idx[257]; for (int i = 0; i < n; i++) idx[i] = (i * 7) % n;
    scatter(a, idx, n); dump(a, sizeof(double) * n); }
  // Regressions from the Phase 1.3 review (each produced a mismatch once).
  { double *buf = arr(n + 4), *c = arr(4); d1_sibling_fact(buf + 4, c, n, 0); dump(buf, sizeof(double) * (n + 4)); }
  { double *a = arr(n * n + 4 * n); d2_two_row_lengths(a, n, 3); dump(a, sizeof(double) * (n * n + 4 * n)); }
  { double *a = arr(n), *b = arr(n); d3_static_scratch(a, b, n); dump(b, sizeof(double) * n); }
  { double *a = arr(n); d4_local_alias(a, n); dump(a, sizeof(double) * n); }
  { double *a = arr(n), *b = arr(n); d4_unresolved_alias(a, b, n, 1); dump(a, sizeof(double) * n); }
  { double *a = arr(n); d5_callee_derived_write(a, n); dump(a, sizeof(double) * n); }
  { for (int i = 0; i < 1024; i++) d6_g[i] = rnd(); d6_callee_reads_global(1000); dump(d6_g, sizeof d6_g); }
  { double *a = arr(n); d7_callee_reads_arg(a, n); dump(a, sizeof(double) * n); }
  { double *a = arr(4); l1_single_iteration(a); dump(a, sizeof(double) * 4); }
  // Regressions from the second Phase 1.3 review.
  { double *a = arr(n); e1_alias_into_callee(a, n); dump(a, sizeof(double) * n); }
  { double *a = arr(n); e2_pointer_in_struct(a, n); dump(a, sizeof(double) * n); }
  { double *a = arr(n); e2_pointer_as_long(a, n); dump(a, sizeof(double) * n); }
  { for (int i = 0; i < 1024; i++) e_g[i] = rnd(); e3_callee_writes_via_struct(1000); dump(e_g, sizeof e_g); }
  { double *a = arr(n); e4_indirect_in_callee(a, n); dump(a, sizeof(double) * n); }
  { int ia[257]; for (int i = 0; i < n; i++) ia[i] = i; e5_atomic(ia, n); dump(ia, sizeof ia); }
  { double *a = arr(n), *b = arr(n); e6_reassigned_param(a, b, n); dump(a, sizeof(double) * n); }
  { double *a = arr(n); e6_global_pointer(a, n); dump(a, sizeof(double) * n); }
  { double a8[8] = {0}; e8_single_iteration_lie(a8); dump(a8, sizeof a8); }
  { double *a = arr(n); a[100] = -1.0; e9_break(a, n); dump(a, sizeof(double) * n); }
  fclose(out);
  return 0;
}
