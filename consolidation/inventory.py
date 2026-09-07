#!/usr/bin/env python3
"""Record every non-build source file before kernel consolidation."""
import hashlib
import json
import os
from pathlib import Path
import stat

BASE = Path(__file__).resolve().parent.parent
OUT = Path(__file__).resolve().parent / "manifests"
TREES = {
    "baseline": BASE / "android_kernel_xiaomi_marble-bouquet",
    "csi": BASE / "CSI-PATCHED/android_kernel_xiaomi_marble",
    "mglru": BASE / "MGLRU-EXPERIMENT/android_kernel_xiaomi_marble",
    "wifi": BASE / "WIFI-MONITOR-EXPERIMENT",
}


def inventory(root):
    entries = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        current = Path(directory)
        if current == root:
            dirs[:] = [name for name in dirs if name not in (".git", "out")]
            files = [name for name in files if name != ".git"]
        for name in list(dirs):
            if (current / name).is_symlink():
                files.append(name)
                dirs.remove(name)
        for name in files:
            path = current / name
            meta = path.lstat()
            item = {"mode": stat.S_IMODE(meta.st_mode)}
            if path.is_symlink():
                item.update(type="symlink", target=os.readlink(path))
            elif stat.S_ISREG(meta.st_mode):
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
                item.update(type="file", size=meta.st_size, sha256=digest)
            else:
                raise ValueError(f"Unexpected special file: {path}")
            entries[str(path.relative_to(root))] = item
    return entries


if __name__ == "__main__":
    manifests = {}
    for label, root in TREES.items():
        path = OUT / (label + ".json")
        if path.exists():
            raise SystemExit(f"Refusing to overwrite original inventory: {path}")
        entries = inventory(root)
        path.write_text(json.dumps(entries, sort_keys=True, indent=2) + "\n")
        manifests[label] = entries
        print(label, len(entries), "source files/links")
    reference = manifests["csi"]
    for label in ("baseline", "mglru", "wifi"):
        other = manifests[label]
        delta = {"only_in_tree": sorted(other.keys() - reference.keys()),
                 "only_in_csi": sorted(reference.keys() - other.keys()),
                 "different": sorted(name for name in reference.keys() & other.keys()
                                     if reference[name] != other[name])}
        (OUT / (label + "-vs-csi.json")).write_text(json.dumps(delta, indent=2) + "\n")
        print(label, {key: len(value) for key, value in delta.items()})
