// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 43 loop(s).
// Phase 1.1 fixture for the loop-nest model (LoopInfo.h, "Nest facts").
// Every loop states its expected nest facts in the comment above it, in both
// directions: perfect vs imperfect, rectangular vs triangular, child vs
// not-a-child. Safety/profitability verdicts are pinned too, but 1.1 must not
// change them: every 2D access below is still UNSAFE until Phase 1.3.

// --- Perfect two-level nest -------------------------------------------------
void perfect2(double *a, int n, int m) {
  // depth 0, 1 child, perfect nest (body is exactly the inner for). ivs: i
  // CHECK: nests.c:[[#LOOP0:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * m + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // depth 1, parent = outer, 0 children, not perfect, rectangular. ivs: i, j
    // CHECK: nests.c:[[#LOOP1:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * m + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < m; j++)
      a[i * m + j] = 0.0;
}

// --- Perfect three-level nest, braced bodies --------------------------------
void perfect3(double *a, int n) {
  // depth 0, 1 child, perfect (a compound body whose only statement is a for)
  // CHECK: nests.c:[[#LOOP2:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[(i * n + j) * n + k]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    // depth 1, 1 child, perfect. ivs: i, j
    // CHECK: nests.c:[[#LOOP3:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[(i * n + j) * n + k]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++) {
      // depth 2, parent = the j loop, 0 children. ivs: i, j, k
      // CHECK: nests.c:[[#LOOP4:@LINE+8]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP3]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j, k{{$}}
      // CHECK: safety     : UNSAFE{{$}}
      // CHECK: - array access not indexed by the induction variable alone: 'a[(i * n + j) * n + k]'{{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int k = 0; k < n; k++) {
        a[(i * n + j) * n + k] = 1.0;
      }
    }
  }
}

// --- Imperfect nest ---------------------------------------------------------
void imperfect(double *a, double *rowsum, int n) {
  // depth 0, 1 child, NOT perfect: a statement sits beside the inner loop
  // CHECK: nests.c:[[#LOOP5:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    rowsum[i] = 0.0;
    // depth 1, parent = outer. ivs: i, j
    // CHECK: nests.c:[[#LOOP6:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP5]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 2.0;
  }
}

// NOT perfect: an empty statement beside the inner loop is still "something
// else" in the body.
void imperfect_null_stmt(double *a, int n) {
  // depth 0, 1 child, NOT perfect
  // CHECK: nests.c:[[#LOOP7:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    ;
    // depth 1, parent = outer
    // CHECK: nests.c:[[#LOOP8:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 3.0;
  }
}

// --- Triangular nests -------------------------------------------------------
void triangular_bound(double *a, int n) {
  // depth 0, 1 child, perfect, rectangular itself
  // CHECK: nests.c:[[#LOOP9:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // depth 1, TRIANGULAR: the bound `i` is the outer induction variable
    // CHECK: nests.c:[[#LOOP10:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < i; j++)
      a[i * n + j] = 4.0;
}

void triangular_start(double *a, int n) {
  // depth 0, 1 child, perfect
  // CHECK: nests.c:[[#LOOP11:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // depth 1, TRIANGULAR: the start `i + 1` uses the outer induction variable
    // CHECK: nests.c:[[#LOOP12:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP11]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = i + 1; j < n; j++)
      a[i * n + j] = 5.0;
}

// --- Sibling inner loops ----------------------------------------------------
void siblings(double *a, double *b, int n) {
  // depth 0, 2 children, NOT perfect (two statements in the body)
  // CHECK: nests.c:[[#LOOP13:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    // depth 1, parent = outer
    // CHECK: nests.c:[[#LOOP14:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 6.0;
    // depth 1, parent = outer (a sibling, not a child of the j loop)
    // CHECK: nests.c:[[#LOOP15:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'b[i * n + k]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int k = 0; k < n; k++)
      b[i * n + k] = 7.0;
  }
}

// --- Inside an unrecognized loop --------------------------------------------
void inside_unrecognized(double *a, int n) {
  // UNRECOGNIZED (non-affine increment), depth 0, 1 child, perfect nest (the
  // body is a single for: a shape fact, true even for an unrecognized loop).
  // ivs: <none>
  // CHECK: nests.c:[[#LOOP16:@LINE+8]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : increment is not affine (only += and -= are handled){{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (increment is not affine (only += and -= are handled)); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i *= 2)
    // depth 1, INSIDE AN UNRECOGNIZED LOOP. ivs: j only (i is not trusted).
    // Its bound uses i, but i is not a recognized induction variable, so it is
    // NOT flagged triangular; the "inside an unrecognized loop" flag is what
    // keeps later phases conservative here.
    // CHECK: nests.c:[[#LOOP17:@LINE+7]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s), inside an unrecognized loop{{$}}
    // CHECK: nest ivs   : j{{$}}
    // CHECK: safety     : SAFE{{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j < i; j++)
      a[j] = 8.0;
}

// --- Inside while / do ------------------------------------------------------
void inside_while(double *a, int n, int steps) {
  while (steps-- > 0) {
    // depth 0 (no enclosing FOR), INSIDE WHILE/DO
    // CHECK: nests.c:[[#LOOP18:@LINE+7]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: safety     : SAFE{{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int i = 0; i < n; i++)
      a[i] = a[i] * 0.5;
  }
}

void inside_do(double *a, int n, int steps) {
  do {
    // depth 0, INSIDE WHILE/DO
    // CHECK: nests.c:[[#LOOP19:@LINE+7]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: safety     : SAFE{{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int i = 0; i < n; i++)
      a[i] = a[i] + 1.0;
  } while (--steps > 0);
}

void for_while_for(double *a, int n, int steps) {
  // depth 0, 1 child, NOT perfect (its body is a while, not a for), not inside while/do
  // CHECK: nests.c:[[#LOOP20:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int s = steps;
    while (s-- > 0) {
      // depth 1, parent = the i loop (the while in between is not a for),
      // INSIDE WHILE/DO. ivs: i, j
      // CHECK: nests.c:[[#LOOP21:@LINE+8]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP20]]:3, 0 child loop(s), inside while/do or a loop header{{$}}
      // CHECK: nest ivs   : i, j{{$}}
      // CHECK: safety     : UNSAFE{{$}}
      // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int j = 0; j < n; j++)
        a[i * n + j] = 9.0;
    }
  }
}

// --- A loop in another loop's init clause is not its child -------------------
void loop_in_init_clause(double *a, int n) {
  // depth 0, 2 children: the j loop, and the k loop (k sits in j's init
  // clause, which is still inside this loop's body). Perfect nest: the body's
  // only statement is the j loop.
  // CHECK: nests.c:[[#LOOP22:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : o{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[o * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int o = 0; o < n; o++) {
    // depth 1, parent = the o loop, 0 children: the k loop sits in this loop's
    // init clause, not its body, so it is NOT this loop's child.
    // CHECK: nests.c:[[#LOOP23:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP22]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : o, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[o * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = ({
           int s = 0;
           // depth 1, parent = the o loop (not the j loop). ivs: o, k
           // CHECK: nests.c:[[#LOOP24:@LINE+8]]:12{{$}}
           // CHECK: kind       : CONSTANT_BOUND{{$}}
           // CHECK: trip count : 4{{$}}
           // CHECK: nest       : depth 1, parent [[#LOOP22]]:3, 0 child loop(s){{$}}
           // CHECK: nest ivs   : o, k{{$}}
           // CHECK: safety     : UNSAFE{{$}}
           // CHECK: - loop-carried dependence: writes to 's', which is not declared inside the loop body{{$}}
           // CHECK: profitability : SEQUENTIAL{{$}}
           for (int k = 0; k < 4; k++)
             s += k;
           s;
         });
         j < n; j++)
      a[o * n + j] = 10.0;
  }
}

// --- A loop in a called function is not a child ------------------------------
static void callee_with_loop(double *row, int n) {
  // depth 0: lexically outside any loop, even though it runs inside one
  // CHECK: nests.c:[[#LOOP25:@LINE+7]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: safety     : SAFE{{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int j = 0; j < n; j++)
    row[j] = 11.0;
}

void caller(double *a, int n) {
  // depth 0, 0 children: the callee's loop is not lexically nested here
  // CHECK: nests.c:[[#LOOP26:@LINE+9]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: callee_with_loop reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - 'callee_with_loop' writes through pointer parameter 'row'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    callee_with_loop(&a[i * n], n);
}

// --- Added after the Phase 1.1 review: shapes the first fixture missed ------

// A loop in a for's CONDITION clause runs once per iteration of that loop.
void loop_in_cond_clause(double *a, int n) {
  // UNRECOGNIZED: the bound contains a statement expression, which can run
  // arbitrary code every iteration (same hazard as a call). Depth 0, 0 children
  // (the k loop is in this loop's header, not its body).
  // CHECK: nests.c:[[#LOOP27:@LINE+8]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : bound expression contains a statement expression{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (bound expression contains a statement expression); safety not evaluated{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int j = 0; j < ({
                    int s = 0;
                    // depth 0, INSIDE WHILE/DO OR A LOOP HEADER (re-run per j)
                    // CHECK: nests.c:[[#LOOP28:@LINE+8]]:21{{$}}
                    // CHECK: kind       : CONSTANT_BOUND{{$}}
                    // CHECK: trip count : 2{{$}}
                    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
                    // CHECK: nest ivs   : k{{$}}
                    // CHECK: safety     : UNSAFE{{$}}
                    // CHECK: - loop-carried dependence: writes to 's', which is not declared inside the loop body{{$}}
                    // CHECK: profitability : SEQUENTIAL{{$}}
                    for (int k = 0; k < 2; k++)
                      s += k;
                    s + n;
                  });
       j++)
    a[j] = 12.0;
}

// A loop in a while's condition is inside the while too.
void loop_in_while_cond(double *a, int n) {
  int t = 3;
  while (({
    int s = 0;
    // depth 0, INSIDE WHILE/DO OR A LOOP HEADER
    // CHECK: nests.c:[[#LOOP29:@LINE+8]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 2{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : k{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - loop-carried dependence: writes to 's', which is not declared inside the loop body{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int k = 0; k < 2; k++)
      s += k;
    s;
  }) < t--)
    a[0] = 13.0;
}

// Shadowing: the inner loop declares its own `i`. Its bound `i < n` refers to
// the inner i, not the outer one, so it is NOT triangular.
void shadowing(double *a, int n) {
  // depth 0, 1 child, perfect
  // CHECK: nests.c:[[#LOOP30:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // depth 1, NOT triangular. ivs: i, i (two different variables both named i)
    // CHECK: nests.c:[[#LOOP31:@LINE+7]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP30]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, i{{$}}
    // CHECK: safety     : SAFE{{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int i = 0; i < n; i++)
      a[i] = 14.0;
}

// Recognized -> unrecognized -> recognized.
void rec_unrec_rec(double *a, int n) {
  // depth 0, 1 child, perfect. ivs: i
  // CHECK: nests.c:[[#LOOP32:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[k]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // UNRECOGNIZED, depth 1, 1 child, perfect. ivs: i (its own j is not trusted)
    // CHECK: nests.c:[[#LOOP33:@LINE+8]]:5{{$}}
    // CHECK: kind       : UNRECOGNIZED{{$}}
    // CHECK: reason     : increment is not affine (only += and -= are handled){{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP32]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: safety     : UNKNOWN{{$}}
    // CHECK: - loop shape not recognized (increment is not affine (only += and -= are handled)); safety not evaluated{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 1; j < n; j *= 2)
      // depth 2, INSIDE AN UNRECOGNIZED LOOP, TRIANGULAR (bound uses the
      // trusted outer i). ivs: i, k
      // CHECK: nests.c:[[#LOOP34:@LINE+7]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP33]]:5, 0 child loop(s), triangular, inside an unrecognized loop{{$}}
      // CHECK: nest ivs   : i, k{{$}}
      // CHECK: safety     : SAFE{{$}}
      // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
      for (int k = 0; k < i; k++)
        a[k] = 15.0;
}

// `#pragma clang loop` wraps the inner loop in an AttributedStmt: still perfect.
void pragma_body(double *a, int n) {
  // depth 0, 1 child, PERFECT (looks through the pragma's attribute wrapper)
  // CHECK: nests.c:[[#LOOP35:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
#pragma clang loop unroll(disable)
    // depth 1, parent = outer
    // CHECK: nests.c:[[#LOOP36:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP35]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 16.0;
  }
}

// Doubled braces are still a single statement: perfect.
void double_braces(double *a, int n) {
  // depth 0, 1 child, PERFECT
  // CHECK: nests.c:[[#LOOP37:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    {
      // depth 1, parent = outer
      // CHECK: nests.c:[[#LOOP38:@LINE+8]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP37]]:3, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j{{$}}
      // CHECK: safety     : UNSAFE{{$}}
      // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int j = 0; j < n; j++)
        a[i * n + j] = 17.0;
    }
  }
}

// A label in front of the inner loop: a goto could enter it, so NOT perfect.
void labelled_inner(double *a, int n) {
  // depth 0, 1 child, NOT perfect (labelled inner loop)
  // CHECK: nests.c:[[#LOOP39:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
  inner:
    // depth 1, parent = outer
    // CHECK: nests.c:[[#LOOP40:@LINE+8]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP39]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'a[i * n + j]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 18.0;
  }
}

// A bound that mentions the outer i only inside sizeof is a constant: NOT
// triangular (the review's false positive).
void sizeof_bound(double (*m)[4], int n) {
  // depth 0, 1 child, perfect
  // CHECK: nests.c:[[#LOOP41:@LINE+8]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: safety     : UNSAFE{{$}}
  // CHECK: - array access not indexed by the induction variable alone: 'm[i][j]'{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CONSTANT_BOUND, trip 4, NOT triangular
    // CHECK: nests.c:[[#LOOP42:@LINE+8]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP41]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: safety     : UNSAFE{{$}}
    // CHECK: - array access not indexed by the induction variable alone: 'm[i]'{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < (int)(sizeof(m[i]) / sizeof(double)); j++)
      m[i][j] = 19.0;
}
