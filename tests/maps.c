// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 30 loop(s).
// Phase 2.1: map() sections from the subscripts' ranges (DataRegions.h),
// collapse(2), and map(tofrom:) for reduction/lastprivate scalars. Also RUN
// by scripts/check_parallel.sh with OFFLOAD=1 and eval/runtime/maps_main.c,
// which offloads every SAFE loop to a host device with its own memory: a
// section too small, a `from` over elements the loop doesn't write, or a
// missing scalar copy-back gives a result different from the sequential one.

#define N 4096
#define R 64
#define C 128
long g_a[N], g_b[N];
long g_m[R][C];

static long work(long x) {
  // CHECK: maps.c:[[#LOOP0:@LINE+10]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 16{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 0 (0 affine){{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop-carried dependence: 'x' carries a value from one iteration to the next{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < 16; k++)
    x = x * 3 + k;
  return x;
}

// --- direction --------------------------------------------------------------

// SAFE; y written only where c[i] is set. `from` would copy uninitialized
// device memory over the elements left alone, and no access to the pointer
// y surely runs, so nothing bounds its section: no GPU offload.
void cond_write(long *restrict y, const long *restrict c, long v, int n) {
  // CHECK: maps.c:[[#LOOP1:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R c[i] : [i]{{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'y' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: can't map 'y': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    if (c[i])
      y[i] = work(v + i);
}

// SAFE; the same on a global array (its extent bounds the section): mapped
// tofrom, never `from`.
void cond_write_g(long v) {
  // CHECK: maps.c:[[#LOOP2:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 4096{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R g_b[i] : [i]{{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  for (int i = 0; i < N; i++)
    if (g_b[i] & 1)
      g_a[i] = work(v + i);
}

// SAFE; every y[i] written, never read: map(from: y[0:n]).
void full_write(long *restrict y, const long *restrict x, int n) {
  // CHECK: maps.c:[[#LOOP3:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'y' and 'x' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): y (map(from: y[0:n]), 8 B/elem), x (map(to: x[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    y[i] = work(x[i]);
}

// SAFE; only even elements written: tofrom, odd ones must survive.
void stride_write(long *restrict y, const long *restrict x, int n) {
  // CHECK: maps.c:[[#LOOP4:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W y[2 * i] : [2*i]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'y' and 'x' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): y (map(tofrom: y[0:(2 * (__INTPTR_TYPE__)n - 1)]), 8 B/elem), x (map(to: x[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    y[2 * i] = work(x[i]);
}

// --- section bounds ---------------------------------------------------------

// SAFE; reads b[1..n], writes y[1..n]: sections b[1:n] and y[1:n], so y[0]
// is left alone even though y is `from`.
void shift(long *restrict y, const long *restrict b, int n) {
  // CHECK: maps.c:[[#LOOP5:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W y[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'y' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): y (map(from: y[1:n]), 8 B/elem), b (map(to: b[1:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    y[i + 1] = work(b[i + 1]);
}

// SAFE, but b[i + 1] is read only on some iterations: its section can't be
// sized from what surely runs, so no GPU offload.
void cond_shift(long *restrict y, const long *restrict b,
                const long *restrict c, int n) {
  // CHECK: maps.c:[[#LOOP6:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W y[i] : [i]{{$}}
  // CHECK-NEXT: R c[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'y' and 'c' can't overlap (restrict){{$}}
  // CHECK: alias      : 'y' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: can't map 'b': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    y[i] = c[i] ? work(b[i + 1]) : 0;
}

// SAFE; counts down over a global: the section is still g_a[0:N].
void descending(void) {
  // CHECK: maps.c:[[#LOOP7:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 4096{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: R g_b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  for (int i = N - 1; i >= 0; i--)
    g_a[i] = work(g_b[i]) + i;
}

// SAFE; a global array, part of it: clamped section of an object.
void global_part(int lo, int n) {
  // CHECK: maps.c:[[#LOOP8:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: R g_b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): g_a (map(from: g_a[(lo < 0 ? 0 : lo):(((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) - ((lo < 0 ? 0 : lo)) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) - ((lo < 0 ? 0 : lo)) + 1 : 0)]), 8 B/elem), g_b (map(to: g_b[(lo < 0 ? 0 : lo):(((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) - ((lo < 0 ? 0 : lo)) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) - ((lo < 0 ? 0 : lo)) + 1 : 0)]), 8 B/elem){{$}}
  // CHECK: - map guard: ((__INTPTR_TYPE__)n - (__INTPTR_TYPE__)lo) >= 1{{$}}
  for (int i = lo; i < n; i++)
    g_a[i] = work(g_b[i]);
}

// --- nests ------------------------------------------------------------------

// SAFE, collapse(2); rows of a pointer to arrays: m[0:n][0:128].
void rows(long (*m)[C], int n) {
  // CHECK: maps.c:[[#LOOP9:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 2048 B across 1 region(s): m (map(tofrom: m[0:n][0:128]), 1024 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: maps.c:[[#LOOP10:@LINE+12]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 128{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < C; j++)
      m[i][j] = work(m[i][j] + j);
}

// SAFE, collapse(2); delinearized a[i*m + j]: the linear section.
void linear(long *restrict a, const long *restrict b, int n, int m) {
  // CHECK: maps.c:[[#LOOP11:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: R b[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(tofrom: a[0:((((__INTPTR_TYPE__)n - 1)) * (__INTPTR_TYPE__)m + ((__INTPTR_TYPE__)m - 1)) + 1]), 8 B/elem), b (map(to: b[0:((((__INTPTR_TYPE__)n - 1)) * (__INTPTR_TYPE__)m + ((__INTPTR_TYPE__)m - 1)) + 1]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1 && m >= 1{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: maps.c:[[#LOOP12:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP11]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: R b[i * m + j] : [i][j] delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
    // CHECK: profitability : GPU_OFFLOAD if (m >= {{[0-9]+}}){{$}}
    // CHECK: - data/iteration: 16 B across 2 region(s): a (map(tofrom: a[((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m:(((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m + ((__INTPTR_TYPE__)m - 1)) - (((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m) + 1]), 8 B/elem), b (map(to: b[((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m:(((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m + ((__INTPTR_TYPE__)m - 1)) - (((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m) + 1]), 8 B/elem){{$}}
    // CHECK: - map guard: ((__INTPTR_TYPE__)i) * (__INTPTR_TYPE__)m >= 0 && m >= 1{{$}}
    for (int j = 0; j < m; j++)
      a[i * m + j] = work(b[i * m + j]);
}

// SAFE outer loop, inner one carries a dependence: no collapse.
void inner_dep(void) {
  // CHECK: maps.c:[[#LOOP13:@LINE+13]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 64{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W g_m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R g_m[i][j - 1] : [i][j - 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: collapse   : no, the inner loop is not SAFE on its own{{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  for (int i = 0; i < R; i++)
    // CHECK: maps.c:[[#LOOP14:@LINE+13]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 127{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W g_m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R g_m[i][j - 1] : [i][j - 1]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W g_m[i][j] and R g_m[i][j - 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 1; j < C; j++)
      g_m[i][j] = g_m[i][j - 1] + work(i + j);
}

// SAFE, triangular: no collapse.
void triangle(long (*m)[C], int n) {
  // CHECK: maps.c:[[#LOOP15:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W m[i][j] : non-affine: uses 'j', which is assigned inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W m[i][j]: W m[i][j] is non-affine (uses 'j', which is assigned inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: maps.c:[[#LOOP16:@LINE+11]]:5{{$}}
    // CHECK: kind       : UNRECOGNIZED{{$}}
    // CHECK: reason     : condition is not a simple relational comparison{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP15]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W m[i][j] : non-affine: uses 'j', which is modified inside the loop{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - loop shape not recognized (condition is not a simple relational comparison); safety not evaluated{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < i && j < C; j++)
      m[i][j] = work(i - j);
}

// SAFE; j is read after the loop (lastprivate): no collapse.
int last_j(void) {
  int i, j;
  // CHECK: maps.c:[[#LOOP17:@LINE+14]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 64{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W g_m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: R g_m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: scalars    : lastprivate(j){{$}}
  // CHECK: collapse   : no, 'j' is read after the loop{{$}}
  // CHECK: profitability : CPU_PARALLEL{{$}}
  for (i = 0; i < R; i++)
    // CHECK: maps.c:[[#LOOP18:@LINE+13]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 128{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP17]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W g_m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: R g_m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
    // CHECK: scalars    : lastprivate(j){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < C; j++)
      g_m[i][j] = work(g_m[i][j]);
  return j;
}

// SAFE; only column 0 of each row written: tofrom, the rest must survive.
void column(long (*m)[C], int n) {
  // CHECK: maps.c:[[#LOOP19:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W m[i][0] : [i][0]{{$}}
  // CHECK-NEXT: R m[i][1] : [i][1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    m[i][0] = work(m[i][1]);
}

// --- scalars and callees ----------------------------------------------------

// SAFE, integer reduction: map(tofrom: s) reduction(+: s).
long sum(const long *a, int n) {
  long s = 0;
  // CHECK: maps.c:[[#LOOP20:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: scalars    : reduction(+: s){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 8 B across 1 region(s): a (map(to: a[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++)
    s += work(a[i]);
  return s;
}

// SAFE, lastprivate(t): its last value comes back.
long last_t(long *a, int n) {
  long t = 0;
  // CHECK: maps.c:[[#LOOP21:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: scalars    : lastprivate(t){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 1 region(s): a (map(tofrom: a[0:n]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1{{$}}
  for (int i = 0; i < n; i++) {
    t = work(a[i]);
    a[i] = t;
  }
  return t;
}

static void scale_row(long *row, int m) {
  // CHECK: maps.c:[[#LOOP22:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W row[j] : [j]{{$}}
  // CHECK-NEXT: R row[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (m >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 1 region(s): row (map(tofrom: row[0:m]), 8 B/elem){{$}}
  // CHECK: - map guard: m >= 1{{$}}
  for (int j = 0; j < m; j++)
    row[j] = work(row[j]);
}

// SAFE; the callee's accesses, composed at the call, size the section.
void callee_rows(long *a, int n, int m) {
  // CHECK: maps.c:[[#LOOP23:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: scale_row reaches 1 function(s), max depth 1: work@1{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W scale_row(): row[j] : [i][j] of 'a' delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: R scale_row(): row[j] : [i][j] of 'a' delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 1 region(s): a (map(tofrom: a[0:((((__INTPTR_TYPE__)n - 1)) * (__INTPTR_TYPE__)m + ((__INTPTR_TYPE__)m - 1)) + 1]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1 && m >= 1{{$}}
  for (int i = 0; i < n; i++)
    scale_row(&a[i * m], m);
}

// --- adversarial review cases ----------------------------------------------

// SAFE; covers g_a[0:n] only if the inner loop runs: `from` needs m >= 1 in
// the guard, or a zero-trip inner loop copies device garbage back.
void cover_inner(int n, int m) {
  // CHECK: maps.c:[[#LOOP24:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : no, the inner loop is not SAFE on its own{{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 8 B across 1 region(s): g_a (map(from: g_a[0:(((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)]), 8 B/elem){{$}}
  // CHECK: - map guard: n >= 1 && m >= 1{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: maps.c:[[#LOOP25:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP24]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W g_a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W g_a[i]: every iteration touches the same element{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < m; j++)
      g_a[i] = work(j);
}

// SAFE; unsigned bounds: the section and guard must not wrap when n < k.
void unsigned_ptr(long *restrict a, const long *restrict b, unsigned k,
                  unsigned n) {
  // CHECK: maps.c:[[#LOOP26:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i - k] : [i - k]{{$}}
  // CHECK-NEXT: R b[i - k] : [i - k]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(from: a[0:(-(__INTPTR_TYPE__)k + (__INTPTR_TYPE__)n)]), 8 B/elem), b (map(to: b[0:(-(__INTPTR_TYPE__)k + (__INTPTR_TYPE__)n)]), 8 B/elem){{$}}
  // CHECK: - map guard: ((__INTPTR_TYPE__)n - (__INTPTR_TYPE__)k) >= 1{{$}}
  for (unsigned i = k; i < n; i++)
    a[i - k] = work(b[i - k]);
}

// SAFE; unsigned bound over a global: n = 0 must map nothing back.
void unsigned_obj(unsigned n) {
  // CHECK: maps.c:[[#LOOP27:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W g_a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : GPU_OFFLOAD if (n >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 8 B across 1 region(s): g_a (map(from: g_a[0:(((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) + 1 > 0 ? ((((__INTPTR_TYPE__)n - 1) > 4095 ? 4095 : ((__INTPTR_TYPE__)n - 1))) + 1 : 0)]), 8 B/elem){{$}}
  // CHECK: - map guard: (__INTPTR_TYPE__)n >= 1{{$}}
  for (unsigned i = 0; i < n; i++)
    g_a[i] = work(i);
}

// SAFE; b[i + 1000] is never evaluated (_Generic's controlling expression),
// so it doesn't widen b's section.
void generic(long *restrict a, const long *restrict b, int n) {
  // CHECK: maps.c:[[#LOOP28:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1000] : [i + 1000]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = b[i] + _Generic(b[i + 1000], long: 1, default: 2);
}

// SAFE, but a and b are only read and may overlap (b == a + 1 below):
// two overlapping sections can't both be mapped, and `restrict` doesn't
// keep read-only pointers apart. No GPU offload.
void read_pair(long *restrict c, const long *restrict a,
               const long *restrict b, int n) {
  // CHECK: maps.c:[[#LOOP29:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: work reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'c' and 'a' can't overlap (restrict){{$}}
  // CHECK: alias      : 'c' and 'b' can't overlap (restrict){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    c[i] = work(a[i]) + b[i];
}

