#!/usr/bin/env python3
"""Read preserved v4 boot metadata and compare module closure; never repack or flash."""

import json
from pathlib import Path
import struct
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / "out-5.15-probe"
    backup = root.parent / "bouquet-slot-b-backup-20260624-155601"
    build = json.loads((out / "full-build-report.json").read_text())
    if build.get("status") != "passed":
        raise RuntimeError("a successful full-build report is required")

    def align(value, page):
        return (value + page - 1) // page * page

    def normalize(name):
        return Path(name).name.removesuffix(".ko").replace("-", "_")

    def cpio_metadata(data):
        pos = 0
        metadata = {}
        modules = 0
        while pos + 110 <= len(data):
            if data[pos:pos + 6] not in (b"070701", b"070702"):
                raise RuntimeError("unsupported CPIO header at %d" % pos)
            fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
            size, namesize = fields[6], fields[11]
            if namesize < 1 or pos + 110 + namesize > len(data):
                raise RuntimeError("invalid CPIO name length")
            name = data[pos + 110:pos + 110 + namesize - 1].decode()
            start = align(pos + 110 + namesize, 4)
            end = start + size
            if end > len(data):
                raise RuntimeError("truncated CPIO member")
            if name == "TRAILER!!!":
                return metadata, modules
            modules += name.endswith(".ko")
            if name in ("lib/modules/modules.load", "lib/modules/modules.load.recovery",
                        "lib/modules/modules.blocklist", "lib/modules/modules.options"):
                metadata[name] = data[start:end].decode()
            pos = align(end, 4)
        raise RuntimeError("missing CPIO trailer")

    with (backup / "boot_b.img").open("rb") as stream:
        header = stream.read(1584)
    if header[:8] != b"ANDROID!" or struct.unpack_from("<I", header, 40)[0] != 4:
        raise RuntimeError("expected preserved boot v4")
    boot = {"version": 4, "kernel_bytes": struct.unpack_from("<I", header, 8)[0],
            "ramdisk_bytes": struct.unpack_from("<I", header, 12)[0],
            "page_size": 4096, "signature_bytes": struct.unpack_from("<I", header, 1580)[0]}

    with (backup / "vendor_boot_b.img").open("rb") as stream:
        header = stream.read(2128)
        if header[:8] != b"VNDRBOOT" or struct.unpack_from("<I", header, 8)[0] != 4:
            raise RuntimeError("expected preserved vendor_boot v4")
        page = struct.unpack_from("<I", header, 12)[0]
        ramdisk_size = struct.unpack_from("<I", header, 24)[0]
        header_size, dtb_size = struct.unpack_from("<II", header, 2096)
        table_size, count, entry_size, bootconfig_size = struct.unpack_from("<IIII", header, 2112)
        if page != 4096 or entry_size < 108 or count > 64 or count * entry_size > table_size:
            raise RuntimeError("unexpected vendor header/table layout")
        ramdisk_offset = align(header_size, page)
        dtb_offset = ramdisk_offset + align(ramdisk_size, page)
        table_offset = dtb_offset + align(dtb_size, page)
        stream.seek(table_offset)
        table = stream.read(table_size)
        if len(table) != table_size:
            raise RuntimeError("truncated vendor ramdisk table")
        fragments = []
        for index in range(count):
            offset = index * entry_size
            size, relative, kind = struct.unpack_from("<III", table, offset)
            name = table[offset + 12:offset + 44].split(b"\0", 1)[0].decode()
            if relative + size > ramdisk_size:
                raise RuntimeError("fragment exceeds vendor ramdisk region")
            stream.seek(ramdisk_offset + relative)
            packed = stream.read(size)
            if len(packed) != size:
                raise RuntimeError("truncated vendor ramdisk fragment")
            if packed[:4] != b"\x02\x21\x4c\x18":
                raise RuntimeError("unexpected preserved ramdisk compression")
            data = subprocess.run(["lz4", "-d", "-c"], input=packed,
                                  capture_output=True, check=True).stdout
            metadata, module_count = cpio_metadata(data)
            fragments.append({"name": name, "type": kind, "compressed_bytes": size,
                              "module_files": module_count, "metadata": metadata})
        stream.seek(table_offset + align(table_size, page))
        bootconfig = stream.read(bootconfig_size).decode(errors="replace")

    modules = {}
    for entry in build["modules"]:
        path = out / entry["path"]
        if path.is_symlink() or not path.resolve().is_relative_to(out):
            raise RuntimeError("redirected module path")
        text = subprocess.check_output(["modinfo", str(path)], text=True)
        fields = {}
        for line in text.splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                fields.setdefault(key, []).append(value.strip())
        name = normalize(path.name)
        if name in modules:
            raise RuntimeError("ambiguous module basename: " + name)
        modules[name] = {"path": entry["path"],
                         "depends": [normalize(x) for x in ",".join(fields.get("depends", [])).split(",") if x],
                         "softdep": fields.get("softdep", [])}
    builtins = {normalize(x) for x in (out / "modules.builtin").read_text().splitlines()}
    normal = [fragment for fragment in fragments if "lib/modules/modules.load" in fragment["metadata"]]
    if len(normal) != 1:
        raise RuntimeError("normal load-list location is ambiguous")
    lines = normal[0]["metadata"]["lib/modules/modules.load"].splitlines()
    names = list(dict.fromkeys(normalize(x.strip()) for x in lines if x.strip() and not x.startswith("#")))
    builtin_replacements = {}
    if "geni-dt" in build["completed_phases"] and "qcom_geni_se" in builtins:
        builtin_replacements["msm_geni_se"] = "qcom_geni_se"
    roots = [name for name in names if name in modules]
    inventory = json.loads((root / "port-5.15-sources.json").read_text())
    roots += [normalize(item["path"]) for item in inventory["boot_integration"]["provider_modules"]]
    active, done, order = set(), set(), []
    unresolved = []

    def visit(name):
        if name in builtins or name in done:
            return
        if name in active:
            raise RuntimeError("hard dependency cycle: " + name)
        if name not in modules:
            unresolved.append(name)
            return
        active.add(name)
        for dependency in modules[name]["depends"]:
            visit(dependency)
        active.remove(name)
        done.add(name)
        order.append(modules[name]["path"])

    for name in roots:
        visit(name)
    report = {
        "source": str(backup), "scope": "Preserved recovery images, not current-device state",
        "build_source_commit": build["source_commit"], "kernel_release": build["kernel_release"],
        "boot": boot, "vendor_boot": {"version": 4, "page_size": page,
        "dtb_bytes": dtb_size, "bootconfig": bootconfig, "fragments": fragments},
        "normal_load_entries": len(lines), "normal_unique_names": len(names),
        "matched_load_names": [name for name in names if name in modules],
        "resolved_builtin_replacements": builtin_replacements,
        "unmatched_load_names": [name for name in names if name not in modules and name not in builtins and name not in builtin_replacements],
        "hard_closure_order": order, "unresolved_hard_dependencies": sorted(set(unresolved)),
        "target_modules": modules,
        "limits": "Name matching and hard-dependency ordering are not a final modules.load plan. Renames, soft dependencies, DT suppliers, console layout, userspace and recovery requirements still need reconciliation. No image/package is written.",
    }
    destination = out / "boot-integration-report.json"
    if out.resolve() != out or destination.is_symlink():
        raise RuntimeError("redirected audit report")
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"normal_unique_names": len(names), "matched": len(report["matched_load_names"]),
                      "unmatched": report["unmatched_load_names"], "hard_closure_modules": len(order),
                      "unresolved_hard_dependencies": report["unresolved_hard_dependencies"]}, indent=2))
    return 1 if unresolved else 0


if __name__ == "__main__":
    raise SystemExit(main())
