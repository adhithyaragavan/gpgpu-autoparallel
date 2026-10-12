// RUN: %p05 -fp-reductions %s -- %cflags | FileCheck %s
// CHECK: Analyzed 2 loop(s).
// With -fp-reductions, floating-point reductions are allowed (the parallel
// result may differ from the sequential one in the last bits).

// SAFE, reduction(+: s)
double fp_sum(const double *a, int n) {
  double s = 0;
  // CHECK: fp_reductions.c:[[#LOOP0:@LINE+11]]:3{{$}}
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

// SAFE, reduction(max: m)
double fp_max(const double *a, int n) {
  double m = 0;
  // CHECK: fp_reductions.c:[[#LOOP1:@LINE+12]]:3{{$}}
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
    m = a[i] > m ? a[i] : m;
  return m;
}
