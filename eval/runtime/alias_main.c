// Runtime check for tests/alias.c (scripts/check_parallel.sh tests/alias.c
// eval/runtime/alias_main.c). Every run-time-checked loop is called twice:
// with disjoint arguments (the guard holds, the loop runs in parallel) and
// with OVERLAPPING ones (the guard must fail and the loop run on one thread:
// in parallel these would race and differ from the sequential build).
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

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int n = 4000, r = 61, m = 67;
  // disjoint
  { long *a = arr(n), *b = arr(n + 1); rt_shift(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(r * m), *b = arr(r * m); rt_rows(a, b, r, m); dump(a, sizeof(long) * r * m); }
  { long *p = arr(n); for (int i = 0; i <= NG; i++) g_arr[i] = rnd(); rt_global(p, n); dump(p, sizeof(long) * n); }
  { long *p = arr(n); g_scale = 7; rt_scalar(p, n); dump(p, sizeof(long) * n); }
  // overlapping: b == a, rows shifted by one, p == g_arr
  { long *a = arr(n + 1); rt_shift(a, a, n); dump(a, sizeof(long) * (n + 1)); }
  { long *a = arr(r * m + 1); rt_rows(a, a + 1, r, m); dump(a, sizeof(long) * (r * m + 1)); }
  { for (int i = 0; i <= NG; i++) g_arr[i] = rnd(); rt_global(g_arr, NG); dump(g_arr, sizeof g_arr); }
  // proven apart (overlap would be undefined or impossible)
  { long *a = arr(n), *b = arr(n + 1); pr_restrict(a, b, n); dump(a, sizeof(long) * n); }
  { double *d = malloc(sizeof(double) * n); int *k = malloc(sizeof(int) * (n + 1));
    for (int i = 0; i <= n; i++) k[i] = (int)rnd();
    pr_types(d, k, n); dump(d, sizeof(double) * n); }
  { pr_callers(NG); dump(g_arr, sizeof(long)); }
  { long *a = arr(NG + 1); rt_callers(a, NG); dump(a, sizeof(long) * (NG + 1)); }
  { long *a = arr(n), *b = arr(n); k_prefix(a, b, n); dump(a, sizeof(long) * n); }
  // review cases, disjoint and overlapping
  { long *a = arr(n + 1), *b = arr(n + 100002); k_shadowed_param(a, b, n); dump(a, sizeof(long) * n); }
  { long *a = arr(n + 1); k_shadowed_param(a, a, n); dump(a, sizeof(long) * (n + 1)); }
  { long *p = arr(n); g_late = 3; k_late_global(p, n); dump(p, sizeof(long) * n); }
  { for (int i = 0; i <= NG; i++) g_arr[i] = rnd(); k_enum_shadow(g_arr, NG); dump(g_arr, sizeof g_arr); }
  // g_cp holds a fixed (never dereferenced) value: an address would differ
  // between the two builds.
  { static char cs[8]; g_cp = (char *)0x0102030405060708UL; rt_global_ptr(cs, 8); dump(cs, 8);
    rt_global_ptr((char *)&g_cp + 1, 7); dump(&g_cp, sizeof g_cp); }
  { long **b = malloc(sizeof(long *) * (n + 1)); void **a = malloc(sizeof(void *) * n);
    for (int i = 0; i <= n; i++) b[i] = (long *)(long)i;
    rt_void_ptrs(a, b, n); dump(a, sizeof(void *) * n);
    rt_void_ptrs((void **)b, b, n); dump(b, sizeof(long *) * (n + 1)); }
  { long *a = arr(n), *b = arr(n + 1); rt_same_width(a, (long long *)b, n); dump(a, sizeof(long) * n);
    rt_same_width(b, (long long *)b, n); dump(b, sizeof(long) * (n + 1)); }
  fclose(out);
  return 0;
}
