// Runtime check for tests/boxes.c (scripts/check_parallel.sh tests/boxes.c
// eval/runtime/boxes_main.c). Every box loop is called with boxes within
// capacity (the inspector passes, the loop runs in parallel), with one box
// OVER capacity (box b's atoms then spill into box b + 1's slots: in parallel
// they would race and differ from the sequential build, so the inspector must
// fail) and, where it has an overlap check, with p and f OVERLAPPING. Empty
// grids with null pointers check that a check reads a pointer only where the
// loop would.
#include <stdio.h>
#include <stdlib.h>

#include SRC

double rng(int k) { return 0.5 * k; }

static unsigned long long seed = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
  return (long)(seed % 100);
}
static real3 *rows(size_t n) {
  real3 *p = malloc(sizeof(real3) * n);
  for (size_t i = 0; i < n; i++)
    for (int k = 0; k < 3; k++) p[i][k] = (double)rnd();
  return p;
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
  const int nb = 512;
  // Room for every box plus a spill of the last one.
  const size_t slots = (size_t)MAXATOMS * nb + 2 * MAXATOMS;
  int *n = malloc(sizeof(int) * nb);
  // over: 0 = within capacity, 1 = some boxes hold 2 * MAXATOMS atoms.
  // ov: f = p + 1 row (overlapping) or a separate array.
  for (int over = 0; over < 2; over++)
    for (int ov = 0; ov < 2; ov++) {
      for (int b = 0; b < nb; b++) n[b] = (int)(rnd() % (MAXATOMS + 1));
      // Full boxes (or over capacity) on every 16th boundary, so that some
      // land on the edge of a thread's chunk: box b's last atoms and box
      // b + 1's first then run on different threads.
      for (int b = 15; b + 1 < nb; b += 16) {
        n[b] = over ? 2 * MAXATOMS : MAXATOMS;
        n[b + 1] = MAXATOMS;
      }
      real3 *p = rows(2 * slots + 1);
      real3 *f = ov ? p + 1 : p + slots + 1;
      struct atoms at = {NULL, p, f, NULL};
      struct boxes bx = {nb, n};
      struct sim s = {&at, &bx, 0.01};
      b_velocity(&s, nb, 0.25); dump(p, sizeof(real3) * (2 * slots + 1));
      b_scale(&s, 1.5); dump(p, sizeof(real3) * (2 * slots + 1));
      b_params(p, (const real3 *)f, n, nb, 0.5);
      dump(p, sizeof(real3) * (2 * slots + 1));
      b_label(p, n, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      g_spill(p, n, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      g_call(p, n, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      int *skip = malloc(sizeof(int) * nb);
      for (int b = 0; b < nb; b++) skip[b] = (int)(rnd() % 2);
      g_branch(p, n, skip, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      int *wait = malloc(sizeof(int) * nb);
      for (int b = 0; b < nb; b++) wait[b] = (int)(rnd() % 4);
      g_spin(p, n, wait, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      free(wait);
      g_bound_written(p, n, nb); dump(p, sizeof(real3) * (2 * slots + 1));
      dump(n, sizeof(int) * nb);
      free(skip);
      free(p);
    }
  // Every box empty: the loops never dereference s->atoms, so the overlap
  // check must not either. No boxes at all: nothing is dereferenced.
  {
    int *z = calloc(nb, sizeof(int));
    struct boxes bx = {nb, z};
    struct sim s = {NULL, &bx, 0.0};
    b_velocity(&s, nb, 1.0);
    b_scale(&s, 2.0);
    b_params(NULL, NULL, z, nb, 1.0);
    b_label(NULL, z, nb);
    struct boxes none = {0, NULL};
    s.boxes = &none;
    b_scale(&s, 2.0);
    b_velocity(&s, 0, 1.0);
    b_params(NULL, NULL, NULL, 0, 1.0);
    b_field_bound(NULL, &none);
    free(z);
  }
  {
    double *a = arr(slots), *c = arr(slots);
    b_flat(a, c, nb); dump(a, sizeof(double) * slots);
    double *d = arr(3 * slots);
    b_stride(d, (int)slots); dump(d, sizeof(double) * 3 * slots);
    struct boxes bx = {(int)slots, NULL};
    b_field_bound(a, &bx); dump(a, sizeof(double) * slots);
    int *cnt = malloc(sizeof(int) * slots);
    g_field_written(a, cnt, &bx); dump(a, sizeof(double) * slots);
    g_counter_written(a, (int)slots / 2); dump(a, sizeof(double) * slots);
    g_start_varies(a, nb); dump(a, sizeof(double) * slots);
    // The review's false SAFE: a wrapping unsigned char counter.
    int *w = calloc(100 * (size_t)nb + 400, sizeof(int));
    g_wrap(w, nb); dump(w, sizeof(int) * (100 * (size_t)nb + 400));
    free(w);
    free(cnt); free(d); free(c); free(a);
  }
  free(n);
  fclose(out);
  return 0;
}
