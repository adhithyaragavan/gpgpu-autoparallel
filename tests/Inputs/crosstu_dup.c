// Third input of tests/crosstu.c (lit only): defines x_twice again, so the
// name is ambiguous across the inputs and never imported.
double x_twice(double v) { return v + v; }
