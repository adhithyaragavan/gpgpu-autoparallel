// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 33 loop(s).
// Phase 2.5: bounds for loop ranges an overlap check (and a map() section)
// couldn't size before (boundingRange in DependenceTest.cpp).
// -tri-ranges: an inner range that mentions another loop's variable
// (`j <= i`, `j = i ..`) is bounded through that variable's range.
// -sibling-ranges: a counter of several sibling loops gets the span of
// their ranges when the ends differ by constants.
// Every SAFE case runs behind an overlap check; eval/runtime/ranges_main.c
// calls each with disjoint and overlapping arguments. Also RUN by
// scripts/check_parallel.sh.

// --- SAFE behind an overlap check -------------------------------------------

// syrk's shape: j <= i, so b is read in [0, n - 1].
void s_lower(long *a, const long *b, int n) {
  // CHECK: ranges.c:[[#LOOP0:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (2 * (double)(n)) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP1:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// covariance's shape: j starts at i, so b is read in [0, n - 1].
void s_upper(long *a, const long *b, int n) {
  // CHECK: ranges.c:[[#LOOP2:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (2 * (double)(n)) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP3:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = i; j < n; j++)
      s += b[j];
    a[i] = s;
  }
}

// Two levels: k starts at j, which runs to i: k in [0, n - 1].
void s_chain(long *a, const long *b, int n) {
  // CHECK: ranges.c:[[#LOOP4:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[k] : [k]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (1 * (double)(n) + 3 * (double)(n) * (double)(n)) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP5:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 1 child loop(s), perfect nest, triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[k] : [k]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
    // CHECK: profitability : CPU_PARALLEL if ((double)(i) * (3 * (double)(n)) >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: no map() section: can't map 'b': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j <= i; j++)
      // CHECK: ranges.c:[[#LOOP6:@LINE+11]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP5]]:5, 0 child loop(s), triangular{{$}}
      // CHECK: nest ivs   : i, j, k{{$}}
      // CHECK: accesses   : 1 (1 affine){{$}}
      // CHECK-NEXT: R b[k] : [k]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 0 pair(s), 0 independent{{$}}
      // CHECK: scalars    : reduction(+: s){{$}}
      // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
      for (int k = j; k < n; k++)
        s += b[k] * j;
    a[i] = s;
  }
}

// Rows: c[i][j] += x[j][k] over j <= i, as in syrk.
void s_rows(int n, int m, long c[][16], const long x[][16]) {
  // CHECK: ranges.c:[[#LOOP7:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: RW c[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R x[j][k] : [j][k]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'x' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)n - 1)) * 16 + ((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)x + 8 * ((0) * 16 + 0) || (__INTPTR_TYPE__)x + 8 * ((((__INTPTR_TYPE__)n - 1)) * 16 + ((__INTPTR_TYPE__)m - 1) + 1) <= (__INTPTR_TYPE__)c + 8 * ((0) * 16 + 0)){{$}}
  // CHECK: collapse   : no, the inner loop needs its own run-time alias check{{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (2 * (double)(m) * (double)(n) + 1 * (double)(n)) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: ranges.c:[[#LOOP8:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 1 child loop(s), perfect nest, triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: RW c[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R x[j][k] : [j][k]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'c' and 'x' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)i) * 16 + (__INTPTR_TYPE__)i + 1) <= (__INTPTR_TYPE__)x + 8 * ((0) * 16 + 0) || (__INTPTR_TYPE__)x + 8 * (((__INTPTR_TYPE__)i) * 16 + ((__INTPTR_TYPE__)m - 1) + 1) <= (__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)i) * 16 + 0)){{$}}
    // CHECK: collapse   : no, the inner loop is not SAFE on its own{{$}}
    // CHECK: profitability : CPU_PARALLEL if ((double)(i) * (2 * (double)(m)) >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      // CHECK: ranges.c:[[#LOOP9:@LINE+14]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP8]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j, k{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: RW c[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R x[j][k] : [j][k]{{$}}
      // CHECK-NEXT: safety     : UNSAFE{{$}}
      // CHECK: - RW c[i][j]: every iteration touches the same element{{$}}
      // CHECK: rejected   : array-dependence{{$}}
      // CHECK: deps       : 1 pair(s), 0 independent{{$}}
      // CHECK: alias      : 'c' and 'x' may overlap: checked at run time{{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int k = 0; k < m; k++)
        c[i][j] += x[j][k];
}

// adi's shape: j is the counter of an ascending and a descending loop over
// the same range.
void s_siblings(long p[][64], long v[][64], const long *u, int n) {
  int i, j;
  // CHECK: ranges.c:[[#LOOP10:@LINE+22]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 8 (8 affine){{$}}
  // CHECK-NEXT: W p[i][0] : [i][0]{{$}}
  // CHECK-NEXT: W p[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R p[i][j - 1] : [i][j - 1]{{$}}
  // CHECK-NEXT: R u[j] : [j]{{$}}
  // CHECK-NEXT: W v[n - 1][i] : [n - 1][i]{{$}}
  // CHECK-NEXT: W v[j][i] : [j][i]{{$}}
  // CHECK-NEXT: R p[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R v[j + 1][i] : [j + 1][i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 12 pair(s), 12 independent (12 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: alias      : 'p' and 'u' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'p' and 'v' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'u' and 'v' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)p + ((8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 3) + 1) ? (8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 3) + 1)) <= (__INTPTR_TYPE__)u + 8 || (__INTPTR_TYPE__)u + 8 * (((__INTPTR_TYPE__)n - 2) + 1) <= (__INTPTR_TYPE__)p + (8 * ((1) * 64 + 0) < 8 * ((1) * 64 + 1) ? 8 * ((1) * 64 + 0) : 8 * ((1) * 64 + 1))) && ((__INTPTR_TYPE__)p + ((8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 3) + 1) ? (8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 3) + 1)) <= (__INTPTR_TYPE__)v + ((8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) < 8 * ((1) * 64 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) : 8 * ((1) * 64 + 1)) < 8 * ((2) * 64 + 1) ? (8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) < 8 * ((1) * 64 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) : 8 * ((1) * 64 + 1)) : 8 * ((2) * 64 + 1)) || (__INTPTR_TYPE__)v + (8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) <= (__INTPTR_TYPE__)p + (8 * ((1) * 64 + 0) < 8 * ((1) * 64 + 1) ? 8 * ((1) * 64 + 0) : 8 * ((1) * 64 + 1))) && ((__INTPTR_TYPE__)u + 8 * (((__INTPTR_TYPE__)n - 2) + 1) <= (__INTPTR_TYPE__)v + ((8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) < 8 * ((1) * 64 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) : 8 * ((1) * 64 + 1)) < 8 * ((2) * 64 + 1) ? (8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) < 8 * ((1) * 64 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 1) : 8 * ((1) * 64 + 1)) : 8 * ((2) * 64 + 1)) || (__INTPTR_TYPE__)v + (8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) > 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1) : 8 * ((((__INTPTR_TYPE__)n - 2)) * 64 + ((__INTPTR_TYPE__)n - 2) + 1)) <= (__INTPTR_TYPE__)u + 8){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n - 1) * (2 + 7 * (double)(((__INTPTR_TYPE__)n - 2))) >= {{[0-9]+}}){{$}}
  for (i = 1; i < n - 1; i++) {
    p[i][0] = 0;
    // CHECK: ranges.c:[[#LOOP11:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W p[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R p[i][j - 1] : [i][j - 1]{{$}}
    // CHECK-NEXT: R u[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W p[i][j] and R p[i][j - 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'p' and 'u' may overlap: checked at run time{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 1; j < n - 1; j++)
      p[i][j] = p[i][j - 1] + u[j];
    v[n - 1][i] = 1;
    // CHECK: ranges.c:[[#LOOP12:@LINE+15]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W v[j][i] : [j][i]{{$}}
    // CHECK-NEXT: R p[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R v[j + 1][i] : [j + 1][i]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W v[j][i] and R v[j + 1][i]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'v' and 'p' may overlap: checked at run time{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = n - 2; j >= 1; j--)
      v[j][i] = p[i][j] + v[j + 1][i];
  }
}

// Sibling ranges a constant apart: j in [0, n - 1] and [1, n], span [0, n].
void s_offset(long *a, const long *b, int n) {
  int i, j;
  // CHECK: ranges.c:[[#LOOP13:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)n + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (4 * (double)(((__INTPTR_TYPE__)n + 1))) >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP14:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (j = 0; j < n; j++)
      s += b[j];
    // CHECK: ranges.c:[[#LOOP15:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (j = 1; j <= n; j++)
      s -= b[j];
    a[i] = s;
  }
}

// --- not SAFE ---------------------------------------------------------------

// Sibling ranges whose ends differ by a symbol: no span, so no check.
void u_mismatch(long *a, const long *b, int n, int m) {
  int i, j;
  // CHECK: ranges.c:[[#LOOP16:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'u_mismatch' restrict, if no call lets what 'b' points to overlap anything else 'u_mismatch' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP17:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 8 B across 1 region(s): b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code uses 'a', which may overlap 'b'{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 131072 && m >= 1 && m >= 131072); transfer 0.5x of its own maps per run (1 section copies over 2 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (j = 0; j < n; j++)
      s += b[j];
    // CHECK: ranges.c:[[#LOOP18:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 8 B across 1 region(s): b (map(to: b[0:m]), 8 B/elem){{$}}
    // CHECK: - map guard: m >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code uses 'a', which may overlap 'b'{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 131072 && m >= 1 && m >= 131072); transfer 0.5x of its own maps per run (1 section copies over 2 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (j = 0; j < m; j++)
      s -= b[j];
    a[i] = s;
  }
}

// The bound only helps an overlap check: a[i] reads a[j] for j < i, a real
// dependence on the same array.
void u_carried(long *a, int n) {
  // CHECK: ranges.c:[[#LOOP19:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[j] : [j]{{$}}
  // CHECK-NEXT: RW a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove R a[j] and RW a[i] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP20:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R a[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j < i; j++)
      s += a[j];
    a[i] += s;
  }
}

// A triangular write: on the GPU path its section isn't fully written, so
// it may not be map(from:) (tests/ranges.test checks map(tofrom:)).
static long T[64][64];
void s_tri_write(int n) {
  // CHECK: ranges.c:[[#LOOP21:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W T[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
  // CHECK: profitability : GPU_OFFLOAD if ((double)(n) * ((2 * (double)(n)) - {{[0-9]+}}) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: ranges.c:[[#LOOP22:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP21]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W T[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      T[i][j] = i + j;
}

// The same through a pointer: now mappable, and the section is the widened
// range, so `from` would copy back the unwritten upper triangle.
void s_tri_ptr(long (*o)[64], int n) {
  // CHECK: ranges.c:[[#LOOP23:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W o[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n) * (2 * (double)(n)) >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: ranges.c:[[#LOOP24:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP23]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W o[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      o[i][j] = i - j;
}

// --- from the review ---------------------------------------------------------

// The check must not load what the loop loads only behind a condition:
// b's bound comes through k's range (s->m), and s may be null when the loop
// runs. UNKNOWN (the driver calls it with s == 0).
struct RS { int m; };
void u_field(long *a, const long *b, int n, struct RS *s) {
  // CHECK: ranges.c:[[#LOOP25:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R s->m : [0]{{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: 's->m' is loaded through pointers the loop may not load{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'u_field' restrict, if no call lets what 'b' points to overlap anything else 'u_field' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 type){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long t = 0;
    if (s)
      // CHECK: ranges.c:[[#LOOP26:@LINE+14]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 1 child loop(s), perfect nest{{$}}
      // CHECK: nest ivs   : i, k{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: R s->m : [0]{{$}}
      // CHECK-NEXT: R b[j] : [j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 0 pair(s), 0 independent{{$}}
      // CHECK: scalars    : reduction(+: t){{$}}
      // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
      // CHECK: profitability : CPU_PARALLEL if (s->m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: can't map 's': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
      for (int k = 0; k < s->m; k++)
        // CHECK: ranges.c:[[#LOOP27:@LINE+11]]:9{{$}}
        // CHECK: kind       : VARIABLE_BOUND{{$}}
        // CHECK: trip count : <not statically known>{{$}}
        // CHECK: nest       : depth 2, parent [[#LOOP26]]:7, 0 child loop(s), triangular{{$}}
        // CHECK: nest ivs   : i, k, j{{$}}
        // CHECK: accesses   : 1 (1 affine){{$}}
        // CHECK-NEXT: R b[j] : [j]{{$}}
        // CHECK-NEXT: safety     : SAFE{{$}}
        // CHECK: deps       : 0 pair(s), 0 independent{{$}}
        // CHECK: scalars    : reduction(+: t){{$}}
        // CHECK: profitability : CPU_PARALLEL if (k >= {{[0-9]+}}){{$}}
        for (int j = 0; j <= k; j++)
          t += b[j];
    a[i] = t;
  }
}

// The same through sibling loops' ranges.
void u_field_sib(long *a, const long *b, int n, struct RS *s) {
  int j;
  // CHECK: ranges.c:[[#LOOP28:@LINE+18]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: R s->m : [0]{{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: R s->m : [0]{{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: 's->m' is loaded through pointers the loop may not load{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'u_field_sib' restrict, if no call lets what 'b' points to overlap anything else 'u_field_sib' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long t = 0;
    if (s) {
      // CHECK: ranges.c:[[#LOOP29:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: R s->m : [0]{{$}}
      // CHECK-NEXT: R b[j] : [j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 0 pair(s), 0 independent{{$}}
      // CHECK: scalars    : reduction(+: t){{$}}
      // CHECK: profitability : CPU_PARALLEL if (s->m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: can't map 's': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
      for (j = 0; j < s->m; j++)
        t += b[j];
      // CHECK: ranges.c:[[#LOOP30:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: R s->m : [0]{{$}}
      // CHECK-NEXT: R b[j] : [j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 0 pair(s), 0 independent{{$}}
      // CHECK: scalars    : reduction(+: t){{$}}
      // CHECK: profitability : CPU_PARALLEL if (s->m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: can't map 's': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
      for (j = 0; j < s->m; j++)
        t += 2 * b[j];
    }
    a[i] = t;
  }
}

// g[i + j] stays inside g (j < 64 - i), though i and j each at their own
// extreme give 126: a widened bound must not make g's extent look
// contradicted, which would leave g[idx[i]] without a bound. SAFE behind a
// check on all of g.
long g_ext[64];
void s_extent(long *out, const int *idx) {
  // CHECK: ranges.c:[[#LOOP31:@LINE+16]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 64{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (3 affine){{$}}
  // CHECK-NEXT: R g_ext[i + j] : [i + j]{{$}}
  // CHECK-NEXT: R g_ext[idx[i]] : non-affine: subscript reads memory ('idx[i]'); in [0, 63]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: W out[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'g_ext' and 'out' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'idx' and 'out' can't overlap (incompatible element types){{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)g_ext + 1016 <= (__INTPTR_TYPE__)out + 0 || (__INTPTR_TYPE__)out + 512 <= (__INTPTR_TYPE__)g_ext + 0){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 64; i++) {
    long s = 0;
    // CHECK: ranges.c:[[#LOOP32:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP31]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R g_ext[i + j] : [i + j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (64 - i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j < 64 - i; j++)
      s += g_ext[i + j];
    s += g_ext[idx[i]];
    out[i] = s;
  }
}
