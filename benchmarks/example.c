// Minimal Day 1 sanity benchmark: a loop that calls a helper function.
// This is exactly the shape naive, intraprocedural-only parallelizers miss —
// the loop's safety depends on knowing that `scale` has no side effects.

double scale(double x, double factor) {
  return x * factor;
}

void process(double *data, int n, double factor) {
  for (int i = 0; i < n; i++) {
    data[i] = scale(data[i], factor);
  }
}

int main() {
  double data[1000];
  for (int i = 0; i < 1000; i++) {
    data[i] = (double)i;
  }
  process(data, 1000, 2.0);
  return 0;
}
