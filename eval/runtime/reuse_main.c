// Runtime check for tests/reuse.c (scripts/check_parallel.sh tests/reuse.c
// eval/runtime/reuse_main.c). Every SAFE loop runs as `parallel for` with
// its private/lastprivate clauses; every array and `last` must match the
// sequential build exactly. The twins run too, sequentially.
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
    t[i] = rnd();
    for (int j = 0; j < N; j++) A[i][j] = rnd(), B[i][j] = rnd(), C[i][j] = rnd();
  }
}
static FILE *out;
static void dump(void) {
  fwrite(A, sizeof A, 1, out); fwrite(B, sizeof B, 1, out);
  fwrite(C, sizeof C, 1, out); fwrite(t, sizeof t, 1, out);
  fwrite(&last, sizeof last, 1, out);
}

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int sizes[] = {N, 37, 2, 1, 0};
  for (int s = 0; s < 5; s++) {
    const int n = sizes[s];
    fill(); s_gemm(n, 1.5, 0.5); dump();
    fill(); s_between(n); dump();
    fill(); s_updown(n); dump();
    fill(); last = -1; s_live_after(n); dump();
    fill(); s_nested(n); dump();
    fill(); u_nested(n); dump();
    fill(); u_noinit(n); dump();
    fill(); u_selfinit(n); dump();
    fill(); u_label(n, 0); dump();
    fill(); u_label(n, 1); dump();
    fill(); u_addr(n); dump();
    fill(); u_bodywrite(n); dump();
    fill(); u_carried(n); dump();
    fill(); u_read_first(n); dump();
    for (int sel = 0; sel < 2; sel++) {
      for (int k = 0; k < N * 10 + 20; k++) V[k] = -1.0;
      u_switch(n, sel); fwrite(V, sizeof V, 1, out);
    }
  }
  fclose(out);
  return 0;
}
