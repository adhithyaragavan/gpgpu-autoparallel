#!/usr/bin/env python3
"""Phase 2.5 item 4: check every OpenMP pragma a tool inserted into the
PolyBench kernels, one pragma group at a time.

    check.py --tool <name>          # outputs/<name>/<kernel>.<k>.c
    check.py --make-p05             # first write outputs/p05/ with P05 itself
    check.py --tool <name> --archer # also run Archer (Linux, LLVM's libarcher)

A candidate file may differ from the original only by inserted `#pragma omp`
lines (blank lines, comment-only lines, `#include <omp.h>` and
`#pragma scop`/`endscop` may also come or go). Anything else marks the
sample "modified" and none of its pragmas are scored.

Each pragma is tied to the statement on the next code line. A pragma that
starts a team (parallel, target, teams) heads a group; any other pragma (for,
atomic, critical, simd, task, taskloop, ...) joins the innermost head whose
statement contains it, or is a group of its own. Each group is inserted into
the original file together with the groups nested inside its statement and
nothing else, so a wrong pragma is pinned to its own loop, and a loop is
judged with the inner pragmas the tool gave it (an inner `parallel for`
makes its loop variable private, which the outer one may rely on). An outer
group that fails while a group nested in it also fails is "nested-wrong":
the blame goes to the inner one only. The whole sample (every pragma
together) is checked as well.

A group that cannot run its loop on several threads is "not-parallel" and
not checked: one without a team (an orphaned `for`, `simd`, a `task` or
`taskloop` outside a parallel region), or one with `num_threads(1)` or a
constant false `if`.

A group's build is checked by:
  compile  clang -fopenmp -O2 -DMEDIUM_DATASET -DPOLYBENCH_DUMP_ARRAYS
  output   the array dump (printed as exact hex floats: the working copy of
           the kernel's header prints with %a instead of %0.2lf) equals the
           sequential build's at every --threads count, --runs times each.
           Exact, except for groups that reassociate floating-point sums (a
           reduction clause, `atomic` or `critical`): there
           |a - b| <= 1e-6 * max(|a|, |b|) + 1e-12 per element, and a
           non-finite value must match exactly. Such a group that misses the
           tolerance with only finite values where the sequential run has
           them is "fp-sensitive": rounding in an ill-conditioned kernel
           (gramschmidt's norms flip sign when the same sum runs in reverse
           order, with no OpenMP at all) looks the same as a wrong result, so
           it is neither wrong nor correct, and is audited by hand.
  archer   (--archer) built with -fsanitize=thread, run at 4 threads with
           Archer loaded: any ThreadSanitizer data race report. A probe
           program must print Archer's banner before anything runs, and a
           run that reports races must have printed it too (without Archer,
           TSan reports libomp's own synchronization as races).
verdict:   no-compile, wrong (output differs, crash, or a race), correct (all
           checks run passed; archer column says if it ran), fp-sensitive,
           nested-wrong, not-parallel, or timeout.
Threads sleep while waiting (OMP_WAIT_POLICY=passive): a `parallel for` on an
inner loop starts a team millions of times, and spinning teams starve each
other when several variants run at once. A variant that times out is run
again alone with a longer limit. If it still doesn't finish it is "timeout",
counted neither as wrong nor as correct: that is no evidence either way.

Results: results/<tool>.csv, one row per group plus one "file" row per
sample, each with P05's own verdict for the loop (outputs/p05/verdicts/).
"""
import argparse
import concurrent.futures as cf
import csv
import difflib
import hashlib
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import time

import cscan

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHER_BANNER = "Archer detected OpenMP application with TSan"
ROOT = os.path.dirname(os.path.dirname(HERE))
THREAD_STARTERS = {"parallel", "target", "teams"}
DATASET = "-DMEDIUM_DATASET"


def norm(line):
    return " ".join(line.split())


def ignorable(line):
    s = norm(line)
    return (s == "" or s.startswith("//") or (s.startswith("/*") and s.endswith("*/"))
            or s in ("#pragma scop", "#pragma endscop", "#include <omp.h>"))


def is_pragma(line):
    return re.match(r"\s*#\s*pragma\s+omp\b", line) is not None


def directive_words(text):
    """`#pragma omp parallel for private(j)` -> ['parallel', 'for']."""
    body = re.sub(r"^\s*#\s*pragma\s+omp\s*", "", text.replace("\\\n", " "))
    words = []
    for w in re.findall(r"[A-Za-z_]+\(?", body):
        if w.endswith("(") or w in ("if", "private", "shared", "reduction"):
            break
        words.append(w)
    return words


class Pragma:
    def __init__(self, raw_lines, at, seq):
        self.raw = raw_lines            # lines as written (continuations kept)
        self.seq = seq                  # position among the candidate's pragmas
        self.text = norm(" ".join(l.rstrip("\\") for l in raw_lines))
        self.at = at                    # insert before this 0-based original line
        self.words = directive_words("\n".join(raw_lines))
        self.stmt = None                # (first token, start line, end line)

    @property
    def head(self):
        return bool(self.words) and self.words[0] in THREAD_STARTERS


def extract_pragmas(orig, cand):
    """Pragmas cand inserts into orig, or None when cand changes anything
    else. orig, cand: lists of lines."""
    sm = difflib.SequenceMatcher(None, [norm(l) for l in orig], [norm(l) for l in cand],
                                 autojunk=False)
    out = []
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        if not all(ignorable(l) for l in orig[i1:i2]):
            return None
        j = j1
        while j < j2:
            line = cand[j]
            if is_pragma(line):
                raw = [line]
                while raw[-1].rstrip().endswith("\\") and j + 1 < j2:
                    j += 1
                    raw.append(cand[j])
                out.append(Pragma(raw, i1, len(out)))
            elif not ignorable(line):
                return None
            j += 1
    return out


def next_code_line(orig, idx):
    while idx < len(orig) and (ignorable(orig[idx]) or orig[idx].lstrip().startswith("#")):
        idx += 1
    return idx + 1  # 1-based


def group_pragmas(src, orig, pragmas):
    for p in pragmas:
        try:
            p.stmt = src.stmt_at(next_code_line(orig, p.at))
        except ValueError:
            p.stmt = None
    heads = [p for p in pragmas if p.head and p.stmt]
    groups = {id(h): [h] for h in heads}
    order = []
    for p in pragmas:
        if p.head and p.stmt:
            order.append(id(p))
            continue
        line = p.stmt[1] if p.stmt else None
        best = None
        for h in heads:
            if line is not None and h.stmt[1] <= line <= h.stmt[2] and h.at <= p.at:
                if best is None or h.stmt[2] - h.stmt[1] <= best.stmt[2] - best.stmt[1]:
                    best = h
        if best is None:
            groups[id(p)] = [p]
            order.append(id(p))
        else:
            groups[id(best)].append(p)
    return [groups[k] for k in order]


def group_loops(group):
    """Lines of the loops a group distributes: the head's statement if it is
    a loop, else those of its `for`-construct members."""
    h = group[0]
    if h.stmt and h.stmt[0] == "for":
        return [h.stmt[1]]
    return [p.stmt[1] for p in group[1:] if p.stmt and p.stmt[0] == "for" and "for" in p.words]


def insert(orig, pragmas):
    """orig with the pragmas inserted, in the candidate's order."""
    at = {}
    for p in sorted(pragmas, key=lambda p: p.seq):
        at.setdefault(p.at, []).extend(p.raw)
    lines = list(orig)
    for idx in sorted(at, reverse=True):
        lines[idx:idx] = at[idx]
    return "\n".join(lines)


def tolerant(pragmas):
    """Reductions, atomic updates and critical sections reassociate
    floating-point sums."""
    return any(re.search(r"\b(reduction|atomic|critical)\b", p.text) for p in pragmas)


def not_parallel(group):
    """Why the group can't run its loop on several threads, or ""."""
    if not group[0].head:
        return "no team: " + " ".join(group[0].words)
    for p in group:
        if re.search(r"\bnum_threads\s*\(\s*1\s*\)", p.text):
            return "num_threads(1)"
        if re.search(r"\bif\s*\(\s*(\w+\s*:\s*)?(0|false)\s*\)", p.text):
            return "if(false)"
    return ""


def nested_in(outer, groups):
    """Indices of the groups whose head lies inside outer's statement."""
    h = outer[0]
    if not h.stmt:
        return []
    return [i for i, g in enumerate(groups) if g is not outer and g[0].stmt
            and h.stmt[1] <= g[0].stmt[1] <= h.stmt[2] and g[0].seq > h.seq]


# --- building and running --------------------------------------------------

class Env:
    def __init__(self, args):
        self.args = args
        self.pb = os.path.abspath(args.polybench)
        # Per platform: a mounted checkout may be checked on macOS and Linux.
        self.work = os.path.join(HERE, "work", "check-" + platform.system().lower())
        os.makedirs(self.work, exist_ok=True)
        self.cc = args.cc
        self.darwin = platform.system() == "Darwin"
        self.sysroot = []
        self.ldflags = ["-lm"]
        if self.darwin:
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            self.sysroot = ["-isysroot", sdk]
            self.ldflags += ["-L/opt/homebrew/opt/libomp/lib", "-L/opt/homebrew/opt/llvm/lib",
                             "-Wl,-rpath,/opt/homebrew/opt/llvm/lib"]
        self.seq = {}
        self.patient = False            # second pass: alone, longer limits
        if args.archer:
            self.probe_archer()

    def probe_archer(self):
        """Stop unless --archer-lib is Archer: a parallel loop must print its
        banner."""
        d = os.path.join(self.work, "probe")
        os.makedirs(d, exist_ok=True)
        c, exe = os.path.join(d, "probe.c"), os.path.join(d, "probe")
        open(c, "w").write("int main(void) { int s = 0;\n"
                           "#pragma omp parallel for reduction(+:s)\n"
                           "for (int i = 0; i < 100; i++) s += i;\n"
                           "return s != 4950; }\n")
        subprocess.run([self.cc, "-fopenmp", "-fsanitize=thread", "-g", c, "-o", exe]
                       + self.sysroot + self.ldflags, check=True)
        rc, out, _ = self.run(exe, 4, 120, {"OMP_TOOL_LIBRARIES": self.args.archer_lib,
                                             "ARCHER_OPTIONS": "verbose=1",
                                             "TSAN_OPTIONS": "ignore_noninstrumented_modules=1"},
                              both=True)
        if rc != 0 or ARCHER_BANNER not in out:
            sys.exit(f"--archer-lib {self.args.archer_lib} is not a working Archer (exit {rc})")

    def headers(self, kernel_path):
        """Working copy of the kernel's directory headers, printing exact
        hex floats."""
        d = os.path.join(self.work, "hdr", os.path.basename(os.path.dirname(kernel_path)))
        if not os.path.isdir(d):
            os.makedirs(d)
            src_dir = os.path.join(self.pb, os.path.dirname(kernel_path))
            for f in os.listdir(src_dir):
                if f.endswith(".h"):
                    t = open(os.path.join(src_dir, f)).read()
                    t = t.replace('"%0.2lf "', '"%a "').replace('"%0.2f "', '"%a "')
                    open(os.path.join(d, f), "w").write(t)
        return d

    def cflags(self, kernel_path, tsan):
        opt = ["-fsanitize=thread", "-g", "-O1"] if tsan else ["-O2"]
        return (opt + ["-fopenmp", DATASET, "-DPOLYBENCH_DUMP_ARRAYS",
                       "-I", self.headers(kernel_path), "-I", os.path.join(self.pb, "utilities")]
                + self.sysroot)

    def build(self, kernel_path, source, tsan):
        h = hashlib.sha1((source + str(tsan) + kernel_path).encode()).hexdigest()[:16]
        d = os.path.join(self.work, "bin", h)
        exe = os.path.join(d, "a.out")
        if os.path.exists(exe):
            return exe, ""
        tmp = d + ".tmp%d" % os.getpid()
        os.makedirs(tmp, exist_ok=True)
        c = os.path.join(tmp, os.path.basename(kernel_path))
        open(c, "w").write(source)
        cmd = ([self.cc] + self.cflags(kernel_path, tsan)
               + [c, os.path.join(self.pb, "utilities", "polybench.c"), "-o",
                  os.path.join(tmp, "a.out")] + self.ldflags)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            err = [l for l in r.stderr.splitlines() if "error" in l]
            shutil.rmtree(tmp, ignore_errors=True)
            return None, (err[0] if err else r.stderr.strip()[:200])
        try:
            os.rename(tmp, d)
        except OSError:  # another worker built the same source
            shutil.rmtree(tmp, ignore_errors=True)
        return exe, ""

    def run(self, exe, threads, timeout, extra_env=None, both=False):
        """(exit code or "timeout", stderr (+ stdout when both), seconds)."""
        env = dict(os.environ, OMP_NUM_THREADS=str(threads), OMP_DYNAMIC="false",
                   OMP_WAIT_POLICY="passive")
        env.update(extra_env or {})
        t0 = time.time()
        try:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout, env=env)
        except subprocess.TimeoutExpired:
            return "timeout", "", time.time() - t0
        return r.returncode, r.stderr + (r.stdout if both else ""), time.time() - t0

    def sequential(self, kernel_path, orig_text):
        if kernel_path not in self.seq:
            exe, err = self.build(kernel_path, orig_text, False)
            if exe is None:
                sys.exit(f"{kernel_path}: original does not compile: {err}")
            rc, out, secs = self.run(exe, 1, 3600)
            if rc != 0:
                sys.exit(f"{kernel_path}: original exits {rc}")
            self.seq[kernel_path] = (out, secs)
        return self.seq[kernel_path]


def _number(x):
    try:
        return float.fromhex(x) if "0x" in x.lower() else float(x)
    except ValueError:
        return None


def numbers_close(a, b):
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        fx, fy = _number(x), _number(y)
        if fx is None or fy is None or not (math.isfinite(fx) and math.isfinite(fy)):
            return False
        if abs(fx - fy) > 1e-6 * max(abs(fx), abs(fy)) + 1e-12:
            return False
    return True


def same_shape(a, b):
    """Same tokens but for finite numbers: a difference only rounding could
    explain (no NaN or Inf appearing, no text changing)."""
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        fx, fy = _number(x), _number(y)
        if fx is None or fy is None or not (math.isfinite(fx) and math.isfinite(fy)):
            return False
    return True


def check_variant(env, kernel_path, orig_text, source, tolerant):
    """compiles, output, archer, detail."""
    args = env.args
    exe, err = env.build(kernel_path, source, False)
    if exe is None:
        return False, "na", "na", err
    want, secs = env.sequential(kernel_path, orig_text)
    timeout = 900.0 if env.patient else max(60.0, 30 * secs)
    output, detail = "ok", ""
    for t in args.threads:
        for _ in range(args.runs):
            rc, got, _ = env.run(exe, t, timeout)
            if rc == "timeout":
                output, detail = "timeout", f"{t} threads: no exit in {timeout:.0f}s"
            elif rc != 0:
                output, detail = "crash", f"{t} threads: exit {rc}"
            elif got != want and not (tolerant and numbers_close(got, want)):
                if tolerant and same_shape(got, want):
                    output, detail = "fp-mismatch", f"{t} threads: beyond the tolerance"
                else:
                    output, detail = "mismatch", f"{t} threads"
            if output != "ok":
                break
        if output != "ok":
            break
    archer = "na"
    if args.archer:
        texe, terr = env.build(kernel_path, source, True)
        if texe is None:
            archer, detail = "crash", detail or ("tsan build: " + terr)
        else:
            log = os.path.join(os.path.dirname(texe), "tsan")
            for f in os.listdir(os.path.dirname(texe)):
                if f.startswith("tsan."):
                    os.remove(os.path.join(os.path.dirname(texe), f))
            rc, err, _ = env.run(texe, 4, 3600.0 if env.patient else max(600.0, 300 * secs), {
                "OMP_TOOL_LIBRARIES": args.archer_lib, "ARCHER_OPTIONS": "verbose=1",
                "TSAN_OPTIONS": f"ignore_noninstrumented_modules=1 log_path={log}"}, both=True)
            # No banner is fine when the run never started the OpenMP runtime
            # (an orphaned `for`): then there is nothing to report either.
            reports = "".join(open(os.path.join(os.path.dirname(texe), f)).read()
                              for f in os.listdir(os.path.dirname(texe)) if f.startswith("tsan."))
            # A run killed by the timeout leaves its race logs but no output to
            # show the banner; the probe already showed the library is Archer.
            if ("ThreadSanitizer: data race" in reports and rc != "timeout"
                    and ARCHER_BANNER not in err):
                sys.exit(f"TSan reported races without Archer loaded ({args.archer_lib})")
            if "ThreadSanitizer: data race" in reports:
                archer = "race"
                m = re.search(r"#0 \S+ (\S+:\d+)", reports)
                detail = detail or ("race at " + (m.group(1).rsplit("/", 1)[-1] if m else "?"))
            elif rc == "timeout":
                archer = "timeout"
            elif rc not in (0,):
                archer = "crash"
                detail = detail or f"archer run: exit {rc}"
            else:
                archer = "clean"
    return True, output, archer, detail


def verdict(compiles, output, archer):
    if not compiles:
        return "no-compile"
    if output in ("mismatch", "crash") or archer in ("race", "crash"):
        return "wrong"
    if output == "timeout" or archer == "timeout":
        return "timeout"
    if output == "fp-mismatch":
        return "fp-sensitive"
    return "correct"


# --- P05's own verdicts and output ----------------------------------------

def p05_cmd(env, kernel_path):
    return ["--", DATASET, "-I", env.headers(kernel_path),
            "-I", os.path.join(env.pb, "utilities")] + env.sysroot


def p05_verdicts(env, kernel_path):
    name = os.path.splitext(os.path.basename(kernel_path))[0]
    cache = os.path.join(HERE, "outputs", "p05", "verdicts", name + ".json")
    if not os.path.exists(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        tmp = os.path.join(env.work, name + ".jsonl")
        if os.path.exists(tmp):
            os.remove(tmp)
        subprocess.run([env.args.p05tool, "-reasons=" + tmp, os.path.join(env.pb, kernel_path)]
                       + p05_cmd(env, kernel_path), capture_output=True, check=True)
        rows = {}
        for l in open(tmp):
            r = json.loads(l)
            rows[str(r["line"])] = {"function": r["function"], "safety": r["safety"],
                                    "target": r["target"],
                                    "categories": sorted({x["category"] for x in r["reasons"]})}
        json.dump(rows, open(cache, "w"), indent=1, sort_keys=True)
    return json.load(open(cache))


def make_p05(env, kernels):
    out = os.path.join(HERE, "outputs", "p05")
    os.makedirs(out, exist_ok=True)
    for kp in kernels:
        name = os.path.splitext(os.path.basename(kp))[0]
        subprocess.run([env.args.p05tool, "-rewrite", "-policy", "all-cpu", "-o",
                        os.path.join(out, name + ".0.c"), os.path.join(env.pb, kp)]
                       + p05_cmd(env, kp), capture_output=True, check=True)
        p05_verdicts(env, kp)
        print("p05", name, flush=True)


# --- driver -----------------------------------------------------------------

FIELDS = ["tool", "kernel", "sample", "group", "nested", "loop_line", "function", "in_kernel",
          "pragma", "compiles", "output", "archer", "verdict", "p05_safety", "p05_target",
          "p05_categories", "detail"]


def strip_if(pragmas):
    """--strip-if: drop every if() clause, so a guarded loop has to be right
    when it does run in parallel."""
    for p in pragmas:
        out = []
        for line in p.raw:
            m = re.search(r"\bif\s*\(", line)
            while m:
                i, depth = m.end(), 1
                while depth:
                    depth += {"(": 1, ")": -1}.get(line[i], 0)
                    i += 1
                line = line[:m.start()] + line[i:]
                m = re.search(r"\bif\s*\(", line)
            out.append(line)
        p.raw = out
        p.text = norm(" ".join(l.rstrip("\\") for l in out))


def sample_outcome(cand_path):
    """run_llm.py's outcome for the reply (complete, truncated, blocked, ...),
    "complete" when there is no record of one (P05, OMPar)."""
    meta = cand_path[:-2] + ".json"
    if os.path.exists(meta):
        return json.load(open(meta)).get("outcome", "complete")
    return "complete"


def tasks_for_sample(env, tool, kp, sample, cand_path):
    orig_text = open(os.path.join(env.pb, kp)).read()
    orig = orig_text.split("\n")
    cand_text = open(cand_path).read()
    cand = cand_text.split("\n")
    name = os.path.splitext(os.path.basename(kp))[0]
    base = {"tool": tool, "kernel": name, "sample": sample}
    outcome = sample_outcome(cand_path)
    if outcome != "complete":
        return [(dict(base, group="file", verdict=outcome, detail="the model's reply was " + outcome), None)]
    pragmas = extract_pragmas(orig, cand)
    if pragmas is None:
        return [(dict(base, group="file", verdict="modified",
                      detail="changes lines other than OpenMP pragmas"), None)]
    if env.args.strip_if:
        strip_if(pragmas)
    # A clause may call an OpenMP routine the reply declared by including omp.h.
    prefix = ["#include <omp.h>"] if re.search(r"^\s*#\s*include\s*<omp\.h>", cand_text, re.M) else []
    src = cscan.Source(orig_text)
    p05 = p05_verdicts(env, kp)
    groups = group_pragmas(src, orig, pragmas)
    out = []
    for g, group in enumerate(groups):
        loops = group_loops(group)
        line = loops[0] if loops else (group[0].stmt[1] if group[0].stmt else "")
        fn = src.function_at(line) if line else ""
        pv = p05.get(str(line), {})
        inner = nested_in(group, groups)
        row = dict(base, group=g, nested=" ".join(map(str, inner)), loop_line=line, function=fn,
                   in_kernel=fn.startswith("kernel_"),
                   pragma=" | ".join(p.text for p in group),
                   p05_safety=pv.get("safety", ""), p05_target=pv.get("target", ""),
                   p05_categories=" ".join(pv.get("categories", [])))
        why = not_parallel(group)
        if why:
            out.append((dict(row, verdict="not-parallel", detail=why), None))
            continue
        with_inner = group + [p for i in inner for p in groups[i]]
        out.append((row, (kp, orig_text, "\n".join(prefix + [insert(orig, with_inner)]),
                          tolerant(with_inner))))
    if pragmas:
        out.append((dict(base, group="file", pragma=f"{len(pragmas)} pragmas"),
                    (kp, orig_text, "\n".join(prefix + [insert(orig, pragmas)]), tolerant(pragmas))))
    else:
        out.append((dict(base, group="file", pragma="0 pragmas", verdict="none"), None))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", help="outputs/<tool>/ to check")
    ap.add_argument("--outputs", help="read the samples from here instead of outputs/<tool>/")
    ap.add_argument("--make-p05", action="store_true", help="write outputs/p05/ with P05 first")
    ap.add_argument("--polybench", default=os.path.join(HERE, "work", "polybench"))
    ap.add_argument("--kernels", nargs="*")
    ap.add_argument("--threads", default="2,8,16")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--archer", action="store_true")
    ap.add_argument("--archer-lib", default="/usr/lib/llvm-22/lib/libarcher.so")
    ap.add_argument("--cc", default=shutil.which("/opt/homebrew/opt/llvm/bin/clang") or "clang")
    ap.add_argument("--p05tool", default=os.environ.get("P05_TOOL", os.path.join(ROOT, "build", "p05tool")))
    ap.add_argument("--csv")
    ap.add_argument("--strip-if", action="store_true",
                    help="drop if() clauses first (P05's run-time guards: check the loops in parallel)")
    args = ap.parse_args()
    if args.archer and not os.path.exists(args.archer_lib):
        ap.error(f"--archer: {args.archer_lib} does not exist")
    args.threads = [int(t) for t in args.threads.split(",")]
    env = Env(args)

    kernels = open(os.path.join(HERE, "kernels.txt")).read().split()
    if args.kernels:
        kernels = [k for k in kernels if os.path.splitext(os.path.basename(k))[0] in args.kernels]
    if args.make_p05:
        make_p05(env, kernels)
        args.tool = args.tool or "p05"
    if not args.tool:
        ap.error("--tool or --make-p05")
    outdir = args.outputs or os.path.join(HERE, "outputs", args.tool)

    work = []
    for kp in kernels:
        name = os.path.splitext(os.path.basename(kp))[0]
        samples = sorted(f for f in os.listdir(outdir) if re.fullmatch(re.escape(name) + r"\.\d+\.c", f))
        for f in samples:
            work += tasks_for_sample(env, args.tool, kp, int(f.split(".")[-2]),
                                     os.path.join(outdir, f))
    # Sequential baselines first (their run time sets the timeouts).
    for kp in {t[0] for _, t in work if t}:
        env.sequential(kp, open(os.path.join(env.pb, kp)).read())

    cache = {}

    def run(item):
        row, t = item
        if t is None:
            return row
        key = hashlib.sha1((t[0] + t[2] + str(t[3])).encode()).hexdigest()
        if key not in cache:
            cache[key] = check_variant(env, *t)
        compiles, output, archer, detail = cache[key]
        return dict(row, compiles=compiles, output=output, archer=archer,
                    verdict=verdict(compiles, output, archer), detail=detail)

    csv_path = args.csv or os.path.join(HERE, "results", args.tool + (".archer" if args.archer else "") + ".csv")
    os.makedirs(os.path.dirname(os.path.abspath(csv_path)), exist_ok=True)
    # Distinct variants run in parallel; identical ones (same pragma in two
    # samples) are checked once.
    uniq = {}
    for row, t in work:
        if t:
            uniq.setdefault(hashlib.sha1((t[0] + t[2] + str(t[3])).encode()).hexdigest(), (row, t))
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for i, _ in enumerate(ex.map(run, uniq.values())):
            print(f"\r{i + 1}/{len(uniq)} variants", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)
    env.patient = True
    for key, item in uniq.items():
        if "timeout" in cache[key][1:3]:
            print("again, alone:", item[0]["kernel"], item[0]["group"], file=sys.stderr, flush=True)
            cache[key] = check_variant(env, *item[1])
    rows = [run(item) for item in work]
    # An outer group fails whenever a group nested in it does: blame the inner one.
    by_sample = {}
    for r in rows:
        if r["group"] != "file":
            by_sample.setdefault((r["kernel"], r["sample"]), {})[r["group"]] = r
    for groups in by_sample.values():
        for r in groups.values():
            inner = [groups[int(i)] for i in r.get("nested", "").split()]
            if r.get("verdict") == "wrong" and any(
                    i.get("verdict") in ("wrong", "fp-sensitive", "no-compile", "timeout") for i in inner):
                r["verdict"] = "nested-wrong"
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        w.writerows(rows)
    wrong = sum(r.get("verdict") == "wrong" for r in rows if r["group"] != "file")
    slow = sum(r.get("verdict") == "timeout" for r in rows if r["group"] != "file")
    print(f"{csv_path}: {sum(r['group'] != 'file' for r in rows)} groups, {wrong} wrong, {slow} timeout")


if __name__ == "__main__":
    main()
