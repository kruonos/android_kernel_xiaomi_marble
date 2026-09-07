#!/usr/bin/env bash
set -euo pipefail
stage=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(dirname -- "$stage")
repo="$workspace/CSI-PATCHED/android_kernel_xiaomi_marble"
main="$workspace/android_kernel_xiaomi_marble-bouquet"
export STAGE="$stage"
exec 9>"$stage/logs/retirement.lock"
flock -n 9 || exit 1

python3 -B verify_union.py
python3 -B - <<'PY'
import json
import os
from pathlib import Path
import subprocess
from inventory import inventory, TREES
stage = Path(os.environ['STAGE'])
repo = stage.parent / 'CSI-PATCHED/android_kernel_xiaomi_marble'
if (stage / 'logs/verify-archives-serial.status').read_text().strip() != 'PASS':
    raise SystemExit('Archive verification not complete')
saved = json.loads((stage / 'manifests/snapshot-verification.json').read_text())
refs = {'baseline': 'preserve/baseline-20260907', 'mglru': 'preserve/mglru-20260907',
        'csi': 'preserve/csi-20260907', 'wifi': 'wifi/monitor-mode'}
for label, ref in refs.items():
    oid = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', ref], text=True).strip()
    if oid != saved[label]['commit']:
        raise SystemExit('Preservation branch changed: ' + ref)
for label in ('baseline', 'mglru', 'wifi'):
    root = TREES[label]
    if root.is_symlink() or not root.is_dir():
        raise SystemExit('Unexpected source location: ' + str(root))
    expected = json.loads((stage / 'manifests' / (label + '.json')).read_text())
    if inventory(root) != expected:
        raise SystemExit('STOP: source changed after preservation: ' + label)
    print('PASS unchanged source: ' + label, flush=True)
print('PASS retirement preconditions: archives, refs, source bytes and feature union', flush=True)
PY

mkdir -p "$stage/releases" "$stage/legacy-entrypoints"
cp -p "$workspace/package_bouquet.sh" "$stage/legacy-entrypoints/package_bouquet.sh"
mv -- "$workspace/CSI-PATCHED/Bouquet_marble_release" "$stage/releases/csi"
mv -- "$workspace/MGLRU-EXPERIMENT/Bouquet_marble_release" "$stage/releases/mglru"

# Each original source and complete out/ directory has a compared, verified archive.
# Linked worktrees are retired through Git so their branch refs remain registered.
git -C "$repo" worktree remove --force "$workspace/WIFI-MONITOR-EXPERIMENT"
git -C "$repo" worktree remove --force "$workspace/MGLRU-EXPERIMENT/android_kernel_xiaomi_marble"
rm -rf -- "$main"
mv -T -- "$repo" "$main"
mv -T -- "$stage" "$main/consolidation"
stage="$main/consolidation"
rmdir -- "$workspace/CSI-PATCHED" "$workspace/MGLRU-EXPERIMENT"

for pair in \
    'QCA6490_MGMT_TX_FIRMWARE_MAP.md:qca6490-mgmt-tx-firmware-map.md' \
    'WIFI_MONITOR_EXPERIMENT_LIMITATIONS.md:wifi-monitor-experiment-limitations.md'; do
    link=${pair%%:*}
    document=${pair#*:}
    if [ -L "$workspace/$link" ]; then
        ln -sfn -- "android_kernel_xiaomi_marble-bouquet/Documentation/$document" "$workspace/$link"
    fi
done
git -C "$main" worktree list --porcelain > "$stage/manifests/final-worktrees.txt"
git -C "$main" fsck --full --no-dangling > "$stage/logs/final-git-fsck.log" 2>&1
printf 'PASS\n' > "$stage/logs/retirement.status"
printf 'PASS: one kernel checkout remains at %s\n' "$main"
du -sh "$main" "$stage/archives"
df -h "$main"
