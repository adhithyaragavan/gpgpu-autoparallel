// Demo benchmark 2: SAXPY (y = a*x + y), the canonical BLAS Level-1 kernel.
// Large constant trip count, dense/coalesced array access, and the multiply-add
// itself lives in a helper function — the same "call inside the hot loop" shape
// as example.c, but at a scale (65536 elements) that's actually worth offloading.
// This is the headline GPU-profitable case.

#define N 65536

double axpy_elem(double a, double x, double y) {
  return a * x + y;
}

void saxpy(double a, double *x, double *y) {
  for (int i = 0; i < N; i++) {
    y[i] = axpy_elem(a, x[i], y[i]);
  }
}

int main() {
  static double x[N], y[N];
  for (int i = 0; i < N; i++) {
    x[i] = (double)i;
    y[i] = 1.0;
  }
  saxpy(2.0, x, y);
  return 0;
}
