// Second input of tests/crosstu.c: the definitions its loops call. Not a
// test of its own (lit skips Inputs/); linked into the runtime driver by
// scripts/check_parallel.sh (EXTRA=tests/Inputs/crosstu_other.c).
double XG[4096];
static double factor = 3.0;
static double hidden[8] = {1, 2, 3, 4, 5, 6, 7, 8};

double x_scale(double v) { return v * factor; }
void x_bump(double *p, int i) { p[i] += 1.0; }
void x_touch(int i) { XG[i] = 0.5 * i; }
double x_read(int i) { return XG[i]; }
static void x_leaf(double *p, int i, double v) { p[i] = v; }
void x_chain(double *p, int i, double v) { x_leaf(p, i, v + factor); }
double x_peek(int i) { return hidden[i % 8]; }
// Lets `hidden` out: a caller's pointer may point into it.
double *x_hidden(void) { return hidden; }
// A loop of its own: analysed in this file, never in tests/crosstu.c.
double x_sum4(const double *p) {
  double s = 0.0;
  for (int k = 0; k < 4; k++)
    s += p[k];
  return s;
}
double x_twice(double v) { return 2.0 * v; }
