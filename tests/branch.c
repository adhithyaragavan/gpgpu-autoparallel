// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 13 loop(s).
// Phase 3.2 (-gpu-alias-checks): a GPU loop whose bases are only checked
// apart at run time runs as `{ if (check) { <GPU loop> } else { <original
// loop, with the guarded CPU pragma when threading pays> } }`. The check
// covers the accesses (AliasGuard) and the map() sections (SectionApart):
// read-only pairs too, whose sections may still partly overlap. Loops that can't be copied keep the CPU pragma. Cases from
// the review: a read-only pair called with b == a + 1, a copy whose
// sections overlap though its accesses don't, interleaved `restrict`
// pointers into one buffer (a known gap: its sections aren't tested, so the
// driver calls it only with separate buffers). Runtime: eval/runtime/branch_main.c (also
// OFFLOAD=1, naive and gated); tests/branch.test pins the rewrites.

// 1. read-only pair: sections must be apart (b == a + 1 in the driver).
void b_add3(double *c, const double *a, const double *b, int n) {
  // CHECK: branch.c:[[#LOOP0:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'c' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0) && ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    c[i] = a[i] + b[i] * 3.0;
}

// 2. accesses apart (c[1..n), a[1..n)), sections c[1:n-1] a[1:n-1] maybe not.
void b_cp1(double *c, const double *a, int n) {
  // CHECK: branch.c:[[#LOOP1:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 8 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 8){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 1; i < n; i++)
    c[i] = a[i] * 2.0;
}

// 3. restrict, interleaved in one buffer: legal, hulls overlap.
void b_inter(double *restrict a, const double *restrict b, int n) {
  // CHECK: branch.c:[[#LOOP2:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[2 * i] : [2*i]{{$}}
  // CHECK-NEXT: R b[2 * i + 1] : [2*i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[2 * i] = b[2 * i + 1] * 2.0;
}

// 4. a kernel: rows of two pointer parameters.
void b_rows(int n, double (*c)[64], double (*a)[64]) {
  // CHECK: branch.c:[[#LOOP3:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W c[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R a[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R a[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R c[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 63 + 1) <= (__INTPTR_TYPE__)a + 8 * ((0) * 64 + 0) || (__INTPTR_TYPE__)a + 8 * ((((__INTPTR_TYPE__)n - 1)) * 64 + 63 + 1) <= (__INTPTR_TYPE__)c + 8 * ((0) * 64 + 0)){{$}}
  // CHECK: collapse   : no, the inner loop needs its own run-time alias check{{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: branch.c:[[#LOOP4:@LINE+15]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP3]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 4 (4 affine){{$}}
    // CHECK-NEXT: W c[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R a[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R a[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R c[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)i) * 64 + 63 + 1) <= (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)i) * 64 + 0) || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)i) * 64 + 63 + 1) <= (__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)i) * 64 + 0)){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 64; j++)
      c[i][j] = a[i][j] * a[i][j] + c[i][j];
}

// 5. a label in the body: no copy, the guarded CPU pragma.
void b_label(double *c, const double *a, int n) {
  // CHECK: branch.c:[[#LOOP5:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
  here:
    c[i] = a[i] + 1.0;
  }
}

// 6. ends in a macro: no copy either.
#define STORE(i) c[i] = a[i] - 1.0
void b_macro(double *c, const double *a, int n) {
  // CHECK: branch.c:[[#LOOP6:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    STORE(i);
}

// 7. inside another loop: the guards go in plain ifs for the CPU loop too
// (a false if(parallel:) still enters a serialized parallel region, once per
// iteration of r), and naive's GPU branch gets `else if (guard) { CPU loop }`.
void b_nest(double *c, const double *a, int m, int n) {
  // CHECK: branch.c:[[#LOOP7:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : r{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: RW c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - RW c[i]: every iteration touches the same element{{$}}
  // CHECK: rejected   : array-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int r = 0; r < m; r++)
    // CHECK: branch.c:[[#LOOP8:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : r, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: RW c[i] : [i]{{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'c' and 'a' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int i = 0; i < n; i++)
      c[i] += a[i] * r;
}

// 8. a box loop in a time loop: its guard calls an inspector helper, which
// must still be defined when the guard sits in a plain if (review).
#define MAXATOMS 64
void b_insp(double (*restrict p)[3], const int *restrict n, int nb, int steps) {
  // CHECK: branch.c:[[#LOOP9:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : t{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: R p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: R p[o][1] : [64*b + i][1]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 ZIV){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int t = 0; t < steps; t++)
    // CHECK: branch.c:[[#LOOP10:@LINE+18]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 1 child loop(s), perfect nest{{$}}
    // CHECK: nest ivs   : t, b{{$}}
    // CHECK: accesses   : 4 (4 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: R p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: R p[o][1] : [64*b + i][1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 3 pair(s), 3 independent (1 ZIV, 2 Banerjee){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: alias      : 'i' stays below 64: its loop's bound checked at run time{{$}}
    // CHECK: alias guard: (((__INTPTR_TYPE__)nb - 1) >= 0 && p05_all_le_int(n, 0, ((__INTPTR_TYPE__)nb - 1), 64)){{$}}
    // CHECK: collapse   : no, the inner loop steps several counters{{$}}
    // CHECK: profitability : CPU_PARALLEL if (nb >= {{[0-9]+}}){{$}}
    // CHECK: - GPU offload declined: pointer disjointness is only checked at run time -> CPU_PARALLEL, guarded by the check{{$}}
    for (int b = 0; b < nb; b++)
      // CHECK: branch.c:[[#LOOP11:@LINE+14]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 2, parent [[#LOOP10]]:5, 0 child loop(s), triangular{{$}}
      // CHECK: nest ivs   : t, b, i{{$}}
      // CHECK: accesses   : 4 (4 affine){{$}}
      // CHECK-NEXT: R n[b] : [b]{{$}}
      // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
      // CHECK-NEXT: R p[o][0] : [64*b + i][0]{{$}}
      // CHECK-NEXT: R p[o][1] : [64*b + i][1]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 3 pair(s), 3 independent (3 SIV){{$}}
      // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
        p[o][0] = p[o][0] * 1.0001 + 3.0 * p[o][1];
}

// 9. a conditional directive that closes after the loop's last token: a copy
// would split it, so no branch (review).
void b_ifdef(double *a, int n, int m) {
  int t = 0;
  while (t < m) {
    // CHECK: branch.c:[[#LOOP12:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 0, 0 child loop(s), inside while/do or a loop header{{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: RW a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: a while loop around a GPU loop may not run, and a pointer's section is owned memory only when the loop touches it{{$}}
    for (int i = 0; i < n; i++)
#ifdef P05_NEVER_DEFINED
      a[i] = 0;
#else
      a[i] += 1.0;
#endif
    t++;
  }
}
