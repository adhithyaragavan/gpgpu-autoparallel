// Day 5-6 fixture for CallResolver. All three demo benchmarks (example.c/
// scale, saxpy.c/axpy_elem, small_update.c/clamp_unit) have a leaf callee --
// none of them exercises more than one call level, so none of them proves
// CallResolver finds anything LoopInfo::Callees didn't already have. This file
// exists purely to exercise the cases that do.

// --- Two-level chain ---------------------------------------------------
// helperB appears nowhere in the loop body and nowhere in LoopInfo::Callees --
// finding it is what proves one-hop resolution goes beyond Day 4.
static double helperB(double x) {
  return x * x;
}

static double helperA(double x) {
  return helperB(x) + 1.0;
}

void two_level_chain(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = helperA(data[i]);
  }
}

// --- Three-level chain ---------------------------------------------------
// Confirms Depth/MaxDepth actually track hop count rather than saturating
// at 1 hop past the loop body.
static double stepC(double x) {
  return x - 1.0;
}

static double stepB(double x) {
  return stepC(x) * 2.0;
}

static double stepA(double x) {
  return stepB(x) + 3.0;
}

void three_level_chain(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = stepA(data[i]);
  }
}

// --- Opaque callee ---------------------------------------------------
// external_transform is declared but never defined in this TU -- a stand-in
// for a real library call. Forward-declared with a prototype rather than
// pulled in via a system header, so this fixture does not depend on the bare
// ClangTool invocation resolving platform SDK include paths.
double external_transform(double x);

static double helperC(double x) {
  return external_transform(x);
}

void opaque_callee(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = helperC(data[i]);
  }
}

// --- Mutual recursion ---------------------------------------------------
// The case that hangs a naive walker. Forward declarations are required
// because ping and pong each call the other before either is fully declared.
double pong(double x);

double ping(double x) {
  if (x <= 0.0)
    return x;
  return pong(x - 1.0);
}

double pong(double x) {
  if (x <= 0.0)
    return x;
  return ping(x - 1.0);
}

void mutual_recursion(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = ping(data[i]);
  }
}
