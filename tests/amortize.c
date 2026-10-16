// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 30 loop(s).
// Phase 2.3: a hoisted region's transfer shared across its kernel runs
// (Hoisting.h). Each loop in a region pays (section copies per entry) /
// (kernel runs per entry), relative to its own maps; a trip count not known
// at compile time counts as 1 run (or as many as possible, whichever costs
// more). Default machine model; tests/hoist.c covers how regions are built.

static double relax(double x) {
  // CHECK: amortize.c:[[#LOOP0:@LINE+10]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 8{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 0 (0 affine){{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < 8; k++)
    x = 0.5 * x + 0.25;
  return x;
}

// 100 steps of two kernels: 4 section copies over 200 kernel runs. Each
// loop alone stays on the host (transfer-bound), but its share of the
// region's transfer makes the GPU the cheapest: hoisted.
void const_steps(double *restrict u, double *restrict v, int n) {
  // CHECK: amortize.c:[[#LOOP1:@LINE+17]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 100{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W v[i] : [i]{{$}}
  // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: W u[i] : [i]{{$}}
  // CHECK-NEXT: R v[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W v[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 100; t++) {
    // CHECK: amortize.c:[[#LOOP2:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP1]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W v[i] : [i]{{$}}
    // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
    // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): v (map(from: v[1:((__INTPTR_TYPE__)n - 2)]), 8 B/elem), u (map(to: u[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 3{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 3 && n - 1 >= 8192 && (int)(0) < (100)); transfer 0.01x of its own maps per run (4 section copies over 200 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 1; i < n - 1; i++)
      v[i] = relax(u[i - 1] + u[i + 1]);
    // CHECK: amortize.c:[[#LOOP3:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP1]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W u[i] : [i]{{$}}
    // CHECK-NEXT: R v[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'u' and 'v' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): u (map(from: u[1:((__INTPTR_TYPE__)n - 2)]), 8 B/elem), v (map(to: v[1:((__INTPTR_TYPE__)n - 2)]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 3{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 3 && n - 1 >= 8192 && (int)(0) < (100)); transfer 0.01x of its own maps per run (4 section copies over 200 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 1; i < n - 1; i++)
      u[i] = relax(v[i]);
  }
}

// The same with the step count a parameter: it may be 1, so the region
// saves nothing and the loops stay on the host.
void param_steps(double *restrict u, double *restrict v, int n, int steps) {
  // CHECK: amortize.c:[[#LOOP4:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W v[i] : [i]{{$}}
  // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: W u[i] : [i]{{$}}
  // CHECK-NEXT: R v[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W v[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: amortize.c:[[#LOOP5:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W v[i] : [i]{{$}}
    // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
    // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (int i = 1; i < n - 1; i++)
      v[i] = relax(u[i - 1] + u[i + 1]);
    // CHECK: amortize.c:[[#LOOP6:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W u[i] : [i]{{$}}
    // CHECK-NEXT: R v[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'u' and 'v' can't overlap (restrict){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (int i = 1; i < n - 1; i++)
      u[i] = relax(v[i]);
  }
}

// 100 steps, and the host reads one array every step: 2 + 2 + 100 copies
// over 200 runs, still about a quarter of the loops' own maps: hoisted,
// with a `target update from` each step.
double host_reads(double *restrict u, double *restrict r, int n) {
  double norm = 0.0;
  // CHECK: amortize.c:[[#LOOP7:@LINE+17]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 100{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W r[i] : [i]{{$}}
  // CHECK-NEXT: R u[i] : [i]{{$}}
  // CHECK-NEXT: W u[i] : [i]{{$}}
  // CHECK-NEXT: R r[i] : [i]{{$}}
  // CHECK-NEXT: R u[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W r[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'r' and 'u' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 100; t++) {
    // CHECK: amortize.c:[[#LOOP8:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W r[i] : [i]{{$}}
    // CHECK-NEXT: R u[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'r' and 'u' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): r (map(from: r[0:n]), 8 B/elem), u (map(to: u[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 16384 && (int)(0) < (100)); transfer 0.26x of its own maps per run (104 section copies over 200 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 0; i < n; i++)
      r[i] = relax(u[i]);
    // CHECK: amortize.c:[[#LOOP9:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W u[i] : [i]{{$}}
    // CHECK-NEXT: R r[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'u' and 'r' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): u (map(from: u[0:n]), 8 B/elem), r (map(to: r[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 16384 && (int)(0) < (100)); transfer 0.26x of its own maps per run (104 section copies over 200 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 0; i < n; i++)
      u[i] = r[i] * 0.5;
    norm += u[0];
  }
  return norm;
}

static double heavy(double x) {
  // CHECK: amortize.c:[[#LOOP10:@LINE+10]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 400{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 0 (0 affine){{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < 400; k++)
    x = x * 0.999 + 0.001;
  return x;
}

// One kernel heavy enough for the GPU on its own (above a size guard), but
// the host reads and writes both arrays every step: 4 + 4 copies per run
// against its own 2, so the region would move more than per-loop maps.
// Refused; the loop keeps its own maps.
void host_rw(double *restrict a, double *restrict b, int n) {
  // CHECK: amortize.c:[[#LOOP11:@LINE+19]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 100{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: heavy reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 7 (7 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[0] : [0]{{$}}
  // CHECK-NEXT: R b[0] : [0]{{$}}
  // CHECK-NEXT: R a[1] : [1]{{$}}
  // CHECK-NEXT: W b[1] : [1]{{$}}
  // CHECK-NEXT: R a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 9 pair(s), 2 independent (2 ZIV){{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 100; t++) {
    // CHECK: amortize.c:[[#LOOP12:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP11]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: heavy reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: it would copy more than its loops' own maps (604 section copies per entry against 200){{$}}
    for (int i = 0; i < n; i++)
      b[i] = heavy(a[i]);
    a[0] = b[0] + a[1];
    b[1] = a[0];
  }
}

// Two kernels in a row: b and c are written, so the region maps both
// tofrom: 5 copies over 2 runs, more than the loops' own 4. Not hoisted.
void pipeline(double *restrict a, double *restrict b, double *restrict c,
              int n) {
  // CHECK: amortize.c:[[#LOOP13:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
  for (int i = 0; i < n; i++)
    b[i] = relax(a[i]);
  // CHECK: amortize.c:[[#LOOP14:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
  for (int i = 0; i < n; i++)
    c[i] = relax(b[i]) * 2.0;
}

// Nested constant steps, inner kernels: 1000 runs share 2 copies.
#define N2 (1 << 20)
double g_a[N2];
void nested_steps(void) {
  // CHECK: amortize.c:[[#LOOP15:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 10{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : s{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: R g_a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W g_a[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int s = 0; s < 10; s++)
    // CHECK: amortize.c:[[#LOOP16:@LINE+12]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 100{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP15]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : s, t{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W g_a[i] : [i]{{$}}
    // CHECK-NEXT: R g_a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W g_a[i]: every iteration touches the same element{{$}}
    // CHECK: deps       : 2 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int t = 0; t < 100; t++)
      // CHECK: amortize.c:[[#LOOP17:@LINE+13]]:7{{$}}
      // CHECK: kind       : CONSTANT_BOUND{{$}}
      // CHECK: trip count : 1048576{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP16]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : s, t, i{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W g_a[i] : [i]{{$}}
      // CHECK-NEXT: R g_a[i] : [i]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : GPU_OFFLOAD{{$}}
      // CHECK: - data/iteration: 16 B across 1 region(s): g_a (map(tofrom: g_a[0:1048576]), 8 B/elem){{$}}
      // CHECK: - data region (line {{[0-9]+}}): resident in target data; transfer 0.001x of its own maps per run (2 section copies over 1000 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
      for (int i = 0; i < N2; i++)
        g_a[i] = g_a[i] * 0.5 + 1.0;
}

// A convergence loop: 1000 steps at most, but it can stop after any, so
// it counts as an unknown number (1 at worst): not hoisted.
void converge(double *restrict u, double *restrict v, int n, double tol) {
  // CHECK: amortize.c:[[#LOOP18:@LINE+17]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 1000{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W v[i] : [i]{{$}}
  // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: W u[i] : [i]{{$}}
  // CHECK-NEXT: R v[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop body can 'break' out of the loop{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 1000; t++) {
    // CHECK: amortize.c:[[#LOOP19:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W v[i] : [i]{{$}}
    // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
    // CHECK-NEXT: R u[i + 1] : [i + 1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'v' and 'u' can't overlap (restrict){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (int i = 1; i < n - 1; i++)
      v[i] = relax(u[i - 1] + u[i + 1]);
    // CHECK: amortize.c:[[#LOOP20:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W u[i] : [i]{{$}}
    // CHECK-NEXT: R v[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'u' and 'v' can't overlap (restrict){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: with the transfer shared across its kernel runs, no loop in it is cheapest on the GPU{{$}}
    for (int i = 1; i < n - 1; i++)
      u[i] = relax(v[i]);
    if (tol > 0.5)
      break;
  }
}

// A heavy GPU loop and a cheap one reading its result: the cheap one stays
// on the host even with its share, so per-loop maps would copy only x
// (2) against the region's 4. Not hoisted.
void drag(double *restrict x, double *restrict c, int n) {
  // CHECK: amortize.c:[[#LOOP21:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: heavy reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 1 region(s): x (map(tofrom: x[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: it would copy more than its loops' own maps (4 section copies per entry against 2){{$}}
  for (int i = 0; i < n; i++)
    x[i] = heavy(x[i]);
  // CHECK: amortize.c:[[#LOOP22:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R c[i] : [i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'c' and 'x' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: it would copy more than its loops' own maps (4 section copies per entry against 2){{$}}
  for (int i = 0; i < n; i++)
    c[i] = c[i] * 0.5 + x[i];
}

// 100 runs, then s*s runs with a host write after every s: 2 + 2s copies
// over 100 + s^2 runs. That peaks near s = 10 (about 0.055 per run,
// against 0.02 at s = 1 or as s grows), so the ratio is sampled, not taken
// at the ends.
void peak(double *restrict a, int n, int s) {
  // CHECK: amortize.c:[[#LOOP23:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 100{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 100; t++)
    // CHECK: amortize.c:[[#LOOP24:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP23]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 1 region(s): a (map(tofrom: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 8192 && (int)(0) < (100) && (int)(0) < (s)); transfer 0.0549x of its own maps per run (2*t + 2 section copies over t^2 + 100 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 0; i < n; i++)
      a[i] = relax(a[i]);
  // CHECK: amortize.c:[[#LOOP25:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : p{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: RW a[0] : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int p = 0; p < s; p++) {
    // CHECK: amortize.c:[[#LOOP26:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : p, q{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W a[i]: every iteration touches the same element{{$}}
    // CHECK: deps       : 2 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int q = 0; q < s; q++)
      // CHECK: amortize.c:[[#LOOP27:@LINE+15]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP26]]:5, 0 child loop(s){{$}}
      // CHECK: nest ivs   : p, q, i{{$}}
      // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W a[i] : [i]{{$}}
      // CHECK-NEXT: R a[i] : [i]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
      // CHECK: profitability : GPU_OFFLOAD{{$}}
      // CHECK: - data/iteration: 16 B across 1 region(s): a (map(tofrom: a[0:n]), 8 B/elem){{$}}
      // CHECK: - map guard: n >= 1{{$}}
      // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && n >= 8192 && (int)(0) < (100) && (int)(0) < (s)); transfer 0.0549x of its own maps per run (2*t + 2 section copies over t^2 + 100 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
      for (int i = 0; i < n; i++)
        a[i] = relax(a[i]);
    a[0] += 1.0;
  }
}

// y = f(i) alone maps only y out (1 section) and just beats the host on
// the GPU; in a region y maps both ways, its share is 2 sections per run,
// and it no longer does. Not hoisted: it keeps its own maps.
void own_gpu(double *restrict y, double *restrict z, int n) {
  // CHECK: amortize.c:[[#LOOP28:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 8 B across 1 region(s): y (map(from: y[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (2x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    y[i] = relax((double)i);
  // CHECK: amortize.c:[[#LOOP29:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W z[i] : [i]{{$}}
  // CHECK-NEXT: R y[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'z' and 'y' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: a loop that picks the GPU with its own maps no longer does with its share of the region's transfer (2x of its own maps per run){{$}}
  for (int i = 0; i < n; i++)
    z[i] = relax(y[i]);
}
