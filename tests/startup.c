// RUN: %p05 -machine=%S/Inputs/startup.json %s -- %cflags | FileCheck %s
// CHECK: Analyzed 8 loop(s).
// CHECK: machine: test-startup ({{.*}}){{$}}
// CHECK: startup costs (runtime 400.000 us, device 100000.000 us) priced: main() is in this file{{$}}
// Phase 2.3: once-per-process startup costs (Profitability.h,
// chargeStartup). This file defines main, so the whole program is visible:
// the runtime's start (400 us in Inputs/startup.json) is split across the
// host constructs, the device's (100 ms) across the GPU ones, each then
// across the runs constant trip counts around the loop show.

#define N (1 << 16)
#define BIG (1 << 24)
double a[N], b[N], c[N];
double big_x[BIG], big_y[BIG];

static double heavy(double x) {
  // CHECK: startup.c:[[#LOOP0:@LINE+10]]:3{{$}}
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

// Threading this loop saves about 150 us, less than its share of the
// runtime's start (400 us / 2 host constructs): sequential.
void small_cpu(void) {
  // CHECK: startup.c:[[#LOOP1:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 65536{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - startup (main() in this file): runtime 400.000 us / 3 construct(s), device 100000.000 us / 3, over 1 run(s): +133.333 us on t_cpu, +33333.333 us on t_gpu{{$}}
  for (int i = 0; i < N; i++)
    a[i] = a[i] * 2.0 + 1.0;
}

// Run 1000 times by the step loop around it: each run pays 1/1000 of its
// share, so threading wins (and the step loop is hoisted, 1000 runs
// sharing 3 section copies: GPU).
void stepped_cpu(void) {
  // CHECK: startup.c:[[#LOOP2:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 1000{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R c[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W b[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < 1000; t++)
    // CHECK: startup.c:[[#LOOP3:@LINE+15]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 65536{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : t, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W b[i] : [i]{{$}}
    // CHECK-NEXT: R b[i] : [i]{{$}}
    // CHECK-NEXT: R c[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : GPU_OFFLOAD{{$}}
    // CHECK: - data/iteration: 24 B across 2 region(s): b (map(tofrom: b[0:65536]), 8 B/elem), c (map(to: c[0:65536]), 8 B/elem){{$}}
    // CHECK: - startup (main() in this file): runtime 400.000 us / 3 construct(s), device 100000.000 us / 3, over 1000 run(s): +0.133 us on t_cpu, +33.333 us on t_gpu{{$}}
    // CHECK: - data region (line {{[0-9]+}}): resident in target data; transfer 0.001x of its own maps per run (3 section copies over 1000 kernel runs per entry); GPU_OFFLOAD there: its share of the region's transfer makes it the cheapest{{$}}
    for (int i = 0; i < N; i++)
      b[i] = b[i] * 0.5 + c[i];
}

// GPU-worthy on its own (above a guard) but the device's start is 100 ms
// shared by one construct: the guard rises far above the default's.
void heavy_gpu(double *restrict x, double *restrict y, int n) {
  // CHECK: startup.c:[[#LOOP4:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: heavy reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'y' and 'x' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): y (map(from: y[0:n]), 8 B/elem), x (map(to: x[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  // CHECK: - startup (main() in this file): runtime 400.000 us / 4 construct(s), device 100000.000 us / 2, over 1 run(s): +100.000 us on t_cpu, +50000.000 us on t_gpu{{$}}
  for (int i = 0; i < n; i++)
    y[i] = heavy(x[i]);
}

// Constant and heavy: 2^24 elements of 1600 ops pay for the device's start
// even alone.
void big_gpu(void) {
  // CHECK: startup.c:[[#LOOP5:@LINE+14]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 16777216{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: heavy reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W big_y[i] : [i]{{$}}
  // CHECK-NEXT: R big_x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD{{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): big_y (map(from: big_y[0:16777216]), 8 B/elem), big_x (map(to: big_x[0:16777216]), 8 B/elem){{$}}
  // CHECK: - startup (main() in this file): runtime 400.000 us / 4 construct(s), device 100000.000 us / 2, over 1 run(s): +100.000 us on t_cpu, +50000.000 us on t_gpu{{$}}
  for (int i = 0; i < BIG; i++)
    big_y[i] = heavy(big_x[i]);
}

// Symbolic and light: threading pays above a guard, which its share of
// the runtime's start raises (tests/machine.test pins the number).
void cpu_n(double *restrict x, int n) {
  // CHECK: startup.c:[[#LOOP6:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - startup (main() in this file): runtime 400.000 us / 3 construct(s), device 100000.000 us / 3, over 1 run(s): +133.333 us on t_cpu, +33333.333 us on t_gpu{{$}}
  for (int i = 0; i < n; i++)
    x[i] = x[i] * 2.0 + 1.0;
}

// Never called from main: it runs no construct, so it doesn't lower the
// others' shares.
void unused(void) {
  // CHECK: startup.c:[[#LOOP7:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 65536{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  // CHECK: - startup (main() in this file): runtime 400.000 us / 4 construct(s), device 100000.000 us / 3, over 1 run(s): +100.000 us on t_cpu, +33333.333 us on t_gpu{{$}}
  for (int i = 0; i < N; i++)
    c[i] = c[i] * 3.0 + a[i];
}

int main(void) {
  small_cpu();
  stepped_cpu();
  heavy_gpu(a, b, N);
  big_gpu();
  cpu_n(c, N);
  return (int)(a[0] + b[0] + big_y[0]);
}
