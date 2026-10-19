/* tests/hints.test: under -std=c89 the restrict hint inserts __restrict. */
void r_c89(long *a, long *b, int n) {
  int i, j;
  for (i = 0; i < n; i++) {
    long s = 0;
    for (j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}
