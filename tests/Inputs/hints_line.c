/* tests/hints.test: a #line directive must not hide the loop from
   check_hints.py (records name the real file and line). */
#line 1 "generated.c"
void r_line(long *a, long *b, int n) {
  for (int i = 0; i < n; i++) {
    long s = 0;
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}
