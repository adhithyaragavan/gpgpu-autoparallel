#!/usr/bin/env python3
"""Phase 3.2: P05 on PolyBench/C 4.2.1 with a GPU (Colab T4, nvc -mp=gpu).

    python3 gpu_run.py --pb polybench --src rewrites [--runs 3] [--csv gpu.csv]

Self-contained (stdlib only): eval/polybench/make_gpu_notebook.py embeds it,
and P05's rewrites, in eval/colab/polybench_t4.ipynb, since the notebook
can't clone the repository. --src holds P05's rewrites, made on the
development machine with -machine machine/colab-t4.json:
  src/gated/<kernel>.c   P05's own choices (GPU, CPU threads or sequential)
  src/naive/<kernel>.c   -policy naive: every SAFE loop it can offload on the GPU

Per kernel (LARGE_DATASET, -O3):
  seq          the original, no OpenMP
  seq-mp       the original with the OpenMP flags (no pragmas): what -mp=gpu
               alone does to the host code and its floating point
  p05          gated rewrite, OpenMP offload
  p05-naive    naive rewrite
  p05-warm, p05-naive-warm
               the same, with an empty target region run before main (a
               constructor in warm.c): the timed kernel no longer pays the
               device's start-up (about 0.16 s on the T4, NOTES 2026-10-16),
               which a long-running application pays once
  seq-nofma, p05-nofma
               seq and p05 built with --nofma-flag (no fused multiply-adds),
               only for --nofma-kernels; p05-nofma is checked against
               seq-nofma. Tells FMA contraction apart from a real ordering
               change (ludcmp differs by 2.4e-9, NOTES 2026-11-28)
Host threads: OMP_NUM_THREADS = --threads (Colab: 2 vCPUs).

Correctness: a POLYBENCH_DUMP_ARRAYS build of each variant printing exact
hex floats, compared with seq's ("exact" or the largest relative
difference). The same run counts GPU kernel launches (NVCOMPILER_ACC_NOTIFY;
nvc only): 0 means every target region's if() chose the host.

Rows are appended to the CSV as they finish; a rerun skips rows already
there, so a disconnected Colab session resumes where it stopped.
"""
import argparse
import csv
import os
import re
import shutil
import subprocess
import sys

FIELDS = ["kernel", "variant", "threads", "status", "time_s", "times", "correct",
          "max_rel_err", "gpu_launches", "target_pragmas", "cpu_pragmas", "detail"]
# name, source, OpenMP, warm-up constructor, no-FMA build
VARIANTS = [("seq", "orig", False, False, False), ("seq-mp", "orig", True, False, False),
            ("p05", "gated", True, False, False),
            ("p05-naive", "naive", True, False, False), ("p05-warm", "gated", True, True, False),
            ("p05-naive-warm", "naive", True, True, False),
            ("seq-nofma", "orig", False, False, True), ("p05-nofma", "gated", True, False, True)]
WARM_C = """#include <omp.h>
__attribute__((constructor)) static void p05_warm(void) {
  int x = 0;
#pragma omp target map(tofrom: x)
  x += 1;
}
"""


def run(cmd, env=None, timeout=None):
    e = dict(os.environ)
    e.update(env or {})
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=e, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


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
            if fx != fx or fy != fy:
                return "differs", float("inf")
            worst = max(worst, abs(fx - fy) / max(abs(fx), abs(fy), 1e-300))
    return ("tolerance" if worst <= 1e-9 else "differs"), worst


class Bench:
    def __init__(self, args):
        self.a = args
        self.pb = os.path.abspath(args.pb)
        self.work = os.path.abspath(args.work)
        os.makedirs(self.work, exist_ok=True)
        self.warm = os.path.join(self.work, "warm.c")
        open(self.warm, "w").write(WARM_C)
        self.extra = []
        # __INTPTR_TYPE__ appears in P05's guards; GCC and Clang predefine it.
        probe = os.path.join(self.work, "probe.c")
        open(probe, "w").write("int main(void) { return (int)(__INTPTR_TYPE__)0; }\n")
        if run([args.cc, probe, "-o", probe + ".out"])[0] != 0:
            self.extra = ["-D__INTPTR_TYPE__=long"]

    def kernels(self):
        with open(os.path.join(self.pb, "utilities", "benchmark_list")) as f:
            return [l.strip()[2:] for l in f if l.strip()]

    def headers(self, kpath, exact):
        """A copy of the kernel's headers in the work directory (exact: printing
        hex floats); sources are compiled next to them, never in --pb."""
        src = os.path.join(self.pb, os.path.dirname(kpath))
        d = os.path.join(self.work, "hdr-hex" if exact else "hdr", os.path.basename(src))
        if not os.path.isdir(d):
            os.makedirs(d)
            for f in os.listdir(src):
                if f.endswith(".h"):
                    t = open(os.path.join(src, f)).read()
                    if exact:
                        t = t.replace('"%0.2lf "', '"%a "').replace('"%0.2f "', '"%a "')
                    open(os.path.join(d, f), "w").write(t)
        return d

    def build(self, name, kpath, src, omp, warm, exact, nofma=False):
        d = os.path.join(self.work, "bin", os.path.basename(os.path.dirname(kpath)))
        os.makedirs(d, exist_ok=True)
        exe = os.path.join(d, name + ("-dump" if exact else ""))
        # `#include "k.h"` searches the source's own directory first: compile
        # a copy next to the headers wanted.
        copy = os.path.join(self.headers(kpath, exact), f"{name}-{os.path.basename(src)}")
        shutil.copy(src, copy)
        defs = ["-D" + self.a.dataset,
                "-DPOLYBENCH_DUMP_ARRAYS" if exact else "-DPOLYBENCH_TIME"]
        flags = (["-O3"] + (self.a.omp_flags.split() if omp else []) + self.extra
                 + (self.a.nofma_flag.split() if nofma else []))
        cmd = ([self.a.cc] + flags + defs + ["-I", self.headers(kpath, exact), "-I",
               os.path.join(self.pb, "utilities"), copy,
               os.path.join(self.pb, "utilities", "polybench.c")]
               + ([self.warm] if warm else []) + ["-lm", "-o", exe])
        rc, _, se = run(cmd)
        if rc != 0:
            return None, (se.strip().splitlines() or ["build failed"])[-1]
        return exe, ""

    def env(self, omp):
        e = {"OMP_NUM_THREADS": str(self.a.threads)}
        if omp:
            e["OMP_TARGET_OFFLOAD"] = self.a.offload
        return e


def pragmas(path):
    text = open(path).read()
    t = len(re.findall(r"#pragma omp target teams", text))
    c = len(re.findall(r"#pragma omp parallel for", text))
    return t, c


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pb", required=True, help="PolyBench/C 4.2.1 directory")
    ap.add_argument("--src", required=True, help="directory with gated/ and naive/")
    ap.add_argument("--work", default="work")
    ap.add_argument("--csv", default="polybench-t4.csv")
    ap.add_argument("--cc", default="nvc")
    ap.add_argument("--omp-flags", default="-mp=gpu -gpu=cc75")
    ap.add_argument("--offload", default="DEFAULT",
                    help="OMP_TARGET_OFFLOAD for the OpenMP variants")
    ap.add_argument("--dataset", default="LARGE_DATASET")
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=600, help="seconds per run")
    ap.add_argument("--kernels", nargs="*")
    ap.add_argument("--variants", nargs="*")
    ap.add_argument("--nofma-flag",
                    help="no fused multiply-adds in host code (default: -Mnofma for "
                         "nvc, else -ffp-contract=off); device code keeps its own, "
                         "which is fine while gated ludcmp/lu launch no kernels")
    ap.add_argument("--nofma-kernels", nargs="*", default=["ludcmp", "lu"])
    args = ap.parse_args()
    if args.nofma_flag is None:
        args.nofma_flag = ("-Mnofma" if os.path.basename(args.cc).startswith("nvc")
                           else "-ffp-contract=off")
    b = Bench(args)

    done, new = set(), True
    try:
        with open(args.csv) as f:
            done = {(r["kernel"], r["variant"]) for r in csv.DictReader(f)}
            new = False
    except FileNotFoundError:
        pass
    out = open(args.csv, "a", newline="")
    w = csv.DictWriter(out, FIELDS)
    if new:
        w.writeheader()

    for kpath in b.kernels():
        k = os.path.splitext(os.path.basename(kpath))[0]
        if args.kernels and k not in args.kernels:
            continue
        ref = {}  # no-FMA build -> seq's dump
        for name, source, omp, warm, nofma in VARIANTS:
            if nofma and k not in args.nofma_kernels:
                continue
            base = "seq-nofma" if nofma else "seq"
            if args.variants and name not in args.variants and name != base:
                continue
            if (k, name) in done and name != base:
                continue
            src = (os.path.join(b.pb, kpath) if source == "orig"
                   else os.path.join(args.src, source, os.path.basename(kpath)))
            tp, cp = pragmas(src)
            row = dict(kernel=k, variant=name, threads=args.threads if omp else 1,
                       target_pragmas=tp, cpu_pragmas=cp)
            # Correctness and GPU launches.
            check, err, launches = "", 0.0, ""
            exe, log = b.build(name, kpath, src, omp, warm, exact=True, nofma=nofma)
            if exe:
                env = b.env(omp)
                if omp:
                    env["NVCOMPILER_ACC_NOTIFY"] = "1"
                rc, _, se = run([exe], env=env, timeout=args.timeout)
                lines = se.splitlines()
                launches = sum(1 for l in lines if l.startswith("launch CUDA kernel"))
                dump = "\n".join(l for l in lines if "CUDA" not in l)
                if rc != 0:
                    check = "timeout" if rc == -1 else f"exit {rc}"
                elif name == base:
                    ref[nofma] = dump
                elif nofma in ref:
                    check, err = compare(ref[nofma], dump)
            if (k, name) in done:
                continue
            exe, log = b.build(name, kpath, src, omp, warm, exact=False, nofma=nofma)
            if not exe:
                w.writerow(dict(row, status="build-failed", detail=log[:200]))
                out.flush()
                print(f"{k:16} {name:15} build failed: {log[:120]}", flush=True)
                continue
            times, status = [], "ok"
            for _ in range(args.runs):
                rc, so, se = run([exe], env=b.env(omp), timeout=args.timeout)
                if rc != 0:
                    status = "timeout" if rc == -1 else f"exit {rc}"
                    break
                times.append(float(so.strip().split()[-1]))
            w.writerow(dict(row, status=status, correct=check,
                            max_rel_err=f"{err:.3g}" if check else "",
                            gpu_launches=launches if omp else "",
                            time_s=f"{min(times):.6f}" if times else "",
                            times=" ".join(f"{x:.6f}" for x in times),
                            detail="" if status == "ok" else se.strip()[-200:]))
            out.flush()
            print(f"{k:16} {name:15} {status:8} "
                  f"{min(times) if times else float('nan'):10.4f}s  {check:9} "
                  f"launches={launches}", flush=True)
    out.close()


if __name__ == "__main__":
    main()
