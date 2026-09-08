#!/usr/bin/env python3
"""Day 19: exact per-element comparison of a benchmark's sequential vs.
OpenMP-rewritten output, both dumped as raw IEEE-754 doubles by passing a
dump path as argv[1] to the built benchmark binary (see benchmarks/*.c).

This is what scripts/build_and_run.sh's tier1-diff stage (a diff of each
binary's printed checksum, sum(arr)) cannot catch: a swapped pair of
elements, or a pair of compensating errors (+delta at index i, -delta at
index j), both leave the sum unchanged even though the array is wrong.

Comparison is exact bit-for-bit, not a tolerance -- see NOTES.md (Day 19)
for why: every rewritten loop in this benchmark set is element-independent
(no reduction), so the sequential and OpenMP builds compute each output
element from the identical operation sequence -- a genuine bit difference
here is a real correctness bug, not benign floating-point reordering.

Usage:
    compare_outputs.py --seq <path> --omp <path> --count <N>
Exit 0, prints "PASS: <N> doubles match exactly", on an exact match.
Exit 1, prints "FAIL: ..." naming the first differing index and both
values, otherwise (or if a dump file is the wrong size).
"""
import argparse
import struct
import sys


def load(path: str, count: int):
    with open(path, "rb") as f:
        data = f.read()
    expected = count * 8
    if len(data) != expected:
        print(f"FAIL: {path} is {len(data)} bytes, expected {expected} "
              f"({count} doubles) -- dump wasn't written, or --count is wrong")
        sys.exit(1)
    return struct.unpack(f"<{count}d", data)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seq", required=True, help="sequential binary's dump file")
    ap.add_argument("--omp", required=True, help="OpenMP binary's dump file")
    ap.add_argument("--count", type=int, required=True, help="element count")
    args = ap.parse_args()

    seq = load(args.seq, args.count)
    omp = load(args.omp, args.count)

    # struct.pack equality, not ==, so NaN/-0.0 compare bit-for-bit rather
    # than by IEEE-754 comparison rules (NaN != NaN, -0.0 == 0.0).
    for i, (a, b) in enumerate(zip(seq, omp)):
        if struct.pack("<d", a) != struct.pack("<d", b):
            print(f"FAIL: first mismatch at index {i}: seq={a!r} omp={b!r}")
            sys.exit(1)

    print(f"PASS: {args.count} doubles match exactly")
    sys.exit(0)


if __name__ == "__main__":
    main()
