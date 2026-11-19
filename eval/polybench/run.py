#!/usr/bin/env python3
"""Phase 3.1: P05 vs Polly vs GCC autopar on PolyBench/C 4.2.1 (CPU, this machine).

    eval/llm_baseline/fetch_polybench.sh          # the pinned suite, once
    python3 eval/polybench/run.py [--kernels gemm syrk] [--runs 5] [--csv out.csv]

Per kernel it builds (LARGE_DATASET, -O3):
  clang-seq          the original, Homebrew clang
  p05                P05's rewrite, its own gated choices (-machine machine/m4.json:
                     no GPU section, so CPU or sequential), clang -fopenmp
  p05-all            P05's rewrite with -policy all-cpu: every loop it proves safe
                     runs in parallel (what the analysis finds, without the cost
                     model's choices)
  polly-seq          clang -mllvm -polly (tiling etc., no parallelism)
  polly              clang -mllvm -polly -mllvm -polly-parallel -fopenmp
  gcc-seq            gcc-15 -O3
  gcc-auto           gcc-15 -O3 -ftree-parallelize-loops=T (one binary per T)
and runs each --runs times per thread count (4 = the M4's performance cores,
10 = all cores), keeping the minimum of PolyBench's own POLYBENCH_TIME.

Correctness: a separate POLYBENCH_DUMP_ARRAYS build of each variant, printing
exact hex floats (%a, as eval/llm_baseline/check.py does), run at 10 threads.
Clang variants are compared with clang-seq, GCC ones with gcc-seq (the two
compilers contract floating point differently). "exact" or the largest
relative difference.

Loops parallelized, kernel_* functions only, as each tool reports them:
  p05   `#pragma omp` lines its rewrite inserts (guarded ones included)
  polly outlined parallel loops (`kernel_*_polly_subfn*` symbols), counted on
        a -fno-inline object (at -O3 the kernel is inlined into main), so
        Polly sees symbolic sizes there; the timed binary may differ
  gcc   "parallelizing ... loop" lines of -fopt-info-loop-optimized
Polly and GCC transform loops, so these are counts, not the same loops.

Rows are appended to the CSV as they finish; a rerun skips (kernel, variant,
threads) rows already there. Work files go to eval/polybench/work/ (ignored).
"""
import argparse
import csv
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PB = os.path.join(REPO, "eval", "llm_baseline", "work", "polybench")
WORK = os.path.join(HERE, "work")
CLANG = "/opt/homebrew/opt/llvm/bin/clang"
GCC = "/opt/homebrew/bin/gcc-15"
TOOL = os.path.join(REPO, "build", "p05tool")
MACHINE = os.path.join(REPO, "machine", "m4.json")
DATASET = "-DLARGE_DATASET"
THREADS = (4, 10)
FIELDS = ["kernel", "variant", "threads", "status", "time_s", "times", "correct",
          "max_rel_err", "loops", "detail"]


def sdk():
    r = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    return ["-isysroot", r.stdout.strip()] if r.returncode == 0 else []


SYSROOT = sdk()
OMP_LINK = ["-L/opt/homebrew/opt/llvm/lib", "-Wl,-rpath,/opt/homebrew/opt/llvm/lib"]


def kernels():
    with open(os.path.join(PB, "utilities", "benchmark_list")) as f:
        return [l.strip()[2:] for l in f if l.strip()]


def headers(kpath, exact):
    """The kernel's headers; exact: printing hex floats."""
    src = os.path.join(PB, os.path.dirname(kpath))
    if not exact:
        return src
    d = os.path.join(WORK, "hdr", os.path.basename(src))
    if not os.path.isdir(d):
        os.makedirs(d)
        for f in os.listdir(src):
            if f.endswith(".h"):
                t = open(os.path.join(src, f)).read()
                t = t.replace('"%0.2lf "', '"%a "').replace('"%0.2f "', '"%a "')
                open(os.path.join(d, f), "w").write(t)
    return d


def kernel_spans(text):
    """[(first line, last line)] of each kernel_* function definition (1-based)."""
    lines = text.split("\n")
    spans = []
    for i, l in enumerate(lines):
        if re.match(r"\s*(static\s+)?void\s+kernel_\w+\s*\(", l):
            depth, started = 0, False
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                started |= "{" in lines[j]
                if started and depth == 0:
                    spans.append((i + 1, j + 1))
                    break
    return spans


def in_kernel(line, spans):
    return any(a <= line <= b for a, b in spans)


def run(cmd, env=None, timeout=None):
    e = dict(os.environ)
    e.update(env or {})
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=e, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


class Variant:
    def __init__(self, name, cc, flags, source="orig", threads=THREADS, ref="clang-seq",
                 per_thread_flag=None):
        self.name, self.cc, self.flags, self.source = name, cc, flags, source
        self.threads, self.ref, self.per_thread_flag = threads, ref, per_thread_flag


VARIANTS = [
    Variant("clang-seq", CLANG, ["-O3"], threads=(1,)),
    Variant("p05", CLANG, ["-O3", "-fopenmp"] + OMP_LINK, source="p05"),
    Variant("p05-all", CLANG, ["-O3", "-fopenmp"] + OMP_LINK, source="p05-all"),
    Variant("polly-seq", CLANG, ["-O3", "-mllvm", "-polly"], threads=(1,)),
    Variant("polly", CLANG, ["-O3", "-mllvm", "-polly", "-mllvm", "-polly-parallel",
                             "-fopenmp"] + OMP_LINK),
    Variant("gcc-seq", GCC, ["-O3"], threads=(1,), ref="gcc-seq"),
    Variant("gcc-auto", GCC, ["-O3", "-fopt-info-loop-optimized"], ref="gcc-seq",
            per_thread_flag="-ftree-parallelize-loops={}"),
]


def source_for(v, kpath, orig, info):
    """The C file the variant compiles (P05: its rewrite, made once)."""
    if v.source == "orig":
        return os.path.join(PB, kpath)
    out = os.path.join(WORK, v.source, os.path.basename(kpath))
    policy = ["-policy", "all-cpu"] if v.source == "p05-all" else []
    if v.source + "_src" not in info:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if os.path.exists(out):
            os.remove(out)
        rc, so, se = run([TOOL, "-rewrite", "-o", out, "-machine=" + MACHINE] + policy + [
                          os.path.join(PB, kpath), "--", DATASET,
                          "-I", headers(kpath, False), "-I", os.path.join(PB, "utilities")]
                         + SYSROOT)
        if rc != 0:
            sys.exit(f"p05tool failed on {kpath}: {se[-400:]}")
        if not os.path.exists(out):  # no pragma: the file is unchanged
            shutil.copy(os.path.join(PB, kpath), out)
        text = open(out).read()
        spans = kernel_spans(text)
        n = sum(1 for i, l in enumerate(text.split("\n"), 1)
                if l.strip().startswith("#pragma omp") and in_kernel(i, spans))
        info[v.source + "_src"], info[v.source + "_loops"] = out, n
    return info[v.source + "_src"]


def build(v, kpath, src, threads, exact):
    tag = f"{v.name}{'' if v.per_thread_flag is None else threads}{'-dump' if exact else ''}"
    d = os.path.join(WORK, "bin", os.path.basename(os.path.dirname(kpath)))
    os.makedirs(d, exist_ok=True)
    exe = os.path.join(d, tag)
    flags = list(v.flags)
    if v.per_thread_flag:
        flags.append(v.per_thread_flag.format(threads))
    defs = [DATASET, "-DPOLYBENCH_DUMP_ARRAYS" if exact else "-DPOLYBENCH_TIME"]
    if exact:
        # `#include "k.h"` searches the source's own directory first: compile a
        # copy next to the hex-printing headers, or the original ones win.
        copy = os.path.join(headers(kpath, True), f"{v.name}-{os.path.basename(src)}")
        shutil.copy(src, copy)
        src = copy
    cmd = ([v.cc] + flags + defs + ["-I", headers(kpath, exact), "-I",
           os.path.join(PB, "utilities"), src, os.path.join(PB, "utilities", "polybench.c"),
           "-lm", "-o", exe] + (SYSROOT if v.cc == CLANG else []))
    rc, so, se = run(cmd)
    if rc != 0:
        return None, se.strip().splitlines()[-1] if se.strip() else "build failed"
    return exe, se


def loops_of(v, kpath, exe, build_log, info):
    spans = kernel_spans(open(os.path.join(PB, kpath)).read())
    if v.source != "orig":
        return info.get(v.source + "_loops", "")
    if v.name == "polly":
        # At -O3 the kernel is inlined into main, whose name the outlined
        # loops then carry: count on a -fno-inline object instead.
        obj = exe + ".count.o"
        rc, _, se = run([v.cc] + v.flags + ["-fno-inline", DATASET, "-c", "-I",
                        headers(kpath, False), "-I", os.path.join(PB, "utilities"),
                        os.path.join(PB, kpath), "-o", obj] + SYSROOT)
        if rc != 0:
            return ""
        rc, so, _ = run(["nm", obj])
        # Mach-O symbols carry a leading underscore.
        return sum(1 for l in so.splitlines()
                   if re.search(r"\s_?kernel_\w*_polly_subfn", l))
    if v.name == "gcc-auto":
        n = 0
        for l in build_log.splitlines():
            m = re.match(r".*?:(\d+):\d+: optimized: parallelizing (outer|inner) loop", l)
            if m and os.path.basename(kpath) in l and in_kernel(int(m.group(1)), spans):
                n += 1
        return n
    return 0


def compare(ref, got):
    """'exact', or the largest relative difference between the numbers."""
    if ref == got:
        return "exact", 0.0
    a, b = ref.split(), got.split()
    if len(a) != len(b):
        return "differs", float("inf")
    worst = 0.0
    for x, y in zip(a, b):
        if x == y:
            continue
        try:
            fx, fy = float.fromhex(x), float.fromhex(y)
        except ValueError:
            try:
                fx, fy = float(x), float(y)
            except ValueError:
                return "differs", float("inf")
        if fx != fy:
            if fx != fx or fy != fy:  # NaN on one side
                return "differs", float("inf")
            worst = max(worst, abs(fx - fy) / max(abs(fx), abs(fy), 1e-300))
    return ("tolerance" if worst <= 1e-9 else "differs"), worst


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kernels", nargs="*", help="names (default: all 30)")
    ap.add_argument("--variants", nargs="*", help="names (default: all)")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--timeout", type=int, default=600, help="seconds per run")
    ap.add_argument("--csv", default=os.path.join(HERE, "results", "m4-large.csv"))
    ap.add_argument("--no-check", action="store_true", help="skip the correctness dumps")
    args = ap.parse_args()
    if not os.path.isdir(PB):
        sys.exit("run eval/llm_baseline/fetch_polybench.sh first")

    done = set()
    if os.path.exists(args.csv):
        with open(args.csv) as f:
            for r in csv.DictReader(f):
                done.add((r["kernel"], r["variant"], r["threads"]))
    os.makedirs(os.path.dirname(args.csv), exist_ok=True)
    new = not os.path.exists(args.csv)
    out = open(args.csv, "a", newline="")
    w = csv.DictWriter(out, FIELDS)
    if new:
        w.writeheader()

    names = set(args.kernels or [])
    for kpath in kernels():
        k = os.path.splitext(os.path.basename(kpath))[0]
        if names and k not in names:
            continue
        info, dumps = {}, {}
        orig = open(os.path.join(PB, kpath)).read()
        for v in VARIANTS:
            if args.variants and v.name not in args.variants and v.name != v.ref:
                continue
            src = source_for(v, kpath, orig, info)
            # Correctness at the largest thread count (references: sequential).
            check, err = "", 0.0
            if not args.no_check:
                t = max(v.threads)
                exe, log = build(v, kpath, src, t, exact=True)
                if exe:
                    rc, _, se = run([exe], env={"OMP_NUM_THREADS": str(t)},
                                    timeout=args.timeout)
                    dumps[v.name] = se if rc == 0 else None
                    ref = dumps.get(v.ref)
                    if rc != 0:
                        check = f"exit {rc}"
                    elif v.name != v.ref and ref is not None:
                        check, err = compare(ref, se)
            for t in v.threads:
                key = (k, v.name, str(t))
                if key in done:
                    continue
                exe, log = build(v, kpath, src, t, exact=False)
                row = dict(kernel=k, variant=v.name, threads=t, correct=check,
                           max_rel_err=f"{err:.3g}" if check else "")
                if not exe:
                    w.writerow(dict(row, status="build-failed", detail=log[:200]))
                    out.flush()
                    continue
                times, status = [], "ok"
                for _ in range(args.runs):
                    rc, so, se = run([exe], env={"OMP_NUM_THREADS": str(t)},
                                     timeout=args.timeout)
                    if rc != 0:
                        status = "timeout" if rc == -1 else f"exit {rc}"
                        break
                    times.append(float(so.strip().split()[-1]))
                w.writerow(dict(row, status=status,
                                time_s=f"{min(times):.6f}" if times else "",
                                times=" ".join(f"{x:.6f}" for x in times),
                                loops=loops_of(v, kpath, exe, log, info)))
                out.flush()
                print(f"{k:16} {v.name:10} T={t:<3} {status:8} "
                      f"{min(times) if times else float('nan'):10.4f}s  {check}", flush=True)
    out.close()


if __name__ == "__main__":
    main()
