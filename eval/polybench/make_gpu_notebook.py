#!/usr/bin/env python3
"""Write eval/colab/polybench_t4.ipynb: P05 on PolyBench with a T4 (Phase 3.2).

    eval/llm_baseline/fetch_polybench.sh       # the pinned suite, once
    ninja -C build
    python3 eval/polybench/make_gpu_notebook.py [--dataset LARGE_DATASET]
    python3 eval/polybench/make_gpu_notebook.py --dataset MINI_DATASET --dump-to DIR
                       # rewrites only, into DIR/{gated,naive} (smoke tests)

Runs p05tool on every kernel with -machine machine/colab-t4.json, gated and
-policy naive, and embeds the rewrites and gpu_run.py in the notebook (the
repository is private, so the notebook can't clone it; it fetches PolyBench
itself at the commit fetch_polybench.sh pins). Run this again after
changing the tool, the machine file or gpu_run.py.

The rewrites hold the dataset's array sizes as constants (map sections such
as `A[i:1][0:1900]`), so they are only valid at the dataset they were made for.
"""
import argparse
import base64
import gzip
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PB = os.path.join(REPO, "eval", "llm_baseline", "work", "polybench")
TOOL = os.path.join(REPO, "build", "p05tool")
MACHINE = os.path.join(REPO, "machine", "colab-t4.json")
OUT = os.path.join(REPO, "eval", "colab", "polybench_t4.ipynb")
FETCH = os.path.join(REPO, "eval", "llm_baseline", "fetch_polybench.sh")


def cell(kind, text, hidden=False):
    meta = {"cellView": "form", "jupyter": {"source_hidden": True}} if hidden else {}
    c = {"cell_type": kind, "metadata": meta, "source": text.splitlines(keepends=True)}
    if kind == "code":
        c.update(execution_count=None, outputs=[])
    return c


def blob(text):
    """text, gzipped and base64-encoded in 76-character lines: notebook editors
    (Colab's, Kaggle's) freeze laying out a single line of hundreds of KB."""
    b = base64.b64encode(gzip.compress(text.encode(), mtime=0)).decode()
    return "\n".join(b[i:i + 76] for i in range(0, len(b), 76))


def unblob(name, text):
    """A Python statement setting name to the text blob() packed."""
    return (f"{name} = gzip.decompress(base64.b64decode(\"\"\"\n{blob(text)}\n\"\"\")).decode()\n")


def sdk():
    r = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    return ["-isysroot", r.stdout.strip()] if r.returncode == 0 else []


def rewrites(dataset):
    """{"gated/k.c": text, "naive/k.c": text} for every kernel."""
    with open(os.path.join(PB, "utilities", "benchmark_list")) as f:
        kpaths = [l.strip()[2:] for l in f if l.strip()]
    tmp = os.path.join(HERE, "work", "gpu-rewrites")
    shutil.rmtree(tmp, ignore_errors=True)
    files = {}
    for kpath in kpaths:
        for policy in ("gated", "naive"):
            out = os.path.join(tmp, policy, os.path.basename(kpath))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            r = subprocess.run([TOOL, "-rewrite", "-o", out, "-machine=" + MACHINE,
                                "-policy", policy, os.path.join(PB, kpath), "--",
                                "-D" + dataset, "-I", os.path.join(PB, os.path.dirname(kpath)),
                                "-I", os.path.join(PB, "utilities")] + sdk(),
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"p05tool failed on {kpath} ({policy}): {r.stderr[-400:]}")
            src = out if os.path.exists(out) else os.path.join(PB, kpath)
            files[f"{policy}/{os.path.basename(kpath)}"] = open(src).read()
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dataset", default="LARGE_DATASET")
    ap.add_argument("--dump-to", help="write the rewrites here instead of a notebook")
    args = ap.parse_args()
    if not os.path.isdir(PB):
        sys.exit("run eval/llm_baseline/fetch_polybench.sh first")
    fetch = open(FETCH).read()
    url = re.search(r"^URL=(\S+)", fetch, re.M).group(1)
    commit = re.search(r"^COMMIT=(\S+)", fetch, re.M).group(1)
    tool_rev = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    files = rewrites(args.dataset)
    if args.dump_to:
        for p, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(args.dump_to, p)), exist_ok=True)
            open(os.path.join(args.dump_to, p), "w").write(text)
        print(f"{len(files)} rewrites ({args.dataset}) in {args.dump_to}")
        return
    n_t = sum(f.count("#pragma omp target teams") for p, f in files.items() if p.startswith("gated"))
    runner = open(os.path.join(HERE, "gpu_run.py")).read()

    setup = ("# Where to work: Colab's /content or Kaggle's /kaggle/working.\n"
             "import base64, gzip, os\n"
             "ROOT = '/kaggle/working' if os.path.isdir('/kaggle/working') else '/content'\n"
             "os.makedirs(ROOT, exist_ok=True)\n"
             "os.environ['P05_ROOT'] = ROOT  # the %%bash cells cd here\n"
             "os.chdir(ROOT)\n"
             + unblob("RUNNER", runner) +
             "open(os.path.join(ROOT, 'gpu_run.py'), 'w').write(RUNNER)\n"
             "print('working in', ROOT)\n")
    cells = [
        cell("markdown", f"""# P05 on PolyBench with a T4 GPU (Phase 3.2)

Runs on **Colab** or **Kaggle** (it works in `/content` or `/kaggle/working`, whichever exists).

- **Colab:** Runtime → Change runtime type → **T4 GPU**, then Runtime → **Run all**. Keep the
  tab open; if the session disconnects, reconnect and Run all again: the run resumes from the
  rows already in the CSV (unless the runtime was reset).
- **Kaggle:** File → Import Notebook (this file). In the right-hand panel, Session options:
  **Accelerator → GPU T4 x2** and **Internet on** (both need a phone-verified account). Then
  **Save Version → Save & Run All (Commit)**: it runs in the background (up to 12 hours) with
  the browser closed, and the CSV appears under the version's **Output**.

Installing the NVIDIA HPC SDK takes about 10 minutes; the benchmark run takes longer (30
kernels, 6 variants, 3 runs each at the LARGE size, plus two strict-IEEE builds of ludcmp and lu).

Variants per kernel: `seq` (nvc -O3, no OpenMP), `seq-mp` (the same source with the OpenMP
flags: what `-mp=gpu` alone does), `p05` (P05's own choices with the T4
machine model: GPU, host threads or sequential per loop), `p05-naive` (every loop P05 can
offload goes to the GPU), and both again as `-warm` (the device started before `main`, so
the timed kernel doesn't pay the ~0.16 s start-up). For ludcmp and lu, `seq-ieee` and
`p05-ieee` are built with `-Kieee -Mnofma` (correctly rounded division, no fused
multiply-adds), checked against each other.
Correctness: exact hex dumps against `seq`. `gpu_launches` counts kernel launches (0 = every guard chose the host).

**When it finishes, download `polybench-t4.csv` (Colab: the last cell offers it; Kaggle: the
version's Output) and give it to Claude**; it goes to `eval/polybench/results/`.

Generated by `eval/polybench/make_gpu_notebook.py` at P05 {tool_rev} ({len(files) // 2}
kernels, {args.dataset}, {n_t} GPU pragmas in the gated rewrites): edit that and `gpu_run.py`, not this
notebook.
"""),
        cell("code", setup, hidden=True),
        cell("code", """%%bash
nvidia-smi --query-gpu=name,compute_cap,driver_version,memory.total --format=csv
nproc; grep -m1 'model name' /proc/cpuinfo
"""),
        cell("code", """%%bash
# NVIDIA HPC SDK (nvc -mp=gpu): the compiler the GPU evaluation uses (NOTES.md, 2026-10-02).
set -e
if ls /opt/nvidia/hpc_sdk/Linux_x86_64/*/compilers/bin/nvc >/dev/null 2>&1; then echo "already installed"; exit 0; fi
curl -s https://developer.download.nvidia.com/hpc-sdk/ubuntu/DEB-GPG-KEY-NVIDIA-HPC-SDK \\
  | gpg --dearmor --yes -o /usr/share/keyrings/nvidia-hpcsdk-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/nvidia-hpcsdk-archive-keyring.gpg] https://developer.download.nvidia.com/hpc-sdk/ubuntu/amd64 /' \\
  > /etc/apt/sources.list.d/nvhpc.list
apt-get update -qq
PKG=$(apt-cache search --names-only '^nvhpc-[0-9]+-[0-9]+$' | awk '{print $1}' | sort -V | tail -1)
echo "Installing $PKG"; apt-get install -y -qq $PKG > /tmp/nvhpc.log 2>&1 || { tail -20 /tmp/nvhpc.log; exit 1; }
ls -d /opt/nvidia/hpc_sdk/Linux_x86_64/*/compilers/bin/nvc
"""),
        cell("code", f"""%%bash
# PolyBench/C 4.2.1 at the commit eval/llm_baseline/fetch_polybench.sh pins.
set -e
cd "$P05_ROOT"
[ -d polybench/.git ] || git clone -q {url} polybench
git -C polybench checkout -q {commit}
test "$(git -C polybench rev-parse HEAD)" = {commit} && echo "polybench at {commit}"
"""),
        cell("code", "# P05's rewrites (made with -machine machine/colab-t4.json), packed by\n"
                     "# make_gpu_notebook.py (gzip + base64: one long line freezes the editor).\n"
                     "import base64, gzip, json, os\nROOT = os.environ['P05_ROOT']\n"
                     + unblob("PACKED", json.dumps(files)) + "FILES = json.loads(PACKED)\n"
                     "for p, text in FILES.items():\n"
                     "    os.makedirs(os.path.dirname(os.path.join(ROOT, 'rewrites', p)), exist_ok=True)\n"
                     "    open(os.path.join(ROOT, 'rewrites', p), 'w').write(text)\n"
                     "print(len(FILES), 'files')\n", hidden=True),
        cell("code", """%%bash
cd "$P05_ROOT"
NVC=$(ls -d /opt/nvidia/hpc_sdk/Linux_x86_64/*/compilers/bin/nvc | sort -V | tail -1)
CC=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d .)
$NVC --version | grep -v '^$' | head -1
python3 gpu_run.py --pb polybench --src rewrites --cc $NVC --omp-flags "-mp=gpu -gpu=cc$CC" \\
  --threads $(nproc) --runs 3 --dataset DATASET --csv polybench-t4.csv
""".replace("DATASET", args.dataset)),
        cell("code", """import csv, math, os
CSV = os.path.join(os.environ['P05_ROOT'], 'polybench-t4.csv')
rows = list(csv.DictReader(open(CSV)))
t = {(r['kernel'], r['variant']): float(r['time_s']) for r in rows if r['status'] == 'ok' and r['time_s']}
bad = [(r['kernel'], r['variant'], r['status'], r['correct']) for r in rows
       if r['status'] != 'ok' or r['correct'] not in ('', 'exact', 'tolerance')]
print('not ok / not correct:', bad or 'none')
for v in ('seq-mp', 'p05', 'p05-naive', 'p05-warm', 'p05-naive-warm', 'p05-ieee'):
    s = 'seq-ieee' if v == 'p05-ieee' else 'seq'
    xs = [t[(k, s)] / t[(k, v)] for (k, vv) in t if vv == v and (k, s) in t]
    if xs:
        print(f"{v:15} geomean speedup over {s} {math.exp(sum(map(math.log, xs)) / len(xs)):.2f} ({len(xs)} kernels)")
try:
    from google.colab import files
    files.download(CSV)
except ImportError:  # Kaggle: files in /kaggle/working are the version's Output
    from IPython.display import FileLink, display
    print('CSV at', CSV)
    display(FileLink(os.path.relpath(CSV)))
"""),
    ]
    nb = {"cells": cells, "metadata": {"accelerator": "GPU", "colab": {"gpuType": "T4"},
          "kernelspec": {"display_name": "Python 3", "name": "python3"}},
          "nbformat": 4, "nbformat_minor": 0}
    with open(OUT, "w") as f:
        json.dump(nb, f, indent=1)
        f.write("\n")
    print(f"wrote {os.path.relpath(OUT, REPO)}: {len(files)} rewrites, {n_t} GPU pragmas (gated)")


if __name__ == "__main__":
    main()
