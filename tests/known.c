// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 10 loop(s).
// Phase 2.5: calls without a body whose effects are known anyway
// (isKnownFunction in CallResolver.h). Const library builtins by Clang's
// own table (sqrt, floor, fabs, pow, __builtin_expect) and assert's failure
// function have no memory effects; everything else without a body stays
// opaque. Every SAFE case has a twin that must not be. Also RUN by
// scripts/check_parallel.sh with eval/runtime/known_main.c.
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- SAFE -------------------------------------------------------------------

// libm: no memory effects (errno is thread-local).
void k_libm(double *a, const double *b, int n) {
  // CHECK: known.c:[[#LOOP0:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = sqrt(fabs(b[i])) + floor(b[i]) + pow(b[i], 2.0);
}

// assert: its failure function only runs when the program is failing.
void k_assert(double *a, const double *b, int n) {
  // CHECK: known.c:[[#LOOP1:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    assert(b[i] >= -1000.0);
    a[i] = 2.0 * b[i];
  }
}

// __builtin_expect: a hint, and the subscript stays affine around it.
void k_expect(int *a, const int *b, int n) {
  // CHECK: known.c:[[#LOOP2:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 4 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 4 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = __builtin_expect(b[i] > 0, 1) ? b[i] : -b[i];
}

// A known call doesn't count as "a call that could change the global" koff,
// so the subscript stays affine.
int koff;
void k_global(double *a, const double *b, int n) {
  // CHECK: known.c:[[#LOOP3:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + koff] : [i + koff]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'a' and 'koff' can't overlap (incompatible element types){{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 * (__INTPTR_TYPE__)koff || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)koff + (__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = sqrt(b[i + koff]);
}

// --- not SAFE ----------------------------------------------------------------

// frexp writes through its pointer argument: not a const builtin.
void g_frexp(double *a, const double *b, int *e, int n) {
  // CHECK: known.c:[[#LOOP4:@LINE+18]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: frexp reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: & e[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'frexp' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'frexp' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'a' and 'e' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = frexp(b[i], &e[i]);
}

// strlen reads memory (pure, not const): stays opaque.
void g_strlen(int *a, char **s, int n) {
  // CHECK: known.c:[[#LOOP5:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: strlen reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R s[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'strlen' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'strlen' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 's' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = (int)strlen(s[i]);
}

// rand has state.
void g_rand(int *a, int n) {
  // CHECK: known.c:[[#LOOP6:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: rand reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'rand' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'rand' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = rand();
}

// A function of the program's own, declared but not defined here, is
// opaque even with a library-like name.
double fast_sqrt(double);
void g_user(double *a, const double *b, int n) {
  // CHECK: known.c:[[#LOOP7:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: fast_sqrt reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'fast_sqrt' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of 'fast_sqrt' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = fast_sqrt(b[i]);
}

// A known call doesn't hide a real dependence.
void g_carried(double *a, int n) {
  // CHECK: known.c:[[#LOOP8:@LINE+13]]:3{{$}}
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
    a[i] = sqrt(a[i - 1]);
}

// A "const" builtin that returns a pointer may differ per thread (review of
// Phase 2.5): not known.
void g_thread(int *a, void *main_tp, int n) {
  // CHECK: known.c:[[#LOOP9:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: __builtin_thread_pointer reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - '__builtin_thread_pointer' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee{{$}}
  // CHECK: hint       : make the definition of '__builtin_thread_pointer' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = __builtin_thread_pointer() == main_tp;
}
