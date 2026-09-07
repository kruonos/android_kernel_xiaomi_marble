#!/usr/bin/env python3
"""Inventory literal Marble DT includes from Git without checking out files."""

import argparse
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="cd3f531d2230a22812f6c3f0ffe0148928d70d03")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]

    def git(*arguments):
        return subprocess.check_output(
            ["git", "-C", str(repo), *arguments], stderr=subprocess.PIPE
        )

    revision = git("rev-parse", "--verify", "--end-of-options",
                   args.ref + "^{commit}").decode().strip()
    base = "arch/arm64/boot/dts/vendor/qcom/"
    roots = [base + "ukee.dts", base + "marble-sm7475-pm8008-overlay.dts"]
    tree = git("ls-tree", "-r", "-z", revision)
    blobs = {}
    modes = {}
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        metadata, path = entry.split(b"\t", 1)
        mode, kind, oid = metadata.split()
        if kind == b"blob" and mode in (b"100644", b"100755", b"120000"):
            blobs[path.decode()] = oid.decode()
            modes[path.decode()] = mode

    files = {}
    unresolved = []
    pending = list(roots)
    # Preserve quoted include names while removing comments; do not evaluate CPP.
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*', re.S)
    directives = re.compile(
        r'^[ \t]*#[ \t]*include\b[ \t]*(?P<cpp>[^\n]*)'
        r'|/include/[ \t]*(?P<dts>"(?:\\.|[^"\\])*"|[^\n]*)'
        r'|"(?:\\.|[^"\\])*"', re.M
    )
    for root in roots:
        if root not in blobs:
            unresolved.append({"source": None, "include": root,
                               "reason": "missing DT root"})
    while pending:
        path = pending.pop()
        if path in files or path not in blobs:
            continue
        raw = git("cat-file", "blob", blobs[path])
        text = raw.decode()
        if modes[path] == b"120000":
            target = posixpath.normpath(posixpath.join(posixpath.dirname(path), text))
            files[path] = {"git_blob": blobs[path], "bytes": len(raw),
                           "symlink_target": text, "includes": [target]}
            chain = {path}
            endpoint = target
            while endpoint in blobs and modes[endpoint] == b"120000" and endpoint not in chain:
                chain.add(endpoint)
                link = git("cat-file", "blob", blobs[endpoint]).decode()
                endpoint = posixpath.normpath(posixpath.join(posixpath.dirname(endpoint), link))
            if endpoint not in blobs or endpoint in chain:
                unresolved.append({"source": path, "include": target,
                                   "reason": "missing or cyclic Git symlink target"})
            else:
                pending.append(target)
            continue
        text = re.sub(r'\\\r?\n', '', text)
        text = tokens.sub(
            lambda match: match.group() if match.group().startswith('"')
            else " " + "\n" * match.group().count("\n"), text
        )
        dependencies = []
        for directive in directives.finditer(text):
            value = directive.group("cpp")
            if value is None:
                value = directive.group("dts")
            if value is None:
                continue
            value = value.strip()
            literal = re.fullmatch(r'(?:"([^"\n]+)"|<([^>\n]+)>)\s*;?', value)
            if not literal:
                unresolved.append({"source": path, "include": value,
                                   "reason": "nonliteral or unsupported include"})
                continue
            include = literal.group(1) or literal.group(2)
            candidates = [
                posixpath.normpath(posixpath.join(posixpath.dirname(path), include)),
                "include/" + include,
                base + include,
            ]
            target = next((item for item in candidates if item in blobs), None)
            if target is None:
                unresolved.append({"source": path, "include": include,
                                   "reason": "include not found in scoped tree"})
                continue
            dependencies.append(target)
            pending.append(target)
        files[path] = {"git_blob": blobs[path], "bytes": len(raw),
                       "includes": sorted(set(dependencies))}

    report = {
        "revision": revision,
        "method": "Lexical literal-include inventory; conditional branches are not evaluated.",
        "limitations": "Not a CPP/DT build or driver-binding compatibility check; symlinks resolve only within the committed Git tree.",
        "roots": roots,
        "file_count": len(files),
        "source_bytes": sum(item["bytes"] for item in files.values()),
        "unresolved_includes": unresolved,
        "files": dict(sorted(files.items())),
    }
    print(json.dumps(report, indent=2))
    return 1 if unresolved else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, OSError, UnicodeError, ValueError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            detail = error.stderr.decode(errors="replace").strip()
        else:
            detail = str(error)
        print("DT inventory failed: " + detail, file=sys.stderr)
        sys.exit(2)
