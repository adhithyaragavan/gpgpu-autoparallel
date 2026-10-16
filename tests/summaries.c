// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 30 loop(s).
//
// Phase 1.5: call-site-aware interprocedural summaries (Summaries.h). Matched
// pairs: the same callee, or the same call shape, on the safe and the unsafe
// side of a call boundary. Also RUN by scripts/check_parallel.sh with
// eval/runtime/summaries_main.c: every SAFE loop here runs as `parallel for`
// and must match the sequential build bit for bit.

#define NG 4096
long g_tab[NG];
long g_count;
int g_off;

// --- 1. a pointer to one element ------------------------------------------

static void set0(long *p, long v) { p[0] = v; }
static void set1(long *p, long v) { p[1] = v + p[0]; }

// SAFE: set0(&a[i]) writes a[i].
void s_elem(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP0:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: set0 reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W set0(): p[0] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    set0(&a[i], b[i]);
}

// UNSAFE: set1(&a[i]) writes a[i + 1] and reads a[i].
void u_elem_next(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP1:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: set1 reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W set1(): p[1] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: R set1(): p[0] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W set1(): p[1] and R set1(): p[0]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    set1(&a[i], i);
}

// --- 2. base pointer plus an index argument --------------------------------

static void put(long *p, int k, long v) { p[k] = v; }
static void put_next(long *p, int k) { p[k + 1] = p[k] * 2; }

// SAFE: put(a, i) writes a[i].
void s_index(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP2:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W put(): p[k] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    put(a, i, b[i] + 1);
}

// UNSAFE: put_next(a, i) writes a[i + 1], reads a[i].
void u_index_next(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP3:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put_next reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W put_next(): p[k + 1] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: R put_next(): p[k] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W put_next(): p[k + 1] and R put_next(): p[k]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    put_next(a, i);
}

// SAFE: two calls, even and odd elements.
void s_two_calls(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP4:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W put(): p[k] : [2*i] of 'a'{{$}}
  // CHECK-NEXT: W put(): p[k] : [2*i + 1] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 GCD, 2 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * ((2 * (__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    put(a, 2 * i, b[i]);
    put(a, 2 * i + 1, -b[i]);
  }
}

// UNSAFE: the second call writes the element the first one writes one
// iteration later.
void u_two_calls(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP5:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W put(): p[k] : [2*i] of 'a'{{$}}
  // CHECK-NEXT: W put(): p[k] : [2*i + 2] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W put(): p[k] and W put(): p[k]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    put(a, 2 * i, b[i]);
    put(a, 2 * i + 2, -b[i]);
  }
}

// SAFE: the call sits in an inner loop: a[4i + k], 0 <= k < 4.
void s_inner_call(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP6:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W put(): p[k] : [4*i + k] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: collapse   : 2 (with 'k'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: summaries.c:[[#LOOP7:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP6]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W put(): p[k] : [4*i + k] of 'a'{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int k = 0; k < 4; k++)
      put(a, 4 * i + k, i + k);
}

// --- 3. rows: the callee loops over a row it is handed ---------------------

static void row_fill(long *row, int m, long v) {
  // CHECK: summaries.c:[[#LOOP8:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W row[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
  for (int j = 0; j < m; j++)
    row[j] = v + j;
}
static void row_fill_inclusive(long *row, int m, long v) {
  // CHECK: summaries.c:[[#LOOP9:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W row[j] : [j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
  for (int j = 0; j <= m; j++)
    row[j] = v + j;
}

// SAFE: row i is a[i*m .. i*m + m - 1].
void s_rows(long *a, int n, int m) {
  // CHECK: summaries.c:[[#LOOP10:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: row_fill reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W row_fill(): row[j] : [i][j] of 'a' delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    row_fill(&a[i * m], m, i);
}

// UNKNOWN: j runs to m, one past the row: row i's last write is row i+1's
// first element. (A real dependence, but the row model can't express it.)
void u_rows_overrun(long *a, int n, int m) {
  // CHECK: summaries.c:[[#LOOP11:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: row_fill_inclusive reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W row_fill_inclusive(): row[j] : non-affine: the argument '&a[i * m]' starts a row of length 'm', and the callee's subscript can't be shown to stay inside it{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W row_fill_inclusive(): row[j]: W row_fill_inclusive(): row[j] is non-affine (the argument '&a[i * m]' starts a row of length 'm', and the callee's subscript can't be shown to stay inside it){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    row_fill_inclusive(&a[i * m], m, i);
}

// SAFE: the caller's inner loop picks the column: a[i][j].
void s_rows_inner_col(long *a, int n, int m) {
  // CHECK: summaries.c:[[#LOOP12:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W put(): p[k] : [i][j] of 'a' delinearized, dims [?][m]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: summaries.c:[[#LOOP13:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W put(): p[k] : [i][j] of 'a' delinearized, dims [?][m]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (m >= {{[0-9]+}}){{$}}
    for (int j = 0; j < m; j++)
      put(&a[i * m], j, i - j);
}

// --- 4. transitive calls ---------------------------------------------------

static void bump(long *p, long v) { p[0] += v; }
static void bump_next(long *q, long v) { bump(q + 1, v); }
static void bump_next_from(long *q) { bump(q + 1, q[0]); }

// SAFE: bump_next(&a[i]) -> bump(&a[i + 1]): one element per iteration.
void s_transitive(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP14:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: bump_next reaches 1 function(s), max depth 1: bump@1{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: RW bump_next(): bump(): p[0] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 2) + 1) <= (__INTPTR_TYPE__)a + 8 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n - 1; i++)
    bump_next(&a[i], b[i]);
}

// UNSAFE: ... and also reads a[i], which iteration i - 1 wrote.
void u_transitive(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP15:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: bump_next_from reaches 1 function(s), max depth 1: bump@1{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R bump_next_from(): q[0] : [i] of 'a'{{$}}
  // CHECK-NEXT: RW bump_next_from(): bump(): p[0] : [i + 1] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - R bump_next_from(): q[0] and RW bump_next_from(): bump(): p[0]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    bump_next_from(&a[i]);
}

// --- 5. globals as implicit parameters -------------------------------------

static void g_put(int k, long v) { g_tab[k] = v; }
static void g_add0(long v) { g_tab[0] += v; }
static void g_tick(void) { g_count++; }

// SAFE: g_put(i) writes g_tab[i] (callers keep n <= NG).
void s_global_elem(const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP16:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: g_put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W g_put(): g_tab[k] : [i] of 'g_tab'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'b' and 'g_tab' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)g_tab + 0 || (__INTPTR_TYPE__)g_tab + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    g_put(i, b[i]);
}

// UNSAFE: every iteration adds into g_tab[0].
void u_global_same(const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP17:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: g_add0 reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: RW g_add0(): g_tab[0] : [0] of 'g_tab'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - RW g_add0(): g_tab[0]: every iteration touches the same element{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'b' and 'g_tab' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    g_add0(b[i]);
}

// UNSAFE: a global scalar written by name.
void u_global_scalar(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP18:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: g_tick reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - 'g_tick' writes global 'g_count'{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'g_count' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = i;
    g_tick();
  }
}

// --- 6. a pointer to a whole scalar ---------------------------------------

// SAFE: t is declared in the loop, so every iteration has its own.
void s_addr_private(long *a, const long *b, int n) {
  // CHECK: summaries.c:[[#LOOP19:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: bump reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: RW bump(): p[0] : [0] of 't'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV); 1 on loop-private arrays skipped{{$}}
  // CHECK: alias      : 'b' and 'a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0 || (__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: pointer disjointness is only checked at run time -> CPU_PARALLEL, guarded by the check{{$}}
  for (int i = 0; i < n; i++) {
    long t = 0;
    bump(&t, b[i]);
    a[i] = t;
  }
}

// UNSAFE: t is shared by every iteration.
long u_addr_shared(const long *b, int n) {
  long t = 0;
  // CHECK: summaries.c:[[#LOOP20:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: bump reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R b[i] : [i]{{$}}
  // CHECK-NEXT: RW bump(): p[0] : [0] of 't'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - RW bump(): p[0]: every iteration touches the same element{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    bump(&t, b[i]);
  return t;
}

// --- 7. callee-private storage ---------------------------------------------

static void scratch(long *p, int k) {
  long tmp[4];
  // CHECK: summaries.c:[[#LOOP21:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 4{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W tmp[j] : [j]{{$}}
  // CHECK-NEXT: R p[k] : [k]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int j = 0; j < 4; j++)
    tmp[j] = p[k] + j;
  p[k] = tmp[3];
}

// SAFE: tmp is scratch's own on every call.
void s_callee_local(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP22:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: scratch reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R scratch(): p[k] : [i] of 'a'{{$}}
  // CHECK-NEXT: W scratch(): p[k] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    scratch(a, i);
}

// --- 8. what summaries can't see stays UNKNOWN -----------------------------

static void rec(long *p, int d) {
  if (d > 0)
    rec(p, d - 1);
  p[0] += d;
}
static void through_idx(long *p, const int *idx, int k) { p[idx[k]] = k; }
static void advance(long *p, int k) {
  p += k;
  *p = k;
}
static void at_global_off(long *p) { p[g_off] = 1; }

// UNKNOWN: rec recurses.
void k_recursive(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP23:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: rec reaches 0 function(s), max depth 0: <nothing further>{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: RW rec(): p[0] : [i] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'rec' recurses; call chain cannot be bounded{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    rec(&a[i], 3);
}

// UNKNOWN: indirect subscript inside the callee.
void k_indirect(long *a, const int *idx, int n) {
  // CHECK: summaries.c:[[#LOOP24:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: through_idx reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W through_idx(): p[idx[k]] : non-affine: subscript reads memory ('idx[k]'){{$}}
  // CHECK-NEXT: R through_idx(): idx[k] : [i] of 'idx'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W through_idx(): p[idx[k]]: W through_idx(): p[idx[k]] is non-affine (subscript reads memory ('idx[k]')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    through_idx(a, idx, i);
}

// UNKNOWN: the callee moves its pointer parameter.
void k_reassigned_param(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP25:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: advance reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W advance(): *p : non-affine: base pointer 'p' is modified inside the function{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W advance(): *p writes (base pointer 'p' is modified inside the function){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    advance(a, i);
}

// UNKNOWN: the callee indexes with a global the loop could change.
void k_global_subscript(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP26:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: at_global_off reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: W at_global_off(): p[g_off] : non-affine: 'at_global_off' indexes with 'g_off', which is not one of its parameters (the loop could change it between calls){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i] and W at_global_off(): p[g_off]: W at_global_off(): p[g_off] is non-affine ('at_global_off' indexes with 'g_off', which is not one of its parameters (the loop could change it between calls)){{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'g_off' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = 2;
    at_global_off(a);
  }
}

// UNKNOWN: the argument is not a named pointer plus an offset.
void k_opaque_arg(long **rows, int n) {
  // CHECK: summaries.c:[[#LOOP27:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: set0 reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: R rows[i] : [i]{{$}}
  // CHECK-NEXT: W set0(): p[0] : non-affine: parameter 'p' receives 'rows[i]' (argument is not a named pointer or array plus an offset){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W set0(): p[0] writes (parameter 'p' receives 'rows[i]' (argument is not a named pointer or array plus an offset)){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    set0(rows[i], i);
}

// UNKNOWN: unprototyped (K&R) calls convert nothing at the call site. Here
// k is really (signed char)i, which repeats every 256 iterations, and p
// steps over bytes, not longs. (Both were false SAFE in the 1.5 review.)
static void kr_narrow(p, k, v) long *p; signed char k; long v; { p[k + 128] = v; }
void k_kr_narrow(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP28:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: kr_narrow reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W kr_narrow(): p[k + 128] : non-affine: parameter 'p' receives 'a' (the call has no prototype, so the argument is not converted to the parameter's type){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W kr_narrow(): p[k + 128] writes (parameter 'p' receives 'a' (the call has no prototype, so the argument is not converted to the parameter's type)){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    kr_narrow(a, i, (long)i);
}

void kr_bytes();
void k_kr_bytes(long *a, int n) {
  // CHECK: summaries.c:[[#LOOP29:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: kr_bytes reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[2 * i + 3] : [2*i + 3]{{$}}
  // CHECK-NEXT: W kr_bytes(): p[8] : non-affine: parameter 'p' receives '&a[2 * i]' (the call has no prototype, so the argument is not converted to the parameter's type){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W kr_bytes(): p[8] writes (parameter 'p' receives '&a[2 * i]' (the call has no prototype, so the argument is not converted to the parameter's type)){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    kr_bytes(&a[2 * i]);
    a[2 * i + 3] = 7;
  }
}
void kr_bytes(p) char *p; { p[8] = 1; }
