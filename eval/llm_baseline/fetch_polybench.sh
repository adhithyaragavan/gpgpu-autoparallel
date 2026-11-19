#!/usr/bin/env bash
# Phase 2.5 item 4: fetch PolyBench/C 4.2.1 at a pinned commit into
# eval/llm_baseline/work/polybench (gitignored; the suite is not vendored).
#
#   eval/llm_baseline/fetch_polybench.sh [dest]
set -euo pipefail
cd "$(dirname "$0")"
DEST=${1:-work/polybench}
URL=https://github.com/MatthiasJReisinger/PolyBenchC-4.2.1
COMMIT=3e872547cef7e5c9909422ef1e6af03cf4e56072
if [[ ! -d "$DEST/.git" ]]; then
  mkdir -p "$(dirname "$DEST")"
  git clone -q "$URL" "$DEST"
fi
git -C "$DEST" checkout -q "$COMMIT"
[[ "$(git -C "$DEST" rev-parse HEAD)" == "$COMMIT" ]] || { echo "wrong commit" >&2; exit 1; }
echo "$DEST at $COMMIT"
