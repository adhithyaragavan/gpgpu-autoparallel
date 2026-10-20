// tests/crosstu.test: -whole-program with several inputs. kern's only
// caller here passes disjoint objects; crosstu_wp_other.c's overlap.
void kern(double *a, const double *b, int n) {
  for (int i = 0; i < n; i++)
    a[i] = b[i] * 2.0;
}
void drive(double *x, double *y, int n) { kern(x, y, n); }
