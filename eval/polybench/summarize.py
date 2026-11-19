#!/usr/bin/env python3
"""Tables from eval/polybench/run.py's CSV (Markdown on stdout).

    python3 eval/polybench/summarize.py [eval/polybench/results/m4-large.csv]

1. Per kernel: loops parallelized (kernel functions, as each tool reports
   them), speedup over clang-seq at 4 and 10 threads, and correctness
   (= exact, ~ within 1e-9 relative, X differs, - not checked).
2. Geometric means of the speedups over all kernels, and how many kernels
   each variant speeds up (>= 1.1x) or slows down (<= 0.9x).
3. P05's decision: gated P05 against the faster of clang-seq and p05-all
   (the cost model's choice vs the measured best of the two it chose between).
"""
import csv
import math
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PAR = ["p05", "p05-all", "polly", "gcc-auto"]
SEQ = ["polly-seq", "gcc-seq"]
MARK = {"exact": "=", "tolerance": "~", "differs": "X", "": "-"}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results", "m4-large.csv")
    rows = list(csv.DictReader(open(path)))
    t = defaultdict(dict)       # kernel -> (variant, threads) -> time
    meta = defaultdict(dict)    # kernel -> variant -> row
    for r in rows:
        if r["status"] == "ok" and r["time_s"]:
            t[r["kernel"]][(r["variant"], int(r["threads"]))] = float(r["time_s"])
        meta[r["kernel"]][r["variant"]] = r
    kernels = [k for k in dict.fromkeys(r["kernel"] for r in rows)
               if ("clang-seq", 1) in t[k]]

    def sp(k, v, n):
        base, x = t[k].get(("clang-seq", 1)), t[k].get((v, n))
        return base / x if base and x else None

    def fmt(x):
        return f"{x:.2f}" if x is not None else "n/a"

    print("## Per kernel (speedup over clang -O3 sequential; LARGE dataset)\n")
    head = ["kernel", "seq (s)", "loops p05/p05-all/polly/gcc"]
    for v in PAR:
        head += [f"{v} T4", f"{v} T10"]
    head += ["polly-seq", "gcc-seq", "correct p05/all/polly/gcc"]
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for k in kernels:
        loops = "/".join(str(meta[k].get(v, {}).get("loops", "")) for v in PAR)
        cells = [k, f"{t[k][('clang-seq', 1)]:.3f}", loops]
        for v in PAR:
            cells += [fmt(sp(k, v, 4)), fmt(sp(k, v, 10))]
        cells += [fmt(sp(k, "polly-seq", 1)), fmt(sp(k, "gcc-seq", 1))]
        cells.append("".join(MARK.get(meta[k].get(v, {}).get("correct", ""), "?")
                             for v in PAR))
        print("| " + " | ".join(cells) + " |")

    print("\n## Over all kernels\n")
    print("| variant | threads | geomean speedup | >= 1.1x | <= 0.9x | kernels |")
    print("|---|---|---|---|---|---|")
    for v, n in [(v, n) for v in PAR for n in (4, 10)] + [(v, 1) for v in SEQ]:
        xs = [sp(k, v, n) for k in kernels]
        xs = [x for x in xs if x]
        if not xs:
            continue
        g = math.exp(sum(math.log(x) for x in xs) / len(xs))
        print(f"| {v} | {n} | {g:.2f} | {sum(x >= 1.1 for x in xs)} | "
              f"{sum(x <= 0.9 for x in xs)} | {len(xs)} |")

    print("\n## P05's decision vs the measured best of its two options\n")
    print("Gated P05 chooses, per loop, between its parallel pragma and running "
          "sequentially. Kernel level: gated time vs min(clang-seq, p05-all) at the same "
          "thread count. 'right' = within 10% of that best.\n")
    print("| threads | right | slower than best | of which: declined a parallel win "
          "| kernels |")
    print("|---|---|---|---|---|")
    for n in (4, 10):
        right = wrong = declined = total = 0
        for k in kernels:
            g, a, s = t[k].get(("p05", n)), t[k].get(("p05-all", n)), t[k].get(("clang-seq", 1))
            if not (g and a and s):
                continue
            total += 1
            best = min(a, s)
            if g <= 1.1 * best:
                right += 1
            else:
                wrong += 1
                declined += a < s and g > 1.1 * a
        print(f"| {n} | {right} | {wrong} | {declined} | {total} |")


if __name__ == "__main__":
    main()
