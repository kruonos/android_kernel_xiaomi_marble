#!/usr/bin/env python3
"""Prove original source bytes remain in snapshot commits or verified archives."""
import hashlib
import json
from pathlib import Path
import subprocess

stage = Path(__file__).resolve().parent
workspace = stage.parent.parent if stage.name == "consolidation" else stage.parent
repo = (stage.parent if stage.name == "consolidation" else
        workspace / "CSI-PATCHED/android_kernel_xiaomi_marble")
refs = {"baseline": "preserve/baseline-20260907", "csi": "preserve/csi-20260907",
        "mglru": "preserve/mglru-20260907", "wifi": "wifi/monitor-mode"}


def git(*args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


if (stage / "logs/verify-archives-serial.status").read_text().strip() != "PASS":
    raise SystemExit("Archive-to-original verification has not passed")

actual_refs = dict(line.split(" ", 1)[::-1] for line in
                   git("for-each-ref", "--format=%(objectname) %(refname)").decode().splitlines())
for line in (stage / "manifests/preserved-refs.txt").read_text().splitlines():
    oid, ref = line.split(" ", 1)
    if actual_refs.get(ref) != oid:
        raise SystemExit(f"Original reference lost or changed: {ref}")

cat = subprocess.Popen(["git", "-C", str(repo), "cat-file", "--batch"],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE)
cache, report = {}, {}
try:
    for label, ref in refs.items():
        entries = {}
        for record in git("ls-tree", "-rz", "--full-tree", ref).split(b"\0"):
            if not record:
                continue
            info, name = record.split(b"\t", 1)
            mode, kind, oid = info.decode().split()
            if kind == "blob":
                entries[name.decode()] = oid
        original = json.loads((stage / "manifests" / (label + ".json")).read_text())
        archive_only, preserved = [], 0
        for name, expected in original.items():
            if name not in entries:
                archive_only.append(name)
                continue
            oid = entries[name]
            if oid not in cache:
                cat.stdin.write((oid + "\n").encode())
                cat.stdin.flush()
                fields = cat.stdout.readline().split()
                if len(fields) != 3 or fields[1] != b"blob":
                    raise ValueError(f"Missing Git blob: {oid}")
                remaining = int(fields[2])
                digest = hashlib.sha256()
                while remaining:
                    block = cat.stdout.read(min(remaining, 1024 * 1024))
                    if not block:
                        raise ValueError("Truncated Git object stream")
                    digest.update(block)
                    remaining -= len(block)
                if cat.stdout.read(1) != b"\n":
                    raise ValueError("Invalid Git batch framing")
                cache[oid] = digest.hexdigest()
            expected_hash = (expected["sha256"] if expected["type"] == "file" else
                             hashlib.sha256(expected["target"].encode()).hexdigest())
            if cache[oid] != expected_hash:
                raise ValueError(f"Snapshot content changed: {label}:{name}")
            preserved += 1
        report[label] = {"commit": git("rev-parse", ref).decode().strip(),
                         "files_verified_in_git": preserved,
                         "archive_only_paths": archive_only,
                         "archive": f"archives/{label}-source.tar.zst",
                         "full_permissions_preserved_in_archive": True}
        print(f"PASS {label}: {preserved} files/links in Git; {len(archive_only)} archive-only", flush=True)
finally:
    cat.stdin.close()
    cat.stdout.close()
    if cat.wait() != 0:
        raise SystemExit("Git object reader failed")
(stage / "manifests/snapshot-verification.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS: original refs and source bytes preserved", flush=True)
