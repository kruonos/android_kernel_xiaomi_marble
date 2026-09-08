#!/usr/bin/env python3
"""Migrate the frozen GENI layout and verify source/compiled hardware invariants."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "arch/arm64/boot/dts/vendor/qcom/waipio-qupv3.dtsi"
BASE = "2ee508c3d3dd"
OUT = ROOT / "out-5.15-probe"
REFERENCE = ROOT / "tools/port_515_geni_reference.json"


def property_value(block, name):
    match = re.search(r"^\t\t" + re.escape(name) + r"\s*=\s*([^;]+);", block, re.M)
    return match.group(1) if match else None


def transformed():
    old = subprocess.check_output(["git", "-C", str(ROOT), "show", BASE + ":" + SOURCE], text=True)
    matches = list(re.finditer(r"^\t(\w+):[^\n]+\{\n.*?^\t\};", old, re.M | re.S))
    blocks = {match.group(1): match.group() for match in matches}
    remaining = old
    for match in reversed(matches):
        remaining = remaining[:match.start()] + remaining[match.end():]
    remaining = re.sub(r"/\*.*?\*/|//[^\n]*", "", remaining, flags=re.S)
    remaining = re.sub(r"^#include[^\n]*", "", remaining, flags=re.M)
    if re.sub(r"\s+", "", remaining) != "&soc{};":
        raise RuntimeError("unrecognized content outside frozen GENI nodes")
    wrappers = ["qupv3_" + str(i) for i in range(3)]
    engines = {key: value for key, value in blocks.items() if key not in wrappers and not key.startswith("gpi_dma")}
    if len(blocks) != 52 or len(engines) != 46:
        raise RuntimeError("unexpected frozen GENI node set")
    groups = {key: [] for key in wrappers}
    assignment = {}
    for label, block in engines.items():
        match = re.fullmatch(r"<&(qupv3_[012])>", property_value(block, "qcom,wrapper-core").strip())
        if not match:
            raise RuntimeError("missing wrapper for " + label)
        wrapper = match.group(1)
        groups[wrapper].append(label)
        assignment[label] = wrapper
    output = [old[:old.index("&soc {")] + "&soc {\n"]
    for number, wrapper in enumerate(wrappers):
        block = blocks[wrapper]
        paths = re.findall(r"<([^>]+)>", property_value(block, "interconnects"))
        if len(paths) != 3:
            raise RuntimeError("unexpected frozen wrapper ICC paths")
        ahb = None
        children = []
        for label in groups[wrapper]:
            child = engines[label]
            clocks = re.findall(r"<([^>]+)>", property_value(child, "clocks"))
            if len(clocks) != 3:
                raise RuntimeError("unexpected engine clocks: " + label)
            pair = [" ".join(item.split()) for item in clocks[1:]]
            if ahb is not None and pair != ahb:
                raise RuntimeError("inconsistent wrapper AHB clocks")
            ahb = pair
            child = re.sub(r"^\t\tqcom,wrapper-core\s*=[^;]+;\n", "", child, flags=re.M)
            console = label == "qupv3_se7_2uart"
            if console:
                child = child.replace('"qcom,msm-geni-console"', '"qcom,geni-debug-uart"')
                child = child.replace('"se-clk", "m-ahb", "s-ahb"', '"se", "m-ahb", "s-ahb"')
            names = '"qup-core", "qup-config"' + ('' if console else ', "qup-memory"')
            routes = [paths[0], "&gem_noc MASTER_APPSS_PROC &config_noc SLAVE_QUP_" + str(number)]
            if not console:
                routes.append(paths[2])
            added = '\t\tinterconnect-names = ' + names + ';\n\t\tinterconnects = ' + ",\n\t\t\t".join("<" + route + ">" for route in routes) + ";\n"
            child = child[:-3] + added + "\t};"
            children.append("\n".join("\t" + line for line in child.splitlines()))
        for name in ("qcom,msm-bus,num-paths", "interconnect-names", "interconnects"):
            block = re.sub(r"^\t\t" + re.escape(name) + r"\s*=[^;]+;\n", "", block, flags=re.M)
        block = block.replace('"qcom,qupv3-geni-se"', '"qcom,geni-se-qup"')
        added = '\t\tclock-names = "m-ahb", "s-ahb";\n\t\tclocks = <' + ahb[0] + '>, <' + ahb[1] + '>;\n'
        added += '\t\t#address-cells = <1>;\n\t\t#size-cells = <1>;\n\t\tranges;\n\n'
        output.append(block[:-3] + added + "\n\n".join(children) + "\n\t};\n")
    output.extend("\n" + blocks["gpi_dma" + str(i)] + "\n" for i in range(3))
    output.append("};\n")
    return old, "\n".join(output), blocks, assignment


def tree(path):
    data = path.read_bytes()
    header = struct.unpack_from(">10I", data)
    if header[0] != 0xD00DFEED or header[1] > len(data):
        raise RuntimeError("invalid FDT")
    strings = data[header[3]:header[3] + header[8]]
    pos, end = header[2], header[2] + header[9]
    stack, nodes = [], {}
    while pos < end:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:
            stop = data.index(b"\0", pos)
            name = data[pos:stop].decode()
            parent = stack[-1] if stack else ""
            current = (parent.rstrip("/") + "/" + name) or "/"
            stack.append(current)
            nodes[current] = {}
            pos = (stop + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, offset = struct.unpack_from(">II", data, pos)
            pos += 8
            name = strings[offset:strings.index(b"\0", offset)].decode()
            nodes[stack[-1]][name] = data[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return nodes
        else:
            raise RuntimeError("invalid FDT token")
    raise RuntimeError("missing FDT end")


def cells(value):
    return list(struct.unpack(">" + "I" * (len(value) // 4), value))


def text(value):
    return value.rstrip(b"\0").decode()


def metadata(nodes, labels):
    phandles = {cells(props["phandle"])[0]: path for path, props in nodes.items() if "phandle" in props}

    def refs(value, count_property):
        values = cells(value)
        result = []
        while values:
            provider = phandles[values.pop(0)]
            count = cells(nodes[provider][count_property])[0] if count_property else 0
            result.append([provider, *values[:count]])
            del values[:count]
        return result

    result = {}
    reference_properties = {"clocks": "#clock-cells", "dmas": "#dma-cells",
                            "iommus": "#iommu-cells", "interrupts-extended": "#interrupt-cells",
                            "interconnects": "#interconnect-cells"}
    for label in labels:
        path = text(nodes["/__symbols__"][label])
        props = nodes[path]
        decoded = {}
        for name, value in props.items():
            if name in ("phandle", "linux,phandle", "qcom,wrapper-core"):
                continue
            if name in reference_properties:
                decoded[name] = refs(value, reference_properties[name])
            elif re.fullmatch(r"pinctrl-\d+", name):
                decoded[name] = refs(value, None)
            else:
                decoded[name] = value.hex()
        result[label] = {"path": path, "properties": decoded}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--migrate", action="store_true")
    parser.add_argument("--build", action="store_true", help="rebuild diagnostic DTs before checking")
    parser.add_argument("--export-reference", action="store_true",
                        help="promote the captured pre-migration metadata to a reproducible test fixture")
    args = parser.parse_args()
    old, new, blocks, assignment = transformed()
    source = ROOT / SOURCE
    if args.export_reference:
        reference = json.loads((OUT / "geni-reference.json").read_text())
        if reference["baseline"] != BASE or set(reference["nodes"]) != set(blocks) or REFERENCE.exists():
            raise RuntimeError("unexpected or already exported GENI reference")
        REFERENCE.write_text(json.dumps(reference, indent=2) + "\n")
        print("Exported hardware-invariant test fixture; no DTB or source-tree copy")
        return
    if args.migrate:
        if source.read_text() != old:
            raise RuntimeError("migration requires the exact frozen input")
        nodes = tree(OUT / "dt-probe/marble-composed.dtb")
        reference = {"baseline": BASE, "nodes": metadata(nodes, blocks)}
        if REFERENCE.exists():
            raise RuntimeError("refusing to overwrite GENI reference metadata")
        REFERENCE.write_text(json.dumps(reference, indent=2) + "\n")
        source.write_text(new)
        print("Migrated 46 engines under three wrappers; retained three root GPI nodes")
        return
    if source.read_text() != new:
        raise RuntimeError("GENI source differs from the reviewed transformation")
    if args.build:
        env = dict(os.environ, TMPDIR=str(ROOT / "consolidation/scratch/port-515"))
        for name in ("ukee", "marble-sm7475-pm8008-overlay"):
            cpp = subprocess.run(
                ["clang-18", "-E", "-nostdinc", "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
                 "-I", str(ROOT / "include"), "-I", str(ROOT / "arch/arm64/boot/dts/vendor/qcom"),
                 str(ROOT / "arch/arm64/boot/dts/vendor/qcom" / (name + ".dts"))],
                env=env, capture_output=True, check=True
            )
            subprocess.run([str(OUT / "scripts/dtc/dtc"), "-@", "-I", "dts", "-O", "dtb", "-o",
                            str(OUT / "dt-probe" / (name + ".dtb"))], input=cpp.stdout, env=env, check=True)
        subprocess.run(["fdtoverlay", "-i", str(OUT / "dt-probe/ukee.dtb"), "-o",
                        str(OUT / "dt-probe/marble-composed.dtb"),
                        str(OUT / "dt-probe/marble-sm7475-pm8008-overlay.dtb")], check=True)
    nodes = tree(OUT / "dt-probe/marble-composed.dtb")
    old_nodes = json.loads(REFERENCE.read_text())["nodes"]
    current = metadata(nodes, blocks)
    active = []
    used = set()
    for label, previous in old_nodes.items():
        now = current[label]
        permitted = {"compatible", "clock-names", "interconnects", "interconnect-names"} if label == "qupv3_se7_2uart" else set()
        if label.startswith("qupv3_") and label not in assignment:
            permitted |= {"compatible", "clock-names", "clocks", "interconnects", "interconnect-names",
                          "qcom,msm-bus,num-paths", "#address-cells", "#size-cells", "ranges"}
        if label in assignment:
            permitted |= {"interconnects", "interconnect-names"}
        before = {k: v for k, v in previous["properties"].items() if k not in permitted}
        after = {k: v for k, v in now["properties"].items() if k not in permitted}
        if before != after:
            raise RuntimeError("hardware property drift: " + label)
        props = nodes[now["path"]]
        if label in assignment:
            wrapper = assignment[label]
            parent = current[wrapper]["path"]
            if now["path"].rsplit("/", 1)[0] != parent:
                raise RuntimeError("incorrect GENI parent: " + label)
            names = text(props["interconnect-names"]).split("\0")
            expected_names = ["qup-core", "qup-config"] + ([] if label == "qupv3_se7_2uart" else ["qup-memory"])
            if names != expected_names:
                raise RuntimeError("incorrect ICC names: " + label)
            routes = now["properties"]["interconnects"]
            original_routes = old_nodes[wrapper]["properties"]["interconnects"]
            number = int(wrapper[-1])
            config_route = [[text(nodes["/__symbols__"]["gem_noc"]), 2],
                            [text(nodes["/__symbols__"]["config_noc"]), 544 + number]]
            expected_routes = original_routes[:2] + config_route
            if label != "qupv3_se7_2uart":
                expected_routes += original_routes[4:6]
            if routes != expected_routes:
                raise RuntimeError("incorrect ICC endpoints: " + label)
            if text(props.get("status", b"okay")) in ("ok", "okay"):
                address = cells(props["reg"])[0]
                if address in used:
                    raise RuntimeError("multiple enabled protocols on one serial engine")
                used.add(address)
                active.append(label)
        elif label.startswith("qupv3_"):
            if text(props["compatible"]) != "qcom,geni-se-qup" or text(props["clock-names"]).split("\0") != ["m-ahb", "s-ahb"]:
                raise RuntimeError("incorrect wrapper interface")
            if props["ranges"] or cells(props["#address-cells"]) != [1] or cells(props["#size-cells"]) != [1]:
                raise RuntimeError("wrapper address translation changed")
            child = next(name for name, parent in assignment.items() if parent == label)
            if now["properties"]["clocks"] != old_nodes[child]["properties"]["clocks"][1:]:
                raise RuntimeError("wrapper AHB clock mismatch")
    console = current["qupv3_se7_2uart"]["path"]
    if text(nodes[console]["compatible"]) != "qcom,geni-debug-uart" or text(nodes[console]["clock-names"]).split("\0")[0] != "se":
        raise RuntimeError("incorrect console interface")
    if text(nodes["/chosen"]["stdout-path"]) != "serial0:115200n8" or text(nodes["/aliases"]["serial0"]) != console:
        raise RuntimeError("console alias/stdout mismatch")
    if text(nodes["/aliases"]["hsuart0"]) != current["qupv3_se20_4uart"]["path"]:
        raise RuntimeError("HS UART numbering changed")
    for number, engine in enumerate((5, 9, 15, 16, 19, 17)):
        if text(nodes["/aliases"]["i2c" + str(number)]) != current["qupv3_se%d_i2c" % engine]["path"]:
            raise RuntimeError("I2C bus numbering changed")
    report = {"status": "passed", "engines": len(assignment), "active_engines": active,
              "wrappers": 3, "gpi_nodes": 3, "hardware_properties_preserved": True,
              "limits": "Static source/DT contract validation, not a hardware transfer or userspace path test."}
    (OUT / "geni-validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
