// Runtime check for tests/fields.c (scripts/check_parallel.sh tests/fields.c
// eval/runtime/fields_main.c). Every loop through struct fields is called
// with disjoint fields (the overlap check holds, the loop runs in parallel)
// and, where it has a run-time check, with OVERLAPPING ones (the check must
// fail: in parallel these would race and differ from the sequential build).
// f_scalar overlaps only through a type pun, which would break the strict
// aliasing the analysis assumes: it gets disjoint arguments only. Null
// roots check that a check reads a path only where the loop would.
#include <stdio.h>
#include <stdlib.h>

#include SRC

void g_opaque(void) {}

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
  // f_ptr, f_value, f_local, f_arg: disjoint, then b = a + 1.
  for (int ov = 0; ov < 2; ov++) {
    double *m = arr(2 * n + 1);
    struct vecs s = {n, 1.5, m, ov ? m + 1 : m + n + 1, NULL};
    f_ptr(&s, n); dump(m, sizeof(double) * (2 * n + 1));
    f_value(s, n); dump(m, sizeof(double) * (2 * n + 1));
    f_local(s, n); dump(m, sizeof(double) * (2 * n + 1));
    f_arg(&s, n); dump(m, sizeof(double) * (2 * n + 1));
  }
  // f_rows: disjoint, then f = p + 1 row.
  for (int ov = 0; ov < 2; ov++) {
    real3 *m = malloc(sizeof(real3) * (2 * n + 1));
    for (int i = 0; i < 2 * n + 1; i++)
      for (int k = 0; k < 3; k++) m[i][k] = (double)rnd();
    struct atoms a = {m, ov ? m + 1 : m + n + 1, NULL};
    struct sim s = {&a, 0.0};
    f_rows(&s, n, 0.25); dump(m, sizeof(real3) * (2 * n + 1));
  }
  {
    struct dom d;
    for (int i = 0; i < 8; i++) { d.lo[i] = rnd(); d.hi[i] = rnd(); d.ext[i] = 0; }
    f_member(&d, 3); f_siblings(&d); dump(&d, sizeof d);
  }
  {
    double *a = arr(n); int *ia = malloc(sizeof(int) * n);
    struct vecs s = {n, 0.5, a, NULL, ia};
    f_types(&s, n); dump(ia, sizeof(int) * n);
    double *x = arr(n);
    f_scalar(&s, x, n); dump(x, sizeof(double) * n);
  }
  { double *a = arr(n); f_based(a, n); dump(a, sizeof(double) * n); }
  // A check that reads a path before the loop must not dereference what
  // the loop itself never does: zero trips through a null root, and a loop
  // that only dereferences s->atoms under a flag that is never set.
  f_ptr(NULL, 0);
  f_rows(NULL, 0, 1.0);
  {
    struct sim sm = {NULL, 0.0};
    int *flag = calloc(n, sizeof(int));
    double *x = arr(n);
    g_cond(&sm, flag, x, n); dump(x, sizeof(double) * n);
  }
  {
    // g_decay only names the array member s->v->lo, which loads nothing.
    struct outer o = {NULL};
    int *flag = calloc(n, sizeof(int));
    double *x = arr(n);
    g_decay(&o, flag, x, n); dump(x, sizeof(double) * n);
  }
  // The loops that must stay sequential, for completeness.
  {
    double *a = arr(n), *b = arr(n);
    struct vecs s = {n, 1.0, a, b, NULL}, t = s;
    g_carried(&s, n); dump(a, sizeof(double) * n);
    double **o = malloc(sizeof(double *) * n);
    g_own(&s, o, n); dump(a, sizeof(double) * n);
    g_root(&s, &t, n); dump(a, sizeof(double) * n);
    char *c = malloc(n);
    g_char(&s, c, n); dump(a, sizeof(double) * n);
    struct atoms at = {NULL, NULL, b}, at2 = {NULL, NULL, a};
    struct sim sm = {&at, 0.0};
    double *x = arr(n);
    g_callee(&sm, &at2, x, n); dump(x, sizeof(double) * n);
    int *flag = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) flag[i] = (int)(rnd() % 2);
    sm.atoms = &at;
    g_cond(&sm, flag, x, n); dump(x, sizeof(double) * n);
    struct anon an; an.q = b; an.r = a;
    g_anon(&an, n); dump(a, sizeof(double) * n);
    struct withu w; w.u.a = b; w.c = a;
    g_union(&w, n); dump(a, sizeof(double) * n);
    gv = s;
    g_global(n); dump(a, sizeof(double) * n);
    union pun *u = calloc(n + 1, sizeof *u);
    float *fo = malloc(sizeof(float) * n);
    g_pun(u, fo, 1, n); dump(fo, sizeof(float) * n);
    struct bits *bt = calloc(n + 1, sizeof *bt);
    g_bits(bt, 1, n); dump(bt, sizeof *bt * (n + 1));
  }
  fclose(out);
  return 0;
}
