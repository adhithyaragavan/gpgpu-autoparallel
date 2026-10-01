#!/usr/bin/env python3
"""Regenerate the FileCheck lines above every loop in a fixture.

    scripts/gen_filecheck.py tests/nests.c [more fixtures...]

The expected values are SNAPSHOTTED from the current tool's output, so a bug
in the tool gets baked in as "expected". That is why the script prints every
loop's prose comment beside the values it pins: you must read that table and
confirm each row matches its comment before committing. This is the step that
caught the 2026-10-02 "no condition clause" mismatch (see NOTES.md).

Idempotent: existing `// CHECK:` / `// RUN:` lines are stripped first. The
fixture is only rewritten after the tool has run successfully on a stripped
temp copy, so a failed run never loses its CHECK lines. Files in a directory
named tests/ get a RUN line; files elsewhere (benchmarks/) are run by a
tests/*.test wrapper instead.

Refuses (exits non-zero) on a fixture where two loops start on the same line:
the CHECK blocks are keyed by line, so one would be silently dropped. Loops
the tool reports from other files (#include'd fragments) are not pinned.

What gets pinned, each anchored with {{$}}: the loop location, kind, trip
count, UNRECOGNIZED reason, nest facts, call-chain reach lines, the safety
verdict plus its first reason (when not SAFE), and profitability. Guard
thresholds are wildcarded so cost-model recalibration doesn't break tests.
"""
import os
import platform
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.environ.get("P05_TOOL", os.path.join(REPO, "build", "p05tool"))
RUN = "// RUN: %p05 %s -- %cflags | FileCheck %s"
A = "{{$}}"


def tool_output(path):
    args = [TOOL, path, "--"]
    if platform.system() == "Darwin":
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True, check=True).stdout.strip()
        args += ["-isysroot", sdk]
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def spec_comment(src, line):
    """The comment block directly above the loop (stripped of CHECK lines)."""
    j, com = line - 2, []
    while j >= 0 and src[j].lstrip().startswith("//"):
        com.insert(0, src[j].strip().lstrip("/ "))
        j -= 1
    return " ".join(com)


def main(paths):
    for path in paths:
        path = os.path.abspath(path)
        rel = os.path.relpath(path, REPO)
        src = [l for l in open(path).read().split("\n")
               if not re.match(r"\s*// (CHECK|RUN): ", l)]
        # Run the tool on a stripped temp copy in the same directory (so its
        # line numbers match `src` and relative #includes still resolve), and
        # only touch the fixture once that has succeeded.
        tmp = os.path.join(os.path.dirname(path), ".gen_" + os.path.basename(path))
        try:
            open(tmp, "w").write("\n".join(src))
            out = tool_output(tmp)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)

        parts = re.split(r"\n(?=\S+:\d+:\d+\n)", out)
        nloops = re.search(r"Analyzed (\d+) loop", parts[0]).group(1)
        checks = {}
        # Each loop's location check captures its line into a FileCheck numeric
        # variable (LOOP<n>, n = report order), and a child's "parent L:C" is
        # checked against that variable rather than a literal line number:
        # inserting these very CHECK lines (or any later edit) moves the parent.
        blocks = [b.splitlines() for b in parts[1:]]
        locs = [re.match(r"(.*):(\d+):(\d+)$", b[0]).groups() for b in blocks]
        index_of_loc = {}
        for n, (f, line, col) in enumerate(locs):
            if f == tmp:  # loops from #include'd files are not pinned here
                if int(line) in {l for (g, l) in index_of_loc if g == tmp}:
                    sys.exit(f"{rel}:{line}: two loops start on this line; put each "
                             f"loop on its own line so each gets its own CHECK block")
            index_of_loc[(f, int(line))] = n
        print(f"\n######## {rel}")
        for n, L in enumerate(blocks):
            fname, line, col = locs[n][0], int(locs[n][1]), int(locs[n][2])
            if fname != tmp:
                continue

            def field(key):
                # "key<spaces>:" exactly, so "nest" doesn't also match "nest ivs".
                pat = re.compile(r"  " + re.escape(key) + r"\s+:")
                return next((l.split(":", 1)[1].strip() for l in L
                             if pat.match(l)), None)

            c = ["kind       : " + field("kind") + A]
            if field("kind") != "UNRECOGNIZED":
                c.append("trip count : " + field("trip count") + A)
            if field("reason"):
                c.append("reason     : " + field("reason") + A)
            def parent_ref(m):
                # "parent L:C" (same file) or "parent FILE:L:C" (another file).
                pfile = m.group(1) or tmp
                if pfile != tmp:
                    sys.exit(f"{rel}:{line}: parent loop is in another file "
                             f"({pfile}); not supported by this generator")
                return f"parent [[#LOOP{index_of_loc[(tmp, int(m.group(2)))]}]]:{m.group(3)}"
            nest = re.sub(r"parent (?:(\S+):)?(\d+):(\d+)", parent_ref, field("nest"))
            c.append("nest       : " + nest + A)
            c.append("nest ivs   : " + field("nest ivs") + A)
            c += [l.strip() + A for l in L
                  if " reaches " in l and l.startswith("    ") and not l.startswith("    -")]
            safety = field("safety")
            c.append("safety     : " + safety + A)
            si = next(i for i, l in enumerate(L) if l.startswith("  safety"))
            if safety != "SAFE":
                c.append(L[si + 1].strip() + A)
            prof = field("profitability")
            c.append("profitability : " + re.sub(r">= \d+\)", ">= {{[0-9]+}})", prof) + A)
            checks[line] = (n, col, c)

            print(f"L{line}: SPEC> {spec_comment(src, line)[:160]}")
            print(f"       GOT > {field('kind')} | {field('reason') or ''} | nest: {field('nest')}"
                  f" | ivs: {field('nest ivs')} | {safety} | {prof}")

        new = [RUN] if os.path.basename(os.path.dirname(path)) == "tests" else []
        new.append(f"// CHECK: Analyzed {nloops} loop(s).")
        # The location CHECK uses the fixture's basename, not the temp copy's.
        for i, l in enumerate(src, start=1):
            if i in checks:
                n, col, c = checks[i]
                ind = " " * (col - 1)
                new.append(f"{ind}// CHECK: {os.path.basename(path)}:[[#LOOP{n}:@LINE+{len(c) + 1}]]:{col}{A}")
                new += [f"{ind}// CHECK: {x}" for x in c]
            new.append(l)
        open(path, "w").write("\n".join(new))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
