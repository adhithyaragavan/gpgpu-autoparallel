// Demo benchmark 2: SAXPY (y = a*x + y), the canonical BLAS Level-1 kernel.
// Large constant trip count, dense/coalesced array access, and the multiply-add
// itself lives in a helper function — the same "call inside the hot loop" shape
// as example.c.
//
// NOT the headline GPU-profitable case, corrected after Days 11-13 actually
// ran the numbers (an earlier version of this comment claimed it was, before
// the cost model existed to check): SAXPY is textbook memory-bound — one
// multiply-add per 24 bytes moved (2 flops / 24 B = 0.08 flop/byte), far
// below any real PCIe link's break-even intensity (~2.85 flop/byte under
// Days 11-13's default machine model). At 65536 elements the tool correctly
// verifies this SAFE but CPU_PARALLEL, transfer-bound on the GPU path — see
// NOTES.md. That result is the actual headline: this benchmark is what
// proves the tool doesn't just offload everything it's allowed to.
// benchmarks/compute_heavy.c is the one kernel that does clear the bar.

#include <stdio.h>

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

  double sum = 0.0;
  for (int i = 0; i < N; i++)
    sum += y[i];
  printf("saxpy checksum %.12e\n", sum);
  return 0;
}
