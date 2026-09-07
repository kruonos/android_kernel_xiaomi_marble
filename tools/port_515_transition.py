#!/usr/bin/env python3
"""Check or apply the initial pinned 5.15 source transition in this checkout."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true",
                        help="apply only after the full worktree patch check passes")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    base = "cd3f531d2230a22812f6c3f0ffe0148928d70d03"
    target = "603278d412c33acb62666bf8e09ccfc0b8732165"

    def git(*arguments):
        return subprocess.check_output(
            ["git", "-C", str(repo), *arguments], text=True
        ).strip()

    if git("branch", "--show-current") != "port/waipio-5.15-20260907":
        raise RuntimeError("refusing transition outside the dedicated port branch")
    if git("rev-parse", "HEAD") != "2fa583b68477ed9572d2897465d3f6b15b6d7f07":
        raise RuntimeError("HEAD changed; reassess the source transition first")
    if shutil.disk_usage(repo).free < 20 * 1024**3:
        raise RuntimeError("at least 20 GiB free is required for this transition")
    allowed = {"build_bouquet.sh", "package_bouquet.sh",
               "consolidation/PORT_5.15_PLAN.md"}
    if set(git("diff", "--name-only").splitlines()) - allowed:
        raise RuntimeError("unexpected tracked changes; refusing source replacement")
    if git("diff", "--cached", "--name-only"):
        raise RuntimeError("staged changes must be reviewed before transition")
    inventory = json.loads(subprocess.check_output(
        [sys.executable, "-B", str(repo / "tools/port_515_dt_inventory.py")]
    ))
    if (inventory["revision"] != base or inventory["file_count"] != 69 or
            inventory["unresolved_includes"]):
        raise RuntimeError("frozen board inventory no longer matches the reviewed set")
    target_paths = set(git("ls-tree", "-r", "--name-only", target).splitlines())
    board = sorted(set(inventory["files"]) - target_paths)
    if len(board) != 35 or not all(path.endswith((".dts", ".dtsi")) for path in board):
        raise RuntimeError("unexpected retained board set")
    keep = board + [
        "drivers/pinctrl/qcom/pinctrl-cape.c",
        "drivers/pinctrl/qcom/pinctrl-cape.h",
        "drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-cape.c",
        "drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-cape.h",
        ".gitignore", "build_bouquet.sh", "package_bouquet.sh", "consolidation",
    ]
    command = ["git", "diff", "--binary", "--full-index", "--no-color",
               "--no-ext-diff", "--no-textconv", "--no-renames", base, target,
               "--", "."] + [":(top,exclude,literal)" + path for path in keep]
    print(json.dumps({"baseline": base, "target": target,
                      "retained_paths": keep, "apply_requested": args.apply}), flush=True)
    # Regenerate the patch as a stream; never save a source archive or patch copy.
    for check in ([True, False] if args.apply else [True]):
        with subprocess.Popen(command, cwd=repo, stdout=subprocess.PIPE) as diff:
            result = subprocess.run(
                ["git", "apply", *(["--check"] if check else []),
                 "--whitespace=nowarn", "-"], cwd=repo, stdin=diff.stdout
            )
            diff.stdout.close()
            diff_status = diff.wait()
        if result.returncode or diff_status:
            raise RuntimeError("source delta failed: check=%s apply=%d diff=%d" %
                               (check, result.returncode, diff_status))
        print("CHECK PASSED" if check else
              "SOURCE DELTA APPLIED; INDEX AND BRANCH TIPS UNCHANGED", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Source transition refused: " + str(error), file=sys.stderr)
        sys.exit(1)
