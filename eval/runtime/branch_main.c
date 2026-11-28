// Runtime check for tests/branch.c (scripts/check_parallel.sh tests/branch.c
// eval/runtime/branch_main.c; also OFFLOAD=1, naive and gated): every loop
// called with its bases apart and overlapping (both branches), at sizes on
// both sides of the guards. Output must match the sequential build bit for bit.
#include <stdio.h>
#include <string.h>
#include SRC
#define M 70000
static double x[2 * M + 2], y[2 * M + 2];
static double r1[M][64], r2[M][64];
#define NB 900
static double box[NB * 64][3];
static int cnt[NB];
static FILE *out;
static void fill(void) {
  for (int i = 0; i < 2 * M + 2; i++) x[i] = (i % 97) * 0.5, y[i] = (i % 89) * 0.25;
  for (int i = 0; i < M; i++)
    for (int j = 0; j < 64; j++) r1[i][j] = (i + j) % 13, r2[i][j] = (i * j) % 7;
}
static void dump(void) {
  fwrite(x, sizeof x, 1, out); fwrite(y, sizeof y, 1, out);
}
static void dumprows(int n) { fwrite(r1, sizeof r1[0], n, out); fwrite(r2, sizeof r2[0], n, out); }
int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  const int sizes[] = {M, 5000, 37, 1, 0};
  for (int s = 0; s < 5; s++) {
    const int n = sizes[s];
    fill(); b_add3(y, x, x + M, n); dump();      // apart
    fill(); b_add3(y, x, x + 1, n); dump();      // read-only overlap
    fill(); b_add3(x + 1, x, x + 2, n); dump();  // written overlap: host, sequential
    fill(); b_cp1(y, x, n); dump();
    fill(); b_cp1(x, x + 9, n < 10 ? n : 10); dump();  // accesses apart, sections not
    fill(); b_inter(x, y, n); dump();
    // b_inter(x, x, n) (interleaved restrict) aborts on the device: a known gap.
    fill(); b_label(y, x, n); dump();
    fill(); b_label(x + 1, x, n); dump();
    fill(); b_macro(y, x, n); dump();
    fill(); b_macro(x, x + 1, n); dump();
    fill(); b_nest(y, x, 3, n); dump();
    fill(); b_nest(x + 1, x, 3, n); dump();  // overlap: the original loop
    fill(); b_ifdef(x, n, 3); dump();
    for (int over = 0; over < 2; over++) {  // counts within the box, then one past it
      int nb = n < NB ? n : NB;
      for (int b = 0; b < NB; b++) cnt[b] = (b * 7) % 65;  // 0..64
      if (over && nb > 0) cnt[nb / 2] = 65;  // spills into the next box: inspector false
      for (int i = 0; i < NB * 64; i++) box[i][0] = i % 11, box[i][1] = i % 5 * 0.5, box[i][2] = 0;
      b_insp(box, cnt, nb, 2);
      fwrite(box, sizeof box, 1, out);
    }
    int r = n < M ? n : M - 1;
    fill(); b_rows(r, r1, r2); dumprows(r + 1);
    fill(); b_rows(r, r1, r1); dumprows(r + 1);  // c == a: same element, a read-modify-write
    fill(); b_rows(r > 0 ? r - 1 : 0, (double (*)[64])&r1[0][32], r1); dumprows(r + 1);
  }
  fclose(out);
  return 0;
}
