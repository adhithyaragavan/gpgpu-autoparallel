// Runtime for the address-logging fuzzer (eval/fuzz/fuzz.py).
//
// The instrumented copy of a generated program routes every memory access
// through rt_acc(), and brackets every loop with LB()/IT()/LE(). That gives the
// ground truth for "is this loop parallel?": a loop instance carries a
// dependence iff two of its iterations touch the same address and at least one
// of those touches is a write. This is checked at every loop level at once.
//
// Each access also says where its variable was declared (`lvl`, the number of
// loops enclosing the declaration). A variable declared inside a loop's body is
// a fresh object per iteration of that loop, so it is ignored for that loop and
// every loop enclosing it. Arrays, parameters and globals have lvl 0.
//
// Pointer accesses also name the region (buffer) the pointer was derived from.
// An address outside that region means the program left the object it indexes
// (undefined behaviour, or an alias the tool is entitled to assume away), so
// every loop instance active at that moment is marked bad and its evidence
// discarded. The same goes for an out-of-range subscript into a declared
// array dimension (CK).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long u64;

// ---- data and regions, shared by the logging and plain drivers -------------

#define REGION_ELEMS 4096
enum { RA, RB, RC, RM, RG, RH, NREGIONS };
static u64 *region_lo[NREGIONS], *region_hi[NREGIONS];
static u64 *region_origin[NREGIONS];  // where subscript 0 is, for witnesses
static const char *region_name[NREGIONS] = {"A", "B", "C", "M", "G", "H"};
static u64 *buf;  // RA..RM, REGION_ELEMS each

static void rt_alloc(void) {
  buf = calloc(4 * REGION_ELEMS, sizeof(u64));
  for (int r = RA; r <= RM; r++) {
    region_lo[r] = buf + r * REGION_ELEMS;
    region_hi[r] = region_lo[r] + REGION_ELEMS;
    region_origin[r] = region_lo[r] + REGION_ELEMS / 2;
  }
}
static void rt_region(int r, u64 *p, size_t elems) {
  region_lo[r] = p;
  region_hi[r] = p + elems;
  region_origin[r] = p;
}
// Pointer parameters point at the middle of their region, so negative
// subscripts stay inside it.
#define PA (buf + RA * REGION_ELEMS + REGION_ELEMS / 2)
#define PB (buf + RB * REGION_ELEMS + REGION_ELEMS / 2)
#define PC (buf + RC * REGION_ELEMS + REGION_ELEMS / 2)
#define PM ((u64(*)[8])(buf + RM * REGION_ELEMS + REGION_ELEMS / 2))

static u64 rng_state;
static u64 rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}
static void rt_fill(u64 *p, size_t elems) {
  for (size_t i = 0; i < elems; i++) p[i] = rng() % 1000;
}

static u64 rt_hash(u64 h, const void *p, size_t bytes) {
  const unsigned char *c = p;
  for (size_t i = 0; i < bytes; i++) h = (h ^ c[i]) * 1099511628211ULL;
  return h;
}

// ---- logging -----------------------------------------------------------------

#define MAX_DEPTH 8
#define MAX_LOOPS 4096
// One active loop instance. `bad`: some access during it left its object, so
// its conflicts are not evidence (CK clamps a bad subscript to 0, which can
// manufacture a conflict). Validity is per instance, not per run: a bad
// subscript elsewhere in the function does not throw away a clean loop's
// evidence. Any clean instance is a real execution of the plain program for
// some input data (the bad access only changed array contents).
struct frame {
  int id; long inst; long iter; int bad, conf, serial;
  long wa, wb; int wreg; long woff;  // first conflict's witness
  // Function-level scalars s0/s1 the tool privatized for this loop: the
  // iteration each was last first-touched in, and the first violation of
  // "every iteration writes it before reading it".
  long pv_iter[2]; int pviol, pv_sc; long pv_at;
};

// Scalar accesses pass a negative reg: -1 a body temporary, SC_S0/SC_S1 the
// function-level scalars s0/s1. Per loop id, what the tool said about them:
// 0 nothing (an ordinary shared variable), 1 private/lastprivate (checked
// here: each iteration must write it before reading it), 2 reduction (its
// result is checked by the differential run, exactly: u64 arithmetic).
// eval/fuzz/fuzz.py writes the table as clauses.h after reading the report.
#define SC_S0 (-2)
#define SC_S1 (-3)
#ifndef FUZZ_CLAUSES
static const unsigned char clause_mode[MAX_LOOPS][2];
#endif
static struct frame stack[MAX_DEPTH];
static int depth;
static long next_inst;
static int invalid;    // this run left an object somewhere: no differential
static int cur_n, cur_m, cur_k, cur_fn;

// Per loop id: whether this run already reported a conflict, and the most
// iterations any clean instance ran (over all runs).
static int reported[MAX_LOOPS];
static long max_iters[MAX_LOOPS];

// (instance, address) -> who touched it. Slots from an older generation are
// empty, so clearing the table between runs is O(1).
#define TABLE_BITS 22
struct slot { unsigned gen; long inst; u64 *addr; long iter; int multi, wrote; };
static struct slot *table;
static unsigned gen = 1;

static void rt_begin(int fn, int n, int m, int k) {
  depth = 0;
  invalid = 0;
  gen++;
  cur_fn = fn, cur_n = n, cur_m = m, cur_k = k;
  memset(reported, 0, sizeof reported);
}

// `parallel`: the loop's if(parallel: ...) condition (1 if it has none). An
// instance where it is false runs sequentially: its conflicts are not counted.
static void LB(int id, int parallel) {
  if (depth == MAX_DEPTH) { fprintf(stderr, "fuzzrt: nest too deep\n"); exit(2); }
  stack[depth] = (struct frame){.id = id, .inst = ++next_inst, .iter = -1,
                                .serial = !parallel, .pv_iter = {-1, -1}};
  depth++;
}
static void IT(void) { stack[depth - 1].iter++; }
static void LE(void) {
  struct frame *f = &stack[--depth];
  if (f->bad) return;
  if (f->iter + 1 > max_iters[f->id]) max_iters[f->id] = f->iter + 1;
  if (f->serial) return;
  if (f->conf && !reported[f->id]) {
    reported[f->id] = 1;
    printf("C %d %d %d %d iterations %ld and %ld both touch %s%+ld\n", f->id, cur_n, cur_m,
           cur_k, f->wa, f->wb, f->wreg < 0 ? "a scalar" : region_name[f->wreg],
           f->wreg < 0 ? 0L : f->woff);
  } else if (f->pviol && !reported[f->id]) {
    reported[f->id] = 1;
    printf("C %d %d %d %d iteration %ld reads private s%d before writing it\n", f->id, cur_n,
           cur_m, cur_k, f->pv_at, f->pv_sc);
  }
}

static void note(int level, u64 *p, int kind, int reg) {
  struct frame *f = &stack[level];
  if (f->iter < 0) return;  // in the loop header, not in an iteration
  int write = kind != 0;
  if (reg <= SC_S0) {
    int sc = SC_S0 - reg;
    int mode = clause_mode[f->id][sc];
    if (mode == 2) return;  // reduction: the differential run checks it
    if (mode == 1) {        // private: first touch per iteration must write
      if (f->pv_iter[sc] != f->iter) {
        f->pv_iter[sc] = f->iter;
        if (kind != 1 && !f->pviol) {
          f->pviol = 1;
          f->pv_at = f->iter;
          f->pv_sc = sc;
        }
      }
      return;
    }
  }
  size_t h = ((size_t)f->inst * 0x9E3779B97F4A7C15ULL ^ (size_t)p * 0xC2B2AE3D27D4EB4FULL);
  h >>= 64 - TABLE_BITS;
  for (;; h = (h + 1) & ((1u << TABLE_BITS) - 1)) {
    struct slot *s = &table[h];
    if (s->gen != gen) {
      *s = (struct slot){gen, f->inst, p, f->iter, 0, write};
      return;
    }
    if (s->inst != f->inst || s->addr != p) continue;
    long other = s->iter;
    if (s->iter != f->iter) s->multi = 1;
    s->wrote |= write;
    if (s->multi && s->wrote && !f->conf) {
      f->conf = 1;
      f->wa = other;
      f->wb = f->iter;
      f->wreg = -1;
      for (int r = 0; r < NREGIONS; r++)
        if (p >= region_lo[r] && p < region_hi[r]) {
          f->wreg = r;
          f->woff = p - region_origin[r];
        }
    }
    return;
  }
}

static void mark_bad(void) {
  invalid = 1;
  for (int level = 0; level < depth; level++) stack[level].bad = 1;
}

static u64 sink;
// kind: 0 read, 1 write, 2 read+write. reg < 0: a scalar, no region check.
static u64 *rt_acc(u64 *p, int lvl, int reg, int kind) {
  if (reg >= 0 && (p < region_lo[reg] || p >= region_hi[reg])) {
    mark_bad();
    return &sink;
  }
  for (int level = lvl; level < depth; level++) note(level, p, kind, reg);
  return p;
}
static long CK(long e, long n) {
  if (e < 0 || e >= n) { mark_bad(); return 0; }
  return e;
}
#define RD(p, lvl, reg) (*rt_acc((p), (lvl), (reg), 0))
#define WR(p, lvl, reg) (*rt_acc((p), (lvl), (reg), 1))
#define RW(p, lvl, reg) (*rt_acc((p), (lvl), (reg), 2))

// A run that never left an object is safe to execute uninstrumented: the
// differential runs only those.
static void rt_end(void) {
  if (depth != 0) { fprintf(stderr, "fuzzrt: unbalanced loops in f%d\n", cur_fn); exit(2); }
  printf("%c %d %d %d %d\n", invalid ? 'I' : 'V', cur_fn, cur_n, cur_m, cur_k);
}
static void rt_finish(void) {
  for (int id = 0; id < MAX_LOOPS; id++)
    if (max_iters[id] > 0) printf("E %d %ld\n", id, max_iters[id]);
}
static void rt_init(void) {
  rt_alloc();
  table = calloc((size_t)1 << TABLE_BITS, sizeof *table);
}
