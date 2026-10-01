// CHECK: Analyzed 9 loop(s).
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
  // CHECK: loop_unrecognized.c:[[#LOOP0:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : init declares multiple induction variables{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (init declares multiple induction variables); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0, j = 0; i < n; i++, j++) {
    a[i] = (double)j;
  }
}

// UNRECOGNIZED: no init clause. `for (;;)` is missing all three clauses, and
// the classifier rejects on the first one it checks (init), so this loop
// covers the no-init path. The no-condition path is covered below.
void infinite(double *a) {
  // CHECK: loop_unrecognized.c:[[#LOOP1:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : no init clause{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (no init clause); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (;;) {
    a[0] = 0.0;
    break;
  }
}

// UNRECOGNIZED: no condition clause. It has an init and an increment, so
// rejection must come from the missing condition specifically.
void no_condition(double *a) {
  // CHECK: loop_unrecognized.c:[[#LOOP2:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : no condition clause{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (no condition clause); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; ; i++) {
    a[i] = 0.0;
    if (i == 7)
      break;
  }
}

// UNRECOGNIZED: condition is not a single relational comparison
void compound_condition(double *a, int n, int flag) {
  // CHECK: loop_unrecognized.c:[[#LOOP3:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : condition is not a simple relational comparison{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (condition is not a simple relational comparison); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n && flag; i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: non-affine increment
void geometric_step(double *a, int n) {
  // CHECK: loop_unrecognized.c:[[#LOOP4:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : increment is not affine (only += and -= are handled){{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (increment is not affine (only += and -= are handled)); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i *= 2) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: the bound is re-evaluated every iteration and may have side
// effects, so it is not a fixed value we can reason about
void call_in_bound(double *a, int n) {
  // CHECK: loop_unrecognized.c:[[#LOOP5:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : bound expression contains a function call{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (bound expression contains a function call); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < bound_of(n); i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: counts down while testing upward — not a counted loop
void direction_mismatch(double *a, int n) {
  // CHECK: loop_unrecognized.c:[[#LOOP6:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : loop counts down but the condition tests upward{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (loop counts down but the condition tests upward); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i--) {
    a[0] = 0.0;
    break;
  }
}

// UNRECOGNIZED: mirrored condition. This one is a known limitation rather than
// a genuinely bad loop — it is a perfectly good counted loop we choose not to
// handle yet, and flagging it is how we avoid silently mishandling it.
void mirrored_condition(double *a, int n) {
  // CHECK: loop_unrecognized.c:[[#LOOP7:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : mirrored condition (bound on the left) is not handled{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (mirrored condition (bound on the left) is not handled); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; n > i; i++) {
    a[i] = 0.0;
  }
}

// UNRECOGNIZED: increment updates a variable other than the induction variable
void wrong_increment_var(double *a, int n) {
  int k = 0;
  // CHECK: loop_unrecognized.c:[[#LOOP8:@LINE+10]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : increment updates a variable other than the induction variable{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (increment updates a variable other than the induction variable); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; k++) {
    a[0] = 0.0;
    break;
  }
}
