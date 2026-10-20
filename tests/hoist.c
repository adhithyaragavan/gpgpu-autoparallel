// RUN: %p05 -policy naive %s -- %cflags | FileCheck %s
// CHECK: Analyzed 53 loop(s).
// Phase 2.2: keeping data on the device across GPU loops (Hoisting.h). Each
// function is a region the tool should hoist into one `target data`, or a
// near miss it must refuse. Run under -policy naive, which hoists every
// region it may, so the regions' shapes are tested here; whether one pays
// for itself is tests/amortize.c. Also RUN by scripts/check_parallel.sh with
// OFFLOAD=1 and eval/runtime/hoist_main.c: on the host device with memory
// of its own, a missing `target update` or a loop left on the host inside
// the region reads stale data and the output differs.

static double relax(double x) {
  // CHECK: hoist.c:[[#LOOP0:@LINE+10]]:3{{$}}
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

// --- hoisted -----------------------------------------------------------------

// Time-step loop around two kernels: one region around the t loop.
void jacobi(double *restrict u, double *restrict v, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP1:@LINE+17]]:3{{$}}
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
    // CHECK: hoist.c:[[#LOOP2:@LINE+17]]:5{{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 3 && (int)(0) < (steps)){{$}}
    for (int i = 1; i < n - 1; i++)
      v[i] = relax(u[i - 1] + u[i + 1]);
    // CHECK: hoist.c:[[#LOOP3:@LINE+16]]:5{{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 3 && (int)(0) < (steps)){{$}}
    for (int i = 1; i < n - 1; i++)
      u[i] = relax(v[i]);
  }
}

// Two kernels in a row: the run is wrapped in { } with one region.
void pipeline(double *restrict a, double *restrict b, double *restrict c,
              int n) {
  // CHECK: hoist.c:[[#LOOP4:@LINE+16]]:3{{$}}
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
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
  for (int i = 0; i < n; i++)
    b[i] = relax(a[i]);
  // CHECK: hoist.c:[[#LOOP5:@LINE+16]]:3{{$}}
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
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): c (map(from: c[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
  for (int i = 0; i < n; i++)
    c[i] = relax(b[i]) * 2.0;
}

// A host statement reads the device's data every step: `target update
// from` before it.
double residual(double *restrict u, double *restrict r, int n, int steps) {
  double norm = 0.0;
  // CHECK: hoist.c:[[#LOOP6:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
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
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP7:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP6]]:3, 0 child loop(s){{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && (int)(0) < (steps)){{$}}
    for (int i = 0; i < n; i++)
      r[i] = relax(u[i]);
    // CHECK: hoist.c:[[#LOOP8:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP6]]:3, 0 child loop(s){{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && (int)(0) < (steps)){{$}}
    for (int i = 0; i < n; i++)
      u[i] = r[i] * 0.5;
    norm += u[0];
  }
  return norm;
}

// The host writes the boundary every step: `from` before, `to` after.
void boundary(double *restrict u, double *restrict w, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP9:@LINE+19]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 7 (7 affine){{$}}
  // CHECK-NEXT: W u[0] : [0]{{$}}
  // CHECK-NEXT: R u[1] : [1]{{$}}
  // CHECK-NEXT: W w[i] : [i]{{$}}
  // CHECK-NEXT: R u[i] : [i]{{$}}
  // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: W u[i] : [i]{{$}}
  // CHECK-NEXT: R w[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W u[0]: every iteration touches the same element{{$}}
  // CHECK: deps       : 11 pair(s), 3 independent (1 ZIV, 2 Banerjee){{$}}
  // CHECK: alias      : 'u' and 'w' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    u[0] = u[1] + 1.0;
    // CHECK: hoist.c:[[#LOOP10:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W w[i] : [i]{{$}}
    // CHECK-NEXT: R u[i] : [i]{{$}}
    // CHECK-NEXT: R u[i - 1] : [i - 1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'w' and 'u' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): w (map(from: w[1:((__INTPTR_TYPE__)n - 1)]), 8 B/elem), u (map(to: u[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 2{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 2 && (int)(0) < (steps)){{$}}
    for (int i = 1; i < n; i++)
      w[i] = relax(u[i] + u[i - 1]);
    // CHECK: hoist.c:[[#LOOP11:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W u[i] : [i]{{$}}
    // CHECK-NEXT: R w[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'u' and 'w' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): u (map(from: u[1:((__INTPTR_TYPE__)n - 1)]), 8 B/elem), w (map(to: w[1:((__INTPTR_TYPE__)n - 1)]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 2{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 2 && (int)(0) < (steps)){{$}}
    for (int i = 1; i < n; i++)
      u[i] = w[i];
  }
}

#define G (1 << 18)
double g_x[G], g_y[G];

// Globals, a while loop as the time step.
void global_steps(int steps) {
  int t = 0;
  while (t < steps) {
    // CHECK: hoist.c:[[#LOOP12:@LINE+14]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 262144{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W g_y[i] : [i]{{$}}
    // CHECK-NEXT: R g_x[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): g_y (map(from: g_y[0:262144]), 8 B/elem), g_x (map(to: g_x[0:262144]), 8 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data{{$}}
    for (int i = 0; i < G; i++)
      g_y[i] = relax(g_x[i]);
    // CHECK: hoist.c:[[#LOOP13:@LINE+13]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 262144{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W g_x[i] : [i]{{$}}
    // CHECK-NEXT: R g_y[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): g_x (map(from: g_x[0:262144]), 8 B/elem), g_y (map(to: g_y[0:262144]), 8 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data{{$}}
    for (int i = 0; i < G; i++)
      g_x[i] = g_y[i] * 0.5;
    t++;
  }
}

// --- refused ------------------------------------------------------------------

// n changes inside the time loop: the sections differ from step to step, so
// the time loop isn't hoisted; the two kernels inside still share a region
// each step.
void shrink(double *restrict a, double *restrict b, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP14:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - could not prove W b[i] independent{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP15:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP14]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'n' changes inside it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP16:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP14]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'n' changes inside it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
    n = n - 1;
  }
}

// Pointer swap: the bases change inside the loop. Refused before hoisting
// is even asked: a reassigned parameter may point anywhere (Phase 1.6).
void swap(double *a, double *b, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP17:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W b[i] : non-affine: base pointer 'b' is modified inside the loop{{$}}
  // CHECK-NEXT: R a[i] : non-affine: base pointer 'a' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot tell what W b[i] writes (base pointer 'b' is modified inside the loop){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP18:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP17]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 'b' is a pointer that may point into 'a' (W b[i], R a[i]){{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    double *tmp = a;
    a = b;
    b = tmp;
  }
}

static double host_sum(const double *p, int n) {
  double s = 0;
  // CHECK: hoist.c:[[#LOOP19:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R p[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

// Host code passes the array to a function: the time loop isn't hoisted;
// the kernels before that call still share a region each step.
double escape(double *restrict a, double *restrict b, int n, int steps) {
  double s = 0;
  // CHECK: hoist.c:[[#LOOP20:@LINE+18]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: host_sum reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R host_sum(): p[i] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP21:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP20]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'host_sum', which reaches memory through a pointer{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP22:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP20]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'host_sum', which reaches memory through a pointer{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
    s += host_sum(a, n);
  }
  return s;
}

// The loop condition reads mapped data on the host: the while loop isn't
// hoisted; the kernels inside still share a region each pass.
void cond_reads(double *restrict a, double *restrict b, int n) {
  while (a[0] < 100.0) {
    // CHECK: hoist.c:[[#LOOP23:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: a while loop around a GPU loop may not run, and a pointer's section is owned memory only when the loop touches it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]) + 1.0;
    // CHECK: hoist.c:[[#LOOP24:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 24 B across 2 region(s): a (map(tofrom: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: a while loop around a GPU loop may not run, and a pointer's section is owned memory only when the loop touches it{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i] + a[i];
  }
}

// a and b may overlap: one region can't map both.
void maybe_alias(double *a, double *b, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP25:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP26:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s){{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'a' and 'b' may overlap, and one region can't map both{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'a' and 'b' may overlap, and one region can't map both{{$}}
    for (int i = 0; i < n; i++)
      a[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP27:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 1 region(s): b (map(tofrom: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'a' and 'b' may overlap, and one region can't map both{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'a' and 'b' may overlap, and one region can't map both{{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(b[i]);
  }
}

// One GPU loop, run once: nothing to save.
void once(double *restrict a, const double *restrict b, int n) {
  // CHECK: hoist.c:[[#LOOP28:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    a[i] = relax(b[i]);
}

// --- adversarial review cases --------------------------------------------------

#define H 4096
double h_x[H], h_y[H];
double h_acc;

static void h_helper(void) {
  h_x[0] = 100.0;
  // CHECK: hoist.c:[[#LOOP29:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 4096{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W h_y[i] : [i]{{$}}
  // CHECK-NEXT: R h_x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): h_y (map(from: h_y[0:4096]), 8 B/elem), h_x (map(to: h_x[0:4096]), 8 B/elem){{$}}
  for (int i = 0; i < H; i++)
    h_y[i] = relax(h_x[i]);
  h_acc += h_y[5];
}

// Host code calls a function that offloads its own loop over the mapped
// globals: that loop would find them present and use the stale device
// copy. Refused at the time loop.
void calls_offloader(int steps) {
  // CHECK: hoist.c:[[#LOOP30:@LINE+21]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: h_helper reaches 1 function(s), max depth 1: relax@1{{$}}
  // CHECK: accesses   : 9 (9 affine){{$}}
  // CHECK-NEXT: W h_x[i] : [i]{{$}}
  // CHECK-NEXT: R h_x[i] : [i]{{$}}
  // CHECK-NEXT: W h_y[i] : [i]{{$}}
  // CHECK-NEXT: R h_y[i] : [i]{{$}}
  // CHECK-NEXT: R h_x[i] : [i]{{$}}
  // CHECK-NEXT: W h_helper(): h_x[0] : [0] of 'h_x'{{$}}
  // CHECK-NEXT: W h_helper(): h_y[i] : [i] of 'h_y'{{$}}
  // CHECK-NEXT: R h_helper(): h_x[i] : [i] of 'h_x'{{$}}
  // CHECK-NEXT: R h_helper(): h_y[5] : [5] of 'h_y'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'h_helper' writes global 'h_acc'{{$}}
  // CHECK: deps       : 16 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP31:@LINE+15]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4096{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP30]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W h_x[i] : [i]{{$}}
    // CHECK-NEXT: R h_x[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 1 region(s): h_x (map(tofrom: h_x[0:4096]), 8 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'h_helper', which offloads loops of its own{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'h_helper', which offloads loops of its own{{$}}
    for (int i = 0; i < H; i++)
      h_x[i] = relax(h_x[i]);
    h_helper();
    // CHECK: hoist.c:[[#LOOP32:@LINE+15]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4096{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP30]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W h_y[i] : [i]{{$}}
    // CHECK-NEXT: R h_y[i] : [i]{{$}}
    // CHECK-NEXT: R h_x[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 24 B across 2 region(s): h_y (map(tofrom: h_y[0:4096]), 8 B/elem), h_x (map(to: h_x[0:4096]), 8 B/elem){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'h_helper', which offloads loops of its own{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code calls 'h_helper', which offloads loops of its own{{$}}
    for (int i = 0; i < H; i++)
      h_y[i] = h_y[i] + h_x[i];
  }
}

struct holder { double *d; };

// Host code reads through a pointer loaded from a struct (it points at a):
// refused. (Phase 2.5: s.d is a field path; `restrict` on a says nothing
// about it, since it holds a pointer based on a.)
double loaded_ptr(double *restrict a, double *restrict b, int n, int steps) {
  struct holder s = {a};
  double r = 0.0;
  // CHECK: hoist.c:[[#LOOP33:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R s.d[3] : [3]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: alias      : 'a' and 's.d' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP34:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP33]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code reaches memory through a pointer it loaded or computed{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP35:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP33]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code reaches memory through a pointer it loaded or computed{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
    r += s.d[3];
  }
  return r;
}

// Host code reads through a local pointer into a: refused.
double local_ptr(double *restrict a, double *restrict b, int n, int steps) {
  double *p = a + 1;
  double r = 0.0;
  // CHECK: hoist.c:[[#LOOP36:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R p[0] : [1] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 5 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP37:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP36]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code uses pointer 'p', which may point into mapped data{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP38:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP36]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: host code uses pointer 'p', which may point into mapped data{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
    r += p[0];
  }
  return r;
}

int h_n = 64;
int *h_np = &h_n;

// The bound h_n is a global whose address is taken (h_np): it may change
// through a pointer inside the region, so it can't be in C. Refused.
void escaped_bound(double *restrict a, double *restrict b, int steps) {
  // CHECK: hoist.c:[[#LOOP39:@LINE+21]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (4 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W *h_np : non-affine: base pointer 'h_np' could be modified by a call inside the loop (global or address-taken){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W *h_np writes (base pointer 'h_np' could be modified by a call inside the loop (global or address-taken)){{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: alias      : 'b' and 'h_n' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'b' and 'h_np' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'a' and 'h_n' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'a' and 'h_np' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP40:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP39]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: alias      : 'b' and 'h_n' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : CPU_PARALLEL{{$}}
    // CHECK: - GPU offload declined: no map() section: the range of b[i] can't be bounded -> CPU_PARALLEL, which needs no map{{$}}
    for (int i = 0; i < h_n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP41:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP39]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: alias      : 'a' and 'h_n' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:h_n]), 8 B/elem), b (map(to: b[0:h_n]), 8 B/elem){{$}}
    // CHECK: - map guard: h_n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: the address of global 'h_n' is taken, so it may change through a pointer{{$}}
    for (int i = 0; i < h_n; i++)
      a[i] = b[i];
    if (t == 1)
      *h_np = 4;
  }
}

// steps = 0 with NULL arrays: the region must not map anything, so the
// time loop's first test (steps > 0) is part of C.
void zero_steps(double *restrict a, double *restrict b, int n, int steps) {
  // CHECK: hoist.c:[[#LOOP42:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP43:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP42]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && (int)(0) < (steps)){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP44:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP42]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1 && (int)(0) < (steps)){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
  }
}

// A GPU loop under an if: its section is owned memory only when it runs, so
// the time loop isn't hoisted.
void under_if(double *restrict a, double *restrict b, int n, int m,
              int steps) {
  // CHECK: hoist.c:[[#LOOP45:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP46:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP45]]:3, 0 child loop(s){{$}}
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
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: a GPU loop in it runs only under a condition{{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: a GPU loop in it runs only under a condition{{$}}
    for (int i = 0; i < n; i++)
      a[i] = relax(a[i]);
    if (m <= n)
      // CHECK: hoist.c:[[#LOOP47:@LINE+16]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP45]]:3, 0 child loop(s){{$}}
      // CHECK: nest ivs   : t, i{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: W b[i] : [i]{{$}}
      // CHECK-NEXT: R a[i] : [i]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
      // CHECK: profitability : GPU_OFFLOAD{{$}}
      // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:m]), 8 B/elem), a (map(to: a[0:m]), 8 B/elem){{$}}
      // CHECK: - map guard: m >= 1{{$}}
      // CHECK: - data region (line {{[0-9]+}}): not hoisted: a GPU loop in it runs only under a condition{{$}}
      // CHECK: - data region (line {{[0-9]+}}): not hoisted: a GPU loop in it runs only under a condition{{$}}
      for (int i = 0; i < m; i++)
        b[i] = a[i] * 2.0;
  }
}

// A typedef in the run would end with the new { }: refused.
double typedef_run(double *restrict a, double *restrict b, int n) {
  // CHECK: hoist.c:[[#LOOP48:@LINE+16]]:3{{$}}
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
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: it declares a type or tag, which the new { } would hide from what follows{{$}}
  for (int i = 0; i < n; i++)
    b[i] = relax(a[i]);
  typedef double real;
  // CHECK: hoist.c:[[#LOOP49:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - data region (line {{[0-9]+}}): not hoisted: it declares a type or tag, which the new { } would hide from what follows{{$}}
  for (int i = 0; i < n; i++)
    a[i] = relax(b[i]);
  real z = a[0];
  return z;
}

// A loop hint on the time loop: no pragma can go between them, so the
// region goes inside its body instead.
void hinted(double *restrict a, double *restrict b, int n, int steps) {
#pragma clang loop unroll(disable)
  // CHECK: hoist.c:[[#LOOP50:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 4 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++) {
    // CHECK: hoist.c:[[#LOOP51:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP50]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: relax reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'b' and 'a' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): b (map(from: b[0:n]), 8 B/elem), a (map(to: a[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      b[i] = relax(a[i]);
    // CHECK: hoist.c:[[#LOOP52:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP50]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
    // CHECK: - map guard: n >= 1{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data if(n >= 1){{$}}
    for (int i = 0; i < n; i++)
      a[i] = b[i];
  }
}

