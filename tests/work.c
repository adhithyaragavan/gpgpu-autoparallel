// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 27 loop(s).
// Phase 3: inner loops of unknown trip count are priced by their ranges
// (-symbolic-trips), and constant-size sections once per run
// (-fixed-sections). Guards test a loop's total work, `n * W`. Cases from
// the review: names in W that change inside a `target data` region (the
// region is still hoisted, its loops priced without W), a window whose
// length is constant though its ends move, a stencil of width 2r + 1.
// Runtime: eval/runtime/work_main.c (also OFFLOAD=1, naive and gated).
#define N 512
double A[N][N], B[N][N], x[N], y[N];
int gm;

void bump(void) { gm = gm + 1; }

// 1. time loop, inner symbolic m, host code between: hoist with m in C.
void r_time(int n, int m, int steps) {
  // CHECK: work.c:[[#LOOP0:@LINE+20]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: accesses   : 9 (9 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W x[0] : [0]{{$}}
  // CHECK-NEXT: R A[0][0] : [0][0]{{$}}
  // CHECK-NEXT: R x[1] : [1]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W A[i][j]: every iteration touches the same element{{$}}
  // CHECK: rejected   : array-dependence, unproven-dependence{{$}}
  // CHECK: deps       : 9 pair(s), 1 independent (1 ZIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: work.c:[[#LOOP1:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : 2 (with 'j'){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): A (map(tofrom: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), B (map(to: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if((double)(n) * ((3 * (double)(m)) - {{[0-9]+}}) >= {{[0-9]+}}); transfer 0.833x of its own maps per run (t + 4 section copies over 2*t kernel runs per entry){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP2:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP1]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : t, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of A[i][j]: 'i' is shadowed in 'r_time', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = 0; j < m; j++)
        A[i][j] = A[i][j] * 0.5 + B[i][j];
    x[0] = A[0][0] + x[1];
    // CHECK: work.c:[[#LOOP3:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : 2 (with 'j'){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): B (map(tofrom: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), A (map(to: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if((double)(n) * ((3 * (double)(m)) - {{[0-9]+}}) >= {{[0-9]+}}); transfer 0.833x of its own maps per run (t + 4 section copies over 2*t kernel runs per entry){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP4:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP3]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : t, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of B[i][j]: 'i' is shadowed in 'r_time', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = 0; j < m; j++)
        B[i][j] = B[i][j] * 0.25 + A[i][j];
  }
}
// 2. m changes in the time loop.
void r_mchange(int n, int m, int steps) {
  // CHECK: work.c:[[#LOOP5:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: accesses   : 6 (6 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - could not prove W A[i][j] independent{{$}}
  // CHECK: rejected   : unproven-dependence, scalar-dependence{{$}}
  // CHECK: deps       : 6 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: work.c:[[#LOOP6:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP5]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : 2 (with 'j'){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): A (map(tofrom: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), B (map(to: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 32 && (__INTPTR_TYPE__)(steps) - (__INTPTR_TYPE__)(0) >= 2); transfer 0.333x of its own maps per run (4 section copies over 2*t kernel runs per entry, t >= 2){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP7:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP6]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : t, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of A[i][j]: 'i' is shadowed in 'r_mchange', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = 0; j < m; j++)
        A[i][j] = A[i][j] * 0.5 + B[i][j];
    m = m / 2 + 1;
    // CHECK: work.c:[[#LOOP8:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP5]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : 2 (with 'j'){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): B (map(tofrom: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), A (map(to: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 32 && (__INTPTR_TYPE__)(steps) - (__INTPTR_TYPE__)(0) >= 2); transfer 0.333x of its own maps per run (4 section copies over 2*t kernel runs per entry, t >= 2){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP9:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP8]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : t, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of B[i][j]: 'i' is shadowed in 'r_mchange', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = 0; j < m; j++)
        B[i][j] = B[i][j] * 0.25 + A[i][j];
  }
}
// 3. global bound modified by a callee between sibling GPU loops.
void r_global(int n) {
  // CHECK: work.c:[[#LOOP10:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if ((double)(n) * ((3 * (double)(gm)) - {{[0-9]+}}) >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 12288 B across 2 region(s): A (map(tofrom: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), B (map(to: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (0.667x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: work.c:[[#LOOP11:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (gm >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of A[i][j]: 'i' is shadowed in 'r_global', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j < gm; j++)
      A[i][j] = A[i][j] * 0.5 + B[i][j];
  bump();
  // CHECK: work.c:[[#LOOP12:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if ((double)(n) * ((3 * (double)(gm)) - {{[0-9]+}}) >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 12288 B across 2 region(s): B (map(tofrom: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), A (map(to: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (0.667x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: work.c:[[#LOOP13:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (gm >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of B[i][j]: 'i' is shadowed in 'r_global', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j < gm; j++)
      B[i][j] = B[i][j] * 0.25 + A[i][j];
}
// 4. run of two GPU loops, bound modified between.
void r_run(int n, int m) {
  // CHECK: work.c:[[#LOOP14:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if ((double)(n) * ((3 * (double)(m)) - {{[0-9]+}}) >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 12288 B across 2 region(s): A (map(tofrom: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), B (map(to: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (0.667x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: work.c:[[#LOOP15:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP14]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of A[i][j]: 'i' is shadowed in 'r_run', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j < m; j++)
      A[i][j] = A[i][j] * 0.5 + B[i][j];
  m = m - 3;
  // CHECK: work.c:[[#LOOP16:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if ((double)(n) * ((3 * (double)(m)) - {{[0-9]+}}) >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 12288 B across 2 region(s): B (map(tofrom: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), A (map(to: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (0.667x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: work.c:[[#LOOP17:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of B[i][j]: 'i' is shadowed in 'r_run', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j < m; j++)
      B[i][j] = B[i][j] * 0.25 + A[i][j];
}
// 5. triangular, enclosing time loop index in the bound.
void r_tri(int n) {
  // CHECK: work.c:[[#LOOP18:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 6 (6 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W A[i][j] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 6 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < n; k++) {
    // CHECK: work.c:[[#LOOP19:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : k, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): A (map(tofrom: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), B (map(to: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 32 && (__INTPTR_TYPE__)(n) - (__INTPTR_TYPE__)(0) >= 2); transfer 0.333x of its own maps per run (4 section copies over 2*t kernel runs per entry, t >= 2){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP20:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP19]]:5, 0 child loop(s), triangular{{$}}
      // CHECK: nest ivs   : k, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (k >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of A[i][j]: 'i' is shadowed in 'r_tri', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = 0; j <= k; j++)
        A[i][j] = A[i][j] * 0.5 + B[i][j];
    // CHECK: work.c:[[#LOOP21:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : k, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 12288 B across 2 region(s): B (map(tofrom: B[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem), A (map(to: A[0:(((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 511 ? 511 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)][0:512]), 4096 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 32 && (__INTPTR_TYPE__)(n) - (__INTPTR_TYPE__)(0) >= 2); transfer 0.333x of its own maps per run (4 section copies over 2*t kernel runs per entry, t >= 2){{$}}
    for (int i = 0; i < n; i++)
      // CHECK: work.c:[[#LOOP22:@LINE+13]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP21]]:5, 0 child loop(s), triangular{{$}}
      // CHECK: nest ivs   : k, i, j{{$}}
      // CHECK: accesses   : 3 (3 affine){{$}}
      // CHECK-NEXT: W B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R B[i][j] : [i][j]{{$}}
      // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
      // CHECK: - GPU offload declined: no map() section: the map() clause can't name a bound of B[i][j]: 'i' is shadowed in 'r_tri', so the check can't name it -> CPU_PARALLEL, which needs no map{{$}}
      for (int j = k; j < n; j++)
        B[i][j] = B[i][j] * 0.25 + A[i][j];
  }
}
// 6. a window: 4 iterations whatever i is (not n).
void r_window(int n) {
  // CHECK: work.c:[[#LOOP23:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: R y[i] : [i]{{$}}
  // CHECK-NEXT: R x[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 5 pair(s), 5 independent (5 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n - 4 >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n - 4; i++) {
    y[i] = 0.0;
    // CHECK: work.c:[[#LOOP24:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP23]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W y[i] : [i]{{$}}
    // CHECK-NEXT: R y[i] : [i]{{$}}
    // CHECK-NEXT: R x[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W y[i]: every iteration touches the same element{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = i; j < i + 4; j++)
      y[i] = y[i] + x[j] * 2.0;
  }
}

// A stencil of width 2r + 1: the inner loop's length doesn't depend on i.
void r_stencil(int n, int r, const double *restrict xs, double *restrict ys) {
  // CHECK: work.c:[[#LOOP25:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R xs[j] : [j]{{$}}
  // CHECK-NEXT: W ys[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'xs' and 'ys' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if ((double)(n - r) * (1 + 3 * (double)((2 * (__INTPTR_TYPE__)r + 1))) >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: can't map 'xs': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = r; i < n - r; i++) {
    double s = 0.0;
    // CHECK: work.c:[[#LOOP26:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R xs[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
    // CHECK: rejected   : fp-reduction{{$}}
    // CHECK: hint       : rerun with -fp-reductions to get reduction(+: s) (accepts results that differ in the last bits) [fixes fp-reduction]{{$}}
    // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = i - r; j <= i + r; j++)
      s += xs[j];
    ys[i] = s;
  }
}
