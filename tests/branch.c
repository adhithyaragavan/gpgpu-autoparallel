// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 7 loop(s).
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
