void kern(double *a, const double *b, int n);
void other(double *m, int n) { kern(m + 1, m, n); }
