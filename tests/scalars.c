// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 31 loop(s).
// Phase 1.4 fixture: scalars a loop writes by name (ScalarAnalysis.h).
// Each function states the expected verdict and clause. The SAFE ones are
// also RUN by scripts/check_parallel.sh with eval/runtime/scalars_main.c:
// every SAFE loop forced to `parallel for` with its clauses, outputs compared
// bit for bit with the sequential build (integers, so reductions are exact).

long g_count;

// ================================================= SAFE: private ============

// SAFE, private(t): t is written before it is read in every iteration.
void s_temp(long *b, const long *a, int n) {
  long t;
  // CHECK: scalars.c:[[#LOOP0:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : private(t){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    t = a[i] * 3;
    b[i] = t + 1;
  }
}

// PolyBench style: counters declared at the top. Outer loop SAFE, private(j).
void s_polybench(long *b, const long *a, int n) {
  int i, j;
  // CHECK: scalars.c:[[#LOOP1:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W b[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * ((0) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(n - 1)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * ((0) * (__INTPTR_TYPE__)n + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++)
    // CHECK: scalars.c:[[#LOOP2:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP1]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0) || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + (__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)(i)) * (__INTPTR_TYPE__)n + 0)){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (j = 0; j < n; j++)
      b[i * n + j] = a[i * n + j] + j;
}

// ================================================= SAFE: reductions =========

// SAFE, reduction(+: s)
long s_sum(const long *a, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP3:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(+: s){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

// SAFE, reduction(+: s): `-=` combines with +
long s_sub(const long *a, int n) {
  long s = 1000;
  // CHECK: scalars.c:[[#LOOP4:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(+: s){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    s -= a[i];
  return s;
}

// SAFE, reduction(*: p), written as p = p * e
long s_prod(const long *a, int n) {
  long p = 1;
  // CHECK: scalars.c:[[#LOOP5:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(*: p){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p = p * (a[i] % 3 + 1);
  return p;
}

// SAFE, reduction(^: x) and reduction(|: o): two reductions at once
long s_bits(const long *a, int n) {
  long x = 0, o = 0;
  // CHECK: scalars.c:[[#LOOP6:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(^: x) reduction(|: o){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    x ^= a[i];
    o = a[i] | o;
  }
  return x + o;
}

// SAFE, reduction(max: m): the if-form
long s_max(const long *a, int n) {
  long m = 0;
  // CHECK: scalars.c:[[#LOOP7:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(max: m){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    if (a[i] > m)
      m = a[i];
  return m;
}

// SAFE, reduction(min: m): the ternary form
long s_min(const long *a, int n) {
  long m = 1L << 40;
  // CHECK: scalars.c:[[#LOOP8:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(min: m){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    m = a[i] < m ? a[i] : m;
  return m;
}

// SAFE, reduction(+: s) under a condition and inside an inner loop
long s_nested_sum(const long *a, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP9:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(+: s){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: scalars.c:[[#LOOP10:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: R a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      if (a[i * n + j] % 2)
        s += a[i * n + j];
  return s;
}

// ================================================= SAFE: lastprivate ========

// SAFE, lastprivate(x): x is read after the loop and written every iteration
long s_last(long *b, const long *a, int n) {
  long x = 0;
  // CHECK: scalars.c:[[#LOOP11:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : lastprivate(x){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * ((__INTPTR_TYPE__)(n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    x = a[i] * 2;
    b[i] = x;
  }
  return x;
}

// SAFE, lastprivate(j): the inner counter is read after the outer loop
int s_last_inner_iv(long *b, int n) {
  int j = -1;
  // CHECK: scalars.c:[[#LOOP12:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W b[i * 4 + j] : [4*i + j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: scalars    : lastprivate(j){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: scalars.c:[[#LOOP13:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W b[i * 4 + j] : [4*i + j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: scalars    : lastprivate(j){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < 4; j++)
      b[i * 4 + j] = i + j;
  return j;
}

// SAFE, lastprivate(i): the counter is read after a loop that provably runs
// (constant trip count 100). The open bug from the fuzzer's NOTES entry.
int s_ivout_const(long *a) {
  int i;
  // CHECK: scalars.c:[[#LOOP14:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 100{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : lastprivate(i){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < 100; i++)
    a[i] = i;
  return i;
}

// ================================================= UNSAFE ===================

// UNSAFE: p carries a value from one iteration to the next.
long u_carried(long *a, int n) {
  long p = 0;
  // CHECK: scalars.c:[[#LOOP15:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'p' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = p;
    p = a[i] + 1;
  }
  return p;
}

// UNSAFE: x is read after the loop but only written on some iterations.
long u_last_conditional(const long *a, int n) {
  long x = -1;
  // CHECK: scalars.c:[[#LOOP16:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    if (a[i] > 50)
      x = a[i];
  return x;
}

// UNSAFE: a `continue` can skip the write, so lastprivate is wrong.
long u_last_continue(const long *a, int n) {
  long x = -1;
  // CHECK: scalars.c:[[#LOOP17:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    if (a[i] < 10)
      continue;
    x = a[i];
  }
  return x;
}

// UNSAFE: two different operators on one variable.
long u_mixed_ops(const long *a, int n) {
  long s = 1;
  // CHECK: scalars.c:[[#LOOP18:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s += a[i];
    s *= 2;
  }
  return s;
}

// UNSAFE: the running value is read elsewhere (a prefix sum, not a reduction).
long u_read_elsewhere(long *b, const long *a, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP19:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s += a[i];
    b[i] = s;
  }
  return s;
}

// UNSAFE: s = e - s is not associative.
long u_reverse_sub(const long *a, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP20:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s = a[i] - s;
  return s;
}

// UNSAFE: a floating-point sum without -fp-reductions.
double u_fp_sum(const double *a, int n) {
  double s = 0;
  // CHECK: scalars.c:[[#LOOP21:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

// UNSAFE: a global counter is shared by every iteration.
void u_global(const long *a, int n) {
  // CHECK: scalars.c:[[#LOOP22:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - writes to 'g_count', a global or static shared by every iteration{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    g_count += a[i];
}

// UNSAFE: t's address is taken, so a pointer can reach the shared copy.
void u_address_taken(long *b, const long *a, int n) {
  long t = 0;
  long *pt = &t;
  // CHECK: scalars.c:[[#LOOP23:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R *pt : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'pt' is a pointer that may point into 'b' (W b[i], R *pt){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    t = a[i];
    b[i] = *pt;
  }
}

// UNSAFE: the bound k is written in the body.
void u_bound_written(long *a, int k) {
  // CHECK: scalars.c:[[#LOOP24:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - writes to 'k', which the loop's own condition or increment uses{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < k; i++) {
    a[i] = 1;
    k = k - 1;
  }
}

// ================================================= UNKNOWN ==================

// UNKNOWN: i is read after a loop that may run zero times (n <= 0 leaves
// i = 0 sequentially; lastprivate(i) would leave it uninitialized).
int k_ivout(long *a, int n) {
  int i;
  // CHECK: scalars.c:[[#LOOP25:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - induction variable 'i' is used after the loop, which may run zero times{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++)
    a[i] = i;
  return i;
}

// ============ Regressions from the Phase 1.4 adversarial review ===============
// Each was SAFE with a reduction clause and computed a different result in
// parallel. All must be UNSAFE.

// [S1] The running value of the "reduction" is used: a prefix sum.
void r_prefix(long *a, const long *b, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP26:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = (s += b[i]);
}

// [S1'] Used in a condition.
void r_prefix_if(long *a, const long *b, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP27:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    if ((s += b[i]) > 100)
      a[i] = 1;
}

// [S2] max compared in unsigned, stored in int: not max of the int values.
int r_mixed_max(const unsigned *b, int n) {
  int m = 0;
  // CHECK: scalars.c:[[#LOOP28:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'm' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    if (b[i] > m)
      m = b[i];
  return m;
}

// [S3] An integer accumulator with a floating-point operand truncates every
// step.
long r_int_fp(const long *b, int n) {
  long s = 0;
  // CHECK: scalars.c:[[#LOOP29:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s += b[i] * 0.5 - 20.0;
  return s;
}

// [S4] _Bool normalizes every step.
_Bool r_bool(const long *b, int n) {
  _Bool s = 0;
  // CHECK: scalars.c:[[#LOOP30:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 's' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s ^= b[i];
  return s;
}
