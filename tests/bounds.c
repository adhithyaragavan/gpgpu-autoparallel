// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 26 loop(s).
//
// Phase 2.5 item 3: bounds for accesses whose subscript isn't affine.
// (a) value bounds: `i % 3`, `idx[i] & 7`, `i / 2`, `i >> 2`, products of
//     bounded terms (ValueBound in AffineAccess.h);
// (b) size bounds: [0, extent - 1] from a constant array type, or for a
//     pointer parameter from the smallest array every visible call passes
//     (AliasAnalyzer::callerExtent).
// A bound lets a pointer pair get a run-time overlap check (alias guard),
// proves two accesses to one array apart when their ranges don't meet, and
// sizes map() sections. Every case has a twin that must stay UNKNOWN (or
// unmapped). Also RUN by scripts/check_parallel.sh with
// eval/runtime/bounds_main.c, which calls every run-time-checked loop with
// disjoint and with OVERLAPPING arguments.

#define NT 262144
#define NR 32
#define NC 16
#define NS 256

long tab[NT];
long grid[NR][NC];

// --- (a) value bounds -------------------------------------------------------

// SAFE, checked at run time: b is read at [0, 2].
void vb_mod(long *a, const long *b, int n) {
  // CHECK: bounds.c:[[#LOOP0:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i % 3] : non-affine: subscript uses operator '%'; in [0, 2]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 24 <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i % 3] + i;
}

// UNKNOWN: i % k has no bound (k isn't a constant).
void vb_mod_sym(long *a, const long *b, int k, int n) {
  // CHECK: bounds.c:[[#LOOP1:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i % k] : non-affine: subscript uses operator '%'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'a' and 'b' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: b[i % k] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'a' of 'vb_mod_sym' restrict, if no call lets what 'a' points to overlap anything else 'vb_mod_sym' accesses (here 'b'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i % k] + i;
}

// SAFE, checked at run time: i may be negative, so i % 4 is in [-3, 3].
void vb_mod_neg(long *a, const long *b, int n) {
  // CHECK: bounds.c:[[#LOOP2:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i + n] : [i + n]{{$}}
  // CHECK-NEXT: R b[i % 4] : non-affine: subscript uses operator '%'; in [-3, 3]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * ((2 * (__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + -24 || (__INTPTR_TYPE__)b + 32 <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = -n; i < n; i++)
    a[i + n] = b[i % 4] + i;
}

// SAFE, checked at run time: an indirect subscript masked to [0, 7].
void vb_mask(long *a, const long *b, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP3:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[idx[i] & 7] : non-affine: subscript uses operator '&'; in [0, 7]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 64 <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[idx[i] & 7];
}

// UNKNOWN: a negative mask bounds nothing.
void vb_mask_neg(long *a, const long *b, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP4:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[idx[i] & -8] : non-affine: subscript uses operator '&'{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'a' and 'b' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: b[idx[i] & -8] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'a' of 'vb_mask_neg' restrict, if no call lets what 'a' points to overlap anything else 'vb_mask_neg' accesses (here 'b'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[idx[i] & -8];
}

// SAFE, checked at run time: i / 2 and i >> 2 are in [0, n - 1].
void vb_div(long *a, const long *b, const long *c, int n) {
  // CHECK: bounds.c:[[#LOOP5:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i / 2] : non-affine: subscript uses operator '/'; in [0, n - 1]{{$}}
  // CHECK-NEXT: R c[i >> 2] : non-affine: subscript uses operator '>>'; in [0, n - 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'a' and 'c' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0) && ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)c + 0 || (__INTPTR_TYPE__)c + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i / 2] + c[i >> 2];
}

// SAFE, checked at run time: (i & 3) * (i & 3) is in [0, 9].
void vb_square(long *a, const long *b, int n) {
  // CHECK: bounds.c:[[#LOOP6:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[(i & 3) * (i & 3)] : non-affine: subscript uses operator '&'; in [0, 9]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0 || (__INTPTR_TYPE__)b + 80 <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[(i & 3) * (i & 3)];
}

// UNKNOWN: unsigned u % 4 - 1 wraps around at u % 4 == 0.
void vb_unsigned_wrap(long *a, const long *b, int n) {
  // CHECK: bounds.c:[[#LOOP7:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[(unsigned)i % 4u - 1u] : non-affine: subscript uses operator '%'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'a' and 'b' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'b' it touches is unknown: b[(unsigned)i % 4u - 1u] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'a' of 'vb_unsigned_wrap' restrict, if no call lets what 'a' points to overlap anything else 'vb_unsigned_wrap' accesses (here 'b'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[(unsigned)i % 4u - 1u];
}

// SAFE: the reads stay in [0, 7] and the writes in [8, n + 7], so no
// element is both read and written.
void vb_apart(long *a, int n) {
  // CHECK: bounds.c:[[#LOOP8:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i + 8] : [i + 8]{{$}}
  // CHECK-NEXT: R a[i % 8] : non-affine: subscript uses operator '%'; in [0, 7]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 range){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: can't map a[i % 8]: it may not run on every iteration, and its range isn't inside what the other accesses to 'a' surely touch -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    a[i + 8] = a[i % 8] + i;
}

// UNKNOWN: the writes start at 7, which i % 8 reads at i = 7.
void vb_apart_neg(long *a, int n) {
  // CHECK: bounds.c:[[#LOOP9:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i + 7] : [i + 7]{{$}}
  // CHECK-NEXT: R a[i % 8] : non-affine: subscript uses operator '%'; in [0, 7]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i + 7] and R a[i % 8]: R a[i % 8] is non-affine (subscript uses operator '%'){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[i + 7] = a[i % 8] + i;
}

// --- (b) size bounds --------------------------------------------------------

// SAFE, checked at run time: tab has 262144 elements, whatever idx holds.
void sz_global(long *p, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP10:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R tab[idx[i]] : non-affine: subscript reads memory ('idx[i]'); in [0, 262143]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'tab' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)p + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)tab + 0 || (__INTPTR_TYPE__)tab + 2097152 <= (__INTPTR_TYPE__)p + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p[i] = tab[idx[i]];
}

// SAFE, checked at run time: a row of grid, and any column of it.
void sz_grid(long *p, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP11:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R grid[i][idx[i]] : non-affine: subscript reads memory ('idx[i]'); in [0, 31][0, 15]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'grid' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)p + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)grid + 8 * ((0) * 16 + 0) || (__INTPTR_TYPE__)grid + 8 * ((31) * 16 + 15 + 1) <= (__INTPTR_TYPE__)p + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p[i] = grid[i][idx[i]];
}

// UNKNOWN: the size of what q points to isn't known.
void sz_pointer(long *p, const long *q, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP12:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R q[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'p' and 'q' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'q' it touches is unknown: q[idx[i]] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'p' of 'sz_pointer' restrict, if no call lets what 'p' points to overlap anything else 'sz_pointer' accesses (here 'q'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i] = q[idx[i]];
}

// Every call passes 256-element arrays: q's map section is q[0:256].
static void sz_static(long *p, const long *q, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP13:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R q[idx[i]] : non-affine: subscript reads memory ('idx[i]'); in [0, 255]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'q' can't overlap (every call passes disjoint objects){{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    p[i] = q[idx[i]];
}

long s_dst[NS], s_src[NS];
void sz_static_caller(const int *idx) { sz_static(s_dst, s_src, idx, NS); }

// One call passes a pointer of unknown size: no section for q.
static void sz_static_mixed(long *p, const long *q, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP14:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R q[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'p' and 'q' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'q' it touches is unknown: q[idx[i]] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'p' of 'sz_static_mixed' restrict, if no call lets what 'p' points to overlap anything else 'sz_static_mixed' accesses (here 'q'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i] = q[idx[i]];
}

long m_dst[NS], m_src[NS];
void sz_static_mixed_caller(const int *idx, const long *other) {
  sz_static_mixed(m_dst, m_src, idx, NS);
  sz_static_mixed(m_dst, other, idx, NS);
}

// A triangular nest over an `A[N][N]` parameter. One call passes `*pa`,
// which may point into a larger allocation, so A has no extent (x does: both
// calls pass t_x). sz_tri_gpu below is the twin where A has one.
static void sz_tri(int n, double A[NR][NR], const double x[NR]) {
  // CHECK: bounds.c:[[#LOOP15:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R x[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'A' and 'x' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)A + 8 * ((((__INTPTR_TYPE__)n - 1)) * 32 + 31 + 1) <= (__INTPTR_TYPE__)x + 0 || (__INTPTR_TYPE__)x + 256 <= (__INTPTR_TYPE__)A + 8 * ((0) * 32 + 0)){{$}}
  // CHECK: collapse   : no, the inner loop needs its own run-time alias check{{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: bounds.c:[[#LOOP16:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP15]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R x[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: alias      : 'A' and 'x' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)A + 8 * (((__INTPTR_TYPE__)i) * 32 + (__INTPTR_TYPE__)i + 1) <= (__INTPTR_TYPE__)x + 0 || (__INTPTR_TYPE__)x + 8 * ((__INTPTR_TYPE__)i + 1) <= (__INTPTR_TYPE__)A + 8 * (((__INTPTR_TYPE__)i) * 32 + 0)){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      A[i][j] = A[i][j] * x[j];
}

double t_a[NR][NR], t_x[NR];
void sz_tri_caller(int n, double (*pa)[NR][NR]) {
  sz_tri(n, t_a, t_x);
  sz_tri(n, *pa, t_x);
}

// Externally visible: other callers may pass anything, so no extent.
void sz_tri_extern(int n, double A[NR][NR], const double x[NR]) {
  // CHECK: bounds.c:[[#LOOP17:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R x[j] : [j]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'A' and 'x' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'x' it touches is unknown: a loop range in x[j] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'A' of 'sz_tri_extern' restrict, if no call lets what 'A' points to overlap anything else 'sz_tri_extern' accesses (here 'x'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: bounds.c:[[#LOOP18:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP17]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R x[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: alias      : 'A' and 'x' may overlap: checked at run time{{$}}
    // CHECK: alias guard: ((__INTPTR_TYPE__)A + 8 * (((__INTPTR_TYPE__)i) * 32 + (__INTPTR_TYPE__)i + 1) <= (__INTPTR_TYPE__)x + 0 || (__INTPTR_TYPE__)x + 8 * ((__INTPTR_TYPE__)i + 1) <= (__INTPTR_TYPE__)A + 8 * (((__INTPTR_TYPE__)i) * 32 + 0)){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      A[i][j] = A[i][j] * x[j];
}

// GPU-mappable twins (static alias proofs): run as target regions by
// check_parallel.sh OFFLOAD=1, where a short section gives wrong output.
// Every call passes t2_a and t2_x: A and x are disjoint, x maps x[0:32].
static void sz_tri_gpu(int n, double A[NR][NR], const double x[NR]) {
  // CHECK: bounds.c:[[#LOOP19:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R x[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'A' and 'x' can't overlap (every call passes disjoint objects){{$}}
  // CHECK: collapse   : no, the inner loop's range is not a fixed, exact rectangle{{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: bounds.c:[[#LOOP20:@LINE+13]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: W A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R A[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R x[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: alias      : 'A' and 'x' can't overlap (every call passes disjoint objects){{$}}
    // CHECK: profitability : CPU_PARALLEL if (i >= {{[0-9]+}}){{$}}
    for (int j = 0; j <= i; j++)
      A[i][j] = A[i][j] * x[j];
}

double t2_a[NR][NR], t2_x[NR];
void sz_tri_gpu_caller(int n) { sz_tri_gpu(n, t2_a, t2_x); }

// Two global objects: the indirect read maps the whole table.
long o_out[NS], o_tab[NT];
void sz_object(const int *idx) {
  // CHECK: bounds.c:[[#LOOP21:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 256{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W o_out[i] : [i]{{$}}
  // CHECK-NEXT: R o_tab[idx[i]] : non-affine: subscript reads memory ('idx[i]'); in [0, 262143]{{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'o_out' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  for (int i = 0; i < NS; i++)
    o_out[i] = o_tab[idx[i]] + i;
}

// The calls pass arrays of different sizes: no extent (q[0:128] would miss
// what the second call reads, q[0:256] would read past the first array).
static void sz_sizes(long *p, const long *q, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP22:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : [i]{{$}}
  // CHECK-NEXT: R q[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'q' can't overlap (every call passes disjoint objects){{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i] = q[idx[i]];
}

long z_dst[NS], z_small[NS / 2], z_big[NS];
void sz_sizes_caller(const int *small_idx, const int *big_idx) {
  sz_sizes(z_dst, z_small, small_idx, NS);
  sz_sizes(z_dst, z_big, big_idx, NS);
}

// Review 2026-10-20: subobjects are not the passed object. A row indexed as
// the whole matrix (BLAS style) and a trailing `d[1]` struct-hack member
// give no extent, so the gather stays UNKNOWN.
long r_rows[4][NS];
static void sz_row(long *b, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP23:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W b[i + NS] : [i + 256]{{$}}
  // CHECK-NEXT: R b[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W b[i + NS] and R b[idx[i]]: R b[idx[i]] is non-affine (subscript reads memory ('idx[i]')){{$}}
  // CHECK: rejected   : indirect-subscript{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    b[i + NS] = b[idx[i]] + 1;
}
void sz_row_caller(const int *idx, int n) { sz_row(r_rows[0], idx, n); }

struct hack { int n; long d[1]; };
static void sz_member(long *out, const long *d, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP24:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W out[i] : [i]{{$}}
  // CHECK-NEXT: R d[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'out' and 'd' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'd' it touches is unknown: d[idx[i]] is non-affine{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'out' of 'sz_member' restrict, if no call lets what 'out' points to overlap anything else 'sz_member' accesses (here 'd'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'out' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    out[i] = d[idx[i]];
}
void sz_member_caller(struct hack *h, long *out, const int *idx) {
  sz_member(out, h->d, idx, h->n);
}

// An affine access past the extent (p[i + 100] of a 100-element array):
// the extent can't be trusted, so none is used for p.
long w_obj[100];
static void sz_past(long *p, const int *idx, int n) {
  // CHECK: bounds.c:[[#LOOP25:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i + 100] : [i + 100]{{$}}
  // CHECK-NEXT: R p[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W p[i + 100] and R p[idx[i]]: R p[idx[i]] is non-affine (subscript reads memory ('idx[i]')){{$}}
  // CHECK: rejected   : indirect-subscript{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    p[i + 100] = p[idx[i]];
}
void sz_past_caller(const int *idx) { sz_past(w_obj, idx, 0); }
