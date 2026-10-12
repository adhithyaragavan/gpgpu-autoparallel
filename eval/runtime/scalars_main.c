// Runtime check for tests/scalars.c (scripts/check_parallel.sh
// tests/scalars.c eval/runtime/scalars_main.c). Every SAFE loop runs as
// `parallel for` with its private/lastprivate/reduction clauses; every output
// array and every returned value must match the sequential build exactly.
#include <stdio.h>
#include <stdlib.h>

#include SRC

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (long)(rng % 100);
}
static long *arr(size_t n) {
  long *p = malloc(n * sizeof(long));
  for (size_t i = 0; i < n; i++) p[i] = rnd();
  return p;
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }
static void val(long v) { dump(&v, sizeof v); }

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 301;
  { long *a = arr(n), *b = arr(n); s_temp(b, a, n); dump(b, sizeof(long) * n); }
  { long *a = arr(n * n), *b = arr(n * n); s_polybench(b, a, n); dump(b, sizeof(long) * n * n); }
  { long *a = arr(n); val(s_sum(a, n)); val(s_sub(a, n)); val(s_prod(a, 40)); val(s_bits(a, n));
    val(s_max(a, n)); val(s_min(a, n)); }
  { long *a = arr(n * n); val(s_nested_sum(a, n)); }
  { long *a = arr(n), *b = arr(n); val(s_last(b, a, n)); dump(b, sizeof(long) * n); }
  { long *b = arr(n * 4); val(s_last_inner_iv(b, n)); dump(b, sizeof(long) * n * 4); }
  { long *a = arr(100); val(s_ivout_const(a)); dump(a, sizeof(long) * 100); }
  { long *a = arr(n); val(u_carried(a, n)); dump(a, sizeof(long) * n); }
  { long *a = arr(n); val(u_last_conditional(a, n)); val(u_last_continue(a, n));
    val(u_mixed_ops(a, 20)); }
  { long *a = arr(n), *b = arr(n); val(u_read_elsewhere(b, a, n)); dump(b, sizeof(long) * n); }
  { long *a = arr(n); val(u_reverse_sub(a, n)); }
  { double d[64]; for (int i = 0; i < 64; i++) d[i] = rnd() / 7.0; double s = u_fp_sum(d, 64); dump(&s, sizeof s); }
  { long *a = arr(n); g_count = 0; u_global(a, n); val(g_count); }
  { long *a = arr(n), *b = arr(n); u_address_taken(b, a, n); dump(b, sizeof(long) * n); }
  { long *a = arr(n); u_bound_written(a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); val(k_ivout(a, n)); val(k_ivout(a, 0)); dump(a, sizeof(long) * n); }
  // Regressions from the Phase 1.4 review.
  { long *a = arr(n), *b = arr(n); for (int i = 0; i < n; i++) b[i] -= 50;
    r_prefix(a, b, n); dump(a, sizeof(long) * n); r_prefix_if(a, b, n); dump(a, sizeof(long) * n); }
  { unsigned u[64]; for (int i = 0; i < 64; i++) u[i] = (unsigned)(rnd() * 50000000u);
    val(r_mixed_max(u, 64)); }
  { long *b = arr(n); val(r_int_fp(b, n)); val(r_bool(b, n)); }
  fclose(out);
  return 0;
}
