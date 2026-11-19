// Runtime check for tests/ranges.c (scripts/check_parallel.sh tests/ranges.c
// eval/runtime/ranges_main.c). Every loop whose overlap check rests on a
// widened range (triangular or sibling loops) runs with disjoint arguments
// (the check holds, the loop runs in parallel) and with overlapping ones at
// several offsets (the check must fail, or the output differs).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include SRC

static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static long rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (long)(rng % 100) - 30;
}
static void fill(long *p, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = rnd();
}
static FILE *out;
static void dump(const void *p, size_t bytes) { fwrite(p, 1, bytes, out); }

typedef void (*ab_fn)(long *, const long *, int);

int main(int argc, char **argv) {
  out = fopen(argv[1], "wb");
  ab_fn ab[] = {s_lower, s_upper, s_chain, s_offset};
  const int sizes[] = {300, 37, 2, 1, 0};
  for (int s = 0; s < 5; s++) {
    const int n = sizes[s];
    const size_t len = 2 * (size_t)n + 16;
    long *m = malloc(len * sizeof(long));
    // Offsets of b from a: overlapping both ways, equal, and disjoint.
    const long offs[] = {-3, -1, 0, 1, 3, n + 6};
    for (size_t f = 0; f < sizeof ab / sizeof ab[0]; f++)
      for (size_t o = 0; o < sizeof offs / sizeof offs[0]; o++) {
        fill(m, len);
        long *a = m + 4;
        ab[f](a, a + offs[o], n);
        dump(m, len * sizeof(long));
      }
    for (size_t o = 0; o < sizeof offs / sizeof offs[0]; o++) {
      fill(m, len);
      u_mismatch(m + 4, m + 4 + offs[o], n, n / 2);
      dump(m, len * sizeof(long));
    }
    {
      struct RS rs = {n / 3};
      for (size_t o = 0; o < sizeof offs / sizeof offs[0]; o++) {
        fill(m, len);
        u_field(m + 4, m + 4 + offs[o], n, (o & 1) ? &rs : 0);
        u_field_sib(m + 4, m + 4 + offs[o], n, (o & 1) ? 0 : &rs);
        dump(m, len * sizeof(long));
      }
    }
    fill(m, len);
    u_carried(m, n);
    dump(m, len * sizeof(long));
    free(m);

    // Rows of 16 and 64: c/x and p/v share one buffer at row offsets.
    const int nr = n < 16 ? n : 16, ns = n < 64 ? n : 64;
    static long buf[160][64];
    const int rows[] = {-2, 0, 1, 3, 40};
    for (size_t o = 0; o < sizeof rows / sizeof rows[0]; o++) {
      fill(&buf[0][0], sizeof buf / sizeof(long));
      long (*c)[16] = (long (*)[16])&buf[60][0];
      s_rows(nr, nr, c, c + rows[o]);
      dump(buf, sizeof buf);
      fill(&buf[0][0], sizeof buf / sizeof(long));
      long (*p)[64] = &buf[40];
      s_siblings(p, p + rows[o], &buf[150][0], ns);
      dump(buf, sizeof buf);
    }
    memset(T, 0, sizeof T);
    s_tri_write(ns);
    dump(T, sizeof T);
    fill(&buf[0][0], sizeof buf / sizeof(long));
    s_tri_ptr(&buf[10], ns);
    dump(buf, sizeof buf);
    {
      int idx[64];
      long o64[64];
      for (int k = 0; k < 64; k++) idx[k] = (int)((rnd() + 30) & 63);
      fill(g_ext, 64);
      s_extent(o64, idx);
      dump(o64, sizeof o64);
      s_extent(g_ext, idx); // out is g_ext itself
      dump(g_ext, sizeof g_ext);
    }
  }
  fclose(out);
  return 0;
}
