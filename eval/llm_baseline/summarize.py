#!/usr/bin/env python3
"""Phase 2.5 item 4: the baseline table, from check.py's CSVs.

    summarize.py results/p05.archer.csv results/<tool>.archer.csv ...

One Markdown table on stdout. Pragma-group counts are for loops in the
kernel_* functions (the code PolyBench times), as a mean per sample, so a
tool asked three times compares with one asked once. Columns:

  samples    replies scored (kernels x samples), and how many were not:
             modified (changed other code), truncated or blocked replies
  groups     pragma groups checked (not-parallel ones are counted apart)
  wrong      output differs, crash, or an Archer race; "rate" is wrong over
             wrong + correct
  P05 SAFE   of the wrong ones, on loops P05 parallelizes itself (so the
             tool's clauses were wrong, not its choice of loop)
  nested     outer groups that fail only with a wrong group inside
  fp-sens.   a reassociating group off by more than the tolerance (hand audit)
  P05 not    correct groups on loops P05 leaves sequential (P05's misses,
  SAFE       if the loop is really parallel; blank P05 verdicts not counted)
  files      whole replies with a wrong result, every loop included
  archer     whether Archer ran on every compiled group
"""
import csv
import os
import sys
from collections import Counter, defaultdict


def main():
    cols = ["tool", "samples (unscored)", "groups", "wrong (rate)", "of which P05 SAFE",
            "nested-wrong", "fp-sensitive", "no-compile", "timeout", "not-parallel",
            "correct", "correct, P05 not SAFE", "files wrong", "archer"]
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    for path in sys.argv[1:]:
        rows = list(csv.DictReader(open(path)))
        if not rows:
            continue
        samples = {(r["kernel"], r["sample"]) for r in rows}
        per_kernel = defaultdict(set)
        for k, s in samples:
            per_kernel[k].add(s)
        n = len(samples) / len(per_kernel)
        files = [r for r in rows if r["group"] == "file"]
        unscored = Counter(r["verdict"] for r in files if r["verdict"] in ("modified", "truncated", "blocked"))
        kernel = [r for r in rows if r["group"] != "file" and r["in_kernel"] == "True"]
        v = Counter(r["verdict"] for r in kernel)

        def mean(x):
            return f"{x / n:.1f}" if n != 1 else str(x)
        wrong_safe = sum(r["verdict"] == "wrong" and r["p05_safety"] == "SAFE" for r in kernel)
        right_missed = sum(r["verdict"] == "correct" and r["p05_safety"] not in ("SAFE", "")
                           for r in kernel)
        decided = v["wrong"] + v["correct"]
        rate = f" ({100 * v['wrong'] / decided:.0f}%)" if decided else ""
        checked = [r for r in rows if r["group"] != "file" and r.get("compiles") == "True"]
        # The file name, not the tool column: p05 is run twice (with and
        # without its run-time guards).
        cells = [os.path.basename(path).split(".")[0],
                 f"{len(samples)} ({', '.join(f'{c} {k}' for k, c in unscored.items()) or 'all scored'})",
                 mean(len(kernel) - v["not-parallel"]), mean(v["wrong"]) + rate, mean(wrong_safe),
                 mean(v["nested-wrong"]), mean(v["fp-sensitive"]), mean(v["no-compile"]),
                 mean(v["timeout"]), mean(v["not-parallel"]), mean(v["correct"]), mean(right_missed),
                 mean(sum(r["verdict"] == "wrong" for r in files)),
                 "yes" if checked and all(r["archer"] != "na" for r in checked) else "no"]
        print("| " + " | ".join(cells) + " |")
    print()
    print("Counts are means per sample over the 30 kernels, for loops in kernel_* functions; "
          "\"files wrong\" counts whole replies, every loop included.")


if __name__ == "__main__":
    main()
