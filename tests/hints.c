// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 45 loop(s).
// Phase 2.5 item 1: rejection categories and hints (Rejection.h).
//
// Each loop is rejected for a stated reason; the CHECK lines pin its
// category and, where one is offered, its hint. tests/hints.test runs
// scripts/check_hints.py on this file: it applies every "fixes" hint of a
// loop the report calls fixable and checks that the loop is then SAFE.
//
// The restrict loops all read b through a triangular inner loop, so the
// range of b they touch depends on i and no run-time overlap check can be
// built (tests/alias.c k_prefix): only a proof helps.

// --- 1. restrict hints, one per declarator shape -----------------------------

// UNKNOWN pointer-overlap; hint: restrict on a plain pointer parameter
// (the hint names b, the first base of the pair; either would prove it).
void r_pointer(long *a, long *b, int n) {
  // CHECK: hints.c:[[#LOOP0:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_pointer' restrict, if no call lets what 'b' points to overlap anything else 'r_pointer' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP1:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `b[restrict ]` (array-syntax parameter).
void r_array_param(long a[], const long b[], int n) {
  // CHECK: hints.c:[[#LOOP2:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_array_param' restrict, if no call lets what 'b' points to overlap anything else 'r_array_param' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP3:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `b[restrict n]` (VLA parameter).
void r_vla_param(int n, long a[n], const long b[n]) {
  // CHECK: hints.c:[[#LOOP4:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_vla_param' restrict, if no call lets what 'b' points to overlap anything else 'r_vla_param' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP5:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `lp restrict b` (typedef'd pointer).
typedef long *lp;
void r_typedef(lp a, lp b, int n) {
  // CHECK: hints.c:[[#LOOP6:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_typedef' restrict, if no call lets what 'b' points to overlap anything else 'r_typedef' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP7:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP6]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `const long *const restrict b`.
void r_const_ptr(long *const a, const long *const b, int n) {
  // CHECK: hints.c:[[#LOOP8:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_const_ptr' restrict, if no call lets what 'b' points to overlap anything else 'r_const_ptr' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP9:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP8]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `long (*restrict b)` (parenthesized).
void r_paren(long (*a), long (*b), int n) {
  // CHECK: hints.c:[[#LOOP10:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_paren' restrict, if no call lets what 'b' points to overlap anything else 'r_paren' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP11:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP10]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; a prototype comes first, the hint edits the
// definition (restrict on a parameter is not part of the function type).
void r_proto(long *a, long *b, int n);
void r_proto(long *a, long *b, int n) {
  // CHECK: hints.c:[[#LOOP12:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_proto' restrict, if no call lets what 'b' points to overlap anything else 'r_proto' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP13:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap; hint: `long *restrict (b)` (the name in
// parentheses: the qualifier goes after the star, not before the name).
void r_paren_name(long *a, long *(b), int n) {
  // CHECK: hints.c:[[#LOOP14:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_paren_name' restrict, if no call lets what 'b' points to overlap anything else 'r_paren_name' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP15:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP14]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// --- 2. no restrict hint ------------------------------------------------------

// UNKNOWN pointer-overlap, no hint: both declarators come from a macro.
#define LONGPTR(x) long *x
void r_macro(LONGPTR(a), LONGPTR(b), int n) {
  // CHECK: hints.c:[[#LOOP16:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP17:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// UNKNOWN pointer-overlap, no hint: 'a' is reassigned, so it no longer
// holds the caller's pointer and restrict on it would prove nothing.
void r_reassigned(long *a, long *b, int n) {
  a = a + 1;
  // CHECK: hints.c:[[#LOOP18:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'a' is a pointer that may point into 'b' (R b[j], W a[i]){{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    long s = 0;
    // CHECK: hints.c:[[#LOOP19:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : reduction(+: s){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
  }
}

// --- 3. several reasons ---------------------------------------------------------

// UNSAFE pointer-overlap + fp-reduction, both with fixing hints: applying
// both makes it SAFE (restrict on b, and -fp-reductions). The inner loop
// is rejected for its own fp reduction on s.
double r_two_fixes(double *a, double *b, int n) {
  double total = 0;
  // CHECK: hints.c:[[#LOOP20:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[j] : [j]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'b' and 'a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: a loop range in b[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap, fp-reduction{{$}}
  // CHECK: hint       : declare parameter 'b' of 'r_two_fixes' restrict, if no call lets what 'b' points to overlap anything else 'r_two_fixes' accesses (here 'a'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hint       : rerun with -fp-reductions to get reduction(+: total) (accepts results that differ in the last bits) [fixes fp-reduction]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    double s = 0;
    // CHECK: hints.c:[[#LOOP21:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP20]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: R b[j] : [j]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - floating-point reduction on 's' (changes the last bits of the result; enable with -fp-reductions){{$}}
    // CHECK: rejected   : fp-reduction{{$}}
    // CHECK: hint       : rerun with -fp-reductions to get reduction(+: s) (accepts results that differ in the last bits) [fixes fp-reduction]{{$}}
    // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j <= i; j++)
      s += b[j];
    a[i] = s;
    total += s;
  }
  return total;
}

// UNSAFE array-dependence + fp-reduction: the fp hint alone can't make it
// SAFE, so no "hints" line.
double r_partial(double *a, int n) {
  double total = 0;
  // CHECK: hints.c:[[#LOOP22:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i] and R a[i - 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: rejected   : array-dependence, fp-reduction{{$}}
  // CHECK: hint       : rerun with -fp-reductions to get reduction(+: total) (accepts results that differ in the last bits) [fixes fp-reduction]{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i++) {
    a[i] = a[i - 1] + 1;
    total += a[i];
  }
  return total;
}

// UNSAFE fp-reduction only: fixable by -fp-reductions.
double r_fp_only(const double *a, int n) {
  double total = 0;
  // CHECK: hints.c:[[#LOOP23:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - floating-point reduction on 'total' (changes the last bits of the result; enable with -fp-reductions){{$}}
  // CHECK: rejected   : fp-reduction{{$}}
  // CHECK: hint       : rerun with -fp-reductions to get reduction(+: total) (accepts results that differ in the last bits) [fixes fp-reduction]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    total += a[i];
  return total;
}

// --- 4. opaque callee: the hint enables analysis --------------------------------

// UNKNOWN opaque-callee; hint (enables analysis): make 'ext_effect' visible.
void ext_effect(int);
void r_opaque(int n) {
  // CHECK: hints.c:[[#LOOP24:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: ext_effect reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 0 (0 affine){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'ext_effect' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'ext_effect' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    ext_effect(i);
}

// The same loop with the hinted change applied, for a callee whose body
// turns out to have no effect the loop cares about: SAFE.
static int seen_effect(int k) { return k * 2; }
void r_opaque_visible(int *out, int n) {
  // CHECK: hints.c:[[#LOOP25:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: seen_effect reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W out[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    out[i] = seen_effect(i);
}

// --- 5. one loop per remaining category (no hint) ------------------------------

// UNKNOWN indirect-subscript.
void c_indirect(double *a, const int *idx, int n) {
  // CHECK: hints.c:[[#LOOP26:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[idx[i]]: W a[idx[i]] is non-affine (subscript reads memory ('idx[i]')){{$}}
  // CHECK: rejected   : indirect-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[idx[i]] = 1.0;
}

// UNKNOWN indirect-subscript, through a local holding the loaded index.
void c_indirect_local(double *a, const int *idx, int n) {
  // CHECK: hints.c:[[#LOOP27:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: W a[k] : non-affine: uses 'k' (= idx[i]): subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[k]: W a[k] is non-affine (uses 'k' (= idx[i]): subscript reads memory ('idx[i]')){{$}}
  // CHECK: rejected   : indirect-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'idx' and 'a' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int k = idx[i];
    a[k] = 1.0;
  }
}

// UNKNOWN indirect-subscript: the loaded index is masked, so the builder
// stops at '&', but the subscript still comes from loaded data.
void c_indirect_masked(double *a, const int *idx, int n) {
  // CHECK: hints.c:[[#LOOP28:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: RW a[idx[i] & 7] : non-affine: subscript uses operator '&'{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test RW a[idx[i] & 7]: RW a[idx[i] & 7] is non-affine (subscript uses operator '&'){{$}}
  // CHECK: rejected   : indirect-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[idx[i] & 7] += 1.0;
}

// UNKNOWN non-affine-subscript: '%' with no load.
void c_modulo(double *a, int n) {
  // CHECK: hints.c:[[#LOOP29:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i % 3] : non-affine: subscript uses operator '%'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i % 3]: W a[i % 3] is non-affine (subscript uses operator '%'){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i % 3] = 1.0;
}

// UNKNOWN non-affine-subscript, not indirect: the loaded stride is the
// same on every iteration. (Also pointer-overlap: 'm' could point into 'a'.)
struct mat {
  int stride;
};
void c_invariant_load(double *a, const struct mat *m, int n) {
  // CHECK: hints.c:[[#LOOP30:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i * m->stride] : non-affine: subscript reads memory ('m->stride'){{$}}
  // CHECK-NEXT: R m->stride : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * m->stride]: W a[i * m->stride] is non-affine (subscript reads memory ('m->stride')){{$}}
  // CHECK: rejected   : non-affine-subscript, pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'a' of 'c_invariant_load' restrict, if no call lets what 'a' points to overlap anything else 'c_invariant_load' accesses (here 'm'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i * m->stride] = 1.0;
}

// UNKNOWN non-affine-subscript.
void c_nonaffine(double *a, int n) {
  // CHECK: hints.c:[[#LOOP31:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * i] : non-affine: subscript multiplies two non-constant terms ('i * i'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * i]: W a[i * i] is non-affine (subscript multiplies two non-constant terms ('i * i')){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i * i] = 1.0;
}

// UNKNOWN unknown-base: rows loaded from memory.
void c_rows(double **m, int n) {
  // CHECK: hints.c:[[#LOOP32:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W m[i][0] : non-affine: base is a pointer loaded from memory ('m[i]'){{$}}
  // CHECK-NEXT: R m[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W m[i][0] writes (base is a pointer loaded from memory ('m[i]')){{$}}
  // CHECK: rejected   : unknown-base{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    m[i][0] = 0.0;
}

// UNSAFE array-dependence.
void c_carried(double *a, int n) {
  // CHECK: hints.c:[[#LOOP33:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i - 1] : [i - 1]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[i] and R a[i - 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: rejected   : array-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 1; i < n; i++)
    a[i] = a[i - 1] * 0.5;
}

// UNSAFE scalar-dependence.
void c_scalar(double *a, int n) {
  double x = 1.0;
  // CHECK: hints.c:[[#LOOP34:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: rejected   : scalar-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = x;
    x = x * 0.5 + a[i];
  }
}

// UNSAFE shared-scalar: a global written every iteration.
int g_last;
void c_global(const int *a, int n) {
  // CHECK: hints.c:[[#LOOP35:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - writes to 'g_last', a global or static shared by every iteration{{$}}
  // CHECK: rejected   : shared-scalar{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    g_last = a[i];
}

// UNSAFE scalar-dependence: 't' keeps its value on iterations that don't
// write it, so the next iteration (or the code after the loop) reads it.
int c_keep(const int *a, int n) {
  int t = 0;
  // CHECK: hints.c:[[#LOOP36:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 't' carries a value from one iteration to the next{{$}}
  // CHECK: rejected   : scalar-dependence{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    if (a[i] > 0)
      t = a[i];
  return t;
}

// UNKNOWN live-out-scalar: 'i' is read after a loop that may run zero times.
int c_liveout(int *a, int n) {
  int i;
  // CHECK: hints.c:[[#LOOP37:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - induction variable 'i' is used after the loop, which may run zero times{{$}}
  // CHECK: rejected   : live-out-scalar{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++)
    a[i] = 0;
  return i;
}

// UNKNOWN early-exit.
void c_break(int *a, int n) {
  // CHECK: hints.c:[[#LOOP38:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body can 'break' out of the loop{{$}}
  // CHECK: rejected   : early-exit{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    if (a[i] < 0)
      break;
    a[i] = 0;
  }
}

// UNKNOWN indirect-call.
void c_fnptr(int *a, int n, int (*f)(int)) {
  // CHECK: hints.c:[[#LOOP39:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body contains an indirect call that cannot be resolved{{$}}
  // CHECK: rejected   : indirect-call{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = f(i);
}

// UNKNOWN recursion.
static int fact(int k) { return k <= 1 ? 1 : k * fact(k - 1); }
void c_recursion(int *a, int n) {
  // CHECK: hints.c:[[#LOOP40:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: fact reaches 0 function(s), max depth 0: <nothing further>{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'fact' recurses; call chain cannot be bounded{{$}}
  // CHECK: rejected   : recursion{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = fact(i);
}

// UNSAFE callee-side-effect: the callee writes a global.
int g_calls;
static int counted(int k) {
  g_calls++;
  return k;
}
void c_callee_global(int *a, int n) {
  // CHECK: hints.c:[[#LOOP41:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: counted reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'counted' writes global 'g_calls'{{$}}
  // CHECK: rejected   : callee-side-effect{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'g_calls' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = counted(i);
}

// UNKNOWN atomic-or-asm.
void c_atomic(int *a, int *count, int n) {
  // CHECK: hints.c:[[#LOOP42:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body uses an atomic builtin{{$}}
  // CHECK: rejected   : atomic-or-asm{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = 0;
    __atomic_fetch_add(count, 1, __ATOMIC_RELAXED);
  }
}

// UNKNOWN unproven-dependence: the distance k is symbolic.
void c_unproven(double *a, int n, int k) {
  // CHECK: hints.c:[[#LOOP43:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i + k] : [i + k]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W a[i] and R a[i + k] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = a[i + k];
}

// UNKNOWN loop-shape: not a counted loop.
void c_shape(int *a, int n) {
  // CHECK: hints.c:[[#LOOP44:@LINE+12]]:3{{$}}
  // CHECK: kind       : UNRECOGNIZED{{$}}
  // CHECK: reason     : condition is not a simple relational comparison{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : <none>{{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: R a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: W a[i] : non-affine: uses 'i', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop shape not recognized (condition is not a simple relational comparison); safety not evaluated{{$}}
  // CHECK: rejected   : loop-shape{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; a[i] != 0 && i < n; i++)
    a[i] = 1;
}
