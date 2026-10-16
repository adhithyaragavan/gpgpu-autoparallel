#!/usr/bin/env python3
"""Measure the cost model's machine parameters and write a -machine file.

    eval/calibrate/calibrate.py --name m4 --out machine/m4.json      (host)
    eval/calibrate/calibrate.py --name colab-t4 --gpu --cc nvc \\
        --cflags "-O2 -mp=gpu -gpu=cc75" --out machine/colab-t4.json

Builds calibrate.c (with -DP05_TARGET for --gpu), runs each benchmark and
derives the fields README.md lists. Startup costs are the median of 7 fresh
processes minus the steady-state cost of the same construct; everything else
is the best of several runs inside one process. Raw numbers go under "raw".
"""
import argparse
import datetime
import json
import os
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FRESH = 7  # fresh processes per startup measurement


def run(binary, *args, env=None):
    r = subprocess.run([binary, *map(str, args)], capture_output=True, text=True,
                       env={**os.environ, **(env or {})})
    if r.returncode != 0:
        sys.exit(f"{os.path.basename(binary)} {' '.join(map(str, args))} failed "
                 f"({r.returncode}): {r.stderr.strip()}")
    return float(r.stdout.strip())


def fresh(binary, *args, env=None):
    return [run(binary, *args, env=env) for _ in range(FRESH)]


def fit_copy(points):
    """Least-squares t = latency + bytes / bandwidth over the sizes of at
    least 1 MiB (smaller ones are all latency); returns (us, GB/s)."""
    pts = [(b, t) for b, t in points if b >= 1 << 20]
    n = len(pts)
    mb = sum(b for b, _ in pts) / n
    mt = sum(t for _, t in pts) / n
    slope = (sum((b - mb) * (t - mt) for b, t in pts) /
             sum((b - mb) ** 2 for b, _ in pts))  # us per byte
    return max(mt - slope * mb, 0.0), 1.0 / slope / 1e3


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cc", default=os.environ.get("CC", "clang"))
    ap.add_argument("--cflags", default="-O2 -fopenmp")
    ap.add_argument("--gpu", action="store_true", help="also measure the device")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="p05cal")
    binary = os.path.join(work, "calibrate")
    cmd = [a.cc, *shlex.split(a.cflags), *(["-DP05_TARGET"] if a.gpu else []),
           os.path.join(HERE, "calibrate.c"), "-o", binary]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    version = next((l.strip() for l in subprocess.run(
        [a.cc, "--version"], capture_output=True, text=True).stdout.splitlines()
                    if l.strip()), a.cc)  # nvc prints a blank line first

    raw = {}

    def step(key, f):
        print(f"  {key} ...", end=" ", flush=True)
        raw[key] = f()
        print(raw[key], flush=True)
        return raw[key]

    threads = step("threads", lambda: run(binary, "threads"))
    stream1 = step("stream_1thread_gbs", lambda: run(binary, "stream1"))
    streamn = step("stream_all_gbs", lambda: run(binary, "streamN"))
    ops1 = step("ops_1thread_gops", lambda: run(binary, "ops1"))
    opsn = step("ops_all_gops", lambda: run(binary, "opsN"))
    region = step("empty_parallel_for_us", lambda: run(binary, "region"))
    first = step("first_parallel_us", lambda: fresh(binary, "first"))
    first0 = step("first_parallel_if0_us", lambda: fresh(binary, "first_if0"))

    cpu = {
        "throughput_gflops": round(ops1, 3),
        # The model's aggregate is throughput * cores: cores is how many of
        # one thread all the threads together are worth (unequal cores and
        # static schedules make it fewer than the hardware count).
        "cores": max(1, round(opsn / ops1)),
        "dram_bandwidth_gbs": round(streamn, 3),
        "single_core_dram_share": round(min(stream1 / streamn, 1.0), 3),
        "thread_start_us": round(region, 3),
        "runtime_init_us": round(max(statistics.median(first) - region, 0.0), 3),
    }
    out = {
        "name": a.name,
        "measured": datetime.date.today().isoformat(),
        "host": f"{platform.machine()} {platform.processor() or platform.system()}, "
                f"{int(threads)} OpenMP threads",
        "compiler": f"{version} ({a.cflags})",
        "cpu": cpu,
    }
    if a.gpu:
        env = {"OMP_TARGET_OFFLOAD": "MANDATORY"}
        launch = step("empty_kernel_us", lambda: run(binary, "tlaunch", env=env))
        tfirst = step("first_target_us", lambda: fresh(binary, "tfirst", env=env))
        sizes = [1 << k for k in range(10, 29, 2)]
        to = step("map_to_us", lambda: [[b, run(binary, "tcopy", b, "to", env=env)]
                                         for b in sizes])
        frm = step("map_from_us", lambda: [[b, run(binary, "tcopy", b, "from", env=env)]
                                           for b in sizes])
        lat_to, bw_to = fit_copy(to)
        lat_from, bw_from = fit_copy(frm)
        raw["map_to_fit"] = {"latency_us": lat_to, "gbs": bw_to}
        raw["map_from_fit"] = {"latency_us": lat_from, "gbs": bw_from}
        tstream = step("device_stream_gbs", lambda: run(binary, "tstream", env=env))
        tops = step("device_ops_gops", lambda: run(binary, "tops", env=env))
        out["gpu"] = {
            "kernel_launch_us": round(launch, 3),
            # One bandwidth prices both directions: the per-byte average.
            "pcie_bandwidth_gbs": round(2.0 / (1.0 / bw_to + 1.0 / bw_from), 3),
            "throughput_gflops": round(tops, 3),
            "bandwidth_gbs": round(tstream, 3),
            "init_us": round(max(statistics.median(tfirst) - launch, 0.0), 3),
        }
        smi = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version",
                              "--format=csv,noheader"], capture_output=True, text=True)
        out["device"] = smi.stdout.strip() or "unknown"
    out["notes"] = a.notes or (
        "Ops are counted as p05tool counts them (calibrate.c). Startup costs: "
        f"median of {FRESH} fresh processes minus the steady-state construct.")
    out["raw"] = raw
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    print(json.dumps({k: out[k] for k in ("name", "cpu", "gpu") if k in out}, indent=2))
    print("wrote", a.out)


if __name__ == "__main__":
    main()
