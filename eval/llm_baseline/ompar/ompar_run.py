#!/usr/bin/env python3
"""Run OMPar (unmodified, commit pinned by the notebook) over loops.jsonl.

    ompar_run.py --model_weights <dir with model.bin> loops.jsonl results.jsonl

Run from the OMPar checkout. OMPar's gen_par calls generate(max_length=64),
and that limit includes the input: with the pinned transformers (4.44.1) a
loop of more than 64 tokens makes generate() raise ValueError, so OMPar as
published gives no pragma for it however good the model is (recorded as
`pragma: null` with the error in `published_error`). Each loop
therefore gets both answers: `pragma` from OMPar exactly as published, and
`pragma_long` with the same classifier decision and pragma_format but
max_new_tokens=64, decoding only the generated tokens. `tokens` is the
input's length, so the report can say how many loops the limit affected
(186 of the 333 loops have 64 or more).
"""
import argparse
import json

import torch
from compAI import OMPAR


def long_pragma(ompar, loop):
    inputs = ompar.tokenizer_gen(loop, return_tensors="pt").to(ompar.device)
    outputs = ompar.model_gen.generate(inputs["input_ids"], max_new_tokens=64)
    # Decode only the new tokens: gen_par's text[len(loop):] is off by one
    # character when decoding cleans up spaces in the loop (" ?").
    new = outputs[0][inputs["input_ids"].shape[-1]:]
    return ompar.pragma_format(ompar.tokenizer_gen.decode(new, skip_special_tokens=True))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vocab_file", default="tokenizer/gpt/gpt_vocab/gpt2-vocab.json")
    ap.add_argument("--merge_file", default="tokenizer/gpt/gpt_vocab/gpt2-merges.txt")
    ap.add_argument("--model_weights", required=True)
    ap.add_argument("loops")
    ap.add_argument("results")
    args = ap.parse_args()

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print("device:", device, flush=True)
    ompar = OMPAR(model_path=args.model_weights, device=device, args=args)
    rows = [json.loads(l) for l in open(args.loops)]
    with open(args.results, "w") as f, torch.no_grad():
        for i, r in enumerate(rows):
            loop = r["code"]
            parallel = bool(ompar.cls_par(loop))
            published, error = None, None
            if parallel:
                try:
                    published = ompar.pragma_format(ompar.gen_par(loop))
                except ValueError as e:  # the input is longer than max_length
                    error = str(e).split(".")[0]
            r2 = {"kernel": r["kernel"], "line": r["line"], "parallel": parallel,
                  "tokens": len(ompar.tokenizer_gen(loop)["input_ids"]),
                  "pragma": published, "published_error": error,
                  "pragma_long": long_pragma(ompar, loop) if parallel else None}
            f.write(json.dumps(r2) + "\n")
            f.flush()
            if i % 20 == 0:
                print(f"{i + 1}/{len(rows)} {r['kernel']}:{r['line']} -> {r2['pragma']}", flush=True)


if __name__ == "__main__":
    main()
