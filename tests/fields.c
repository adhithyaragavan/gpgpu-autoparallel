// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 22 loop(s).
//
// Phase 2.5: bases reached through struct fields (FieldPath in
// AffineAccess.h): `s->a[i]`, `sd.a[i]`, `s->atoms->p[i][k]`, `p->buf[i]`,
// and in-loop locals set from them (`double *a = sd.a;`). A path is a base
// only while nothing the loop or its callees write can change what it loads.
// Different paths are like pointer parameters nobody is known to call: a
// Types proof, sibling array members of one object, or a run-time overlap
// check that reads the path before the loop (only when the loop dereferences
// the same pointers anyway once it runs). Field accesses (`s->n`) are kept
// apart from accesses of other scalar types. Every SAFE case has a twin that
// must not be. Also RUN by scripts/check_parallel.sh with
// eval/runtime/fields_main.c, which calls every checked loop with disjoint
// and with OVERLAPPING fields.

typedef double real3[3];
struct vecs { int n; double scale; double *a, *b; int *ia; };
struct atoms { real3 *p, *f; double *u; };
struct sim { struct atoms *atoms; double e; };
struct dom { double lo[8], hi[8], ext[8]; };
struct holder { double *d; };

// --- SAFE -------------------------------------------------------------------

// Two pointer fields: checked at run time, behind a test that the loop runs
// (reading s->a dereferences s, which the loop does on its first iteration).
void f_ptr(struct vecs *s, int n) {
  // CHECK: fields.c:[[#LOOP0:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W s->a[i] : [i]{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: R s->b[i] : [i]{{$}}
  // CHECK-NEXT: R s->b : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: alias      : 's->a' and 's->b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)n - 1) >= 0 && ((__INTPTR_TYPE__)s->a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)s->b + 0 || (__INTPTR_TYPE__)s->b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)s->a + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    s->a[i] = s->b[i] * 2.0;
}

// A struct by value: reading sd.a dereferences nothing, no runs test.
void f_value(struct vecs sd, int n) {
  // CHECK: fields.c:[[#LOOP1:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W sd.a[i] : [i]{{$}}
  // CHECK-NEXT: R sd.b[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'sd.a' and 'sd.b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)sd.a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)sd.b + 0 || (__INTPTR_TYPE__)sd.b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)sd.a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    sd.a[i] = sd.b[i] + 1.0;
}

// XSBench: locals set from fields inside the loop stand for the fields.
void f_local(struct vecs sd, int n) {
  // CHECK: fields.c:[[#LOOP2:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i] of 'sd.a'{{$}}
  // CHECK-NEXT: R b[i] : [i] of 'sd.b'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'sd.a' and 'sd.b' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)sd.a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)sd.b + 0 || (__INTPTR_TYPE__)sd.b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)sd.a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    double *a = sd.a;
    const double *b = sd.b;
    a[i] = b[i] + sd.scale;
  }
}

// CoMD: rows through two levels of pointers.
void f_rows(struct sim *s, int n, double dt) {
  // CHECK: fields.c:[[#LOOP3:@LINE+29]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 18 (18 affine){{$}}
  // CHECK-NEXT: RW s->atoms->p[i][0] : [i][0]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[i][0] : [i][0]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[i][1] : [i][1]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[i][1] : [i][1]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[i][2] : [i][2]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[i][2] : [i][2]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 42 pair(s), 42 independent (6 SIV, 36 type){{$}}
  // CHECK: alias      : 's->atoms->p' and 's->atoms->f' may overlap: checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)n - 1) >= 0 && ((__INTPTR_TYPE__)s->atoms->p + ((8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1)) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 2 + 1) ? (8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1)) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 2 + 1)) <= (__INTPTR_TYPE__)s->atoms->f + ((8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) < 8 * ((0) * 3 + 2) ? (8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) : 8 * ((0) * 3 + 2)) || (__INTPTR_TYPE__)s->atoms->f + ((8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1)) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 2 + 1) ? (8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) > 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1) ? 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 0 + 1) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 1 + 1)) : 8 * ((((__INTPTR_TYPE__)n - 1)) * 3 + 2 + 1)) <= (__INTPTR_TYPE__)s->atoms->p + ((8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) < 8 * ((0) * 3 + 2) ? (8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) : 8 * ((0) * 3 + 2)))){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++) {
    s->atoms->p[i][0] += dt * s->atoms->f[i][0];
    s->atoms->p[i][1] += dt * s->atoms->f[i][1];
    s->atoms->p[i][2] += dt * s->atoms->f[i][2];
  }
}

// An array member: one object, distinct elements; was UNSAFE (the write to
// d->lo[i] counted as a write of all of *d every iteration).
void f_member(struct dom *d, int n) {
  // CHECK: fields.c:[[#LOOP4:@LINE+11]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 8{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W d->lo[i] : [i]{{$}}
  // CHECK-NEXT: R d->lo[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 8; i++)
    d->lo[i] = d->lo[i] * 2.0 + n;
}

// Sibling array members of one object never overlap: no check.
void f_siblings(struct dom *d) {
  // CHECK: fields.c:[[#LOOP5:@LINE+12]]:3{{$}}
  // CHECK: kind       : CONSTANT_BOUND{{$}}
  // CHECK: trip count : 8{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W d->ext[i] : [i]{{$}}
  // CHECK-NEXT: R d->hi[i] : [i]{{$}}
  // CHECK-NEXT: R d->lo[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < 8; i++)
    d->ext[i] = d->hi[i] - d->lo[i];
}

// int and double objects can't overlap (strict aliasing): no check.
void f_types(struct vecs *s, int n) {
  // CHECK: fields.c:[[#LOOP6:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W s->ia[i] : [i]{{$}}
  // CHECK-NEXT: R s->ia : [0]{{$}}
  // CHECK-NEXT: R s->a[i] : [i]{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: alias      : 's->ia' and 's->a' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    s->ia[i] = (int)s->a[i];
}

// A double field read next to a double array: checked at run time; the
// int field s->n beside an int-free loop needs nothing.
void f_scalar(struct vecs *s, double *x, int n) {
  // CHECK: fields.c:[[#LOOP7:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R s->scale : [0]{{$}}
  // CHECK-NEXT: R x[i] : [i]{{$}}
  // CHECK-NEXT: R s->n : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (2 SIV, 1 type){{$}}
  // CHECK: alias      : 'x' and 's' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)x + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)s + 0 || (__INTPTR_TYPE__)s + 40 <= (__INTPTR_TYPE__)x + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    x[i] = s->scale * x[i] + s->n;
}

// A path passed to a callee: the callee's write lands on s->a.
static void put(double *p, int k, double v) { p[k] = v; }
void f_arg(struct vecs *s, int n) {
  // CHECK: fields.c:[[#LOOP8:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: put reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: R s->b[i] : [i]{{$}}
  // CHECK-NEXT: R s->b : [0]{{$}}
  // CHECK-NEXT: W put(): p[k] : [i] of 's->a'{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: alias      : 's->b' and 's->a' may overlap: checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)n - 1) >= 0 && ((__INTPTR_TYPE__)s->b + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)s->a + 0 || (__INTPTR_TYPE__)s->a + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)s->b + 0)){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n; i++)
    put(s->a, i, s->b[i] + 3.0);
}

// The field holds a copy of the restrict parameter `a`: restrict says
// nothing about it, so the pair is checked at run time (and here always
// overlaps).
void f_based(double *restrict a, int n) {
  struct holder h = {a};
  // CHECK: fields.c:[[#LOOP9:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: R h.d[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 'a' and 'h.d' may overlap: checked at run time{{$}}
  // CHECK: alias guard: ((__INTPTR_TYPE__)a + 8 * (((__INTPTR_TYPE__)n - 2) + 1) <= (__INTPTR_TYPE__)h.d + 8 || (__INTPTR_TYPE__)h.d + 8 * (((__INTPTR_TYPE__)n - 1) + 1) <= (__INTPTR_TYPE__)a + 0){{$}}
  // CHECK: profitability : CPU_PARALLEL if (n - 1 >= {{[0-9]+}}){{$}}
  for (int i = 0; i < n - 1; i++)
    a[i] = h.d[i + 1] * 0.5;
}

// --- not SAFE ---------------------------------------------------------------

// A real dependence through a path.
void g_carried(struct vecs *s, int n) {
  // CHECK: fields.c:[[#LOOP10:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (4 affine){{$}}
  // CHECK-NEXT: W s->a[i + 1] : [i + 1]{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: R s->a[i] : [i]{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - W s->a[i + 1] and R s->a[i]: the same element is touched 1 iteration(s) apart{{$}}
  // CHECK: rejected   : array-dependence{{$}}
  // CHECK: deps       : 4 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n - 1; i++)
    s->a[i + 1] = s->a[i] + 1.0;
}

// The loop itself writes a pointer field: s->a may change.
void g_own(struct vecs *s, double **out, int n) {
  // CHECK: fields.c:[[#LOOP11:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (3 affine){{$}}
  // CHECK-NEXT: W s->a[i] : non-affine: base is a pointer loaded from memory ('s->a'), which the loop may change: a write of type 'double *' inside the loop may change it{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: W out[i] : [i]{{$}}
  // CHECK-NEXT: R s->b : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W s->a[i] writes (base is a pointer loaded from memory ('s->a'), which the loop may change: a write of type 'double *' inside the loop may change it){{$}}
  // CHECK: rejected   : unknown-base{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 's' and 'out' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s->a[i] = 1.0;
    out[i] = s->b;
  }
}

// A callee changes s->atoms.
static void g_set(struct sim *s, struct atoms *o) { s->atoms = o; }
void g_callee(struct sim *s, struct atoms *o, double *x, int n) {
  // CHECK: fields.c:[[#LOOP12:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: g_set reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 5 (3 affine){{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R s->atoms->u[i] : non-affine: base is a pointer loaded from memory ('s->atoms->u'), which the loop may change: g_set(): s->atoms writes a 'struct atoms *'{{$}}
  // CHECK-NEXT: R s->atoms->u : non-affine: base is a pointer loaded from memory ('s->atoms'), which the loop may change: g_set(): s->atoms writes a 'struct atoms *'{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: W g_set(): s->atoms : [0] of 's'{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - R s->atoms->u[i] reads memory this analysis cannot name (base is a pointer loaded from memory ('s->atoms->u'), which the loop may change: g_set(): s->atoms writes a 'struct atoms *'), which a write in the loop could alias{{$}}
  // CHECK: rejected   : unknown-base, array-dependence{{$}}
  // CHECK: deps       : 5 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    x[i] = s->atoms->u[i];
    g_set(s, o);
  }
}

// The root is reassigned in the loop.
void g_root(struct vecs *s, struct vecs *t, int n) {
  // CHECK: fields.c:[[#LOOP13:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W s->a[i] : non-affine: base is a pointer loaded from memory ('s->a'), which the loop may change: 's' is modified inside the loop{{$}}
  // CHECK-NEXT: R s->a : non-affine: base pointer 's' is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot tell what W s->a[i] writes (base is a pointer loaded from memory ('s->a'), which the loop may change: 's' is modified inside the loop){{$}}
  // CHECK: rejected   : unknown-base, scalar-dependence{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s->a[i] = 2.0;
    s = t;
  }
}

// A char write may change any pointer field.
void g_char(struct vecs *s, char *c, int n) {
  // CHECK: fields.c:[[#LOOP14:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (2 affine){{$}}
  // CHECK-NEXT: W s->a[i] : non-affine: base is a pointer loaded from memory ('s->a'), which the loop may change: a write of type 'char' inside the loop may change it{{$}}
  // CHECK-NEXT: R s->a : [0]{{$}}
  // CHECK-NEXT: W c[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot tell what W s->a[i] writes (base is a pointer loaded from memory ('s->a'), which the loop may change: a write of type 'char' inside the loop may change it){{$}}
  // CHECK: rejected   : unknown-base{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: alias      : 's' and 'c' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    s->a[i] = 1.0;
    c[i] = 0;
  }
}

// The loop dereferences s->atoms only when flag[i] is set: reading the path
// before the loop could dereference a pointer the loop never does.
void g_cond(struct sim *s, const int *flag, double *x, int n) {
  // CHECK: fields.c:[[#LOOP15:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 5 (5 affine){{$}}
  // CHECK-NEXT: R flag[i] : [i]{{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R s->atoms->u[i] : [i]{{$}}
  // CHECK-NEXT: R s->atoms->u : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'x' and 's->atoms->u' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: evaluating 's->atoms->u' before the loop would dereference a pointer the loop may never dereference{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (1 SIV, 2 type){{$}}
  // CHECK: alias      : 'flag' and 'x' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    if (flag[i])
      x[i] = s->atoms->u[i];
}

// An anonymous struct member has no name a check could use.
struct anon { struct { double *q; }; double *r; };
void g_anon(struct anon *s, int n) {
  // CHECK: fields.c:[[#LOOP16:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (3 affine){{$}}
  // CHECK-NEXT: W s->r[i] : [i]{{$}}
  // CHECK-NEXT: R s->r : [0]{{$}}
  // CHECK-NEXT: R s->q[i] : non-affine: base is a pointer loaded from memory ('s->q'){{$}}
  // CHECK-NEXT: R s->q : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - R s->q[i] reads memory this analysis cannot name (base is a pointer loaded from memory ('s->q')), which a write in the loop could alias{{$}}
  // CHECK: rejected   : unknown-base{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 type){{$}}
  // CHECK: alias      : 's->r' and 's' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s->r[i] = s->q[i];
}

// A union's fields overlap: not a path.
union un { double *a; long *b; };
struct withu { union un u; double *c; };
void g_union(struct withu *s, int n) {
  // CHECK: fields.c:[[#LOOP17:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 4 (3 affine){{$}}
  // CHECK-NEXT: W s->c[i] : [i]{{$}}
  // CHECK-NEXT: R s->c : [0]{{$}}
  // CHECK-NEXT: R s->u.a[i] : non-affine: base is a pointer loaded from memory ('s->u.a'){{$}}
  // CHECK-NEXT: R s->u : [0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - R s->u.a[i] reads memory this analysis cannot name (base is a pointer loaded from memory ('s->u.a')), which a write in the loop could alias{{$}}
  // CHECK: rejected   : unknown-base{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 type){{$}}
  // CHECK: alias      : 's->c' and 's' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++)
    s->c[i] = s->u.a[i];
}

// A global root and a call: the call may change the global.
struct vecs gv;
void g_opaque(void);
void g_global(int n) {
  // CHECK: fields.c:[[#LOOP18:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: g_opaque reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (0 affine){{$}}
  // CHECK-NEXT: W gv.a[i] : non-affine: base is a pointer loaded from memory ('gv.a'), which the loop may change: 'gv' could be modified by a call inside the loop (global or address-taken){{$}}
  // CHECK-NEXT: R gv.b[i] : non-affine: base is a pointer loaded from memory ('gv.b'), which the loop may change: 'gv' could be modified by a call inside the loop (global or address-taken){{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'g_opaque' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee, unknown-base{{$}}
  // CHECK: hint       : make the definition of 'g_opaque' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 0 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    gv.a[i] = gv.b[i];
    g_opaque();
  }
}

// Review fixes (2026-10-20). Field types keep accesses apart only under
// strict aliasing: never through a union member (read through another
// member) or for a bit-field (one memory location with its neighbours).
union pun { int i; float f; };
static void pun_set(union pun *q, int v) { q->i = v; }
static float pun_get(union pun *q) { return q->f; }
void g_pun(union pun *u, float *out, int k, int n) {
  // CHECK: fields.c:[[#LOOP19:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: pun_get reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: pun_set reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: W out[i] : [i]{{$}}
  // CHECK-NEXT: R pun_get(): q->f : [i] of 'u'{{$}}
  // CHECK-NEXT: W pun_set(): q->i : [i + k] of 'u'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove R pun_get(): q->f and W pun_set(): q->i independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'out' and 'u' may overlap: checked at run time{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    out[i] = pun_get(u + i);
    pun_set(u + i + k, i);
  }
}

struct bits { unsigned short lo : 4; unsigned int hi : 4; };
static void bits_lo(struct bits *q, int v) { q->lo = (unsigned short)v; }
static void bits_hi(struct bits *q, int v) { q->hi = (unsigned)v; }
void g_bits(struct bits *p, int k, int n) {
  // CHECK: fields.c:[[#LOOP20:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: bits_lo reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: bits_hi reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W bits_lo(): q->lo : [i + k] of 'p'{{$}}
  // CHECK-NEXT: W bits_hi(): q->hi : [i] of 'p'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W bits_lo(): q->lo and W bits_hi(): q->hi independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    bits_lo(p + i + k, (i + 1) & 15);
    bits_hi(p + i, (i * 7 + 3) & 15);
  }
}

// `s->v->lo` (an array member) loads nothing, so it doesn't make s->v a
// pointer the loop dereferences: the check would read s->v->a, which the
// loop only does under the flag. UNKNOWN, no check.
struct inner { double *a, *b; double lo[4]; };
struct outer { struct inner *v; };
void g_decay(struct outer *s, int *flag, double *x, int n) {
  // CHECK: fields.c:[[#LOOP21:@LINE+25]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 11 (11 affine){{$}}
  // CHECK-NEXT: R s->v->lo : [0]{{$}}
  // CHECK-NEXT: R s->v : [0]{{$}}
  // CHECK-NEXT: W x[i] : [i]{{$}}
  // CHECK-NEXT: R flag[i] : [i]{{$}}
  // CHECK-NEXT: W s->v->a[i] : [i]{{$}}
  // CHECK-NEXT: R s->v->a : [0]{{$}}
  // CHECK-NEXT: R s->v : [0]{{$}}
  // CHECK-NEXT: R s->v->b[i] : [i]{{$}}
  // CHECK-NEXT: R s->v->b : [0]{{$}}
  // CHECK-NEXT: R s->v : [0]{{$}}
  // CHECK-NEXT: R row[0] : [0] of 's->v->lo'{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 's->v' and 's->v->a' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: evaluating 's->v->a' before the loop would dereference a pointer the loop may never dereference{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: deps       : 12 pair(s), 12 independent (2 SIV, 10 type){{$}}
  // CHECK: alias      : 's->v' and 'x' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'x' and 'flag' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'flag' and 's->v->a' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < n; i++) {
    double *row = s->v->lo;
    x[i] = 1.0;
    if (flag[i]) s->v->a[i] = s->v->b[i] + row[0];
  }
}
