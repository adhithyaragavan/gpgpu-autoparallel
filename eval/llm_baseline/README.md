# LLM parallelizer baseline (Phase 2.5 item 4)

Do LLM parallelizers get loops wrong, and how often? Each tool adds OpenMP pragmas to the same
30 PolyBench/C 4.2.1 kernels, and every pragma goes through the same checks as P05's own.

| File | What it does |
|---|---|
| `fetch_polybench.sh` | clones PolyBench at a pinned commit into `work/` (not vendored) |
| `prompt.md`, `run_llm.py` | the fixed prompt; sends it to OpenAI or Gemini, keeps raw replies in `outputs/<model>/` |
| `ompar/` | OMPar works per loop on a GPU: `bridge.py` exports loops and applies its answers, `eval/colab/ompar.ipynb` runs it |
| `check.py` | checks each pragma group alone, and each whole file (its docstring has the exact rules) |
| `selftest.py` | plants wrong and right pragmas, and fails if `check.py` misjudges one |
| `summarize.py` | the results table |

## Running it

```sh
eval/llm_baseline/fetch_polybench.sh

# An LLM (the key stays in your shell; about 90 requests per model):
OPENAI_API_KEY=... python3 eval/llm_baseline/run_llm.py --provider openai --model <exact id>
GEMINI_API_KEY=... python3 eval/llm_baseline/run_llm.py --provider gemini --model <exact id>

# OMPar: run eval/colab/ompar.ipynb on a T4, then
python3 eval/llm_baseline/ompar/bridge.py apply results.jsonl

# Check (Linux container: Archer needs libarcher, which Homebrew's LLVM doesn't ship):
docker run --rm -v "$PWD":/p05 p05-linux bash -c 'cd /p05/eval/llm_baseline &&
  python3 selftest.py --archer --cc clang &&
  python3 check.py --tool <name> --archer --cc clang'
python3 eval/llm_baseline/summarize.py eval/llm_baseline/results/*.archer.csv
```

`check.py --make-p05` regenerates `outputs/p05/` and needs `build/p05tool`.

## What a result does and does not say

- **wrong** is evidence of a real defect: an output that differs (a NaN where the sequential run
  has a number counts), a crash, or a race Archer reported on that run.
- **correct** only means no defect showed on this input (MEDIUM dataset, the kernel's own data). It
  is not a proof, for any tool.
- **fp-sensitive** is a group that reassociates floating-point sums (reduction, `atomic`,
  `critical`) and misses the tolerance: in an ill-conditioned kernel that is what plain rounding
  does (gramschmidt's norms flip sign when the same sum runs in reverse, without OpenMP), so it is
  neither wrong nor correct, and each one is audited by hand.
- **nested-wrong** is an outer loop's group that fails only together with a wrong group nested
  inside it: the inner group carries the blame. Groups are checked with the pragmas nested inside
  them, because an inner `parallel for` makes its loop variable private and the outer pragma may
  rely on that.
- **not-parallel** groups can't run their loop on several threads (an orphaned `for`, `simd`, a
  `task` outside a team, `num_threads(1)`, a constant false `if`): they are not counted as
  parallelized loops.
- **timeout** counts as neither.
- A loop can be parallel-safe and still make the program slower: this table is about correctness.
  Speedups are Phase 3.
- Kernels read a fixed input, and overlapping arguments are never tried here. A pragma that is wrong
  only when two arguments overlap looks correct in this table, for every tool. P05's alias guards
  are checked with overlapping calls in `eval/runtime/alias_main.c`, and
  `check.py --tool p05 --strip-if` re-checks P05 with every run-time guard removed, so its loops run
  in parallel (58 of its 107 pragmas have one).
- OMPar is reported twice. `ompar` is the published code: its generator is called with
  `max_length=64`, which counts the input, and with the pinned transformers it raises an error for
  every loop longer than that, so it gives no pragma for 86 of the 198 loops its classifier marks
  parallel. `ompar-long` uses the same classifier and generator with 64 new tokens allowed.
