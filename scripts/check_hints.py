#!/usr/bin/env python3
"""Check that every "fixes" hint the tool prints really fixes its loop.

    scripts/check_hints.py [--tool build/p05tool] [--flags "-ipa=coarse"]
                           [--json out.json] fixture.c [more.c ...] -- <clang args>

For each fixture, the tool is run with -reasons. For every loop it calls
fixable (every reason has a hint that claims to fix it), the hints' edits
are applied to a copy of the fixture (insertions only; they never add a
line, so the loop keeps its line number) and the tool is run again with the
hints' flags. The loop must now be SAFE. A hint whose edit is in another
file (a header) can't be applied here and is counted as unverified.

Prints one line per fixable loop and a summary per file; exits 1 if any
fixable loop stays rejected. --json writes the per-category counts of the
rejected loops (Phase 3's rejection breakdown) and how many were fixed.
"""
import argparse
import collections
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_tool(tool, flags, path, clang_args, out_jsonl, fatal=True):
    """The tool's records for path; on failure exit, or (fatal=False)
    return the error text instead."""
    if os.path.exists(out_jsonl):
        os.remove(out_jsonl)
    cmd = [tool] + flags + ["-reasons=" + out_jsonl, path, "--"] + clang_args
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       text=True)
    # The tool exits 0 even when the input doesn't compile; Clang's errors
    # are on stderr.
    if (r.returncode != 0 or not os.path.exists(out_jsonl)
            or " error: " in r.stderr):
        if not fatal:
            return r.stderr.strip() or f"exit status {r.returncode}"
        sys.exit(f"check_hints: tool failed on {path}:\n{r.stderr}")
    with open(out_jsonl) as f:
        return [json.loads(line) for line in f if line.strip()]


def key(rec):
    return (rec["function"], rec["line"])


def main():
    argv = sys.argv[1:]
    clang_args = []
    if "--" in argv:
        k = argv.index("--")
        argv, clang_args = argv[:k], argv[k + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=os.path.join(REPO, "build", "p05tool"))
    ap.add_argument("--flags", default="")
    ap.add_argument("--json")
    ap.add_argument("fixtures", nargs="+")
    a = ap.parse_args(argv)
    flags = a.flags.split()

    failed = 0
    totals = collections.Counter()
    by_cat = collections.Counter()
    work = tempfile.mkdtemp(prefix="check_hints.")
    try:
        for fx in a.fixtures:
            fx_real = os.path.realpath(fx)
            recs = run_tool(a.tool, flags, fx, clang_args,
                            os.path.join(work, "orig.jsonl"))
            recs = [r for r in recs if os.path.realpath(r["file"]) == fx_real]
            # Loops are matched by (function, line) after the edit; two
            # loops on one line would be ambiguous.
            seen = collections.Counter(key(r) for r in recs)
            rejected = [r for r in recs if r["safety"] != "SAFE"]
            fixed = unverified = 0
            for r in rejected:
                for c in {x["category"] for x in r["reasons"]}:
                    by_cat[c] += 1
            for r in rejected:
                if not r["fixable"]:
                    continue
                where = f"{os.path.basename(fx)}:{r['line']} ({r['function']})"
                edits, extra = {}, []
                foreign = False
                for x in r["reasons"]:
                    for e in x["hint"]["edits"]:
                        if "flag" in e:
                            if e["flag"] not in extra:
                                extra.append(e["flag"])
                        elif os.path.realpath(e["file"]) != fx_real:
                            foreign = True
                        else:
                            edits[e["offset"]] = e["insert"]
                if foreign or seen[key(r)] > 1:
                    unverified += 1
                    print(f"UNVERIFIED {where}: "
                          + ("an edit is outside the fixture" if foreign
                             else "another loop starts on the same line"))
                    continue
                with open(fx, "rb") as f:
                    src = f.read()
                for off in sorted(edits, reverse=True):
                    src = src[:off] + edits[off].encode() + src[off:]
                # Next to the original, so relative #includes still resolve.
                fd, copy = tempfile.mkstemp(
                    prefix=".hinted.", suffix="." + os.path.basename(fx),
                    dir=os.path.dirname(fx_real))
                with os.fdopen(fd, "wb") as f:
                    f.write(src)
                try:
                    after = run_tool(a.tool, flags + extra, copy, clang_args,
                                     os.path.join(work, "after.jsonl"),
                                     fatal=False)
                finally:
                    os.remove(copy)
                hints = "; ".join(dict.fromkeys(
                    x["hint"]["text"] for x in r["reasons"]))
                if isinstance(after, str):
                    failed += 1
                    print(f"FAIL   {where}: the hinted source does not "
                          f"compile: {hints}\n         {after[:300]}")
                    continue
                match = [x for x in after if key(x) == key(r)
                         and os.path.basename(x["file"]) ==
                         os.path.basename(copy)]
                if len(match) == 1 and match[0]["safety"] == "SAFE":
                    fixed += 1
                    print(f"FIXED  {where}: {hints}")
                else:
                    failed += 1
                    got = match[0]["safety"] if len(match) == 1 else "missing"
                    print(f"FAIL   {where}: still {got} after: {hints}")
                    if len(match) == 1:
                        for x in match[0]["reasons"]:
                            print(f"         - {x['text']}")
            nfix = sum(1 for r in rejected if r["fixable"])
            print(f"{fx}: {len(recs)} loop(s), {len(rejected)} rejected, "
                  f"{nfix} fixable by hints, {fixed} verified fixed, "
                  f"{unverified} unverified")
            totals.update(loops=len(recs), rejected=len(rejected),
                          fixable=nfix, fixed=fixed, unverified=unverified)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if a.json:
        with open(a.json, "w") as f:
            json.dump({"totals": dict(totals),
                       "rejected_by_category": dict(sorted(by_cat.items()))},
                      f, indent=2)
            f.write("\n")
    if failed:
        print(f"check_hints: {failed} fixable loop(s) were not fixed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
