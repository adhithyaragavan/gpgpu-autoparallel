// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 50 loop(s).
// Phase 1.3 fixture for the dependence tests (DependenceTest.h). Each loop's
// comment states the expected verdict and which test decides it. Both
// directions: loops that must be proven parallel, loops with a real carried
// dependence (UNSAFE), and loops the tests can't decide (UNKNOWN).
//
// The functions writing value-dependent results are also RUN by
// scripts/check_parallel.sh: built once sequentially and once with every
// emitted pragma forced on (guards removed), and the outputs compared bit for
// bit. A wrong SAFE verdict shows up there as a mismatch.

// ===================================================== SAFE (independent) ==

// Outer i: SAFE (rows distinct, delinearized [i][j] vs [j][i] on different
// arrays; only b is written). Inner j: SAFE.
void transpose(double *b, const double *a, int n) {
  // CHECK: dependence.c:[[#LOOP0:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W b[j * n + i] : [j][i] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * ((0) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * ((0) * (__INTPTR_TYPE__)n + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: dependence.c:[[#LOOP1:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[j * n + i] : [j][i] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(i) + 1) <= (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * ((0) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(i))){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      b[j * n + i] = a[i * n + j];
}

// Outer i: SAFE. The scalar acc is declared inside the outer body, so it is
// private per i. Inner j: UNSAFE (acc += ... is a reduction, Phase 1.4).
void matvec(double *y, const double *m, const double *x, int n) {
  // CHECK: dependence.c:[[#LOOP2:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R m[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R x[j] : [j]{{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'm' and 'y' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'x' and 'y' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)m + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)y + 0 || (__INTPTR_TYPE__)y + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)m + 8 * ((0) * (__INTPTR_TYPE__)n + 0)) && ((__INTPTR_TYPE__)x + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)y + 0 || (__INTPTR_TYPE__)y + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)x + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    double acc = 0.0;
    // CHECK: dependence.c:[[#LOOP3:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R m[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R x[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - floating-point reduction on 'acc' (changes the last bits of the result; enable with -fp-reductions){{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      acc += m[i * n + j] * x[j];
    y[i] = acc;
  }
}

// SAFE by Banerjee with symbolic bounds: a[i] (i < n) never reaches a[i + n].
void halves(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP4:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + n] : [i + n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 Banerjee){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = a[i + n] * 2.0;
}

// SAFE by GCD: a[2i] is even, a[2i + 1] is odd.
void even_odd(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP5:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[2 * i] : [2*i]{{$}}
  // CHECK-NEXT: R a[2 * i + 1] : [2*i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 GCD, 1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[2 * i] = a[2 * i + 1] + 1.0;
}

// SAFE by the step: i is always even, so a[i] and a[i + 1] never meet.
void stride_two(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP6:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i += 2)
    a[i] = a[i + 1] - 1.0;
}

// SAFE by SIV: the distance (10) is not smaller than the iteration span (10).
void distance_beyond_span(double *a) {
  // CHECK: dependence.c:[[#LOOP7:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 10{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + 10] : [i + 10]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 10; i++)
    a[i] = a[i + 10] + 3.0;
}

// Outer i: SAFE (rows). Inner j: UNSAFE (a[i][j] reads a[i][j - 1], distance 1).
void row_prefix(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP8:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j - 1] : [i][j - 1] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: dependence.c:[[#LOOP9:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP8]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j - 1] : [i][j - 1] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W a[i * n + j] and R a[i * n + j - 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 1; j < n; j++)
      a[i * n + j] = a[i * n + j - 1] + a[i * n + j];
}

// Outer i: UNSAFE (reads row i - 1, distance 1). Inner j: SAFE (within one
// row-pair, j only meets j).
void column_prefix(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP10:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[(i - 1) * n + j] : [i - 1][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i * n + j] and R a[(i - 1) * n + j]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i++)
    // CHECK: dependence.c:[[#LOOP11:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[(i - 1) * n + j] : [i - 1][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 3 pair(s), 3 independent (1 ZIV, 2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = a[(i - 1) * n + j] + a[i * n + j];
}

// Three dimensions, all loops SAFE.
void scale3(double *c, const double *a, int n) {
  // CHECK: dependence.c:[[#LOOP12:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W c[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
  // CHECK-NEXT: R a[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * (((0) * (__INTPTR_TYPE__)n + 0) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)c + 8 * (((0) * (__INTPTR_TYPE__)n + 0) * (__INTPTR_TYPE__)n + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: dependence.c:[[#LOOP13:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W c[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
    // CHECK-NEXT: R a[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0) * (__INTPTR_TYPE__)n + 0)){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      // CHECK: dependence.c:[[#LOOP14:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP13]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j, k{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W c[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
      // CHECK-NEXT: R a[(i * n + j) * n + k] : [i][j][k] delinearized, dims [?][n][n]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
      // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(j)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(j)) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(j)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(j)) * (__INTPTR_TYPE__)n + 0)){{$}}
      // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
      for (int k = 0; k < n; k++)
        c[(i * n + j) * n + k] = 2.0 * a[(i * n + j) * n + k] + i - k;
}

// Static 2-D array, read and written in place: both loops SAFE.
double grid2[64][64];
void scale_grid(void) {
  // CHECK: dependence.c:[[#LOOP15:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 64{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W grid2[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R grid2[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 64; i++)
    // CHECK: dependence.c:[[#LOOP16:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP15]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W grid2[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R grid2[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 64; j++)
      grid2[i][j] = grid2[i][j] * 0.5 + j;
}

// =================================================== UNSAFE (dependent) ====

// UNSAFE: a[i] reads a[i - 1], written one iteration earlier.
void prefix_sum(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP17:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i] and R a[i - 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i++)
    a[i] = a[i - 1] + a[i];
}

// UNSAFE: every iteration writes acc[0].
void accumulate_into_array(double *acc, const double *a, int n) {
  // CHECK: dependence.c:[[#LOOP18:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: RW acc[0] : [0]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - RW acc[0]: every iteration touches the same element{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'acc' and 'a' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    acc[0] += a[i];
}

// Outer i: UNKNOWN. a[i][j] vs a[i - 1][j + 1] is a distance-1 dependence
// whenever the inner loop has room for the +1 shift (n >= 3), which can't be
// proven for a symbolic n, so it is reported as not disproved rather than
// proven. Inner j: SAFE.
void wavefront(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP19:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[(i - 1) * n + j + 1] : [i - 1][j + 1] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W a[i * n + j] and R a[(i - 1) * n + j + 1] independent{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i++)
    // CHECK: dependence.c:[[#LOOP20:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[(i - 1) * n + j + 1] : [i - 1][j + 1] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (1 ZIV, 1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n - 1; j++)
      a[i * n + j] = a[(i - 1) * n + j + 1] + 1.0;
}

// UNSAFE: every outer iteration writes the same a[j] (inner loop's index).
// Inner j: SAFE.
void same_row_each_time(double *a, const double *b, int n) {
  // CHECK: dependence.c:[[#LOOP21:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[j] : [j]{{$}}
  // CHECK-NEXT: R b[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[j]: every iteration touches the same element{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: dependence.c:[[#LOOP22:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP21]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[j] : [j]{{$}}
    // CHECK-NEXT: R b[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      a[j] = b[i * n + j];
}

// ====================================================== UNKNOWN ============

// UNKNOWN: with i <= n, a[i] and a[i + n] can meet (i = n, i' = 0), but the
// distance is the symbol n, so no exact distance is known.
void halves_inclusive(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP23:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + n] : [i + n]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W a[i] and R a[i + n] independent{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i <= n; i++)
    a[i] = a[i + n] * 2.0;
}

// UNKNOWN: the offset k could be 0, positive or negative.
void unknown_offset(double *a, int n, int k) {
  // CHECK: dependence.c:[[#LOOP24:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + k] : [i + k]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W a[i] and R a[i + k] independent{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = a[i + k] + 1.0;
}

// UNKNOWN: an indirect write a[idx[i]] can't be compared.
void scatter(double *a, const int *idx, int n) {
  // CHECK: dependence.c:[[#LOOP25:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[idx[i]]: W a[idx[i]] is non-affine (subscript reads memory ('idx[i]')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[idx[i]] = 1.0;
}

// ============ Regressions from the Phase 1.3 adversarial review ===============
// Each was verdicted SAFE by the first Phase 1.3 build and produced different
// results when run in parallel (scripts/check_parallel.sh). All must now be
// UNKNOWN or UNSAFE.

// [D1] A sibling inner loop's "it runs" fact must not be used for accesses
// outside it: with m == 0, every iteration reads a[-m] = a[0], which
// iteration 0 writes. Outer loop: UNKNOWN.
void d1_sibling_fact(double *a, const double *c, int n, int m) {
  // CHECK: dependence.c:[[#LOOP26:@LINE+14]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R c[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[-m] : [-m]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W a[i] and R a[-m] independent{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = n - 1; i >= 0; i--) {
    double s = 0;
    // CHECK: dependence.c:[[#LOOP27:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP26]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R c[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < m; j++)
      s += c[j];
    a[i] = a[-m] + s + 1.0;
  }
}

// [D2] Rows of different lengths n and m over one array are different
// shapes: [i][j] (row n) and [i][k] (row m) can overlap. Outer loop: UNKNOWN.
void d2_two_row_lengths(double *a, int n, int m) {
  // CHECK: dependence.c:[[#LOOP28:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: W a[i * m + k] : [i][k] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: R a[i * m + k] : [i][k] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * n + j] and W a[i * m + k]: they index 'a' with different shapes{{$}}
  // CHECK: deps       : 7 pair(s), 4 independent (4 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    // CHECK: dependence.c:[[#LOOP29:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = a[i * n + j] + 1.0;
    // CHECK: dependence.c:[[#LOOP30:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i * m + k] : [i][k] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: R a[i * m + k] : [i][k] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
    for (int k = 0; k < m; k++)
      a[i * m + k] = a[i * m + k] * 2.0 + 1.0;
  }
}

// [D3] A `static` array in the body is ONE array shared by all iterations,
// not private. Outer loop: UNSAFE.
void d3_static_scratch(const double *a, double *b, int n) {
  // CHECK: dependence.c:[[#LOOP31:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W t[k] : [k]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R t[63 - k] : [-k + 63]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W t[k]: every iteration touches the same element{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    static double t[64];
    // CHECK: dependence.c:[[#LOOP32:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP31]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W t[k] : [k]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int k = 0; k < 64; k++)
      t[k] = a[i] + k;
    double s = 0;
    // CHECK: dependence.c:[[#LOOP33:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP31]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R t[63 - k] : [-k + 63]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int k = 0; k < 64; k++)
      s = s * 0.5 + t[63 - k];
    b[i] = s;
  }
}

// [D4] q is a (never reassigned) alias of a: q[i + 1] is a[i + 1], read
// back as a[i] one iteration later. Resolved through the local pointer:
// UNSAFE, distance 1.
void d4_local_alias(double *a, int n) {
  double *q = a;
  // CHECK: dependence.c:[[#LOOP34:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W q[i + 1] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W q[i + 1] and R a[i]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    q[i + 1] = a[i] + 1.0;
}

// [D4'] A local pointer this analysis can't resolve (chosen at run time) may
// point anywhere: UNKNOWN.
void d4_unresolved_alias(double *a, double *b, int n, int pick) {
  double *q = pick ? a : b;
  // CHECK: dependence.c:[[#LOOP35:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W q[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'q' is a pointer that may point into 'a' (W q[i + 1], R a[i]){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    q[i + 1] = a[i] + 1.0;
}

// [D5] The callee writes through a pointer derived from its parameter
// (p[1] via q): iteration i writes a[i + 1] and reads a[i]. UNKNOWN before
// Phase 1.5; the call-site summary proves the distance-1 dependence: UNSAFE.
static void d5_bump(double *p) {
  double *q = p + 1;
  *q = *p + 1.0;
}
void d5_callee_derived_write(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP36:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: d5_bump reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W d5_bump(): *q : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: R d5_bump(): *p : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W d5_bump(): *q and R d5_bump(): *p: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    d5_bump(&a[i]);
}

// [D6] The callee reads the global the loop writes: g[i] = g[i + 1].
// UNKNOWN before Phase 1.5; now proven: UNSAFE.
double d6_g[1024];
static double d6_peek(int i) { return d6_g[i + 1]; }
void d6_callee_reads_global(int n) {
  // CHECK: dependence.c:[[#LOOP37:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: d6_peek reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W d6_g[i] : [i]{{$}}
  // CHECK-NEXT: R d6_peek(): d6_g[i + 1] : [i + 1] of 'd6_g'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W d6_g[i] and R d6_peek(): d6_g[i + 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    d6_g[i] = d6_peek(i) + 1.0;
}

// [D7] The callee reads through a pointer argument into the array the loop
// writes: b[i] = a[i + 1] in disguise while a[i] is written. UNKNOWN before
// Phase 1.5; now proven: UNSAFE.
static double d7_next(const double *p, int i) { return p[i + 1]; }
void d7_callee_reads_arg(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP38:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: d7_next reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R d7_next(): p[i + 1] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i] and R d7_next(): p[i + 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = d7_next(a, i) * 0.5;
}

// [L1] At most one iteration: nothing can be carried. SAFE.
void l1_single_iteration(double *a) {
  // CHECK: dependence.c:[[#LOOP39:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 1{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[0] : [0]{{$}}
  // CHECK-NEXT: R a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : at most one iteration: nothing can be carried{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 2; i < 5; i += 3)
    a[0] = a[0] + 1.0;
}

// ============ Regressions from the second Phase 1.3 review ====================
// Each was SAFE and produced different results in parallel. Now UNKNOWN.

// [E1] A local alias handed to a callee that reads through it: rd reads
// a[i + 1] while the loop writes a[i]. UNKNOWN before Phase 1.5; with q
// resolved to a, now proven: UNSAFE.
static double e_rd(const double *p, int i) { return p[i + 1]; }
void e1_alias_into_callee(double *a, int n) {
  double *q = a;
  // CHECK: dependence.c:[[#LOOP40:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: e_rd reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R e_rd(): p[i + 1] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i] and R e_rd(): p[i + 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = e_rd(q, i) * 0.5;
}

// [E2] The pointer travels inside a struct passed by value. UNKNOWN.
struct e_sp { double *p; };
static double e_rds(struct e_sp s, int i) { return s.p[i + 1]; }
void e2_pointer_in_struct(double *a, int n) {
  struct e_sp s = {a};
  // CHECK: dependence.c:[[#LOOP41:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: e_rds reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R e_rds(): s.p[i + 1] : non-affine: base is a pointer loaded from memory ('s.p'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - R e_rds(): s.p[i + 1] reads memory this analysis cannot name (base is a pointer loaded from memory ('s.p')), which a write in the loop could alias{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = e_rds(s, i) * 0.5;
}

// [E2'] The pointer travels as an integer. UNKNOWN.
static double e_rdl(long x, int i) { return ((double *)x)[i + 1]; }
void e2_pointer_as_long(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP42:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: e_rdl reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R e_rdl(): ((double *)x)[i + 1] : non-affine: base is not a named array or pointer ('(double *)x'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - R e_rdl(): ((double *)x)[i + 1] reads memory this analysis cannot name (base is not a named array or pointer ('(double *)x')), which a write in the loop could alias{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = e_rdl((long)a, i) * 0.5;
}

// [E3] The callee writes through a local struct holding a global's address.
// UNKNOWN.
double e_g[1024];
struct e_spg { double *p; };
static void e_wr_struct(int i) {
  struct e_spg s = {e_g};
  s.p[i + 1] = s.p[i] + 1.0;
}
void e3_callee_writes_via_struct(int n) {
  // CHECK: dependence.c:[[#LOOP43:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: e_wr_struct reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W e_wr_struct(): s.p[i + 1] : non-affine: base is a pointer loaded from memory ('s.p'){{$}}
  // CHECK-NEXT: R e_wr_struct(): s.p[i] : non-affine: base is a pointer loaded from memory ('s.p'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W e_wr_struct(): s.p[i + 1] writes (base is a pointer loaded from memory ('s.p')){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    e_wr_struct(i);
}

// [E4] The callee calls through a function pointer. UNKNOWN.
static void e_wr(double *p, int x) { p[x + 1] = p[x] + 1.0; }
static void e_call_fp(void (*f)(double *, int), double *a, int i) { f(a, i); }
void e4_indirect_in_callee(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP44:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: e_call_fp reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 0 (0 affine){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'e_call_fp' calls through a function pointer{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    e_call_fp(e_wr, a, i);
}

// [E5] An atomic builtin writes a[i + 1]. UNKNOWN.
void e5_atomic(int *a, int n) {
  // CHECK: dependence.c:[[#LOOP45:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: & a[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body uses an atomic builtin{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    __atomic_store_n(&a[i + 1], a[i] + 1, __ATOMIC_RELAXED);
}

// [E6] A parameter reassigned in the function no longer keeps the no-alias
// assumption: b IS a. UNKNOWN.
void e6_reassigned_param(double *a, double *b, int n) {
  b = a;
  // CHECK: dependence.c:[[#LOOP46:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' is a pointer that may point into 'a' (W a[i], R b[i + 1]){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = b[i + 1] + 1.0;
}

// [E6'] A global pointer may point anywhere: gp aliases a here. UNKNOWN.
double *e_gp;
void e6_global_pointer(double *a, int n) {
  e_gp = a;
  // CHECK: dependence.c:[[#LOOP47:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R e_gp[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'e_gp' is a pointer that may point into 'a' (W a[i], R e_gp[i + 1]){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'e_gp' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = e_gp[i + 1] + 1.0;
}

// [E8] "At most one iteration" must not be trusted when the counter is
// modified inside: this runs until a[5] reaches 50. UNKNOWN.
void e8_single_iteration_lie(double *a) {
  // CHECK: dependence.c:[[#LOOP48:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 1{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (0 affine){{$}}
  // CHECK-NEXT: RW a[i + 5] : non-affine: induction variable 'i' has its address taken inside the loop{{$}}
  // CHECK-NEXT: R a[5] : non-affine: induction variable 'i' has its address taken inside the loop, so the loop's iteration space is unknown{{$}}
  // CHECK-NEXT: RW *pi : non-affine: base pointer 'pi' is set inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what RW *pi writes (base pointer 'pi' is set inside the loop){{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i <= 0; i++) {
    int *pi = &i;
    a[i + 5] += 1.0;
    if (a[5] < 50.0)
      *pi -= 1;
  }
}

// [E9] An early exit: OpenMP can't parallelize it (the pragma wouldn't even
// compile). UNKNOWN.
void e9_break(double *a, int n) {
  // CHECK: dependence.c:[[#LOOP49:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body can 'break' out of the loop{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    if (a[i] < 0)
      break;
    a[i] = 1.0;
  }
}
