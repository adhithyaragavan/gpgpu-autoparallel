#!/usr/bin/env python3
"""Phase 2.5 item 4: OMPar takes one loop at a time and returns a pragma, so
its loops are carried to Colab and its pragmas back into whole files.

    bridge.py export              # ompar/loops.jsonl: every for loop, as written
    bridge.py apply results.jsonl # outputs/ompar/ and outputs/ompar-long/

The two output sets differ only in how much OMPar may generate: ompar_run.py
explains why (OMPar's max_length of 64 includes the input).
"""
import json
import os
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.dirname(HERE)
sys.path.insert(0, BASE)
import cscan  # noqa: E402


def kernels(polybench):
    for path in open(os.path.join(BASE, "kernels.txt")).read().split():
        name = os.path.splitext(os.path.basename(path))[0]
        yield name, path, open(os.path.join(polybench, path)).read()


def export(polybench):
    with open(os.path.join(HERE, "loops.jsonl"), "w") as f:
        for name, _, text in kernels(polybench):
            src, lines = cscan.Source(text), text.split("\n")
            for line in src.for_lines():
                _, start, end = src.stmt_at(line)
                code = textwrap.dedent("\n".join(lines[start - 1:end]).expandtabs(8))
                f.write(json.dumps({"kernel": name, "line": line, "code": code}) + "\n")


def apply(results, polybench):
    rows = [json.loads(l) for l in open(results)]
    want = {(json.loads(l)["kernel"], json.loads(l)["line"])
            for l in open(os.path.join(HERE, "loops.jsonl"))}
    got = {(r["kernel"], r["line"]) for r in rows}
    if got != want:
        sys.exit(f"results cover {len(got)} loops, expected {len(want)}: rerun the notebook")
    for field, tool in (("pragma", "ompar"), ("pragma_long", "ompar-long")):
        out = os.path.join(BASE, "outputs", tool)
        os.makedirs(out, exist_ok=True)
        for name, _, text in kernels(polybench):
            lines = text.split("\n")
            here = sorted((r["line"], r[field]) for r in rows if r["kernel"] == name and r[field])
            for line, pragma in reversed(here):
                lines.insert(line - 1, "#pragma " + pragma)
            with open(os.path.join(out, name + ".0.c"), "w") as f:
                f.write("\n".join(lines))


if __name__ == "__main__":
    pb = os.environ.get("POLYBENCH", os.path.join(BASE, "work", "polybench"))
    if sys.argv[1:] == ["export"]:
        export(pb)
    elif len(sys.argv) == 3 and sys.argv[1] == "apply":
        apply(sys.argv[2], pb)
    else:
        sys.exit(__doc__)
