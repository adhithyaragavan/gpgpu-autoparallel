// RUN: %p05 %s %S/Inputs/crosstu_other.c %S/Inputs/crosstu_dup.c -- %cflags | FileCheck %s
// CHECK: Analyzed 9 loop(s).
//
// Phase 2.5: calls into other inputs of the same program (CrossTU.h). With
// several inputs, each file gets the definitions of the external functions
// it calls that exactly one other input defines (here
// tests/Inputs/crosstu_other.c), copied in by clang's ASTImporter; their
// globals and types merge with this file's declarations. Every SAFE case has
// a twin that must not be. Also RUN by scripts/check_parallel.sh with
// eval/runtime/crosstu_main.c (EXTRA=tests/Inputs/crosstu_other.c).

extern double XG[4096];
double x_scale(double v);
void x_bump(double *p, int i);
void x_touch(int i);
double x_read(int i);
void x_chain(double *p, int i, double v);
double x_peek(int i);
double x_sum4(const double *p);
double x_twice(double v);
double x_nowhere(double v);
static int hidden; // not the other file's static array of that name

// --- SAFE -------------------------------------------------------------------

// A pure callee that reads the other file's static.
void c_pure(double *a, const double *b, int n) {
  // CHECK: crosstu.c:[[#LOOP0:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_scale reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_scale(b[i]);
}

// A callee writing through its pointer argument: composed like a local one.
void c_ptr(double *a, int n) {
  // CHECK: crosstu.c:[[#LOOP1:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_bump reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: RW x_bump(): p[i] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    x_bump(a, i);
}

// Two levels deep, through the other file's static helper.
void c_chain(double *a, const double *b, int n) {
  // CHECK: crosstu.c:[[#LOOP2:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_chain reaches 1 function(s), max depth 1: x_leaf@1{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W x_chain(): x_leaf(): p[i] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    x_chain(a, i, b[i]);
}

// The other file's global, read only, merged with this file's extern.
void c_global(double *a, int n) {
  // CHECK: crosstu.c:[[#LOOP3:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_read reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R XG[i] : [i]{{$}}
  // CHECK-NEXT: R x_read(): XG[i] : [i] of 'XG'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'XG' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)XG + 0 || (__INTPTR_TYPE__)XG + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_read(i) + XG[i];
}

// A callee with a loop of its own, on rows of b: SAFE (the summary goes
// through x_sum4's loop), but declined, since crosstu_other.c's own run
// decides that loop and may give it a target region.
void c_rows(double *a, const double *b, int n) {
  // CHECK: crosstu.c:[[#LOOP4:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_sum4 reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R x_sum4(): p[k] : [k + 4*i] of 'b'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * ((4 * (__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  // CHECK: - declined: it calls 'x_sum4' (crosstu_other.c), defined in another file, whose loops may get a target region there, and one can't run inside another construct -> SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_sum4(&b[4 * i]);
}

// --- not SAFE ----------------------------------------------------------------

// The callee writes the global the loop reads, one element ahead: a real
// dependence across files (needs XG to be ONE declaration).
void g_global(int n) {
  // CHECK: crosstu.c:[[#LOOP5:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_touch reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W XG[i] : [i]{{$}}
  // CHECK-NEXT: R XG[i] : [i]{{$}}
  // CHECK-NEXT: W x_touch(): XG[i] : [i + 1] of 'XG'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W XG[i] and W x_touch(): XG[i]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: rejected   : array-dependence{{$}}
  // CHECK: deps       : 5 pair(s), 3 independent (3 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
  {
    XG[i] = XG[i] + 1.0;
    x_touch(i + 1);
  }
}

// x_twice is defined in two inputs: ambiguous, never imported.
void g_dup(double *a, int n) {
  // CHECK: crosstu.c:[[#LOOP6:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_twice reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'x_twice' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'x_twice' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_twice(a[i]);
}

// Defined nowhere: opaque.
void g_nowhere(double *a, int n) {
  // CHECK: crosstu.c:[[#LOOP7:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_nowhere reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'x_nowhere' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'x_nowhere' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_nowhere(a[i]);
}

// The callee reads the other file's static `hidden`, which that file lets
// out (x_hidden), so `a` may point into it; and this file can't name it for
// a run-time check (its own `hidden` is a different object). The other file's
// `factor` (c_pure) never escapes, so it is a distinct object.
void g_static(double *a, int n) {
  // CHECK: crosstu.c:[[#LOOP8:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: x_peek reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R x_peek(): hidden[i % 8] : non-affine: subscript uses operator '%'; in [0, 7]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'a' and 'hidden' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: global 'hidden' is only declared in another file{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'a' of 'g_static' restrict, if no call lets what 'a' points to overlap anything else 'g_static' accesses (here 'hidden'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = x_peek(i) + hidden;
}
