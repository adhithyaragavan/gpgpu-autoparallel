#!/usr/bin/env python3
"""Phase 2.5 item 4: ask an LLM to parallelize each PolyBench kernel file
with the fixed prompt in prompt.md, and save its answers for check.py.

    OPENAI_API_KEY=... run_llm.py --provider openai --model <exact id>
    GEMINI_API_KEY=... run_llm.py --provider gemini --model <exact id>
    GROQ_API_KEY=...   run_llm.py --provider groq --model <exact id>

Writes outputs/<model, '/' as '_'>/<kernel>.<k>.c (the code block of reply k) and
<kernel>.<k>.json (request parameters, timestamp, outcome and the raw
response; check.py scores a truncated or blocked reply as that, not as a
rule violation), for
k = 0 .. --samples - 1. Existing outputs are kept, so a run can be resumed.
The key is read from the environment only and is never written anywhere.
--dry-run prints the first request instead of sending anything.
No sampling parameters are sent unless given: the provider's defaults are
used and recorded as "default".
"""
import argparse
import datetime
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# Chat-completions endpoints, and the variable holding each one's key.
OPENAI_COMPATIBLE = {
    "openai": ("https://api.openai.com/v1/chat/completions", "OPENAI_API_KEY"),
    "groq": ("https://api.groq.com/openai/v1/chat/completions", "GROQ_API_KEY"),
}


def request_for(provider, model, prompt, temperature, max_tokens=None):
    if provider in OPENAI_COMPATIBLE:
        body = {"model": model, "messages": [{"role": "user", "content": prompt}]}
        if temperature is not None:
            body["temperature"] = temperature
        if max_tokens is not None:
            body["max_completion_tokens"] = max_tokens
        url, key_var = OPENAI_COMPATIBLE[provider]
        headers = {"Authorization": "Bearer " + os.environ.get(key_var, "")}
    else:
        body = {"contents": [{"role": "user", "parts": [{"text": prompt}]}]}
        config = {}
        if temperature is not None:
            config["temperature"] = temperature
        if max_tokens is not None:
            config["maxOutputTokens"] = max_tokens
        if config:
            body["generationConfig"] = config
        headers = {"x-goog-api-key": os.environ.get("GEMINI_API_KEY", "")}
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
    headers["Content-Type"] = "application/json"
    # Groq's front end refuses urllib's default User-Agent (Cloudflare 1010).
    headers["User-Agent"] = "p05-llm-baseline/1.0"
    return url, headers, body


def reply_text(provider, resp):
    """(text, outcome): outcome is complete, truncated (the reply hit the
    output limit) or blocked (no text: a safety or recitation stop)."""
    if provider in OPENAI_COMPATIBLE:
        choice = (resp.get("choices") or [{}])[0]
        text = (choice.get("message") or {}).get("content") or ""
        reason = choice.get("finish_reason")
        truncated = reason == "length"
    else:
        cand = (resp.get("candidates") or [{}])[0]
        text = "".join(p.get("text", "") for p in (cand.get("content") or {}).get("parts", []))
        reason = cand.get("finishReason")
        truncated = reason == "MAX_TOKENS"
    if truncated:
        return text, "truncated"
    if not text:
        return text, "blocked"
    return text, "complete"


def code_block(text):
    """The longest fenced code block (CRLF line ends allowed), else the
    whole reply."""
    text = text.replace("\r\n", "\n")
    blocks = re.findall(r"```[^\n`]*\n(.*?)```", text, re.S)
    return max(blocks, key=len) if blocks else text


def send(url, headers, body, attempts=6):
    data = json.dumps(body).encode()
    for a in range(attempts):
        try:
            req = urllib.request.Request(url, data=data, headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=600) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            msg = e.read().decode(errors="replace")[:500]
            if e.code in (429, 500, 502, 503, 504) and a + 1 < attempts:
                time.sleep(2 ** a * 5)
                continue
            sys.exit(f"HTTP {e.code}: {msg}")
        except urllib.error.URLError as e:
            if a + 1 < attempts:
                time.sleep(2 ** a * 5)
                continue
            sys.exit(f"network error: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provider", choices=["openai", "gemini", "groq"], required=True)
    ap.add_argument("--model", required=True, help="exact model id, recorded with each output")
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--temperature", type=float, default=None)
    ap.add_argument("--max-tokens", type=int, default=None,
                    help="output cap, reasoning included (Groq's default for gpt-oss-120b is 3072)")
    ap.add_argument("--polybench", default=os.path.join(HERE, "work", "polybench"))
    ap.add_argument("--kernels", nargs="*", help="kernel names (default: all of kernels.txt)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    key_var = OPENAI_COMPATIBLE[args.provider][1] if args.provider in OPENAI_COMPATIBLE else "GEMINI_API_KEY"
    if not args.dry_run and not os.environ.get(key_var):
        sys.exit(f"set {key_var} in the environment")
    template = open(os.path.join(HERE, "prompt.md")).read()
    paths = open(os.path.join(HERE, "kernels.txt")).read().split()
    if args.kernels:
        paths = [p for p in paths if os.path.splitext(os.path.basename(p))[0] in args.kernels]
    # A model id may contain '/' (openai/gpt-oss-120b): one directory per model.
    outdir = os.path.join(HERE, "outputs", args.model.replace("/", "_"))
    os.makedirs(outdir, exist_ok=True)

    for path in paths:
        name = os.path.splitext(os.path.basename(path))[0]
        src = open(os.path.join(args.polybench, path)).read()
        prompt = template.replace("{{SOURCE}}", src.rstrip("\n"))
        for k in range(args.samples):
            out_c = os.path.join(outdir, f"{name}.{k}.c")
            if os.path.exists(out_c):
                continue
            url, headers, body = request_for(args.provider, args.model, prompt, args.temperature,
                                             args.max_tokens)
            if args.dry_run:
                print(url)
                print(json.dumps(body, indent=1)[:3000])
                return
            started = datetime.datetime.now(datetime.timezone.utc).isoformat()
            resp = send(url, headers, body)
            text, outcome = reply_text(args.provider, resp)
            meta = {
                "provider": args.provider, "model": args.model, "kernel": path,
                "sample": k, "requested": started, "outcome": outcome,
                "temperature": "default" if args.temperature is None else args.temperature,
                "max_tokens": "default" if args.max_tokens is None else args.max_tokens,
                "response": resp,
            }
            with open(os.path.join(outdir, f"{name}.{k}.json"), "w") as f:
                json.dump(meta, f, indent=1)
            with open(out_c, "w") as f:
                f.write(code_block(text))
            print(f"{name}.{k}: {len(text)} chars, {outcome}", flush=True)


if __name__ == "__main__":
    main()
