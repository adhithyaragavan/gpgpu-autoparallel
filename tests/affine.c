// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 72 loop(s).
// Phase 1.2 fixture for affine subscript extraction (AffineAccess.h).
// Each loop's comment lists the expected access lines: kind (R/W/RW/&), the
// affine subscript per dimension, or "non-affine" with the reason. Both
// directions are covered: shapes that must come out affine, and shapes that
// must NOT (a wrong affine form would be a soundness bug in Phase 1.3).
// Verdicts are pinned too, but Phase 1.2 must not change any of them.

int g_bound;
void opaque_call(void);

// --- Affine, one dimension --------------------------------------------------

// W a[i] : [i]   R b[i + 1] : [i + 1]   R b[2 * i - 3] : [2*i - 3]
void shifts(double *a, const double *b, int n) {
  // CHECK: affine.c:[[#LOOP0:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R b[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: R b[2 * i - 3] : [2*i - 3]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)b + 8 || (__INTPTR_TYPE__)b + (8 * ((__INTPTR_TYPE__)n + 1) > 8 * ((2 * (__INTPTR_TYPE__)n - 5) + 1) ? 8 * ((__INTPTR_TYPE__)n + 1) : 8 * ((2 * (__INTPTR_TYPE__)n - 5) + 1)) <= (__INTPTR_TYPE__)a + 16){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 2; i < n; i++)
    a[i] = b[i + 1] + b[2 * i - 3];
}

// Symbols: n is a parameter nothing in the loop writes.
// W a[n - i - 1] : [-i + n - 1]  (IV terms print first, then symbols, then the constant)
void reverse_index(double *a, int n) {
  // CHECK: affine.c:[[#LOOP1:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[n - i - 1] : [-i + n - 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[n - i - 1] = 0.0;
}

// Folded constants: N * 2 is a compile-time constant.
// W a[i + N * 2] : [i + 8]
#define N 4
void folded_constant(double *a, int n) {
  // CHECK: affine.c:[[#LOOP2:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i + N * 2] : [i + 8]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i + N * 2] = 1.0;
}

// --- Multi-dimensional arrays ------------------------------------------------

// W m[i][j] : [i][j]  (static 2D array: one subscript per dimension)
double grid[64][64];
void static_2d(void) {
  // CHECK: affine.c:[[#LOOP3:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 64{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W grid[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 64; i++)
    // CHECK: affine.c:[[#LOOP4:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP3]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W grid[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 64; j++)
      grid[i][j] = 0.0;
}

// W m[i][j] : [i][j]  (pointer to rows of 4)
void row_pointer(double (*m)[4], int n) {
  // CHECK: affine.c:[[#LOOP5:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP6:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP5]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 4; j++)
      m[i][j] = 2.0;
}

// W m[i][j] : [i][j]  (VLA parameter)
void vla_2d(int n, int k, double m[n][k]) {
  // CHECK: affine.c:[[#LOOP7:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: 'm' points to arrays of non-constant size -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP8:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP7]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W m[i][j] : [i][j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < k; j++)
      m[i][j] = 3.0;
}

// NOT 2D: `double **` — a[i] loads a pointer.
// W a[i][j] : non-affine (base is a pointer loaded from memory)   R a[i] : [i]
void pointer_to_pointer(double **a, int n) {
  // CHECK: affine.c:[[#LOOP9:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i][j] : non-affine: base is a pointer loaded from memory ('a[i]'){{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W a[i][j] writes (base is a pointer loaded from memory ('a[i]')){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP10:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP9]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 2 (1 affine){{$}}
    // CHECK-NEXT: W a[i][j] : non-affine: base is a pointer loaded from memory ('a[i]'){{$}}
    // CHECK-NEXT: R a[i] : [i]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - cannot tell what W a[i][j] writes (base is a pointer loaded from memory ('a[i]')){{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++)
      a[i][j] = 4.0;
}

// A row of a 2D array passed to a call: & m[i] : [i]
void consume_row(double *row);
void pass_row(double m[8][8]) {
  // CHECK: affine.c:[[#LOOP11:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 8{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: consume_row reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: & m[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'consume_row' has no visible body in this TU{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 8; i++)
    consume_row(m[i]);
}

// --- Delinearization ---------------------------------------------------------

// W a[i * n + j] : [i][j] delinearized, dims [?][n]
// (j runs exactly 0..n-1, so 0 <= j < n)
void linear_2d(double *a, int n) {
  // CHECK: affine.c:[[#LOOP12:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP13:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP12]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 5.0;
}

// Column-major: W a[j * n + i] : [j][i] delinearized, dims [?][n]
void column_major(double *a, int n) {
  // CHECK: affine.c:[[#LOOP14:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[j * n + i] : [j][i] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'i'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int j = 0; j < n; j++)
    // CHECK: affine.c:[[#LOOP15:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP14]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : j, i{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[j * n + i] : [j][i] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int i = 0; i < n; i++)
      a[j * n + i] = 6.0;
}

// NOT delinearizable: j runs 0..m-1, not 0..n-1, so j may exceed a row of n.
// W a[i * n + j] : non-affine (multiplies two non-constant terms)
void wrong_bound(double *a, int n, int m) {
  // CHECK: affine.c:[[#LOOP16:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : non-affine: subscript multiplies two non-constant terms ('i * n'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript multiplies two non-constant terms ('i * n')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP17:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP16]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : non-affine: subscript multiplies two non-constant terms ('i * n'){{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript multiplies two non-constant terms ('i * n')){{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < m; j++)
      a[i * n + j] = 7.0;
}

// NOT delinearizable: j starts at 1.
// W a[i * n + j] : non-affine
void wrong_start(double *a, int n) {
  // CHECK: affine.c:[[#LOOP18:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: collapse   : 2 (with 'j'){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP19:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP18]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    for (int j = 1; j < n; j++)
      a[i * n + j] = 8.0;
}

// --- Pointer arithmetic ------------------------------------------------------

// W *(p + i) : [i]   R *(q - i) : [-i]   W *p : [0]
void pointer_forms(double *p, const double *q, int n) {
  // CHECK: affine.c:[[#LOOP20:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W *(p + i) : [i]{{$}}
  // CHECK-NEXT: R *(q - i) : [-i]{{$}}
  // CHECK-NEXT: W *p : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - writes to a target this analysis cannot resolve to a variable{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'p' and 'q' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    *(p + i) = *(q - i);
    *p = 9.0;
  }
}

// --- Forward substitution of single-assignment locals -------------------------

// int k = 2 * i + 1;  W a[k] : [2*i + 1]
void substituted_local(double *a, int n) {
  // CHECK: affine.c:[[#LOOP21:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[k] : [2*i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    int k = 2 * i + 1;
    a[k] = 10.0;
  }
}

// k is reassigned, so it is not substituted: W a[k] : non-affine (k changes)
void reassigned_local(double *a, int n) {
  // CHECK: affine.c:[[#LOOP22:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[k] : non-affine: uses 'k', which is assigned inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[k]: W a[k] is non-affine (uses 'k', which is assigned inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int k = i;
    k = k + 1;
    a[k] = 11.0;
  }
}

// Declared BEFORE the inner loop from the outer IV only.
// Outer loop: r is substituted, W a[r + j] : [4*i + j].
// Inner loop: r is a constant while it runs, so it stays a symbol: [j + r].
void substituted_before_inner(double *a, int n) {
  // CHECK: affine.c:[[#LOOP23:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[r + j] : [4*i + j]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    int r = i * 4;
    // CHECK: affine.c:[[#LOOP24:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP23]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[r + j] : [j + r]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 4; j++)
      a[r + j] = 12.0;
  }
}

// --- Non-affine: must never come out affine ----------------------------------

// The inner loop's own header update of j is how an IV changes, so the OUTER
// loop's view of a[i][j]-shaped accesses stays affine (see static_2d). But
// here the inner BODY also changes j, so j is not a well-behaved IV at either
// level: W a[i * 8 + j] : non-affine (both loops).
void inner_iv_modified(double *a, int n) {
  // CHECK: affine.c:[[#LOOP25:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * 8 + j] : non-affine: induction variable 'j' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * 8 + j]: W a[i * 8 + j] is non-affine (induction variable 'j' is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP26:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 8{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP25]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[i * 8 + j] : non-affine: induction variable 'j' is modified inside the loop{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop body reassigns the induction variable 'j'{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 8; j++) {
      a[i * 8 + j] = 23.0;
      j++;
    }
}

// R idx[i] : [i]   W a[idx[i]] : non-affine (subscript reads memory)
void indirect(double *a, const int *idx, int n) {
  // CHECK: affine.c:[[#LOOP27:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[idx[i]] : non-affine: subscript reads memory ('idx[i]'){{$}}
  // CHECK-NEXT: R idx[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[idx[i]]: W a[idx[i]] is non-affine (subscript reads memory ('idx[i]')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'idx' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    a[idx[i]] = 13.0;
}

// W a[i * i] : non-affine   W a[i / 2] : non-affine (operator '/')
// W a[i % 4] : non-affine (operator '%')
void nonlinear(double *a, int n) {
  // CHECK: affine.c:[[#LOOP28:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (0 affine){{$}}
  // CHECK-NEXT: W a[i * i] : non-affine: subscript multiplies two non-constant terms ('i * i'){{$}}
  // CHECK-NEXT: W a[i / 2] : non-affine: subscript uses operator '/'; in [0, n - 1]{{$}}
  // CHECK-NEXT: W a[i % 4] : non-affine: subscript uses operator '%'; in [0, 3]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * i]: W a[i * i] is non-affine (subscript multiplies two non-constant terms ('i * i')){{$}}
  // CHECK: deps       : 6 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i * i] = 14.0;
    a[i / 2] = 15.0;
    a[i % 4] = 16.0;
  }
}

// A symbol written in the loop is not a symbol: W a[i + off] : non-affine
void changing_offset(double *a, int n) {
  int off = 0;
  // CHECK: affine.c:[[#LOOP29:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i + off] : non-affine: uses 'off', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[i + off]: W a[i + off] is non-affine (uses 'off', which is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i + off] = 17.0;
    off = off + 2;
  }
}

// A global symbol with a call in the loop: the call could change it.
// W a[i + g_bound] : non-affine (a call could modify it)
void global_with_call(double *a, int n) {
  // CHECK: affine.c:[[#LOOP30:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: opaque_call reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i + g_bound] : non-affine: uses 'g_bound', which could be modified by a call inside the loop (global or address-taken){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'opaque_call' has no visible body in this TU{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'a' and 'g_bound' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i + g_bound] = 18.0;
    opaque_call();
  }
}

// A global symbol and no call or aliasing write: invariant.
// W a[i + g_bound] : [i + g_bound]
void global_without_call(double *a, int n) {
  // CHECK: affine.c:[[#LOOP31:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i + g_bound] : [i + g_bound]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'g_bound' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i + g_bound] = 19.0;
}

// Address-taken local, written through a same-typed pointer in the loop.
// W *q : [0]   W a[i + base] : non-affine (a write through a pointer could modify it)
void address_taken_symbol(double *a, int n) {
  int base = 3;
  int *q = &base;
  // CHECK: affine.c:[[#LOOP32:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[i + base] : non-affine: uses 'base', which could be modified by a write through a pointer inside the loop{{$}}
  // CHECK-NEXT: W *q : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[i + base]: W a[i + base] is non-affine (uses 'base', which could be modified by a write through a pointer inside the loop){{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i + base] = 20.0;
    *q = i;
  }
}

// The base pointer moves: W p[i] : non-affine (base pointer reassigned)
void moving_base(double *p, int n) {
  // CHECK: affine.c:[[#LOOP33:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W p[i] : non-affine: base pointer 'p' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot tell what W p[i] writes (base pointer 'p' is modified inside the loop){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    p[i] = 21.0;
    p = p + 1;
  }
}

// A struct member base is opaque: W s->v[i] : non-affine (struct member)
struct vec { double v[16]; };
void member_base(struct vec *s) {
  // CHECK: affine.c:[[#LOOP34:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 16{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W s->v[i] : non-affine: base is a struct member ('s->v'){{$}}
  // CHECK-NEXT: W s->v : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot tell what W s->v[i] writes (base is a struct member ('s->v')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 16; i++)
    s->v[i] = 22.0;
}

// --- Access kinds -------------------------------------------------------------

// RW a[i] (+=), RW b[i] (++), & c[i] (address taken)
void consume(double *x);
void kinds(double *a, int *b, double *c, int n) {
  // CHECK: affine.c:[[#LOOP35:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: consume reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: RW a[i] : [i]{{$}}
  // CHECK-NEXT: RW b[i] : [i]{{$}}
  // CHECK-NEXT: & c[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'consume' has no visible body in this TU{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'a' and 'b' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'a' and 'c' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'b' and 'c' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] += 1.0;
    b[i]++;
    consume(&c[i]);
  }
}

// ============ Regressions from the Phase 1.2 adversarial review ===============
// Each of these produced a WRONG affine form (or a missing/mis-kinded access)
// in the first Phase 1.2 build. 1.3 would have used them to prove loops safe.

// [R1a] An inner loop's variable used AFTER that loop is not a symbol: j == i
// there. Outer loop: W a[j] and R a[j + 1] both non-affine ('j' is modified).
void r1a_inner_iv_after_loop(int *a, int n) {
  int i, j;
  // CHECK: affine.c:[[#LOOP36:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W a[j] : non-affine: uses 'j', which is modified inside the loop{{$}}
  // CHECK-NEXT: R a[j + 1] : non-affine: uses 'j', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[j]: W a[j] is non-affine (uses 'j', which is modified inside the loop){{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: affine.c:[[#LOOP37:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP36]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 0 (0 affine){{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'j' is used after the loop, which may run zero times{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < i; j++)
      ;
    a[j] = a[j + 1];
  }
}

// [R1b] Reused induction variable. Outer loop: every access non-affine (its
// iteration space is unknown: the inner header rewrites i). Inner loop: an
// ordinary loop, W a[i] : [i], R a[i + 1] : [i + 1].
void r1b_reused_iv(int *a, int n) {
  int i;
  // CHECK: affine.c:[[#LOOP38:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: induction variable 'i' is modified inside the loop{{$}}
  // CHECK-NEXT: R a[i + 1] : non-affine: induction variable 'i' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop body reassigns the induction variable 'i'{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP39:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP38]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[i] : [i]{{$}}
    // CHECK-NEXT: R a[i + 1] : [i + 1]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W a[i] and R a[i + 1]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (i = 0; i < n; i++)
      a[i] = a[i + 1];
}

// [R1b'] Same variable, access after the inner loop: i is 3 there.
// Outer loop: W a[i] non-affine (iteration space unknown).
void r1b_reused_iv_after(int *a, int n) {
  int i;
  // CHECK: affine.c:[[#LOOP40:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: induction variable 'i' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - loop body reassigns the induction variable 'i'{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (i = 0; i < n; i++) {
    // CHECK: affine.c:[[#LOOP41:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 3{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP40]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, i{{$}}
    // CHECK: accesses   : 0 (0 affine){{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : lastprivate(i){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (i = 0; i < 3; i++)
      ;
    a[i] = 0;
  }
}

// [R1c] A parameter that an inner loop uses as its counter is not a symbol.
// Outer loop: W a[i + k] non-affine ('k' is modified).
void r1c_param_as_inner_iv(int *a, int n, int k) {
  // CHECK: affine.c:[[#LOOP42:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i + k] : non-affine: uses 'k', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[i + k]: W a[i + k] is non-affine (uses 'k', which is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i + k] = 1;
    // CHECK: affine.c:[[#LOOP43:@LINE+10]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP42]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, k{{$}}
    // CHECK: accesses   : 0 (0 affine){{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - induction variable 'k' is used after the loop, which may run zero times{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (k = 0; k < i; k++)
      ;
  }
}

// [R1d] Forward substitution must not capture a variable an inner loop
// changes. Outer loop: W a[k] non-affine (k = j, and 'j' is modified).
void r1d_substitution_of_inner_iv(int *a, int n) {
  int j = 0;
  // CHECK: affine.c:[[#LOOP44:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[k] : non-affine: uses 'k' (= j): uses 'j', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[k]: W a[k] is non-affine (uses 'k' (= j): uses 'j', which is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int k = j;
    // CHECK: affine.c:[[#LOOP45:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP44]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[k] : [k]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W a[k]: every iteration touches the same element{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (j = 0; j < i; j++)
      a[k] = 1;
  }
}

// [R1d'] Delinearization must not use a row length an inner loop changes.
// Outer loop: W a[i * n + j] non-affine (NOT delinearized). Inner j loop: n
// is fixed while it runs, so there it delinearizes: [i][j].
void r1d_row_length_changed(int *a, int n) {
  // CHECK: affine.c:[[#LOOP46:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 2 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : non-affine: uses 'n', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (uses 'n', which is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    // CHECK: affine.c:[[#LOOP47:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP46]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : [i][j] delinearized, dims [?][n]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
    // CHECK: - data region (line {{[0-9]+}}): not hoisted: 'i' changes inside it{{$}}
    for (int j = 0; j < n; j++)
      a[i * n + j] = 0;
    // CHECK: affine.c:[[#LOOP48:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP46]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, n{{$}}
    // CHECK: accesses   : 0 (0 affine){{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 0 pair(s), 0 independent{{$}}
    // CHECK: scalars    : lastprivate(n){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (n = 0; n < 4; n++)
      ;
  }
}

// [R2] The row index must stay in [0, n): here the body pushes j to n.
// Both loops: W a[i * n + j] non-affine (j modified, NOT delinearized).
void r2_row_index_escapes(int *a, int n) {
  // CHECK: affine.c:[[#LOOP49:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : non-affine: subscript multiplies two non-constant terms ('i * n'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript multiplies two non-constant terms ('i * n')){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP50:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP49]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : non-affine: subscript multiplies two non-constant terms ('i * n'){{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - loop body reassigns the induction variable 'j'{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < n; j++) {
      if (j == n - 1)
        j = n;
      a[i * n + j] = i;
    }
}

// [R2'] J's loop encloses the analyzed loop and changes n in its own body.
// Inner i loop: W a[i * n + j] non-affine (NOT delinearized).
void r2_row_length_changed_outside(int *a, int n) {
  // CHECK: affine.c:[[#LOOP51:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : j{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : non-affine: uses 'n', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (uses 'n', which is modified inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int j = 0; j < n; j++) {
    n = n - 1;
    // CHECK: affine.c:[[#LOOP52:@LINE+11]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 4{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP51]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : j, i{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : non-affine: subscript multiplies two non-constant terms ('i * n'){{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript multiplies two non-constant terms ('i * n')){{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int i = 0; i < 4; i++)
      a[i * n + j] = 0;
  }
}

// [R3] Narrowing or sign-changing casts wrap around: non-affine.
// W a[(unsigned char)i], RW a[(char)i], W a[(short)(i * 1000)],
// W a[(unsigned)k] all non-affine; W a[(long)k] : [k] (widening keeps values).
void r3_casts(int *a, long n) {
  // CHECK: affine.c:[[#LOOP53:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (0 affine){{$}}
  // CHECK-NEXT: W a[(unsigned char)i] : non-affine: subscript has a narrowing or sign-changing conversion ('(unsigned char)i'), which wraps around{{$}}
  // CHECK-NEXT: RW a[(char)i] : non-affine: subscript has a narrowing or sign-changing conversion ('(char)i'), which wraps around{{$}}
  // CHECK-NEXT: W a[(short)(i * 1000)] : non-affine: subscript has a narrowing or sign-changing conversion ('(short)(i * 1000)'), which wraps around{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[(unsigned char)i]: W a[(unsigned char)i] is non-affine (subscript has a narrowing or sign-changing conversion ('(unsigned char)i'), which wraps around){{$}}
  // CHECK: deps       : 6 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (long i = 0; i < n; i++) {
    a[(unsigned char)i] = 1;
    a[(char)i] += 1;
    a[(short)(i * 1000)] = 2;
  }
  // CHECK: affine.c:[[#LOOP54:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : k{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[(unsigned)k] : non-affine: subscript has a narrowing or sign-changing conversion ('(unsigned)k'), which wraps around{{$}}
  // CHECK-NEXT: W a[(long)k] : [k]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[(unsigned)k]: W a[(unsigned)k] is non-affine (subscript has a narrowing or sign-changing conversion ('(unsigned)k'), which wraps around){{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0; k < n; k++) {
    a[(unsigned)k] = 3;
    a[(long)k] = 4;
  }
}

// [R4] Writing a member of an element writes the element.
// W s[0] : [0], W s[i] : [i], R s[i + 1] : [i + 1], RW s[i] : [i] (++),
// & s[i] : [i] (address of a member).
struct pt { int x, y; };
void use_int(int *p);
void r4_member_kinds(struct pt *s, int n) {
  // CHECK: affine.c:[[#LOOP55:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: use_int reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: W s[0] : [0]{{$}}
  // CHECK-NEXT: W s[i] : [i]{{$}}
  // CHECK-NEXT: R s[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: RW s[i] : [i]{{$}}
  // CHECK-NEXT: & s[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'use_int' has no visible body in this TU{{$}}
  // CHECK: deps       : 12 pair(s), 6 independent (5 SIV, 1 Banerjee){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s[0].x = i;
    s[i].x = s[i + 1].y;
    ++s[i].y;
    use_int(&s[i].x);
  }
}

// [R5] Accesses through `->` are recorded.
// W (s + i)->x : [i], RW s->y : [0]
void r5_arrow(struct pt *s, int n) {
  // CHECK: affine.c:[[#LOOP56:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W (s + i)->x : [i]{{$}}
  // CHECK-NEXT: RW s->y : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - writes to a target this analysis cannot resolve to a variable{{$}}
  // CHECK: deps       : 3 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    (s + i)->x = 0;
    s->y += i;
  }
}

// [R6] A pointer declared in the body is replaced by what it points into.
// q = a + n - i, so q[i] is a[n]: RW q[i] : [n] of 'a' (the same element on
// every iteration, which 1.3 must see as a conflict).
void r6_body_pointer(int *a, int n) {
  // CHECK: affine.c:[[#LOOP57:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: RW q[i] : [n] of 'a'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - RW q[i]: every iteration touches the same element{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int *q = a + n - i;
    q[i] += 1;
  }
}

// [R6'] Row pointer into a 1-D array. Outer loop: W row[j] : [8*i + j] of 'a'.
void r6_row_pointer(double *a, int n) {
  // CHECK: affine.c:[[#LOOP58:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: & a[i * 8] : [8*i]{{$}}
  // CHECK-NEXT: W row[j] : [j + 8*i] of 'a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 Banerjee){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: the address of a[i * 8] is passed on -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < n; i++) {
    double *row = &a[i * 8];
    // CHECK: affine.c:[[#LOOP59:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 8{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP58]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W row[j] : [j]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int j = 0; j < 8; j++)
      row[j] = 0.0;
  }
}

// [R6''] A pointer whose address escaped and is moved through the alias:
// W p[i] non-affine. Also W *pp : [0] and R *pp : [0].
void r6_escaped_pointer(int *p, int n) {
  int **pp = &p;
  // CHECK: affine.c:[[#LOOP60:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W p[i] : non-affine: base pointer 'p' could be modified by a write through a pointer inside the loop{{$}}
  // CHECK-NEXT: W *pp : [0]{{$}}
  // CHECK-NEXT: R *pp : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot tell what W p[i] writes (base pointer 'p' could be modified by a write through a pointer inside the loop){{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    p[i] = 0;
    *pp = *pp - 1;
  }
}

// [R6'''] A global pointer base with a call in the loop: W gp[i] non-affine.
double *gp;
void r6_global_pointer(int n) {
  // CHECK: affine.c:[[#LOOP61:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: opaque_call reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W gp[i] : non-affine: base pointer 'gp' could be modified by a call inside the loop (global or address-taken){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'opaque_call' has no visible body in this TU{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    gp[i] = 0.0;
    opaque_call();
  }
}

// [M] sizeof does not evaluate its operand: only W a[i] is an access.
void m_sizeof_operand(long *a, const double *b, int n) {
  // CHECK: affine.c:[[#LOOP62:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    a[i] = sizeof(b[i + 7]);
}

// [L] *(arr + i) with a named ARRAY is affine: W *(arr + i) : [i]
double arr2[32];
void l_array_deref(void) {
  // CHECK: affine.c:[[#LOOP63:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 32{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W *(arr2 + i) : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - writes to a target this analysis cannot resolve to a variable{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 32; i++)
    *(arr2 + i) = 1.0;
}

// ============ Regressions from the second Phase 1.2 review ====================

// [R7] A void* round-trip changes the element size: c[i] is byte i, a[i] is
// int i. W c[i] non-affine (c set inside the loop), W a[i] : [i].
void r7_void_roundtrip(int *a, int n) {
  // CHECK: affine.c:[[#LOOP64:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W c[i] : non-affine: base pointer 'c' is set inside the loop{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W c[i] writes (base pointer 'c' is set inside the loop){{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    void *v = a;
    char *c = v;
    c[i] = 0;
    a[i] = 1;
  }
}

// [R7'] Two typed views of one void* buffer: both non-affine (set inside the
// loop through a conversion), never both "[i] of 'buf'".
void r7_two_views(void *buf, int n) {
  // CHECK: affine.c:[[#LOOP65:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W x[i] : non-affine: base pointer 'x' is set inside the loop{{$}}
  // CHECK-NEXT: W c[i] : non-affine: base pointer 'c' is set inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W x[i] writes (base pointer 'x' is set inside the loop){{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    int *x = buf;
    char *c = buf;
    x[i] = 1;
    c[i] = 2;
  }
}

// [R8] Unsigned wraparound: u + -1 is u - 1, not u + 4294967295.
// W a[u] : [u], R a[u + -1] non-affine (unsigned constant that wraps).
// In the second loop, a[u - 1] : [u - 1] (an ordinary subtraction).
void r8_unsigned_wrap(int *a, unsigned n) {
  // CHECK: affine.c:[[#LOOP66:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : u{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[u] : [u]{{$}}
  // CHECK-NEXT: R a[u + -1] : non-affine: subscript adds an unsigned constant that wraps around ('u + -1'){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[u] and R a[u + -1]: R a[u + -1] is non-affine (subscript adds an unsigned constant that wraps around ('u + -1')){{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (unsigned u = 1; u < n; u++)
    a[u] = a[u + -1];
  // CHECK: affine.c:[[#LOOP67:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : u{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[u] : [u]{{$}}
  // CHECK-NEXT: R a[u - 1] : [u - 1]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W a[u] and R a[u - 1]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (unsigned u = 1; u < n; u++)
    a[u] = a[u - 1];
}

// [R9] j unsigned vs n int: `j < n` compares as unsigned, so 0 <= j < n is
// not proven. W a[i * n + j] non-affine (NOT delinearized).
void r9_mixed_signedness(int *a, int n) {
  // CHECK: affine.c:[[#LOOP68:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i * n + j] : non-affine: subscript has a narrowing or sign-changing conversion ('i * n'), which wraps around{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript has a narrowing or sign-changing conversion ('i * n'), which wraps around){{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    // CHECK: affine.c:[[#LOOP69:@LINE+11]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP68]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : i, j{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[i * n + j] : non-affine: subscript has a narrowing or sign-changing conversion ('i * n'), which wraps around{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - cannot test W a[i * n + j]: W a[i * n + j] is non-affine (subscript has a narrowing or sign-changing conversion ('i * n'), which wraps around){{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (unsigned j = 0; j < n; j++)
      a[i * n + j] = 0;
}

// [R10] Inline asm writes its outputs: W a[i] non-affine (i modified).
void r10_asm(int *a, int n) {
  // CHECK: affine.c:[[#LOOP70:@LINE+11]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[i] : non-affine: induction variable 'i' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - loop body contains inline asm{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    a[i] = 0;
    __asm__ volatile("" : "+r"(i));
  }
}

// [R11] sizeof of a VLA type evaluates its size: RW a[i + 1] IS an access
// (and W b[i] : [i]).
void r11_sizeof_vla(int *a, long *b, int n) {
  // CHECK: affine.c:[[#LOOP71:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W b[i] : [i]{{$}}
  // CHECK-NEXT: RW a[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'b' and 'a' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    b[i] = sizeof(int[a[i + 1]++]);
}
