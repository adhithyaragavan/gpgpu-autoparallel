// Loops that should NOT be recognized as canonical counted loops. Every loop
// in this file must classify as UNRECOGNIZED, each with a distinct reason — a
// single CONSTANT_BOUND or VARIABLE_BOUND here means the classifier is waving
// through a shape it does not actually understand, which is worse than
// rejecting a loop we could have handled.
//
// Pairs with benchmarks/loop_shapes.c.

int bound_of(int n) { return n; }

// UNRECOGNIZED: two induction variables
void multiple_induction_vars(double *a, int n) {
  for (int i = 0, j = 0; i < n; i++, j++) {
    a[i] = (double)j;
  }
}

// UNRECOGNIZED: no condition clause
void infinite(double *a) {
  for (;;) {
    a[0] = 0.0;
    break;
  }
}

// UNRECOGNIZED: condition is not a single relational comparison
void compound_condition(double *a, int n, int flag) {
  for (int i = 0; i < n && flag; i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: non-affine increment
void geometric_step(double *a, int n) {
  for (int i = 1; i < n; i *= 2) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: the bound is re-evaluated every iteration and may have side
// effects, so it is not a fixed value we can reason about
void call_in_bound(double *a, int n) {
  for (int i = 0; i < bound_of(n); i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: counts down while testing upward — not a counted loop
void direction_mismatch(double *a, int n) {
  for (int i = 0; i < n; i--) {
    a[0] = 0.0;
    break;
  }
}

// UNRECOGNIZED: mirrored condition. This one is a known limitation rather than
// a genuinely bad loop — it is a perfectly good counted loop we choose not to
// handle yet, and flagging it is how we avoid silently mishandling it.
void mirrored_condition(double *a, int n) {
  for (int i = 0; n > i; i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: increment updates a variable other than the induction variable
void wrong_increment_var(double *a, int n) {
  int k = 0;
  for (int i = 0; i < n; k++) {
    a[0] = 0.0;
    break;
  }
}
