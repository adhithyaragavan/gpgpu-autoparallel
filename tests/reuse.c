// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 42 loop(s).
// Phase 2.5: a loop counter declared at function scope and reused by
// sibling loops (`int j; for (i..) { for (j..) ..; for (j..) .. }`, as in
// PolyBench gemm, syrk, adi). Inside one of those loops only its own header
// writes j, so writes outside it (the sibling's header, an assignment in
// between) do not make j unknown there (isCleanIV, -reuse-counters). It still
// needs the loop's init to assign j without reading it, and no label in the
// loop's body. Every SAFE case has a twin that must not be. Also RUN by
// scripts/check_parallel.sh with eval/runtime/reuse_main.c.

#define N 64

static double A[N][N], B[N][N], C[N][N];
static double t[N];
static int last;

// --- SAFE -------------------------------------------------------------------

// gemm: j is the counter of two sibling loops, k of the loop around one.
void s_gemm(int n, double alpha, double beta) {
  int i, j, k;
  // CHECK: reuse.c:[[#LOOP0:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: RW C[i][j] : [i][j]{{$}}
  // CHECK-NEXT: RW C[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][k] : [i][k]{{$}}
  // CHECK-NEXT: R B[k][j] : [k][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: scalars    : private(j, k){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP1:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: RW C[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (j = 0; j < n; j++)
      C[i][j] *= beta;
    // CHECK: reuse.c:[[#LOOP2:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: RW C[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][k] : [i][k]{{$}}
    // CHECK-NEXT: R B[k][j] : [k][j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - RW C[i][j]: every iteration touches the same element{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (k = 0; k < n; k++)
      // CHECK: reuse.c:[[#LOOP3:@LINE+12]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP2]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, k, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: RW C[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][k] : [i][k]{{$}}
      // CHECK-NEXT: R B[k][j] : [k][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
      for (j = 0; j < n; j++)
        C[i][j] += alpha * A[i][k] * B[k][j];
  }
}

// A plain write to j between the loops, and a read of j after the first
// one (its exit value, n, the same in every iteration of i).
void s_between(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP4:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W t[i] : [i]{{$}}
  // CHECK-NEXT: W C[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j - 1] : [i][j - 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 4 pair(s), 4 independent (4 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP5:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      A[i][j] = B[i][j] + 1.0;
    t[i] = j;
    j = 7;
    // CHECK: reuse.c:[[#LOOP6:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W C[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j - 1] : [i][j - 1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (j = 1; j < n; j++)
      C[i][j] = A[i][j - 1];
  }
}

// adi's shape: an ascending and a descending sibling.
void s_updown(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP7:@LINE+18]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 8 (8 affine){{$}}
  // CHECK-NEXT: W B[i][0] : [i][0]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j - 1] : [i][j - 1]{{$}}
  // CHECK-NEXT: W B[i][n - 1] : [i][n - 1]{{$}}
  // CHECK-NEXT: W C[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R C[i][j + 1] : [i][j + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 14 pair(s), 14 independent (14 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
  for (i = 1; i < n - 1; i++) {
    B[i][0] = 1.0;
    // CHECK: reuse.c:[[#LOOP8:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j - 1] : [i][j - 1]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W B[i][j] and R B[i][j - 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 1; j < n - 1; j++)
      B[i][j] = A[i][j] * 0.5 + B[i][j - 1];
    B[i][n - 1] = 1.0;
    // CHECK: reuse.c:[[#LOOP9:@LINE+14]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W C[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R C[i][j + 1] : [i][j + 1]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W C[i][j] and R C[i][j + 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = n - 2; j >= 1; j--)
      C[i][j] = B[i][j] + C[i][j + 1];
  }
}

// j is read after the whole nest: the outer loop must make it lastprivate.
void s_live_after(int n) {
  int i, j = -1; // n == 0: the loop leaves j alone
  // CHECK: reuse.c:[[#LOOP10:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: scalars    : lastprivate(j){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP11:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (j = 0; j < i; j++)
      A[i][j] = 2.0;
    // CHECK: reuse.c:[[#LOOP12:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      B[i][j] = A[i][j];
  }
  last = j;
}

// Nested, not sibling: the inner header rewrites the middle loop's j, so the
// middle loop's range is unknown. But the only access uses the inner loop's
// j, whose range is exact, and row i is the outer loop's own.
void s_nested(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP13:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: scalars    : private(j){{$}}
  // CHECK: collapse   : no, the inner loop is not SAFE on its own{{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (i = 0; i < n; i++)
    // CHECK: reuse.c:[[#LOOP14:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (0 affine){{$}}
    // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop, so the loop's iteration space is unknown; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: R B[i][j] : non-affine: induction variable 'j' is modified inside the loop, so the loop's iteration space is unknown; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop body reassigns the induction variable 'j'{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      // CHECK: reuse.c:[[#LOOP15:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP14]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j, j{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : UNKNOWN{{$}}
      // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
      // CHECK: rejected   : live-out-scalar{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (j = 0; j < n; j++)
        A[i][j] = B[i][j];
}

// --- not SAFE ---------------------------------------------------------------

// Nested again, now with an access that uses the middle loop's j: its value
// there is whatever the inner loop left (n), so the range is unknown.
void u_nested(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP16:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W C[i][j - 1] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W C[i][j - 1]: W C[i][j - 1] is non-affine (induction variable 'j' is modified inside the loop){{$}}
  // CHECK: rejected   : loop-shape{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++)
    // CHECK: reuse.c:[[#LOOP17:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 1 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (0 affine){{$}}
    // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop, so the loop's iteration space is unknown; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: R B[i][j] : non-affine: induction variable 'j' is modified inside the loop, so the loop's iteration space is unknown; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: W C[i][j - 1] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop body reassigns the induction variable 'j'{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: deps       : 2 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++) {
      // CHECK: reuse.c:[[#LOOP18:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP17]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j, j{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : UNKNOWN{{$}}
      // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
      // CHECK: rejected   : live-out-scalar{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (j = 0; j < n; j++)
        A[i][j] = B[i][j];
      C[i][j - 1] = 1.0;
    }
}

// The second loop has no init: it continues from where the first stopped.
void u_noinit(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP19:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (1 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W A[i][j] : non-affine: uses 'j', which is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: R A[i][j - 1] : non-affine: uses 'j', which is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W A[i][j] and W A[i][j]: W A[i][j] is non-affine (uses 'j', which is modified inside the loop){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 5 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP20:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n / 2; j++)
      A[i][j] = 1.0;
    // CHECK: reuse.c:[[#LOOP21:@LINE+12]]:5{{$}}
    // CHECK: kind       : UNRECOGNIZED{{$}}
    // CHECK: reason     : no init clause{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: accesses   : 2 (0 affine){{$}}
    // CHECK-NEXT: W A[i][j] : non-affine: uses 'j', which is modified inside the loop; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: R A[i][j - 1] : non-affine: uses 'j', which is modified inside the loop; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - loop shape not recognized (no init clause); safety not evaluated{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (; j < n; j++)
      A[i][j] = A[i][j - 1] + 1.0;
  }
}

// The second loop's init reads j: its start comes from the first loop.
void u_selfinit(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP22:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W A[i][j] and W A[i][j]: W A[i][j] is non-affine (induction variable 'j' is modified inside the loop){{$}}
  // CHECK: rejected   : loop-shape{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP23:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP22]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n / 2; j++)
      A[i][j] = 1.0;
    // CHECK: reuse.c:[[#LOOP24:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP22]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - loop's init reads its own induction variable 'j'{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = j + 1; j < n; j++)
      A[i][j] = 2.0;
  }
}

// A label in the second loop's body: the goto enters it with any j.
void u_label(int n, int skip) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP25:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body contains 'goto'{{$}}
  // CHECK: rejected   : early-exit, loop-shape{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP26:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n / 2; j++)
      A[i][j] = 1.0;
    if (skip)
      goto inside;
    // CHECK: reuse.c:[[#LOOP27:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - loop body contains a label a 'goto' can jump to{{$}}
    // CHECK: rejected   : early-exit{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = n / 2; j < n; j++) {
    inside:
      A[i][j] = 2.0;
    }
  }
}

// j's address escapes: a write through p can change it anywhere.
void u_addr(int n) {
  int i, j;
  int *p = &j;
  // CHECK: reuse.c:[[#LOOP28:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R *p : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'p' is a pointer that may point into 'A' (W A[i][j], R *p){{$}}
  // CHECK: rejected   : pointer-overlap, shared-scalar{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP29:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is global, static or address-taken{{$}}
    // CHECK: rejected   : shared-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      A[i][j] = 1.0;
    // CHECK: reuse.c:[[#LOOP30:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R *p : [0]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 'p' is a pointer that may point into 'A' (W A[i][j], R *p){{$}}
    // CHECK: rejected   : pointer-overlap, shared-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      A[i][j] = (double)*p;
  }
}

// The first loop's own body writes j: that loop's j is unknown, and so is
// the row of A it writes.
void u_bodywrite(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP31:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (1 affine){{$}}
  // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: R B[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W A[i][j]: W A[i][j] is non-affine (induction variable 'j' is modified inside the loop){{$}}
  // CHECK: rejected   : loop-shape{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP32:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP31]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (0 affine){{$}}
    // CHECK-NEXT: W A[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: R B[i][j] : non-affine: induction variable 'j' is modified inside the loop; in [0, 63][0, 63]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop body reassigns the induction variable 'j'{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++) {
      A[i][j] = 1.0;
      if (B[i][j] < 0.0)
        j++;
    }
    // CHECK: reuse.c:[[#LOOP33:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP31]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (j = 0; j < n; j++)
      B[i][j] = 1.0;
  }
}

// Reuse is fine, but row i reads row i - 1: a real dependence.
void u_carried(int n) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP34:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i - 1][j] : [i - 1][j]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W A[i][j] and R A[i - 1][j] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 4 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 1; i < n; i++) {
    // CHECK: reuse.c:[[#LOOP35:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP34]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i - 1][j] : [i - 1][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (1 ZIV, 1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (j = 0; j < n; j++)
      A[i][j] = A[i - 1][j] + 1.0;
    // CHECK: reuse.c:[[#LOOP36:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP34]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (j = 0; j < n; j++)
      B[i][j] = A[i][j];
  }
}

// j is read before either loop writes it: its value comes from the previous
// iteration of i, so it is not private.
void u_read_first(int n) {
  int i, j = 0;
  // CHECK: reuse.c:[[#LOOP37:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W t[i] : [i]{{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: RW A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'j' carries a value from one iteration to the next{{$}}
  // CHECK: rejected   : scalar-dependence{{$}}
  // CHECK: deps       : 4 pair(s), 4 independent (4 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    t[i] = j;
    // CHECK: reuse.c:[[#LOOP38:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP37]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (j = 0; j < i; j++)
      A[i][j] = 1.0;
    // CHECK: reuse.c:[[#LOOP39:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP37]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: RW A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: rejected   : live-out-scalar{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < n; j++)
      A[i][j] += 2.0;
  }
}

// Found by the review: a switch around the loop can enter its body at a
// case label, skipping the init (j = -10 there), so iteration i writes rows
// i and i + 1. The case label also makes the inner pragma not compile.
static double V[N * 10 + 20];
void u_switch(int n, int sel) {
  int i, j;
  // CHECK: reuse.c:[[#LOOP40:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W V[10 * i + j + 10] : non-affine: induction variable 'j' is modified inside the loop; in [0, 659]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W V[10 * i + j + 10]: W V[10 * i + j + 10] is non-affine (induction variable 'j' is modified inside the loop){{$}}
  // CHECK: rejected   : loop-shape{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    j = -10;
    switch (sel) {
    case 0:
      // CHECK: reuse.c:[[#LOOP41:@LINE+12]]:7{{$}}
      // CHECK: kind       : CONSTANT_BOUND{{$}}
      // CHECK: trip count : 10{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP40]]:3, 0 child loop(s){{$}}
      // CHECK: nest ivs   : i, j{{$}}
      // CHECK: accesses   : 1 (1 affine){{$}}
      // CHECK-NEXT: W V[10 * i + j + 10] : [10*i + j + 10]{{$}}
      // CHECK-NEXT: safety     : UNKNOWN{{$}}
      // CHECK: - loop body contains a 'case' label of a switch outside the loop{{$}}
      // CHECK: rejected   : early-exit{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (j = 0; j < 10; j++) {
      case 1:
        V[10 * i + j + 10] = i;
      }
    }
  }
}
