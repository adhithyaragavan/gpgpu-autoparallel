// RUN: %p05 %s -- %cflags | FileCheck %s
// CHECK: Analyzed 36 loop(s).
// Phase 2.5: CoMD's per-box loops. Atoms of box b live in slots
// [MAXATOMS * b, MAXATOMS * b + nAtoms[b]), walked with two counters:
//   for (int iOff = MAXATOMS * b, ii = 0; ii < nAtoms[b]; ii++, iOff++)
// Three things make the box loop SAFE: the second counter follows the first
// (linked counters), a check before the loop that no box holds more than
// MAXATOMS (an inspector over nAtoms), and overlap checks for pointers only
// the inner loop dereferences (evaluated only when some box is non-empty).
// Plus integer fields the loop can't change as symbols (`s->boxes->nLocal`).
// Twins below show each requirement failing.
// Runtime check: scripts/check_parallel.sh tests/boxes.c eval/runtime/boxes_main.c

#define MAXATOMS 64

typedef double real3[3];

struct atoms {
  real3 *r, *p, *f;
  int *gid;
};
struct boxes {
  int nLocal;
  int *nAtoms;
};
struct sim {
  struct atoms *atoms;
  struct boxes *boxes;
  double dt;
};

double rng(int k);

// --- SAFE -------------------------------------------------------------------

// advanceVelocity: the box loop, bounded by a parameter.
void b_velocity(struct sim *s, int nBoxes, double dt) {
  // CHECK: boxes.c:[[#LOOP0:@LINE+35]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : iBox{{$}}
  // CHECK: accesses   : 21 (21 affine){{$}}
  // CHECK-NEXT: R s->boxes->nAtoms[iBox] : [iBox]{{$}}
  // CHECK-NEXT: R s->boxes->nAtoms : [0]{{$}}
  // CHECK-NEXT: R s->boxes : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][0] : [64*iBox + ii][0]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[iOff][0] : [64*iBox + ii][0]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][1] : [64*iBox + ii][1]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[iOff][1] : [64*iBox + ii][1]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][2] : [64*iBox + ii][2]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: R s->atoms->f[iOff][2] : [64*iBox + ii][2]{{$}}
  // CHECK-NEXT: R s->atoms->f : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 48 pair(s), 48 independent (3 ZIV, 3 Banerjee, 42 type){{$}}
  // CHECK: alias      : 's->boxes->nAtoms' and 's->atoms->p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 's->atoms->p' and 's->atoms->f' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'ii' stays below 64: its loop's bound checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)nBoxes - 1) >= 0 && p05_all_le_int(s->boxes->nAtoms, 0, ((__INTPTR_TYPE__)nBoxes - 1), 64)) && (((__INTPTR_TYPE__)nBoxes - 1) >= 0 && p05_any_gt_int(s->boxes->nAtoms, 0, ((__INTPTR_TYPE__)nBoxes - 1), 0) && ((__INTPTR_TYPE__)s->atoms->p + ((8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1) ? 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1)) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 2 + 1) ? (8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1) ? 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1)) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 2 + 1)) <= (__INTPTR_TYPE__)s->atoms->f + ((8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) < 8 * ((0) * 3 + 2) ? (8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) : 8 * ((0) * 3 + 2)) || (__INTPTR_TYPE__)s->atoms->f + ((8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1) ? 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1)) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 2 + 1) ? (8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) > 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1) ? 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 0 + 1) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 1 + 1)) : 8 * (((64 * (__INTPTR_TYPE__)nBoxes - 1)) * 3 + 2 + 1)) <= (__INTPTR_TYPE__)s->atoms->p + ((8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) < 8 * ((0) * 3 + 2) ? (8 * ((0) * 3 + 0) < 8 * ((0) * 3 + 1) ? 8 * ((0) * 3 + 0) : 8 * ((0) * 3 + 1)) : 8 * ((0) * 3 + 2)))){{$}}
  // CHECK: collapse   : no, the inner loop steps several counters{{$}}
  // CHECK: profitability : CPU_PARALLEL if (nBoxes >= {{[0-9]+}}){{$}}
  for (int iBox = 0; iBox < nBoxes; iBox++) {
    // CHECK: boxes.c:[[#LOOP1:@LINE+33]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP0]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : iBox, ii{{$}}
    // CHECK: accesses   : 21 (21 affine){{$}}
    // CHECK-NEXT: R s->boxes->nAtoms[iBox] : [iBox]{{$}}
    // CHECK-NEXT: R s->boxes->nAtoms : [0]{{$}}
    // CHECK-NEXT: R s->boxes : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][0] : [64*iBox + ii][0]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: R s->atoms->f[iOff][0] : [64*iBox + ii][0]{{$}}
    // CHECK-NEXT: R s->atoms->f : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][1] : [64*iBox + ii][1]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: R s->atoms->f[iOff][1] : [64*iBox + ii][1]{{$}}
    // CHECK-NEXT: R s->atoms->f : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][2] : [64*iBox + ii][2]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: R s->atoms->f[iOff][2] : [64*iBox + ii][2]{{$}}
    // CHECK-NEXT: R s->atoms->f : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 's->atoms->p' and 's->atoms->f' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 's->atoms->p' it touches is unknown: a loop range in s->atoms->p[iOff][0] can't be bounded{{$}}
    // CHECK: rejected   : pointer-overlap{{$}}
    // CHECK: deps       : 48 pair(s), 48 independent (6 SIV, 42 type){{$}}
    // CHECK: alias      : 's->boxes->nAtoms' and 's->atoms->p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int iOff = MAXATOMS * iBox, ii = 0; ii < s->boxes->nAtoms[iBox];
         ii++, iOff++) {
      s->atoms->p[iOff][0] += dt * s->atoms->f[iOff][0];
      s->atoms->p[iOff][1] += dt * s->atoms->f[iOff][1];
      s->atoms->p[iOff][2] += dt * s->atoms->f[iOff][2];
    }
  }
}

// setTemperature's rescale: bounded by a field the loop can't change.
void b_scale(struct sim *s, double factor) {
  // CHECK: boxes.c:[[#LOOP2:@LINE+27]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : iBox{{$}}
  // CHECK: accesses   : 14 (14 affine){{$}}
  // CHECK-NEXT: R s->boxes->nLocal : [0]{{$}}
  // CHECK-NEXT: R s->boxes : [0]{{$}}
  // CHECK-NEXT: R s->boxes->nAtoms[iBox] : [iBox]{{$}}
  // CHECK-NEXT: R s->boxes->nAtoms : [0]{{$}}
  // CHECK-NEXT: R s->boxes : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][0] : [64*iBox + ii][0]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][1] : [64*iBox + ii][1]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: RW s->atoms->p[iOff][2] : [64*iBox + ii][2]{{$}}
  // CHECK-NEXT: R s->atoms->p : [0]{{$}}
  // CHECK-NEXT: R s->atoms : [0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 36 pair(s), 36 independent (3 ZIV, 3 Banerjee, 30 type){{$}}
  // CHECK: alias      : 's->boxes->nAtoms' and 's->atoms->p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'ii' stays below 64: its loop's bound checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)s->boxes->nLocal - 1) >= 0 && p05_all_le_int(s->boxes->nAtoms, 0, ((__INTPTR_TYPE__)s->boxes->nLocal - 1), 64)){{$}}
  // CHECK: collapse   : no, the inner loop steps several counters{{$}}
  // CHECK: profitability : CPU_PARALLEL if (s->boxes->nLocal >= {{[0-9]+}}){{$}}
  for (int iBox = 0; iBox < s->boxes->nLocal; ++iBox) {
    // CHECK: boxes.c:[[#LOOP3:@LINE+22]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP2]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : iBox, ii{{$}}
    // CHECK: accesses   : 12 (12 affine){{$}}
    // CHECK-NEXT: R s->boxes->nAtoms[iBox] : [iBox]{{$}}
    // CHECK-NEXT: R s->boxes->nAtoms : [0]{{$}}
    // CHECK-NEXT: R s->boxes : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][0] : [64*iBox + ii][0]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][1] : [64*iBox + ii][1]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: RW s->atoms->p[iOff][2] : [64*iBox + ii][2]{{$}}
    // CHECK-NEXT: R s->atoms->p : [0]{{$}}
    // CHECK-NEXT: R s->atoms : [0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 30 pair(s), 30 independent (6 SIV, 24 type){{$}}
    // CHECK: alias      : 's->boxes->nAtoms' and 's->atoms->p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int iOff = MAXATOMS * iBox, ii = 0; ii < s->boxes->nAtoms[iBox];
         ++ii, ++iOff) {
      s->atoms->p[iOff][0] *= factor;
      s->atoms->p[iOff][1] *= factor;
      s->atoms->p[iOff][2] *= factor;
    }
  }
}

// The same with plain pointer parameters: the inspector reads n, the
// overlap check needs no "some box is non-empty" test.
void b_params(real3 *p, const real3 *f, const int *n, int nb, double dt) {
  // CHECK: boxes.c:[[#LOOP4:@LINE+18]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: RW p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: R f[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'p' and 'f' may overlap: checked at run time{{$}}
  // CHECK: alias      : 'i' stays below 64: its loop's bound checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)nb - 1) >= 0 && p05_all_le_int(n, 0, ((__INTPTR_TYPE__)nb - 1), 64)) && ((__INTPTR_TYPE__)p + 8 * (((64 * (__INTPTR_TYPE__)nb - 1)) * 3 + 0 + 1) <= (__INTPTR_TYPE__)f + 8 * ((0) * 3 + 0) || (__INTPTR_TYPE__)f + 8 * (((64 * (__INTPTR_TYPE__)nb - 1)) * 3 + 0 + 1) <= (__INTPTR_TYPE__)p + 8 * ((0) * 3 + 0)){{$}}
  // CHECK: collapse   : no, the inner loop steps several counters{{$}}
  // CHECK: profitability : CPU_PARALLEL if (nb >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: pointer disjointness is only checked at run time -> CPU_PARALLEL, guarded by the check{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP5:@LINE+17]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP4]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: RW p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: R f[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 'p' and 'f' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'p' it touches is unknown: a loop range in p[o][0] can't be bounded{{$}}
    // CHECK: rejected   : pointer-overlap{{$}}
    // CHECK: hint       : declare parameter 'p' of 'b_params' restrict, if no call lets what 'p' points to overlap anything else 'b_params' accesses (here 'f'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
    // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] += dt * f[o][0];
}

// Labels each atom with its box: an assignment, so a box over capacity
// (whose atoms land in the next box's slots) would make the result depend on
// which box writes last.
void b_label(real3 *p, const int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP6:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][1] : [64*b + i][1]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'i' stays below 64: its loop's bound checked at run time{{$}}
  // CHECK: alias guard: (((__INTPTR_TYPE__)nb - 1) >= 0 && p05_all_le_int(n, 0, ((__INTPTR_TYPE__)nb - 1), 64)){{$}}
  // CHECK: collapse   : no, the inner loop steps several counters{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  // CHECK: - GPU offload declined: pointer disjointness is only checked at run time, and threading does not beat sequential -> SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP7:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP6]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][1] : [64*b + i][1]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][1] = b;
}

// Two counters and a constant inner bound: no inspector needed. The inner
// loop is SAFE too, but never gets a pragma itself (not OpenMP's canonical
// form).
void b_flat(double *restrict a, const double *restrict c, int nb) {
  // CHECK: boxes.c:[[#LOOP8:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: W a[o] : [64*b + i]{{$}}
  // CHECK-NEXT: R c[o] : [64*b + i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 Banerjee){{$}}
  // CHECK: alias      : 'a' and 'c' can't overlap (restrict){{$}}
  // CHECK: collapse   : no, the inner loop steps several counters{{$}}
  // CHECK: profitability : GPU_OFFLOAD if (nb >= {{[0-9]+}}){{$}}
  // CHECK: - data/iteration: 16 B across 2 region(s): a (map(tofrom: a[0:(64 * (__INTPTR_TYPE__)nb)]), 8 B/elem), c (map(to: c[0:(64 * (__INTPTR_TYPE__)nb)]), 8 B/elem){{$}}
  // CHECK: - map guard: nb >= 1{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP9:@LINE+12]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP8]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: W a[o] : [64*b + i]{{$}}
    // CHECK-NEXT: R c[o] : [64*b + i]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'a' and 'c' can't overlap (restrict){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < MAXATOMS; i++, o++)
      a[o] = c[o] * 2.0;
}

// A counter stepping by 3 per iteration of a stride-1 induction variable.
void b_stride(double *restrict a, int n) {
  // CHECK: boxes.c:[[#LOOP10:@LINE+10]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 1 (1 affine){{$}}
  // CHECK-NEXT: W a[k] : [3*i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int k = 0, i = 0; i < n; i++, k += 3)
    a[k] = 1.0;
}

// A scalar field bound with no boxes at all.
void b_field_bound(double *a, const struct boxes *bx) {
  // CHECK: boxes.c:[[#LOOP11:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R bx->nLocal : [0]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : SAFE{{$}}
  // CHECK: deps       : 2 pair(s), 2 independent (1 SIV, 1 type){{$}}
  // CHECK: profitability : CPU_PARALLEL if (bx->nLocal >= {{[0-9]+}}){{$}}
  // CHECK: - GPU offload declined: no map() section: can't map 'bx': no access to it runs on every iteration, so the ends of its range aren't surely touched -> CPU_PARALLEL, which needs no map{{$}}
  for (int i = 0; i < bx->nLocal; i++)
    a[i] = 2.0 * i;
}

// --- not SAFE ---------------------------------------------------------------

// The body writes the second counter: it no longer follows the first.
void g_counter_written(double *restrict a, int n) {
  // CHECK: boxes.c:[[#LOOP12:@LINE+13]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: W a[o] : non-affine: uses the counter 'o', which is modified inside the loop{{$}}
  // CHECK-NEXT: R a[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[o]: W a[o] is non-affine (uses the counter 'o', which is modified inside the loop){{$}}
  // CHECK: rejected   : non-affine-subscript, scalar-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int o = 0, i = 0; i < n; i++, o++) {
    a[o] = 1.0;
    if (a[i] > 2.0)
      o++;
  }
}

// Box b writes the first slot of box b + 1 too: a real dependence the
// inspector can't remove.
void g_spill(real3 *p, const int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP13:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o + 1][0] : [64*b + i + 1][0]{{$}}
  // CHECK-NEXT: R p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o + 1][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 2 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP14:@LINE+15]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP13]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 3 (3 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o + 1][0] : [64*b + i + 1][0]{{$}}
    // CHECK-NEXT: R p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : UNSAFE{{$}}
    // CHECK: - W p[o + 1][0] and R p[o][0]: the same element is touched 1 iteration(s) apart{{$}}
    // CHECK: rejected   : array-dependence{{$}}
    // CHECK: deps       : 2 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o + 1][0] = p[o][0] + 1.0;
}

// The loop writes the bound array: a check before the loop proves nothing.
void g_bound_written(real3 *p, int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP15:@LINE+15]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: W n[b] : [b]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 3 pair(s), 2 independent (2 SIV){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++) {
    // CHECK: boxes.c:[[#LOOP16:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP15]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = 0.0;
    n[b] = 0;
  }
}

// A call in the loop: it might change the bounds.
void g_call(real3 *p, const int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP17:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: rng reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'rng' has no visible body in this TU{{$}}
  // CHECK: rejected   : opaque-callee, unproven-dependence{{$}}
  // CHECK: hint       : make the definition of 'rng' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP18:@LINE+16]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP17]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: rng reaches 0 function(s), max depth 0: <nothing> (leaf){{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - 'rng' has no visible body in this TU{{$}}
    // CHECK: rejected   : opaque-callee{{$}}
    // CHECK: hint       : make the definition of 'rng' visible in this translation unit so its effects can be analyzed [enables analysis]{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = rng(o);
}

// The inner loop runs only for some boxes: the loop may not read the bound
// of every box, so the check may not either.
void g_branch(real3 *p, const int *n, const int *skip, int nb) {
  // CHECK: boxes.c:[[#LOOP19:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R skip[b] : [b]{{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'skip' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    if (!skip[b])
      // CHECK: boxes.c:[[#LOOP20:@LINE+12]]:7{{$}}
      // CHECK: kind       : VARIABLE_BOUND{{$}}
      // CHECK: trip count : <not statically known>{{$}}
      // CHECK: nest       : depth 1, parent [[#LOOP19]]:3, 0 child loop(s), triangular{{$}}
      // CHECK: nest ivs   : b, i{{$}}
      // CHECK: accesses   : 2 (2 affine){{$}}
      // CHECK-NEXT: R n[b] : [b]{{$}}
      // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
      // CHECK-NEXT: safety     : SAFE{{$}}
      // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
      // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
      // CHECK: profitability : SEQUENTIAL{{$}}
      for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
        p[o][0] = 0.0;
}

// The loop writes an int through a pointer: s->boxes->nLocal may change.
void g_field_written(double *a, int *cnt, const struct boxes *bx) {
  // CHECK: boxes.c:[[#LOOP21:@LINE+17]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 0 child loop(s){{$}}
  // CHECK: nest ivs   : i{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R bx->nLocal : [0]{{$}}
  // CHECK-NEXT: W a[i] : [i]{{$}}
  // CHECK-NEXT: W cnt[i] : [i]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - 'bx' and 'cnt' may overlap (not restrict, and not every call is known to pass disjoint objects), and no run-time check is possible: the range of 'cnt' it touches is unknown: a loop range in cnt[i] can't be bounded{{$}}
  // CHECK: rejected   : pointer-overlap{{$}}
  // CHECK: hint       : declare parameter 'bx' of 'g_field_written' restrict, if no call lets what 'bx' points to overlap anything else 'g_field_written' accesses (here 'cnt'): restrict promises that for the whole function [fixes pointer-overlap]{{$}}
  // CHECK: hints      : applying every hint above makes this loop SAFE{{$}}
  // CHECK: deps       : 3 pair(s), 3 independent (2 SIV, 1 type){{$}}
  // CHECK: alias      : 'a' and 'cnt' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int i = 0; i < bx->nLocal; i++) {
    a[i] = 1.0;
    cnt[i] = i;
  }
}

// A counter whose start reads a value the outer loop changes.
void g_start_varies(double *restrict a, int nb) {
  int base = 0;
  // CHECK: boxes.c:[[#LOOP22:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[o] : non-affine: uses the counter 'o' (starts at base): uses 'base', which is modified inside the loop{{$}}
  // CHECK-NEXT: safety     : UNSAFE{{$}}
  // CHECK: - cannot test W a[o]: W a[o] is non-affine (uses the counter 'o' (starts at base): uses 'base', which is modified inside the loop){{$}}
  // CHECK: rejected   : non-affine-subscript, scalar-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++) {
    // CHECK: boxes.c:[[#LOOP23:@LINE+10]]:5{{$}}
    // CHECK: kind       : CONSTANT_BOUND{{$}}
    // CHECK: trip count : 64{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP22]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 1 (1 affine){{$}}
    // CHECK-NEXT: W a[o] : [i + base]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = base, i = 0; i < MAXATOMS; i++, o++)
      a[o] = 1.0;
    base += MAXATOMS / 2;
  }
}

// --- found by the adversarial review ----------------------------------------

// A narrow linked counter wraps (200 + i passes 255 at i = 56) while the loop
// still ends: box b's writes reach box b - 2's slots.
void g_wrap(int *restrict a, int nb) {
  // CHECK: boxes.c:[[#LOOP24:@LINE+12]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 1 (0 affine){{$}}
  // CHECK-NEXT: W a[100 * b + o + 100] : non-affine: uses 'o', which is assigned inside the loop{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W a[100 * b + o + 100]: W a[100 * b + o + 100] is non-affine (uses 'o', which is assigned inside the loop){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP25:@LINE+11]]:5{{$}}
    // CHECK: kind       : UNRECOGNIZED{{$}}
    // CHECK: reason     : a counter other than the induction variable could wrap (unsigned, or narrower than int){{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP24]]:3, 0 child loop(s){{$}}
    // CHECK: nest ivs   : b{{$}}
    // CHECK: accesses   : 1 (0 affine){{$}}
    // CHECK-NEXT: W a[100 * b + o + 100] : non-affine: uses 'o', which is modified inside the loop{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - loop shape not recognized (a counter other than the induction variable could wrap (unsigned, or narrower than int)); safety not evaluated{{$}}
    // CHECK: rejected   : loop-shape{{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (unsigned char o = 200, i = 0; i < 100; i++, o++)
      a[100 * b + o + 100] = b;
}

// An unsigned inner variable compares a negative bound as huge, which a
// signed check passes.
void g_unsigned_iv(real3 *p, const int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP26:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (1 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[MAXATOMS * b + i][0] : non-affine: subscript has a narrowing or sign-changing conversion ('MAXATOMS * b'), which wraps around{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - cannot test W p[MAXATOMS * b + i][0]: W p[MAXATOMS * b + i][0] is non-affine (subscript has a narrowing or sign-changing conversion ('MAXATOMS * b'), which wraps around){{$}}
  // CHECK: rejected   : non-affine-subscript{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP27:@LINE+14]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP26]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (1 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[MAXATOMS * b + i][0] : non-affine: subscript has a narrowing or sign-changing conversion ('MAXATOMS * b'), which wraps around{{$}}
    // CHECK-NEXT: safety     : UNKNOWN{{$}}
    // CHECK: - cannot test W p[MAXATOMS * b + i][0]: W p[MAXATOMS * b + i][0] is non-affine (subscript has a narrowing or sign-changing conversion ('MAXATOMS * b'), which wraps around){{$}}
    // CHECK: rejected   : non-affine-subscript{{$}}
    // CHECK: deps       : 1 pair(s), 0 independent{{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (unsigned i = 0; i < n[b]; i++)
      p[MAXATOMS * b + i][0] = 0.0;
}

// A volatile bound may change after the check.
void g_volatile(real3 *p, volatile int *n, int nb) {
  // CHECK: boxes.c:[[#LOOP28:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP29:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP28]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = 0.0;
}

// The file already uses the helper's name.
static int p05_all_le_short;
void g_name(real3 *p, const short *n, int nb) {
  // CHECK: boxes.c:[[#LOOP30:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP31:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP30]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = p05_all_le_short;
}

// A loop before the inner one may never end: the check would read bounds the
// loop never reaches.
void g_spin(real3 *p, const int *n, const int *wait, int nb) {
  // CHECK: boxes.c:[[#LOOP32:@LINE+16]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s){{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 3 (3 affine){{$}}
  // CHECK-NEXT: R wait[b] : [b]{{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'wait' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++) {
    int w = 0;
    while (w < wait[b])
      w++;
    // CHECK: boxes.c:[[#LOOP33:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP32]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = w;
  }
}

// A 128-bit bound: no helper for it.
void g_int128(real3 *p, const __int128 *n, int nb) {
  // CHECK: boxes.c:[[#LOOP34:@LINE+14]]:3{{$}}
  // CHECK: kind       : VARIABLE_BOUND{{$}}
  // CHECK: trip count : <not statically known>{{$}}
  // CHECK: nest       : depth 0, 1 child loop(s), perfect nest{{$}}
  // CHECK: nest ivs   : b{{$}}
  // CHECK: accesses   : 2 (2 affine){{$}}
  // CHECK-NEXT: R n[b] : [b]{{$}}
  // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
  // CHECK-NEXT: safety     : UNKNOWN{{$}}
  // CHECK: - could not prove W p[o][0] independent{{$}}
  // CHECK: rejected   : unproven-dependence{{$}}
  // CHECK: deps       : 1 pair(s), 0 independent{{$}}
  // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
  // CHECK: profitability : SEQUENTIAL{{$}}
  for (int b = 0; b < nb; b++)
    // CHECK: boxes.c:[[#LOOP35:@LINE+12]]:5{{$}}
    // CHECK: kind       : VARIABLE_BOUND{{$}}
    // CHECK: trip count : <not statically known>{{$}}
    // CHECK: nest       : depth 1, parent [[#LOOP34]]:3, 0 child loop(s), triangular{{$}}
    // CHECK: nest ivs   : b, i{{$}}
    // CHECK: accesses   : 2 (2 affine){{$}}
    // CHECK-NEXT: R n[b] : [b]{{$}}
    // CHECK-NEXT: W p[o][0] : [64*b + i][0]{{$}}
    // CHECK-NEXT: safety     : SAFE{{$}}
    // CHECK: deps       : 1 pair(s), 1 independent (1 SIV){{$}}
    // CHECK: alias      : 'n' and 'p' can't overlap (incompatible element types){{$}}
    // CHECK: profitability : SEQUENTIAL{{$}}
    for (int o = MAXATOMS * b, i = 0; i < n[b]; i++, o++)
      p[o][0] = 0.0;
}
