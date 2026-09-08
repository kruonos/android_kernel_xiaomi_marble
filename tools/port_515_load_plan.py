#!/usr/bin/env python3
"""Create a diagnostic-only module load plan with hard/soft dependency ordering."""

import fnmatch
import hashlib
import heapq
import json
from pathlib import Path
import subprocess

from port_515_geni import tree, text, cells


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / "out-5.15-probe"
    source = json.loads((root / "port-5.15-sources.json").read_text())
    build = json.loads((out / "full-build-report.json").read_text())
    audit = json.loads((out / "boot-integration-report.json").read_text())
    if build.get("status") != "passed" or audit["build_source_commit"] != build["source_commit"]:
        raise RuntimeError("a matching successful build and boot inventory are required")
    policy = source["boot_stabilization"]
    dispositions = policy["legacy_diagnostic_dispositions"]
    missing = set(audit["unmatched_load_names"]) - dispositions.keys()
    if missing:
        raise RuntimeError("unclassified baseline load entries: " + repr(sorted(missing)))
    modules = audit["target_modules"]
    normalize = lambda name: Path(name).name.removesuffix(".ko").replace("-", "_")
    builtins = {normalize(name) for name in (out / "modules.builtin").read_text().splitlines()}
    roots = list(audit["matched_load_names"])
    roots += [normalize(item["path"]) for item in source["boot_integration"]["provider_modules"]]
    roots += policy["additional_early_roots"]
    roots = list(dict.fromkeys(roots))

    def soft(name):
        result = {"pre": [], "post": []}
        for specification in modules[name]["softdep"]:
            direction = None
            for token in specification.split():
                if token in ("pre:", "post:"):
                    direction = token[:-1]
                elif direction:
                    result[direction].append(normalize(token))
                else:
                    raise RuntimeError("invalid softdep: " + specification)
        return result

    closure = set()
    pending = list(roots)
    while pending:
        name = pending.pop()
        if name in builtins or name in closure:
            continue
        if name not in modules:
            raise RuntimeError("missing required hard/soft module: " + name)
        closure.add(name)
        ordering = soft(name)
        pending += modules[name]["depends"] + ordering["pre"] + ordering["post"]
    edges = {name: set() for name in closure}
    for name in closure:
        ordering = soft(name)
        for dependency in modules[name]["depends"] + ordering["pre"]:
            if dependency in closure:
                edges[dependency].add(name)
        for dependent in ordering["post"]:
            if dependent in closure:
                edges[name].add(dependent)
    degree = {name: 0 for name in closure}
    for successors in edges.values():
        for successor in successors:
            degree[successor] += 1
    rank = {name: index for index, name in enumerate(roots)}
    ready = [(rank.get(name, -1), name) for name in closure if degree[name] == 0]
    heapq.heapify(ready)
    order = []
    while ready:
        _, name = heapq.heappop(ready)
        order.append(name)
        for successor in sorted(edges[name]):
            degree[successor] -= 1
            if degree[successor] == 0:
                heapq.heappush(ready, (rank.get(successor, -1), successor))
    if len(order) != len(closure):
        raise RuntimeError("hard/soft dependency ordering cycle")

    nodes = tree(out / "dt-probe/marble-composed.dtb")
    for node, module, compatible in (
        ("/soc/clock-controller@100000", "gcc_waipio", "qcom,cape-gcc"),
        ("/soc/clock-controller@af00000", "dispcc_waipio", "qcom,cape-dispcc"),
    ):
        if compatible not in text(nodes[node]["compatible"]).split("\0"):
            raise RuntimeError("unexpected active clock provider")
        aliases = subprocess.check_output(["modinfo", "-F", "alias", str(out / modules[module]["path"])], text=True).splitlines()
        if not any(fnmatch.fnmatchcase("of:NclockTnullC" + compatible, alias) for alias in aliases):
            raise RuntimeError("clock module does not bind active Cape DT: " + module)
    ufs = nodes["/soc/ufshc@1d84000"]
    clocks = text(ufs["clock-names"]).split("\0")
    rates = cells(ufs["freq-table-hz"])
    if len(rates) != len(clocks) * 2:
        raise RuntimeError("UFS clock table shape mismatch")
    for clock in ("core_clk", "core_clk_ice", "core_clk_unipro"):
        maximum = rates[clocks.index(clock) * 2 + 1]
        if maximum != policy["ufs_max_hz"]:
            raise RuntimeError("unexpected UFS bring-up maximum: " + clock)
    for prefix in ("axi", "ice", "unipro"):
        for suffix in ("turbo-clk-freq", "turbo-l1-clk-freq"):
            if cells(ufs[prefix + "-" + suffix]) != [policy["ufs_max_hz"]]:
                raise RuntimeError("UFS turbo setting bypasses bring-up cap")

    hashes = {item["path"]: item["sha256"] for item in build["modules"]}
    records = []
    for name in order:
        relative = modules[name]["path"]
        artifact = out / relative
        if artifact.is_symlink() or not artifact.resolve().is_relative_to(out):
            raise RuntimeError("redirected module artifact")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if digest != hashes[relative]:
            raise RuntimeError("module changed since successful build: " + name)
        records.append({"name": name, "path": relative, "sha256": digest})
    destination = out / "boot-plan"
    if destination.resolve() != destination:
        raise RuntimeError("redirected load-plan output")
    destination.mkdir(exist_ok=True)
    for name in ("modules.load", "plan.json"):
        if (destination / name).is_symlink():
            raise RuntimeError("redirected plan file")
    report = {"scope": "diagnostic normal boot candidate only", "source_commit": build["source_commit"],
              "kernel_release": build["kernel_release"], "module_count": len(records), "modules": records,
              "ordering": "hard and pre/post soft dependencies topologically checked",
              "baseline_dispositions": {name: dispositions[name] for name in audit["unmatched_load_names"]},
              "resolved_builtins": audit["resolved_builtin_replacements"], "ufs_max_hz": policy["ufs_max_hz"],
              "recovery_ready": False, "flash_ready": False,
              "limits": "No module copying or image/ZIP creation. DT supplier/runtime, deferred peripheral and userspace requirements are not proven by dependency closure."}
    (destination / "plan.json").write_text(json.dumps(report, indent=2) + "\n")
    (destination / "modules.load").write_text("\n".join(Path(item["path"]).name for item in records) + "\n")
    print(json.dumps({"module_count": len(records), "classified_baseline_omissions": len(audit["unmatched_load_names"]),
                      "cape_clock_matches": True, "ufs_max_hz": policy["ufs_max_hz"], "flash_ready": False}, indent=2))


if __name__ == "__main__":
    main()
