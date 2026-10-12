// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 17 loop(s).
//
// Phase 1.6: aliasing made explicit (AliasAnalysis.h). Each loop reads and
// writes through two bases that could be the same memory; the pairs differ
// in how that is settled: a proof (restrict, element types, every caller), a
// run-time check, or nothing (UNKNOWN). Also RUN by scripts/check_parallel.sh
// with eval/runtime/alias_main.c, which calls the run-time-checked loops with
// DISJOINT and with OVERLAPPING arguments: an overlapping call must run
// sequentially (its guard is false) and still match the sequential build.

#define NG 4096
long g_arr[NG + 1];
long g_scale;

// --- 1. run-time check: two plain pointer parameters ----------------------

// SAFE, checked at run time: a[i] = b[i + 1] is a[i] = a[i + 1] if b == a.
void rt_shift(long *a, const long *b, int n) {
  // CHECK: alias.c:[[#LOOP0:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 || (__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)(n) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i + 1] + 1;
}

// SAFE, checked at run time, rows: a[i*m + j] vs b[i*m + j].
void rt_rows(long *a, const long *b, int n, int m) {
  // CHECK: alias.c:[[#LOOP1:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: R b[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)m + (__INTPTR_TYPE__)(m - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * ((0) * (__INTPTR_TYPE__)m + 0) || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)m + (__INTPTR_TYPE__)(m - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * ((0) * (__INTPTR_TYPE__)m + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: alias.c:[[#LOOP2:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP1]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: R b[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)m + (__INTPTR_TYPE__)(m - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)m + 0) || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)m + (__INTPTR_TYPE__)(m - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)m + 0)){{$}}
    // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
    for (int j = 0; j < m; j++)
      a[i * m + j] = b[i * m + j] * 2;
}

// SAFE, checked at run time: a parameter against a global array.
void rt_global(long *p, int n) {
  // CHECK: alias.c:[[#LOOP3:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R g_arr[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'g_arr' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)p + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)g_arr + 8 || (__INTPTR_TYPE__)g_arr + 8 * ((__INTPTR_TYPE__)(n) + 1) <= (__INTPTR_TYPE__)p + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p[i] = g_arr[i + 1] - 3;
}

// SAFE, checked at run time: a parameter against a global read by name.
void rt_scalar(long *p, int n) {
  // CHECK: alias.c:[[#LOOP4:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'g_scale' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)p + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)&g_scale + 0 || (__INTPTR_TYPE__)&g_scale + 8 <= (__INTPTR_TYPE__)p + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p[i] = g_scale + i;
}

// --- 2. proven apart: no check ---------------------------------------------

// SAFE: restrict.
void pr_restrict(long *restrict a, const long *restrict b, int n) {
  // CHECK: alias.c:[[#LOOP5:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i + 1] + 1;
}

// SAFE: double vs int elements can't overlap (strict aliasing).
void pr_types(double *d, const int *k, int n) {
  // CHECK: alias.c:[[#LOOP6:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W d[i] : [i]{{$}}
  // CHECK-NEXT: R k[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'd' and 'k' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    d[i] = k[i + 1] * 0.5;
}

// SAFE: every call passes two different arrays.
static void pr_callers_inner(long *x, const long *y, int n) {
  // CHECK: alias.c:[[#LOOP7:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R y[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'x' and 'y' can't overlap (every call passes disjoint objects){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    x[i] = y[i + 1] * 3;
}
void pr_callers(int n) {
  static long X[NG + 1], Y[NG + 1];
  pr_callers_inner(X, Y, n);
  g_arr[0] = X[0] + X[n - 1];
}

// SAFE, checked at run time: one call passes overlapping arguments, so the
// callers prove nothing.
static void rt_callers_inner(long *x, const long *y, int n) {
  // CHECK: alias.c:[[#LOOP8:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R y[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'x' and 'y' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)x + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)y + 8 || (__INTPTR_TYPE__)y + 8 * ((__INTPTR_TYPE__)(n) + 1) <= (__INTPTR_TYPE__)x + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    x[i] = y[i + 1] * 3;
}
void rt_callers(long *a, int n) {
  static long X[NG + 1], Y[NG + 1];
  rt_callers_inner(X, Y, n);
  rt_callers_inner(a, a, n);
}

// --- 3. no proof and no check: UNKNOWN -------------------------------------

// UNKNOWN: j's range depends on i, so b's touched range can't be bounded.
void k_prefix(long *a, const long *b, int n) {
  // CHECK: alias.c:[[#LOOP9:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: alias.c:[[#LOOP10:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j < i; j++)
      s += b[j];
    a[i] = s;
  }
}

// --- 4. from the Phase 1.6 review --------------------------------------------

// UNKNOWN: the check would name 'a', but the 'a' visible at the loop is the
// inner local, not the parameter x stands for.
void k_shadowed_param(long *a, long *b, int n) {
  long *x = a;
  {
    long *a = b + 100000;
    // CHECK: alias.c:[[#LOOP11:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W x[i] : [i] of 'a'{{$}}
    // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 'a' and 'b' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: 'a' is shadowed in 'k_shadowed_param', so the check can't name it{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int i = 0; i < n; i++)
      x[i] = b[i + 1] + (a != 0);
  }
}

// UNKNOWN: the callee's global 'g_late' is declared after this loop, so the
// check can't name it there.
long late_get(int i);
void k_late_global(long *p, int n) {
  // CHECK: alias.c:[[#LOOP12:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: late_get reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'p' and 'g_late' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: global 'g_late' is declared after the loop{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i] = late_get(i);
}
long g_late;
long late_get(int i) { return g_late + i; }

// UNKNOWN: 'g_arr' (reached through the callee) is shadowed by an enumerator.
static long arr_get(int k) { return g_arr[k]; }
void k_enum_shadow(long *p, int n) {
  enum { g_arr = 0 };
  // CHECK: alias.c:[[#LOOP13:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: arr_get reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R arr_get(): g_arr[k] : [i + 1] of 'g_arr'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'p' and 'g_arr' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: 'g_arr' is shadowed in 'k_enum_shadow', so the check can't name it{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i] = arr_get(i + 1) + g_arr;
}

// SAFE, checked at run time: the global pointer read by name is 8 bytes, not
// the 1 byte it points to (c may legally point into it).
char *g_cp;
void rt_global_ptr(char *c, int n) {
  // CHECK: alias.c:[[#LOOP14:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'g_cp' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)&g_cp + 0 || (__INTPTR_TYPE__)&g_cp + 8 <= (__INTPTR_TYPE__)c + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    c[i] = (char)(((unsigned long)g_cp >> (8 * i)) + 1);
}

// SAFE, checked at run time: void * and long * elements may alias.
void rt_void_ptrs(void **a, long **b, int n) {
  // CHECK: alias.c:[[#LOOP15:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 || (__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)(n) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i + 1];
}

// SAFE, checked at run time: long and long long have the same width.
void rt_same_width(long *a, long long *b, int n) {
  // CHECK: alias.c:[[#LOOP16:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 || (__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)(n) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i + 1];
}
