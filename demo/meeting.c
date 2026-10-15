// Five loops for a 10-minute walkthrough. Run:
//   build/p05tool demo/meeting.c --   (report)   then look at demo/meeting.omp.c
#include <stddef.h>

static void scale_row(double *row, int n, double s) {
  for (int j = 0; j < n; j++)
    row[j] *= s;
}

// 1. Loop calls a helper. The helper's accesses are substituted per call:
//    row i touches a[i][0..n-1] only, so rows are independent -> SAFE.
void scale_matrix(double *a, int n, double s) {
  for (int i = 0; i < n; i++)
    scale_row(&a[i * n], n, s);
}

static void accumulate(double *a, int i) {
  a[i] += a[i - 1];              // reads what the previous iteration wrote
}

// 2. Same idea, but the helper reads a[i-1], which iteration i-1 writes:
//    a real cross-iteration dependence, found through the call -> UNSAFE.
void prefix_sum(double *a, int n) {
  for (int i = 1; i < n; i++)
    accumulate(a, i);
}

// 3. Two pointer parameters: a caller could pass y == x + 1. Can't be proven
//    either way at compile time -> parallel only if a run-time overlap check
//    passes (emitted into the pragma's if()).
void shift_copy(double *y, const double *x, int n) {
  for (int i = 0; i < n; i++)
    y[i] = 2.0 * x[i + 1];
}

// 4. Same loop with restrict: the programmer promises no overlap -> SAFE, no check.
void shift_copy_r(double *restrict y, const double *restrict x, int n) {
  for (int i = 0; i < n; i++)
    y[i] = 2.0 * x[i + 1];
}

// 5. Safe but tiny: the cost model says threads would cost more than they save.
void small(double *restrict y, const double *restrict x) {
  for (int i = 0; i < 8; i++)
    y[i] = x[i] + 1.0;
}

