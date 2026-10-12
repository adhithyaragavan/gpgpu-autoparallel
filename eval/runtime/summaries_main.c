// Runtime check for tests/summaries.c (scripts/check_parallel.sh
// tests/summaries.c eval/runtime/summaries_main.c). Every SAFE loop runs as
// `parallel for`; every output array, global and returned value must match
// the sequential build exactly.
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
  const int n = 3001, m = 37;
  { long *a = arr(n), *b = arr(n); s_elem(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + 1); u_elem_next(a, n); dump(a, sizeof(long) * (n + 1)); }
  { long *a = arr(n), *b = arr(n); s_index(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); u_index_next(a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(2 * n + 2), *b = arr(n); s_two_calls(a, b, n); dump(a, sizeof(long) * (2 * n + 2)); }
  { long *a = arr(2 * n + 2), *b = arr(n); u_two_calls(a, b, n); dump(a, sizeof(long) * (2 * n + 2)); }
  { long *a = arr(4 * n); s_inner_call(a, n); dump(a, sizeof(long) * 4 * n); }
  { long *a = arr(n * m); s_rows(a, n, m); dump(a, sizeof(long) * n * m); }
  { long *a = arr(n * m + 1); u_rows_overrun(a, n, m); dump(a, sizeof(long) * (n * m + 1)); }
  { long *a = arr(n * m); s_rows_inner_col(a, n, m); dump(a, sizeof(long) * n * m); }
  { long *a = arr(n), *b = arr(n); s_transitive(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); u_transitive(a, n); dump(a, sizeof(long) * n); }
  { long *b = arr(NG); s_global_elem(b, NG); dump(g_tab, sizeof g_tab); }
  { long *b = arr(n); u_global_same(b, n); dump(g_tab, sizeof g_tab); }
  { long *a = arr(n); u_global_scalar(a, n); dump(a, sizeof(long) * n); val(g_count); }
  { long *a = arr(n), *b = arr(n); s_addr_private(a, b, n); dump(a, sizeof(long) * n); }
  { long *b = arr(n); val(u_addr_shared(b, n)); }
  { long *a = arr(n); s_callee_local(a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); k_recursive(a, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n); int *idx = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) idx[i] = (i * 7) % n;
    k_indirect(a, idx, n); dump(a, sizeof(long) * n); }
  { long *a = arr(2 * n); k_reassigned_param(a, n); dump(a, sizeof(long) * 2 * n); }
  { long *a = arr(n); g_off = 5; k_global_subscript(a, n); dump(a, sizeof(long) * n); }
  { long **rows = malloc(sizeof(long *) * n); long *a = arr(n);
    for (int i = 0; i < n; i++) rows[i] = &a[i];
    k_opaque_arg(rows, n); dump(a, sizeof(long) * n); }
  { long *a = arr(384); k_kr_narrow(a, 256); dump(a, sizeof(long) * 384); }
  { long *a = arr(2 * n + 4); k_kr_bytes(a, n); dump(a, sizeof(long) * (2 * n + 4)); }
  fclose(out);
  return 0;
}
