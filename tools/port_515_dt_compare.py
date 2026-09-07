#!/usr/bin/env python3
"""Compare frozen and current DT warnings using one compiler, without source copies."""

from collections import Counter
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / "out-5.15-probe"
    dtc = out / "scripts/dtc/dtc"
    if out.is_symlink() or out.resolve() != out or not dtc.is_file():
        raise RuntimeError("run isolated compile preparation before DT comparison")
    inventory = json.loads(subprocess.check_output(
        [sys.executable, "-B", str(root / "tools/port_515_dt_inventory.py")]
    ))
    if inventory["unresolved_includes"]:
        raise RuntimeError("unresolved frozen includes")
    env = dict(os.environ, TMPDIR=str(root / "consolidation/scratch/port-515"))
    for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
        env.pop(key, None)

    with ExitStack() as stack:
        fds = []

        def memory_file(data):
            fd = os.memfd_create("marble-dt-audit", os.MFD_CLOEXEC)
            stack.callback(os.close, fd)
            fds.append(fd)
            if os.write(fd, data) != len(data):
                raise RuntimeError("short write to anonymous memory file")
            os.lseek(fd, 0, os.SEEK_SET)
            return "/proc/self/fd/%d" % fd

        virtual_root = {"type": "directory", "name": str(root), "contents": []}
        directories = {(): virtual_root}
        contents = {}
        for path, entry in inventory["files"].items():
            resolved = path
            visited = set()
            while "symlink_target" in inventory["files"][resolved]:
                if resolved in visited:
                    raise RuntimeError("cyclic frozen symlink: " + path)
                visited.add(resolved)
                resolved = inventory["files"][resolved]["includes"][0]
            oid = inventory["files"][resolved]["git_blob"]
            if oid not in contents:
                contents[oid] = memory_file(subprocess.check_output(
                    ["git", "-C", str(root), "cat-file", "blob", oid]
                ))
            parts = PurePosixPath(path).parts
            for length in range(1, len(parts)):
                prefix = parts[:length]
                if prefix not in directories:
                    directory = {"type": "directory", "name": parts[length - 1],
                                 "contents": []}
                    directories[prefix] = directory
                    directories[prefix[:-1]]["contents"].append(directory)
            directories[parts[:-1]]["contents"].append(
                {"type": "file", "name": parts[-1], "external-contents": contents[oid]}
            )
        overlay = memory_file(json.dumps({
            "version": 0, "case-sensitive": True, "use-external-names": False,
            "fallthrough": False, "roots": [virtual_root],
        }).encode())
        results = {}
        for label in ("baseline", "current"):
            warnings = Counter()
            blobs = {}
            for source in inventory["roots"]:
                command = ["clang-18", "-E", "-nostdinc", "-undef", "-D__DTS__",
                           "-x", "assembler-with-cpp", "-I", str(root / "include"),
                           "-I", str(root / "arch/arm64/boot/dts/vendor/qcom")]
                if label == "baseline":
                    command.extend(["-ivfsoverlay", overlay])
                command.append(str(root / source))
                cpp = subprocess.run(command, cwd=root, env=env, pass_fds=tuple(fds),
                                     capture_output=True, check=True)
                if any(match.group(1) for match in re.finditer(
                        rb'"(?:\\.|[^"\\])*"|(/include/|/incbin/)', cpp.stdout)):
                    raise RuntimeError("unisolated DTC file inclusion in " + source)
                compiled = subprocess.run(
                    [str(dtc), "-@", "-I", "dts", "-O", "dtb", "-o", "-"],
                    input=cpp.stdout, cwd=root, env=env, capture_output=True, check=True
                )
                blobs[source] = hashlib.sha256(compiled.stdout).hexdigest()
                for match in re.finditer(r"Warning \(([^)]+)\): ([^\n]*)",
                                         compiled.stderr.decode(errors="replace")):
                    warnings[match.group(1) + ": " + match.group(2)] += 1
            results[label] = {"warnings": warnings, "dtb_sha256": blobs}

    baseline = results["baseline"]["warnings"]
    current = results["current"]["warnings"]
    added = current - baseline
    removed = baseline - current
    report = {
        "baseline_revision": inventory["revision"],
        "current_source": "uncommitted worktree",
        "method": "Same clang-18 and target-built DTC/flags for both. Frozen Git blobs supplied through anonymous memfds and a no-fallthrough LLVM VFS overlay; no baseline source copy.",
        "normalization": "Compare warning category/message multisets; ignore file/line location prefixes.",
        "dtc_sha256": hashlib.sha256(dtc.read_bytes()).hexdigest(),
        "baseline_warning_count": sum(baseline.values()),
        "current_warning_count": sum(current.values()),
        "added_warnings": dict(sorted(added.items())),
        "removed_warnings": dict(sorted(removed.items())),
        "baseline": results["baseline"], "current": results["current"],
        "limits": "This compares compiler diagnostics, not DT schemas, complete vendor overlays or runtime behavior. Inherited warnings are not thereby proven harmless.",
    }
    destination = out / "dt-warning-comparison.json"
    if destination.is_symlink():
        raise RuntimeError("refusing redirected comparison report")
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in (
        "baseline_warning_count", "current_warning_count", "added_warnings", "removed_warnings"
    )}, indent=2))
    return 1 if added else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print("DT comparison failed: " + str(error), file=sys.stderr)
        if error.stderr:
            print(error.stderr.decode(errors="replace"), file=sys.stderr)
        sys.exit(2)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print("DT comparison failed: " + str(error), file=sys.stderr)
        sys.exit(2)
