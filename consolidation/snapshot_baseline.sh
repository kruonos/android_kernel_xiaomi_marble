#!/usr/bin/env bash
set -euo pipefail
stage=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(dirname -- "$stage")
tree="$workspace/android_kernel_xiaomi_marble-bouquet"
export STAGE="$stage"

python3 -B - <<'PY'
import json
import os
from pathlib import Path
from inventory import inventory
stage = Path(os.environ['STAGE'])
tree = stage.parent / 'android_kernel_xiaomi_marble-bouquet'
expected = json.loads((stage / 'manifests/baseline.json').read_text())
actual = inventory(tree)
if actual != expected:
    raise SystemExit('STOP: baseline source changed since preservation; original retained')
with (stage / 'manifests/baseline-paths.nul').open('wb') as stream:
    for name in sorted(expected):
        stream.write(os.fsencode(name) + b'\0')
print('Exact baseline source inventory verified', flush=True)
PY

git -C "$tree" status --short > "$stage/logs/baseline-precommit-status.txt"
git -C "$tree" diff --stat
git -C "$tree" log --all --oneline -10
if git -C "$tree" rev-parse --verify HEAD >/dev/null 2>&1; then
    printf 'STOP: expected the original unborn baseline repository\n' >&2
    exit 1
fi
git -C "$tree" switch -c preserve/baseline-20260907
git -C "$tree" --literal-pathspecs add --force \
    --pathspec-from-file="$stage/manifests/baseline-paths.nul" --pathspec-file-nul
git -C "$tree" diff --cached --shortstat
git -C "$tree" -c gc.auto=0 commit \
    -m 'preserve: snapshot standalone Bouquet source before consolidation'
git -C "$tree" rev-parse HEAD > "$stage/manifests/baseline-commit.txt"
printf 'PASS: baseline source committed; no source files or build outputs removed\n'
