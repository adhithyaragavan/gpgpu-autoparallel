// Loops that SHOULD be recognized. Every loop in this file must classify as
// CONSTANT_BOUND or VARIABLE_BOUND — a single UNRECOGNIZED here is a bug.
//
// Expected classification is written above each loop. See
// benchmarks/loop_unrecognized.c for the matching negative cases; the pair is
// what proves the classifier discriminates rather than just saying yes.

double helper(double x) { return x * 2.0; }

// CONSTANT_BOUND, trip 1000
void literal_bound(double *a) {
  for (int i = 0; i < 1000; i++) {
    a[i] = (double)i;
  }
}

// CONSTANT_BOUND, trip 100 — exercises the <= off-by-one
void inclusive_bound(double *a) {
  for (int i = 0; i <= 99; i++) {
    a[i] = 0.0;
  }
}

// CONSTANT_BOUND, trip 256 — step != 1, exercises ceiling division
void strided(double *a) {
  for (int i = 0; i < 1024; i += 4) {
    a[i] = 1.0;
  }
}

// CONSTANT_BOUND, trip 10 — counts down
void downward(double *a) {
  for (int i = 10; i > 0; i--) {
    a[i] = 0.0;
  }
}

// CONSTANT_BOUND, trip 512 — the case a literal-only check gets wrong.
// `N` is not an integer literal, but its value is known at compile time.
void const_variable_bound(double *a) {
  const int N = 512;
  for (int i = 0; i < N; i++) {
    a[i] = 0.0;
  }
}

// CONSTANT_BOUND, trip 64 — bound is a folded constant expression
void folded_expression_bound(double *a) {
  for (int i = 0; i < 8 * 8; i++) {
    a[i] = 0.0;
  }
}

// VARIABLE_BOUND, bound `n` (parameter). Also exercises callee recording:
// `helper` must show up in the body callee list.
void param_bound(double *a, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = helper(a[i]);
  }
}

// VARIABLE_BOUND, bound text `n - 1`, no bound decl
void expression_bound(double *a, int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] = a[i + 1];
  }
}

// VARIABLE_BOUND, bound `len` (local, not a parameter)
void local_bound(double *a, int n) {
  int len = n / 2;
  for (int i = 0; i < len; i++) {
    a[i] = 0.0;
  }
}

// VARIABLE_BOUND, bound `n` — induction variable declared outside the loop,
// so the init clause is an assignment rather than a declaration.
void external_induction_var(double *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    a[i] = 0.0;
  }
}
