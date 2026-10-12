// RUN: %p05 -policy naive %s -- %cflags | FileCheck %s
// CHECK: Analyzed 11 loop(s).
// Phase 1.3 GPU-map interlock (Profitability.cpp, LoopSafety::GpuMappable).
// Run under -policy naive, which tries to offload EVERY safe loop: the map()
// clauses this version writes are base[0:bound], so offload must be declined
// for any SAFE loop whose accesses that region doesn't cover. Each loop states
// what must happen.

int g_scale;
static double total(const double *p, int n) {
  double s = 0;
  // CHECK: interlock.c:[[#LOOP0:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R p[k] : [k]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < n; k++)
    s += p[k];
  return s;
}
static double scaled(double x) { return x * g_scale; }

// Exactly a[i] from 0: GPU_OFFLOAD (naive), the one shape maps cover.
void exact(double *a, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP1:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i] * 2.0;
}

// Reads b[i + 1]: b[0:n] would miss b[n]. Declined.
void shifted(double *a, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP2:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i + 1];
}

// 2-D through a[i * n + j]: a[0:n] would cover only the first row. Declined.
void two_d(double *a, int n) {
  // CHECK: interlock.c:[[#LOOP3:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: interlock.c:[[#LOOP4:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP3]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL{{$}}
    // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 1.0;
}

// Starts at -4: a[0:n] misses a[-4..-1]. Declined.
void negative_start(double *a, int n) {
  // CHECK: interlock.c:[[#LOOP5:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = -4; i < n; i++)
    a[i] = a[i] * 2.0;
}

// The callee reads all of a through a pointer argument; no map for a.
// Declined.
void pointer_argument(double *b, const double *a, int n) {
  // CHECK: interlock.c:[[#LOOP6:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: total reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    b[i] = total(a, n);
}

// The callee reads a global that no map clause covers. Declined.
void callee_global(double *a, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP7:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: scaled reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    a[i] = scaled(b[i]);
}

// Increment of the exact element: GPU_OFFLOAD, and a must be mapped tofrom
// (the device's writes come back), not to.
void increment(double *a, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP8:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: RW a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 5 pair(s), 5 independent (5 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 24 B across 2 region(s): a (map(tofrom: a[0:n]), 8 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  for (int i = 0; i < n; i++) {
    a[i]++;
    a[i] = a[i] + b[i];
  }
}

// A member store writes part of the element: s must be mapped tofrom.
struct pt2 { double x, y; };
void member_store(struct pt2 *s, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP9:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W s[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 40 B across 2 region(s): s (map(tofrom: s[0:n]), 16 B/elem), b (map(to: b[0:n]), 8 B/elem){{$}}
  for (int i = 0; i < n; i++)
    s[i].x = b[i] * 2.0;
}

// *(b + i) is not subscript syntax, so no region would be collected for b.
// Declined.
void deref_form(double *a, const double *b, int n) {
  // CHECK: interlock.c:[[#LOOP10:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R *(b + i) : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - GPU offload declined: map() regions are only sized correctly when every access is exactly base[i] (range-based maps are Phase 2.1) -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    a[i] = *(b + i) * 2.0;
}
