#!/usr/bin/env python3
"""Verify the integration equals the Wi-Fi tree plus both preserved feature sets."""
import subprocess
from pathlib import Path

stage = Path(__file__).resolve().parent
workspace = stage.parent
repo = workspace / "CSI-PATCHED/android_kernel_xiaomi_marble"


def git(*args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def tree(ref):
    result = {}
    for record in git("ls-tree", "-rz", ref).split(b"\0"):
        if record:
            meta, name = record.split(b"\t", 1)
            result[name] = meta
    return result


expected = tree("wifi/monitor-mode")
for ref in ("preserve/csi-20260907", "preserve/mglru-20260907"):
    selected = tree(ref)
    paths = git("diff", "--name-only", "-z", "6e5ec94f50e6", ref).split(b"\0")
    for path in filter(None, paths):
        if path in selected:
            expected[path] = selected[path]
        else:
            expected.pop(path, None)
actual = tree("HEAD")
if expected != actual:
    bad = sorted(name.decode() for name in expected.keys() | actual.keys()
                 if expected.get(name) != actual.get(name))
    raise SystemExit("Unexpected integration differences:\n" + "\n".join(bad))
if git("status", "--porcelain", "--untracked-files=normal").strip():
    raise SystemExit("Working tree is no longer clean")
print(f"PASS: all {len(actual)} integrated tracked paths match the expected feature union")
