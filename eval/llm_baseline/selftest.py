#!/usr/bin/env python3
"""Phase 2.5 item 4: check.py must flag planted wrong pragmas and pass
right ones. Writes candidates into work/selftest/, runs check.py on them and
compares each group's verdict with the expected one.

    selftest.py [--archer]   (--archer: also require Archer to see the races)
"""
import csv
import os
import subprocess
import sys

import check

HERE = os.path.dirname(os.path.abspath(__file__))
PB = os.path.join(HERE, "work", "polybench")
OUT = os.path.join(HERE, "work", "selftest")

# kernel, sample, edits [(original line, stripped; which match; pragma to
# insert before it)], replacement (old, new) or None, expected verdicts per
# group, then for the "file" row. Line matches are counted in the original
# file, so -1 is the kernel's loop when init_array has the same one.
# "race": a race the output can't show (-O2 keeps the shared counter in a
# register): wrong with --archer, either verdict without. "value": wrong by
# output alone (no race for Archer to see).
GEMM_I, GEMM_K = "for (i = 0; i < _PB_NI; i++) {", "for (k = 0; k < _PB_NK; k++) {"
SEIDEL_T = "for (t = 0; t <= _PB_TSTEPS - 1; t++)"
SEIDEL_I, SEIDEL_J = "for (i = 1; i<= _PB_N - 2; i++)", "for (j = 1; j <= _PB_N - 2; j++)"
PF = "#pragma omp parallel for"
CASES = [
    ("gemm", 0, [(GEMM_I, -1, PF)], None, ["wrong", "wrong"]),           # j, k shared
    ("gemm", 1, [(GEMM_I, -1, PF + " private(j,k)")], None, ["correct", "correct"]),
    ("gemm", 2, [(GEMM_K, -1, PF + " private(j)")], None, ["wrong", "wrong"]),  # += across k
    ("gemm", 3, [(GEMM_I, -1, PF + " private(j,k)")],
     ("\tC[i][j] *= beta;", "\tC[i][j] = beta;"), ["modified"]),
    ("gemm", 4, [(GEMM_I, -1, "#pragma omp parallel"), (GEMM_I, -1, "#pragma omp for private(j,k)")],
     None, ["correct", "correct"]),                                     # one group of two
    # The outer group is checked with the inner one, whose construct makes k
    # private; the inner + atomic is right on its own.
    ("gemm", 5, [(GEMM_I, -1, PF + " private(i,j)"), (GEMM_K, -1, PF + " private(j)"),
                 ("C[i][j] += alpha * A[i][k] * B[k][j];", -1, "#pragma omp atomic")],
     None, ["correct", "correct", "correct"]),
    ("gemm", 6, [(GEMM_I, -1, PF + " private(j)")], None, ["race", "race"]),  # k shared
    ("seidel-2d", 0, [(SEIDEL_I, -1, PF + " private(j)")], None, ["wrong", "wrong"]),
    # Both wrong: the outer one is blamed on the inner.
    ("seidel-2d", 1, [(SEIDEL_T, -1, PF + " private(i,j)"), (SEIDEL_I, -1, PF + " private(j)")],
     None, ["nested-wrong", "wrong", "wrong"]),
    # Never parallel: not scored (the file still runs).
    ("seidel-2d", 2, [(SEIDEL_I, -1, PF + " private(j) if(0)")], None, ["not-parallel", "correct"]),
    ("seidel-2d", 3, [(SEIDEL_I, -1, "#pragma omp for private(j)")], None, ["not-parallel", "correct"]),
    ("seidel-2d", 4, [(SEIDEL_J, -1, "#pragma omp simd")], None, ["not-parallel", "correct"]),
    ("seidel-2d", 5, [(SEIDEL_I, -1, PF + " private(j) num_threads(1)")], None,
     ["not-parallel", "correct"]),
    # A team without worksharing: every thread runs the whole loop.
    ("seidel-2d", 6, [(SEIDEL_I, -1, "#pragma omp parallel private(j)")], None, ["wrong", "wrong"]),
    # A taskloop belongs to the team around it.
    ("seidel-2d", 7, [(SEIDEL_I, -1, "#pragma omp parallel"), (SEIDEL_I, -1, "#pragma omp single"),
                      (SEIDEL_I, -1, "#pragma omp taskloop private(j)")], None, ["wrong", "wrong"]),
    ("floyd-warshall", 0, [("for (i = 0; i < n; i++)", -1, PF + " reduction(+:)")],
     None, ["no-compile", "no-compile"]),
    # A wrong reduction operator: NaN everywhere, which no tolerance may pass.
    ("ludcmp", 0, [("for (k = 0; k < i; k++) {", 0, PF + " reduction(*:w)")], None,
     ["value", "value"]),
    # The right reduction on an ill-conditioned kernel: rounding flips signs.
    ("gramschmidt", 0, [("for (i = 0; i < _PB_M; i++)", 0, PF + " reduction(+:nrm)")], None,
     ["fp-sensitive", "fp-sensitive"]),
    ("covariance", 0, [("for (i = 0; i < _PB_N; i++)", 0, PF),
                       ("mean[j] += data[i][j];", 0, "#pragma omp critical")], None,
     ["correct", "correct"]),
    ("covariance", 1, [("for (i = 0; i < _PB_N; i++)", 0, PF),
                       ("mean[j] += data[i][j];", 0, "#pragma omp atomic")], None,
     ["correct", "correct"]),
    # Bare pragmas on both loops: the inner one makes j private.
    ("jacobi-2d", 0, [("for (i = 1; i < _PB_N - 1; i++)", -1, PF),
                      ("for (j = 1; j < _PB_N - 1; j++)", -1, PF)], None,
     ["correct", "correct", "correct"]),
]


def write_cases():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    paths = {os.path.splitext(os.path.basename(p))[0]: p
             for p in open(os.path.join(HERE, "kernels.txt")).read().split()}
    for kernel, k, edits, repl, _ in CASES:
        lines = open(os.path.join(PB, paths[kernel])).read().split("\n")
        at = []
        for n, (anchor, which, pragma) in enumerate(edits):
            idx = [i for i, l in enumerate(lines) if l.strip() == anchor]
            if not idx:
                sys.exit(f"{kernel}: anchor not found: {anchor!r}")
            at.append((idx[which], n, pragma))
        for pos, _, pragma in sorted(at, reverse=True):
            lines.insert(pos, pragma)
        text = "\n".join(lines)
        if repl:
            text = text.replace(*repl)
        open(os.path.join(OUT, f"{kernel}.{k}.c"), "w").write(text)


def main():
    assert check.numbers_close("0x1p+0 7", "0x1.0000000000001p+0 7")
    assert not check.numbers_close("0x1p+0", "0x1.01p+0")
    assert not check.numbers_close("0x1p+0 a", "0x1p+0 b")
    assert not check.numbers_close("nan", "0x1p+0") and not check.numbers_close("inf", "0x1p+0")
    assert check.same_shape("0x1p+0", "-0x1p-4") and not check.same_shape("nan", "0x1p+0")
    assert check.verdict(True, "timeout", "clean") == "timeout"
    assert check.verdict(True, "timeout", "race") == "wrong"
    assert check.verdict(True, "ok", "timeout") == "timeout"
    write_cases()
    csv_path = os.path.join(HERE, "work", "selftest.csv")
    cmd = [sys.executable, os.path.join(HERE, "check.py"), "--tool", "selftest",
           "--outputs", OUT, "--csv", csv_path,
           "--kernels"] + sorted({c[0] for c in CASES}) + sys.argv[1:]
    subprocess.run(cmd, check=True)
    rows = list(csv.DictReader(open(csv_path)))
    bad = 0
    for kernel, k, _, _, want in CASES:
        got = [r["verdict"] for r in rows if r["kernel"] == kernel and r["sample"] == str(k)]
        archer = [r["archer"] for r in rows if r["kernel"] == kernel and r["sample"] == str(k)]
        archer_run = "--archer" in sys.argv
        ok = len(got) == len(want) and all(
            g == w or (w == "value" and g == "wrong")
            or (w == "race" and (g == "wrong" if archer_run else g in ("wrong", "correct")))
            for g, w in zip(got, want))
        if archer_run:
            # Archer alone must see every planted race (not just the output check).
            ok = ok and all(a == "race" for a, w in zip(archer, want)
                            if w in ("wrong", "race", "nested-wrong"))
        print(("PASS" if ok else "FAIL"), f"{kernel}.{k}", got, archer if "--archer" in sys.argv else "")
        bad += not ok
    if archer_run:
        # A library that isn't there, or isn't Archer, must stop the run: TSan
        # without Archer reports libomp's own synchronization as races.
        for lib in ("/nonexistent/libarcher.so", "/usr/lib/llvm-22/lib/libomp.so"):
            r = subprocess.run(cmd + ["--kernels", "gemm", "--archer-lib", lib,
                                      "--csv", csv_path + ".bad"], capture_output=True)
            ok = r.returncode != 0
            print(("PASS" if ok else "FAIL"), "refuses --archer-lib", lib)
            bad += not ok
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
