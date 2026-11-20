// Runtime check for tests/work.c (scripts/check_parallel.sh tests/work.c
// eval/runtime/work_main.c; also OFFLOAD=1, naive and gated): data regions
// whose loops' bounds change inside them, at sizes on both sides of the
// guards. Every array must match the sequential build bit for bit.
#include <stdio.h>
#include <stdlib.h>
#include SRC
static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static double rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (double)((long)(rng % 100) - 30) / 7.0;
}
static void fill(void) {
  for (int i = 0; i < N; i++) {
    x[i] = rnd(); y[i] = rnd();
    for (int j = 0; j < N; j++) A[i][j] = rnd(), B[i][j] = rnd();
  }
}
static FILE *out;
static void dump(void) {
  fwrite(A, sizeof A, 1, out); fwrite(B, sizeof B, 1, out);
  fwrite(x, sizeof x, 1, out); fwrite(y, sizeof y, 1, out);
}
int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int sizes[] = {512, 300, 37, 1, 0};
  for (int s = 0; s < 5; s++) {
    const int n = sizes[s];
    for (int s2 = 0; s2 < 5; s2++) {
      const int m = sizes[s2];
      fill(); r_time(n, m, 3); dump();
      fill(); r_mchange(n, m, 3); dump();
      fill(); gm = m > 0 ? m - 1 : 0; r_global(n); dump();
      fill(); r_run(n, m); dump();
    }
    fill(); r_tri(n); dump();
    fill(); r_window(n); dump();
    for (int r = 0; r < 4; r++) {
      fill(); r_stencil(n, r, x, y); dump();
    }
  }
  fclose(out);
  return 0;
}
