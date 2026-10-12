#!/usr/bin/env python3
"""Address-logging fuzzer for p05tool's safety verdicts.

Generates random C functions with loop nests, runs p05tool on them, and checks
every loop the tool calls SAFE against ground truth:

1. Address oracle. An instrumented copy of the same functions logs every
   memory access and the iteration of every enclosing loop it happened in
   (eval/fuzz/fuzzrt.h). Each function is run for every combination of small
   parameter values. A SAFE loop is a *false SAFE* if, in any valid run, two of
   its iterations touch the same address and one of them writes. This does not
   depend on thread timing.
2. Differential run. The tool's `-policy all-cpu` rewrite (every SAFE outermost
   loop gets `#pragma omp parallel for`) is compiled with -fopenmp and run
   against the sequential build, on the same inputs, several times. The output
   hashes must match. This also catches emitted pragmas that don't compile.

Stated assumptions the generator respects, so the oracle tests the tool's
analysis and not its documented assumptions: subscripts stay inside the object
they index, and (for the address oracle only) distinct pointer parameters point
at disjoint buffers. The differential run also repeats every file with
overlapping parameters (B == A, B == A + 1, C == B - 1): since Phase 1.6 a SAFE
loop over two pointers is either proven disjoint or guarded by a run-time
overlap check, so it must still match the sequential build. A loop
instance during which some access leaves its object is discarded, not counted;
only runs that never leave an object are used for the differential run.

A loop the tool calls UNSAFE that never conflicts in any run is reported as an
overclaim for information only: the conflict may need parameter values outside
the brute-forced range.

  eval/fuzz/fuzz.py [--seed S] [--files F] [--funcs N] [--keep DIR]
Exit status is 1 if any false SAFE, output mismatch, or tool/compile failure
was found.
"""
import argparse
import itertools
import os
import platform
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

NS = [-1, 0, 1, 2, 3, 5, 8]
MS = [-1, 0, 1, 3, 4]
KS = [-1, 0, 2]
COMBOS = list(itertools.product(NS, MS, KS))
SIG = ("u64 *A, u64 *B, u64 *C, u64 (*M)[8], int n, int m, int k")
IVS = ["i", "j", "l"]


def sreg(name):
    """Scalar code for fuzzrt.h: s0/s1 are tracked per loop clause (SC_S0,
    SC_S1); body temporaries are -1."""
    return {"s0": "SC_S0", "s1": "SC_S1"}.get(name, "-1")

# Pointer arrays: name -> region id in fuzzrt.h. `q` is a local alias set at
# the top of some functions (q = A + c or q = B).
REGION = {"A": "RA", "B": "RB", "C": "RC", "M": "RM", "G": "RG", "H": "RH"}

HELPERS = """\
u64 G[64];
u64 H[8][8];
static void h_set(u64 *p, u64 v) { p[0] = v; }
static void h_add(u64 *p, int o, u64 v) { p[o] += v; }
static u64 h_get(const u64 *p, int o) { return p[o]; }
static void h_gset(int o, u64 v) { G[o] = v; }
static u64 h_gget(int o) { return G[o]; }
static u64 h_pure(u64 x) { return x * 3 + 1; }
"""

HELPERS_LOG = """\
u64 G[64];
u64 H[8][8];
static void h_set(u64 *p, u64 v, int reg) { WR(&p[0], 0, reg) = v; }
static void h_add(u64 *p, int o, u64 v, int reg) { RW(&p[o], 0, reg) += v; }
static u64 h_get(const u64 *p, int o, int reg) { return RD((u64 *)&p[o], 0, reg); }
static void h_gset(int o, u64 v) { WR(&G[CK(o, 64)], 0, RG) = v; }
static u64 h_gget(int o) { return RD(&G[CK(o, 64)], 0, RG); }
static u64 h_pure(u64 x) { return x * 3 + 1; }
"""


class Scope:
    """What a statement can name: induction variables (outermost first),
    scalars with their declaration level, and whether `q` exists."""

    def __init__(self, ivs, scalars, has_q, q_reg):
        self.ivs = ivs
        self.scalars = scalars  # list of (name, lvl)
        self.has_q = has_q
        self.q_reg = q_reg

    def child(self, iv):
        return Scope(self.ivs + [iv], list(self.scalars), self.has_q, self.q_reg)


class Gen:
    def __init__(self, rnd):
        self.r = rnd
        self.tmp = 0
        # Per function: a "clean" function indexes its arrays the way real
        # loop code does (each array has one layout, reads are small shifts
        # of the writes), so many of its loops are parallel and the oracle
        # tests the boundary between SAFE and not. Other functions are
        # unconstrained, to probe the edges of what the analysis sees.
        self.clean = False
        self.writes = []
        self.layout = {}
        self.row = "n"

    def pick(self, weighted):
        items, weights = zip(*weighted)
        return self.r.choices(items, weights)[0]

    # ---- subscripts ---------------------------------------------------------

    def nonneg(self, sc):
        """A subscript into a declared dimension (G, H, a column of M): no
        negative coefficients or constants, so it is usually in range and
        the loop instance it is in keeps its evidence."""
        terms = []
        for iv in sc.ivs:
            c = self.pick([(0, 3), (1, 6), (2, 1)])
            if c:
                terms.append(iv if c == 1 else f"{c} * {iv}")
        c = self.pick([(0, 5), (1, 2), (2, 1), (3, 1)])
        if c or not terms:
            terms.append(str(c))
        return " + ".join(terms)

    def affine(self, sc, allow_params=True):
        r = self.r
        terms = []
        for iv in sc.ivs:
            c = self.pick([(0, 3), (1, 6), (-1, 1), (2, 1)])
            if c == 1:
                terms.append(iv)
            elif c == -1:
                terms.append(f"-{iv}")
            elif c:
                terms.append(f"{c} * {iv}")
        if allow_params and r.random() < 0.25:
            terms.append(r.choice(["n", "m", "k", "-n"]))
        c = self.pick([(0, 5), (1, 2), (-1, 2), (2, 1), (3, 1), (-2, 1)])
        if c or not terms:
            terms.append(str(c))
        return " + ".join(terms).replace("+ -", "- ")

    def subscript(self, sc, depth):
        """A subscript for a flat pointer: mostly affine, sometimes
        linearized 2D, sometimes deliberately non-affine."""
        r = self.r
        kind = self.pick([("affine", 10), ("linear", 4), ("nonaffine", 2), ("indirect", 1)])
        if kind == "linear" and len(sc.ivs) >= 2:
            a, b = r.sample(sc.ivs, 2)
            row = r.choice(["n", "m", "8"])
            off = r.choice(["", "", " + 1", " - 1", f" + {row}"])
            return f"{a} * {row} + {b}{off}"
        if kind == "nonaffine" and sc.ivs:
            iv = r.choice(sc.ivs)
            return r.choice([f"{iv} * {iv}", f"{iv} / 2", f"{iv} % 3", f"{iv} * n"])
        if kind == "indirect" and depth < 1:
            arr = r.choice(["A", "B", "C"])
            p, l = self.split(self.read_ref(sc, depth + 1, arr))
            return f"{p} & 7\x00{l} & 7"
        return self.affine(sc)

    def clean_sub(self, sc, arr, shift):
        """Subscript(s) for `arr` under its per-function layout, shifted by a
        small constant. Returns a list: one entry per dimension."""
        r = self.r
        ivs = sc.ivs
        if not ivs:
            return [str(r.randint(0, 3))] * (2 if arr == "M" else 1)
        cur = ivs[-1] if r.random() < 0.75 else r.choice(ivs)
        outer = ivs[-2] if len(ivs) >= 2 else None
        lay = self.layout[arr]

        def plus(e, c):
            return e if c == 0 else (f"{e} + {c}" if c > 0 else f"{e} - {-c}")
        if shift is None:  # a ZIV read: one fixed element
            if arr == "M":
                return [r.choice(["0", "1", "n", "k"]), r.choice(["0", "1", "3"])]
            return [r.choice(["0", "1", "3", "n", "k", "m - 1"])]
        if arr == "M":
            if outer and r.random() < 0.8:
                return [outer, plus(cur, shift)] if r.random() < 0.7 else [plus(outer, shift), cur]
            return [plus(cur, shift), str(r.randint(0, 3))]
        if lay == "lin" and outer:
            return [plus(f"{outer} * {self.row} + {cur}", shift)]
        if lay == "two":
            return [plus(f"2 * {cur}", shift)]
        if lay == "sym2":
            # Symbolic term with a coefficient > 1 next to one without it:
            # GCD / symbolic-distance territory.
            return [plus(f"2 * {cur} + n" if r.random() < 0.6 else f"2 * {cur}", shift)]
        return [plus(cur, shift)]

    # ---- references: (plain, log) text pairs -------------------------------

    def ref(self, sc, depth, arr=None, role=None):
        """An array element as (plain text, log lvalue address, region).
        `role` is "w" or "r" for a clean function's writes and reads."""
        r = self.r
        if self.clean and role and arr is None:
            if role == "w" or r.random() < 0.5:
                arr = r.choice(self.writes)
                shift = 0 if role == "w" else self.pick(
                    [(0, 4), (1, 2), (-1, 2), (2, 1), (3, 1), (-3, 1), (4, 1), (None, 1)])
            else:
                arr = r.choice([a for a in ("A", "B", "C", "M") if a not in self.writes])
                shift = self.pick([(0, 4), (1, 1), (-1, 1)])
            subs = self.clean_sub(sc, arr, shift)
            if arr == "M":
                return f"M[{subs[0]}][{subs[1]}]", f"&M[{subs[0]}][CK({subs[1]}, 8)]", "RM"
            reg = sc.q_reg if arr == "q" else REGION[arr]
            return f"{arr}[{subs[0]}]", f"&{arr}[{subs[0]}]", reg
        if arr is None:
            choices = [("A", 5), ("B", 4), ("C", 2), ("M", 2), ("G", 1), ("H", 1)]
            if sc.has_q:
                choices.append(("q", 3))
            arr = self.pick(choices)
        if arr == "M":
            s1, s2 = self.subscript(sc, depth), self.nonneg(sc)
            p1, l1 = self.split(s1)
            p2, l2 = self.split(s2)
            return f"M[{p1}][{p2}]", f"&M[{l1}][CK({l2}, 8)]", "RM"
        if arr == "H":
            s1, s2 = self.nonneg(sc), self.nonneg(sc)
            return f"H[{s1}][{s2}]", f"&H[CK({s1}, 8)][CK({s2}, 8)]", "RH"
        if arr == "G":
            s = self.nonneg(sc)
            return f"G[{s}]", f"&G[CK({s}, 64)]", "RG"
        s = self.subscript(sc, depth)
        p, l = self.split(s)
        reg = sc.q_reg if arr == "q" else REGION[arr]
        return f"{arr}[{p}]", f"&{arr}[{l}]", reg

    def split(self, s):
        """Subscripts may contain a nested read (indirect). Those are built
        as (plain, log) pairs and joined with a marker."""
        if "\x00" in s:
            p, l = s.split("\x00")
            return p, l
        return s, s

    def read_ref(self, sc, depth, arr=None):
        p, l, reg = self.ref(sc, depth, arr)
        # Return a combined string that split() can take apart.
        return f"{p}\x00RD({l}, 0, {reg})"

    # ---- expressions (plain, log) -------------------------------------------

    def expr(self, sc, depth=0):
        r = self.r
        n = self.pick([(1, 3), (2, 4), (3, 1)])
        plain, log = [], []
        for _ in range(n):
            kind = self.pick([("read", 8), ("scalar", 3), ("iv", 2), ("const", 2),
                              ("pure", 1), ("get", 0.4 if self.clean else 1),
                              ("gget", 0.3 if self.clean else 1)])
            if kind == "read":
                p, l, reg = self.ref(sc, depth, role="r")
                plain.append(p)
                log.append(f"RD({l}, 0, {reg})")
            elif kind == "scalar" and sc.scalars:
                name, lvl = r.choice(sc.scalars)
                plain.append(name)
                log.append(f"RD(&{name}, {lvl}, {sreg(name)})")
            elif kind == "iv" and sc.ivs:
                iv = r.choice(sc.ivs)
                plain.append(f"(u64){iv}")
                log.append(f"(u64){iv}")
            elif kind == "pure":
                p, l = self.expr(sc, depth + 1) if depth < 1 else ("n", "n")
                plain.append(f"h_pure({p})")
                log.append(f"h_pure({l})")
            elif kind == "get":
                arr = r.choice(["A", "B", "C"])
                p, l = self.split(self.subscript(sc, depth))
                plain.append(f"h_get({arr}, {p})")
                log.append(f"h_get({arr}, {l}, {REGION[arr]})")
            elif kind == "gget":
                s = self.nonneg(sc)
                plain.append(f"h_gget({s})")
                log.append(f"h_gget({s})")
            else:
                c = str(r.randint(1, 9))
                plain.append(c)
                log.append(c)
        op = r.choice([" + ", " ^ ", " * ", " + "])
        return op.join(plain), op.join(log)

    # ---- statements: lists of (plain, log) lines ----------------------------

    def stmt(self, sc, level, ind):
        """One statement inside the body of the loop at `level`."""
        r = self.r
        if self.clean:
            kind = self.pick([("store", 8), ("compound", 3), ("incr", 1), ("temp", 3),
                              ("outer_scalar", 0.4), ("reduce", 2.5), ("priv", 2.5),
                              ("call", 1.5), ("if", 1.5)])
        else:
            kind = self.pick([("store", 8), ("compound", 3), ("incr", 1), ("temp", 2),
                              ("outer_scalar", 0.7), ("reduce", 1), ("priv", 1),
                              ("call", 1), ("if", 2)])
        pad = "  " * ind
        if kind in ("store", "compound", "incr"):
            p, l, reg = self.ref(sc, 0, role="w")
            if kind == "incr":
                return [(f"{pad}{p}++;", f"{pad}RW({l}, 0, {reg})++;")]
            ep, el = self.expr(sc)
            op = "=" if kind == "store" else r.choice(["+=", "^=", "*="])
            mac = "WR" if kind == "store" else "RW"
            return [(f"{pad}{p} {op} {ep};",
                     f"{pad}{{ u64 v_ = {el}; {mac}({l}, 0, {reg}) {op} v_; }}")]
        if kind == "temp":
            name = f"t{self.tmp}"
            self.tmp += 1
            ep, el = self.expr(sc)
            sc.scalars.append((name, level + 1))
            return [(f"{pad}u64 {name} = {ep};",
                     f"{pad}u64 {name}; {{ u64 v_ = {el}; WR(&{name}, {level + 1}, -1) = v_; }}")]
        if kind == "priv":
            # Write a function-level scalar, then use it in the same
            # iteration: the tool may privatize it (private, or lastprivate
            # because s0/s1 are returned) when nothing reads it first.
            name = r.choice(["s0", "s1"])
            ep, el = self.expr(sc)
            p, l, reg = self.ref(sc, 0, role="w")
            ep2, el2 = self.expr(sc)
            return [(f"{pad}{name} = {ep};",
                     f"{pad}{{ u64 v_ = {el}; WR(&{name}, 0, {sreg(name)}) = v_; }}"),
                    (f"{pad}{p} = {name} + {ep2};",
                     f"{pad}{{ u64 v_ = RD(&{name}, 0, {sreg(name)}) + {el2}; WR({l}, 0, {reg}) = v_; }}")]
        if kind == "outer_scalar":
            name = r.choice(["s0", "s1"])
            ep, el = self.expr(sc)
            return [(f"{pad}{name} = {ep};",
                     f"{pad}{{ u64 v_ = {el}; WR(&{name}, 0, {sreg(name)}) = v_; }}")]
        if kind == "reduce":
            name = r.choice(["s0", "s1"])
            ep, el = self.expr(sc)
            form = r.choice(["add", "mul", "max", "xor"])
            if form == "max":
                return [(f"{pad}if ({ep} > {name}) {name} = {ep};",
                         f"{pad}if ({el} > RD(&{name}, 0, {sreg(name)})) {{ u64 v_ = {el}; "
                         f"WR(&{name}, 0, {sreg(name)}) = v_; }}")]
            op = {"add": "+=", "mul": "*=", "xor": "^="}[form]
            return [(f"{pad}{name} {op} {ep};",
                     f"{pad}{{ u64 v_ = {el}; RW(&{name}, 0, {sreg(name)}) {op} v_; }}")]
        if kind == "call":
            form = self.pick([("set", 2), ("add", 2), ("gset", 0.3 if self.clean else 2)])
            ep, el = self.expr(sc)
            if form == "gset":
                s = self.nonneg(sc)
                return [(f"{pad}h_gset({s}, {ep});",
                         f"{pad}{{ u64 v_ = {el}; h_gset({s}, v_); }}")]
            if self.clean:
                arr = r.choice([a for a in self.writes if a != "M"])
                sp = sl = self.clean_sub(sc, arr, 0)[0]
                reg = sc.q_reg if arr == "q" else REGION[arr]
                if form == "set":
                    return [(f"{pad}h_set(&{arr}[{sp}], {ep});",
                             f"{pad}{{ u64 v_ = {el}; h_set(&{arr}[{sl}], v_, {reg}); }}")]
                return [(f"{pad}h_add({arr}, {sp}, {ep});",
                         f"{pad}{{ u64 v_ = {el}; h_add({arr}, {sl}, v_, {reg}); }}")]
            arr = r.choice(["A", "B", "C"])
            if form == "set":
                sp, sl = self.split(self.subscript(sc, 0))
                return [(f"{pad}h_set(&{arr}[{sp}], {ep});",
                         f"{pad}{{ u64 v_ = {el}; h_set(&{arr}[{sl}], v_, {REGION[arr]}); }}")]
            sp, sl = self.split(self.subscript(sc, 0))
            return [(f"{pad}h_add({arr}, {sp}, {ep});",
                     f"{pad}{{ u64 v_ = {el}; h_add({arr}, {sl}, v_, {REGION[arr]}); }}")]
        # if
        p, l, reg = self.ref(sc, 0, role="r")
        # A copy of the scope: a temp declared under the if is out of scope
        # afterwards.
        inner = self.stmt(Scope(sc.ivs, list(sc.scalars), sc.has_q, sc.q_reg), level, ind + 1)
        lines = [(f"{pad}if ({p} & 1) {{", f"{pad}if (RD({l}, 0, {reg}) & 1) {{")]
        lines += inner
        lines.append((f"{pad}}}", f"{pad}}}"))
        return lines

    def loop(self, sc, level, ind, max_depth, loops):
        """A loop at `level` and its body. Appends (id, plain line index) to
        `loops`; returns lines."""
        r = self.r
        iv = IVS[level]
        outer = sc.ivs[-1] if sc.ivs else None
        shape = self.pick([("up", 10), ("le", 2), ("step2", 2), ("down", 2), ("tri", 2 if outer else 0)])
        start = self.pick([("0", 6), ("1", 2), ("k", 1), ("2", 1)])
        # Small constant bounds put the iteration span next to the dependence
        # distances (shifts up to 4).
        bound = self.pick([("n", 5), ("m", 3), ("n + 1", 1), ("m - 1", 1), ("5", 1), ("3", 1),
                           ("4", 1), ("n + k", 1)])
        # A third of loops use a counter declared at the top of the function
        # (`int i, j, l;`), which the tool must privatize (or lastprivate).
        decl = "" if r.random() < 0.33 else "int "
        if shape == "up":
            head = f"int {iv} = {start}; {iv} < {bound}; {iv}++"
        elif shape == "le":
            head = f"int {iv} = {start}; {iv} <= {bound}; {iv}++"
        elif shape == "step2":
            head = f"int {iv} = {start}; {iv} < {bound}; {iv} += 2"
        elif shape == "down":
            head = f"int {iv} = {bound} - 1; {iv} >= {start}; {iv}--"
        else:
            head = r.choice([f"int {iv} = 0; {iv} < {outer}; {iv}++",
                             f"int {iv} = {outer}; {iv} < {bound}; {iv}++",
                             f"int {iv} = {outer} + 1; {iv} < {bound}; {iv}++"])
        if not decl:
            head = head.replace("int ", "", 1)
        lid = len(loops)
        pad = "  " * ind
        lines = [(f"{pad}for ({head}) {{", f"{pad}LB({lid}); for ({head}) {{ IT();")]
        loops.append(lid)
        body_sc = sc.child(iv)
        nstmts = self.pick([(1, 3), (2, 4), (3, 2)])
        inner_at = r.randint(0, nstmts) if level + 1 < max_depth and r.random() < 0.55 else -1
        for s in range(nstmts + 1):
            if s == inner_at:
                lines += self.loop(body_sc, level + 1, ind + 1, max_depth, loops)
            if s < nstmts:
                lines += self.stmt(body_sc, level, ind + 1)
        lines.append((f"{pad}}}", f"{pad}}} LE();"))
        # Read the counter after the loop: needs lastprivate (or, if the
        # loop may run zero times, must not be parallelized at all).
        if not decl and r.random() < 0.5:
            lines.append((f"{pad}s1 += (u64){iv};",
                          f"{pad}RW(&s1, 0, SC_S1) += (u64){iv};"))
        return lines

    def function(self, idx, loops):
        r = self.r
        has_q = r.random() < 0.3
        q_init, q_reg = r.choice([("A + 1", "RA"), ("A + 2", "RA"), ("B", "RB"), ("A", "RA")])
        sc = Scope([], [("s0", 0), ("s1", 0)], has_q, q_reg)
        self.clean = r.random() < 0.65
        pool = ["A", "B", "C", "M"] + (["q"] if has_q else [])
        self.writes = r.sample(["A", "B", "C"] + (["q"] if has_q else []), 1)
        if r.random() < 0.4:
            self.writes.append(r.choice([a for a in pool if a not in self.writes]))
        self.layout = {a: self.pick([("one", 5), ("two", 1), ("lin", 3), ("sym2", 1)])
                       for a in pool}
        self.row = r.choice(["n", "n", "8", "m"])
        self.tmp = 0
        lines = [(f"u64 f{idx}({SIG}) {{", f"u64 f{idx}({SIG}) {{"),
                 ("  u64 s0 = (u64)n, s1 = 1;", "  u64 s0 = (u64)n, s1 = 1;"),
                 ("  int i = 0, j = 0, l = 0;", "  int i = 0, j = 0, l = 0;")]
        if has_q:
            lines.append((f"  u64 *q = {q_init};", f"  u64 *q = {q_init};"))
        nloops = self.pick([(1, 3), (2, 1)])
        for _ in range(nloops):
            lines += self.loop(sc, 0, 1, self.pick([(1, 2), (2, 4), (3, 2)]), loops)
            if r.random() < 0.3:
                lines += self.stmt(sc, -1, 1)
        lines.append(("  return s0 * 31 + s1;", "  return s0 * 31 + s1;"))
        lines.append(("}", "}"))
        return lines


def generate(seed, nfuncs):
    """Returns (plain source, log source, {loop id: plain line no},
    {loop id: function index})."""
    g = Gen(random.Random(seed))
    plain = ["typedef unsigned long u64;", HELPERS]
    log = ['#include "clauses.h"', '#include "fuzzrt.h"', HELPERS_LOG]
    line_of, func_of = {}, {}
    loops = []
    for f in range(nfuncs):
        first = len(loops)
        body = g.function(f, loops)
        base = sum(x.count("\n") + 1 for x in plain)
        for off, (p, _) in enumerate(body):
            m = re.match(r"\s*for \(", p)
            if m:
                lid = next(x for x in range(first, len(loops)) if x not in line_of)
                line_of[lid] = base + off + 1
                func_of[lid] = f
        plain.append("\n".join(p for p, _ in body))
        log.append("\n".join(l for _, l in body))
    names = ", ".join(f"f{f}" for f in range(nfuncs))
    table = f"typedef u64 (*fn_t)({SIG});\nstatic fn_t fns[] = {{{names}}};\n"
    log.append(table + LOG_MAIN.replace("NFUNCS", str(nfuncs)))
    return "\n".join(plain) + "\n", "\n".join(log) + "\n", line_of, func_of, table


COMBO_INIT = """\
static int combos[][3] = {COMBOS};
static void setup(int f, int c) {
  rng_state = 0x9E3779B97F4A7C15ULL ^ (u64)(f * 1000 + c + 1);
  rt_fill(buf, 4 * REGION_ELEMS);
  rt_fill(G, 64);
  rt_fill(&H[0][0], 64);
}
"""

LOG_MAIN = COMBO_INIT + """\
int main(int argc, char **argv) {
  rt_init();
  rt_region(RG, G, 64);
  rt_region(RH, &H[0][0], 64);
  // Aliasing variant (as in the plain driver): the aliased parameter's
  // accesses must stay inside the buffer it now points into, so its region
  // becomes that buffer. Only the V (valid) lines of such a run are used.
  int alias = argc > 1 ? atoi(argv[1]) : 0;
  u64 *pa = PA, *pb = PB, *pc = PC;
  if (alias == 1 || alias == 2) {
    pb = alias == 1 ? pa : pa + 1;
    region_lo[RB] = region_lo[RA]; region_hi[RB] = region_hi[RA];
    region_origin[RB] = pb;
  }
  if (alias == 3) {
    pc = pb - 1;
    region_lo[RC] = region_lo[RB]; region_hi[RC] = region_hi[RB];
    region_origin[RC] = pc;
  }
  for (int f = 0; f < NFUNCS; f++)
    for (int c = 0; c < (int)(sizeof combos / sizeof combos[0]); c++) {
      setup(f, c);
      rt_begin(f, combos[c][0], combos[c][1], combos[c][2]);
      fns[f](pa, pb, pc, PM, combos[c][0], combos[c][1], combos[c][2]);
      rt_end();
    }
  rt_finish();
  return 0;
}
"""

# The plain driver runs only the (function, combo) pairs the logging run found
# valid, so it never executes undefined behaviour.
PLAIN_MAIN = COMBO_INIT + """\
int main(int argc, char **argv) {
  rt_alloc();
  FILE *in = fopen(argv[1], "r");
  // Optional aliasing variant (Phase 1.6): 1: B == A, 2: B == A + 1,
  // 3: C == B - 1. Every subscript a valid run uses stays inside buf.
  int alias = argc > 2 ? atoi(argv[2]) : 0;
  u64 *pa = PA, *pb = PB, *pc = PC;
  if (alias == 1) pb = pa;
  if (alias == 2) pb = pa + 1;
  if (alias == 3) pc = pb - 1;
  int f, n, m, k;
  while (fscanf(in, "%d %d %d %d", &f, &n, &m, &k) == 4) {
    int c = 0;
    while (combos[c][0] != n || combos[c][1] != m || combos[c][2] != k) c++;
    setup(f, c);
    u64 r = fns[f](pa, pb, pc, PM, n, m, k);
    u64 h = rt_hash(1469598103934665603ULL, buf, 4 * REGION_ELEMS * sizeof(u64));
    h = rt_hash(h, G, sizeof G);
    h = rt_hash(h, H, sizeof H);
    h = rt_hash(h, &r, sizeof r);
    printf("%d %d %d %d %lx\\n", f, n, m, k, h);
  }
  return 0;
}
"""


def combos_c():
    return "{" + ", ".join("{%d, %d, %d}" % c for c in COMBOS) + "}"


def toolchain():
    cc = os.environ.get("CC") or shutil.which("/opt/homebrew/opt/llvm/bin/clang") or "clang"
    sysroot, omp = [], ["-fopenmp"]
    if platform.system() == "Darwin":
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
        sysroot = ["-isysroot", sdk]
        omp += ["-L/opt/homebrew/opt/libomp/lib", "-L/opt/homebrew/opt/llvm/lib",
                "-Wl,-rpath,/opt/homebrew/opt/llvm/lib"]
    return cc, sysroot, omp


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def parse_report(text, fname):
    """{line: verdict}, {line: {scalar: mode}} with mode 1 = private or
    lastprivate, 2 = reduction (see fuzzrt.h's clause_mode)."""
    verdicts, clauses = {}, {}
    cur = None
    for line in text.splitlines():
        m = re.match(r"^\S*%s:(\d+):\d+$" % re.escape(fname), line)
        if m:
            cur = int(m.group(1))
            continue
        m = re.match(r"^\s+safety\s+: (\S+)", line)
        if m and cur is not None:
            verdicts[cur] = m.group(1)
        m = re.match(r"^\s+scalars\s+:(.*)$", line)
        if m and cur is not None:
            modes = {}
            for kind, names in re.findall(r"(lastprivate|private|reduction)\(([^)]*)\)", m.group(1)):
                names = names.split(":", 1)[-1] if kind == "reduction" else names
                for nm in names.split(","):
                    modes[nm.strip()] = 2 if kind == "reduction" else 1
            clauses[cur] = modes
    return verdicts, clauses


def fuzz_file(seed, nfuncs, work, tool, cc, sysroot, omp, runs, stats, failures):
    plain, log, line_of, func_of, table = generate(seed, nfuncs)
    log = log.replace("{COMBOS}", combos_c())
    prog = os.path.join(work, "prog.c")
    with open(prog, "w") as fh:
        fh.write(plain)
    with open(os.path.join(work, "prog_log.c"), "w") as fh:
        fh.write(log)
    shutil.copy(os.path.join(HERE, "fuzzrt.h"), work)
    driver = ('#include "fuzzrt.h"\n#include SRC\n' + table +
              PLAIN_MAIN.replace("{COMBOS}", combos_c()))
    with open(os.path.join(work, "driver.c"), "w") as fh:
        fh.write(driver)

    def fail(kind, msg):
        failures.append((seed, kind, msg))

    # 1. The tool's verdicts, and its all-cpu rewrite.
    omp_c = os.path.join(work, "prog.omp.c")
    if os.path.exists(omp_c):  # stale, from an earlier --keep run
        os.remove(omp_c)
    res = run([tool, "-rewrite", "-policy", "all-cpu", prog, "-o", omp_c, "--"] + sysroot)
    if res.returncode != 0:
        fail("tool", f"p05tool exited {res.returncode}:\n{res.stderr[-2000:]}")
        return
    res_tool = res
    verdicts, clauses = parse_report(res.stdout, "prog.c")
    m = re.search(r"Analyzed (\d+) loop", res.stdout)
    if not m or int(m.group(1)) != len(line_of):
        fail("tool", f"tool analyzed {m and m.group(1)} loops, generator wrote {len(line_of)}")
    by_id = {}
    for lid, line in line_of.items():
        v = verdicts.get(line)
        if v is None:
            fail("tool", f"no verdict for the loop at prog.c:{line}")
            continue
        by_id[lid] = v
        stats[v] = stats.get(v, 0) + 1
        if v == "SAFE" and clauses.get(line):
            stats["clause_loops"] = stats.get("clause_loops", 0) + 1
            if {"s0", "s1"} & set(clauses[line]):
                stats["checked_clause_loops"] = stats.get("checked_clause_loops", 0) + 1

    # 2. Address oracle. It learns the scalar clauses first, so a privatized
    # s0/s1 is checked (written before read in every iteration) rather than
    # counted as a conflict, and a reduction is left to the differential run.
    entries = []
    for lid, line in line_of.items():
        modes = clauses.get(line, {})
        m0, m1 = modes.get("s0", 0), modes.get("s1", 0)
        if m0 or m1:
            entries.append(f"[{lid}] = {{{m0}, {m1}}}")
    with open(os.path.join(work, "clauses.h"), "w") as fh:
        fh.write("#define FUZZ_CLAUSES 1\n"
                 "static const unsigned char clause_mode[4096][2] = {%s};\n"
                 % ", ".join(entries))
    exe = os.path.join(work, "log")
    res = run([cc, "-O1", "-w"] + sysroot + [os.path.join(work, "prog_log.c"), "-o", exe])
    if res.returncode != 0:
        fail("harness", "instrumented build failed:\n" + res.stderr[-2000:])
        return
    res = run([exe])
    if res.returncode != 0:
        fail("harness", f"instrumented run exited {res.returncode}:\n{res.stderr[-1000:]}")
        return
    valid, conflicts, exercised = [], {}, {}
    for line in res.stdout.splitlines():
        parts = line.split()
        if parts[0] in ("V", "I"):
            stats["runs"] = stats.get("runs", 0) + 1
        if parts[0] == "V":
            valid.append(" ".join(parts[1:]))
        elif parts[0] == "C":
            conflicts.setdefault(int(parts[1]), " ".join(parts[2:]))
        elif parts[0] == "E":
            exercised[int(parts[1])] = int(parts[2])
    src_lines = plain.splitlines()
    for lid, v in by_id.items():
        if v == "SAFE":
            if exercised.get(lid, 0) >= 2:
                stats["safe_exercised"] = stats.get("safe_exercised", 0) + 1
            if lid in conflicts:
                f = func_of[lid]
                fn = function_text(src_lines, f)
                fail("false-SAFE",
                     f"prog.c:{line_of[lid]} is SAFE, but with (n, m, k) = "
                     f"({', '.join(conflicts[lid].split()[:3])}) "
                     f"{' '.join(conflicts[lid].split()[3:])}\n{fn}")
        elif v == "UNSAFE" and lid not in conflicts and exercised.get(lid, 0) >= 2:
            stats["unsafe_no_witness"] = stats.get("unsafe_no_witness", 0) + 1
            if os.environ.get("FUZZ_SHOW_OVERCLAIMS"):
                print(f"[overclaim?] seed {seed} prog.c:{line_of[lid]}\n"
                      f"{function_text(src_lines, func_of[lid])}")

    # 3. Differential run of the all-cpu rewrite. With no SAFE loop the tool
    # writes no rewrite; the parallel build is then the original under
    # -fopenmp, which must still match.
    stats["valid_runs"] = stats.get("valid_runs", 0) + len(valid)
    par_src = omp_c
    if not os.path.exists(omp_c):
        if "nothing annotated" not in res_tool.stdout:
            fail("tool", "p05tool wrote no rewrite and did not say why")
            return
        par_src = prog
    vfile = os.path.join(work, "valid.txt")
    with open(vfile, "w") as fh:
        fh.write("\n".join(valid) + "\n")
    seq, par = os.path.join(work, "seq"), os.path.join(work, "par")
    for out, src, extra in ((seq, prog, []), (par, par_src, omp)):
        res = run([cc, "-O1", "-w"] + sysroot + extra + [f'-DSRC="{src}"', "-I", work,
                                                          os.path.join(work, "driver.c"), "-o", out])
        if res.returncode != 0:
            fail("compile", f"build of {os.path.basename(src)} failed:\n{res.stderr[-2000:]}")
            return
    seq_res = run([seq, vfile])
    want = seq_res.stdout
    if seq_res.returncode != 0 or len(want.splitlines()) != len(valid):
        fail("harness", f"sequential run exited {seq_res.returncode} after "
                        f"{len(want.splitlines())} of {len(valid)} valid runs")
        return
    stats["pragmas"] = stats.get("pragmas", 0) + open(par_src).read().count("#pragma omp")
    env = dict(os.environ, OMP_NUM_THREADS=os.environ.get("OMP_NUM_THREADS", "8"))
    # Variant 0: disjoint parameters. 1-3: overlapping ones (B == A, B == A + 1,
    # C == B - 1), which the address oracle doesn't model: a SAFE loop whose
    # safety rests on a run-time alias check must then run sequentially, so the
    # parallel output must still match.
    for variant in (0, 1, 2, 3):
        if variant:
            # Aliasing can change control flow, so the runs valid without it
            # may not be: take the variant's own valid runs from the
            # instrumented build.
            lres = run([exe, str(variant)])
            if lres.returncode != 0:
                fail("harness", f"instrumented aliased run {variant} exited "
                                f"{lres.returncode}")
                return
            vvalid = [" ".join(l.split()[1:]) for l in lres.stdout.splitlines()
                      if l.startswith("V ")]
            vfile = os.path.join(work, f"valid{variant}.txt")
            with open(vfile, "w") as fh:
                fh.write("\n".join(vvalid) + "\n")
            seq_res = run([seq, vfile, str(variant)])
            if seq_res.returncode != 0:
                fail("harness", f"sequential aliased run {variant} exited "
                                f"{seq_res.returncode}")
                return
            want = seq_res.stdout
            stats["alias_variants"] = stats.get("alias_variants", 0) + 1
        for _ in range(runs):
            got = run([par, vfile] + ([str(variant)] if variant else []), env=env)
            if got.returncode != 0 or got.stdout != want:
                bad = [a for a, b in zip(want.splitlines(), got.stdout.splitlines()) if a != b]
                first = bad[0].split() if bad else ["?"]
                fn = function_text(src_lines, int(first[0])) if first[0].isdigit() else ""
                fail("mismatch", f"parallel output differs (aliasing variant {variant}; "
                                 f"first: f{first[0]}, n m k = "
                                 f"{' '.join(first[1:4])}; exit {got.returncode})\n{fn}")
                return


def function_text(src_lines, f):
    out, on = [], False
    for no, line in enumerate(src_lines, 1):
        if line.startswith(f"u64 f{f}("):
            on = True
        if on:
            out.append(f"{no:5}  {line}")
            if line == "}":
                break
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1, help="first seed (one per file)")
    ap.add_argument("--files", type=int, default=10, help="number of generated files")
    ap.add_argument("--funcs", type=int, default=12, help="functions per file")
    ap.add_argument("--runs", type=int, default=3, help="parallel runs per file")
    ap.add_argument("--keep", help="keep each file's work directory under this path")
    ap.add_argument("--max-report", type=int, default=5, help="failures printed in full")
    args = ap.parse_args()

    tool = os.environ.get("P05_TOOL", os.path.join(ROOT, "build", "p05tool"))
    cc, sysroot, omp = toolchain()
    stats, failures = {}, []
    for seed in range(args.seed, args.seed + args.files):
        if args.keep:
            work = os.path.join(args.keep, f"seed{seed}")
            os.makedirs(work, exist_ok=True)
            fuzz_file(seed, args.funcs, work, tool, cc, sysroot, omp, args.runs, stats, failures)
        else:
            with tempfile.TemporaryDirectory() as work:
                fuzz_file(seed, args.funcs, work, tool, cc, sysroot, omp, args.runs, stats, failures)

    total = sum(stats.get(v, 0) for v in ("SAFE", "UNKNOWN", "UNSAFE"))
    print(f"fuzzed {args.files} file(s), seeds {args.seed}..{args.seed + args.files - 1}: "
          f"{total} loops: SAFE {stats.get('SAFE', 0)} "
          f"({stats.get('safe_exercised', 0)} ran >= 2 iterations), "
          f"UNKNOWN {stats.get('UNKNOWN', 0)}, UNSAFE {stats.get('UNSAFE', 0)}")
    print(f"  {stats.get('pragmas', 0)} pragma(s) forced on in the differential runs")
    print(f"  {stats.get('clause_loops', 0)} SAFE loop(s) with private/lastprivate/reduction "
          f"clauses ({stats.get('checked_clause_loops', 0)} on s0/s1, checked per iteration)")
    print(f"  UNSAFE loops that ran >= 2 iterations but never conflicted (information only): "
          f"{stats.get('unsafe_no_witness', 0)}")
    # A pass must have checked something: a harness that silently discards
    # everything would otherwise report OK.
    if stats.get("runs") and stats.get("valid_runs", 0) < stats["runs"] // 2:
        failures.append((args.seed, "coverage", f"only {stats.get('valid_runs', 0)} of "
                         f"{stats['runs']} runs stayed inside their objects"))
    if total and stats.get("safe_exercised", 0) == 0:
        failures.append((args.seed, "coverage", "no SAFE loop ran >= 2 iterations"))
    print(f"  {stats.get('valid_runs', 0)} of {stats.get('runs', 0)} runs never left an object")
    for seed, kind, msg in failures[:args.max_report]:
        print(f"\n[{kind}] seed {seed}: {msg}")
    if failures:
        print(f"\nFAIL: {len(failures)} problem(s)"
              f" ({', '.join(sorted(set(k for _, k, _ in failures)))});"
              f" reproduce with --seed <seed> --files 1 --keep DIR")
        return 1
    print("OK: no false SAFE, parallel output identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
