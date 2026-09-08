#!/usr/bin/env python3
"""Build an experimental diagnostic ZIP using the pinned Bouquet installer."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile

from port_515_geni import tree, text


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out-5.15-probe"
MUTABLE = {"Image.7z", "_dtb.7z", "_modules_hyperos.7z", "anykernel.sh", "local-build-manifest.txt"}


def digest(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            result.update(block)
    return result.hexdigest()


def replace_once(value, old, new):
    if value.count(old) != 1:
        raise RuntimeError("unexpected installer patch context: " + old[:90])
    return value.replace(old, new, 1)


def patch_installer(original, sha1, release):
    value = original.decode()
    value = replace_once(value, "kernel.string=Bouquet Kernel local rebuild by Pzqqt",
                         "kernel.string=Bouquet " + release + " EXPERIMENTAL DIAGNOSTIC")
    for key in ("STOCK", "KSU", "SUSFS"):
        value, count = re.subn(r'^SHA1_' + key + r'="[0-9a-f]{40}"$',
                              'SHA1_' + key + '="' + sha1 + '"', value, flags=re.M)
        if count != 1:
            raise RuntimeError("unexpected Image hash variable")
    value = replace_once(value, "if strings ${home}/kernelsu.ko | grep -q 'clang version 12.0.5'; then",
                         "if false; then # No inherited 5.10 KSU LKM is valid for this 5.15 build.")
    value = replace_once(value, "\t\t# TODO: chmod init\n", "\t\t${bin}/magiskboot cpio ${split_img}/ramdisk.cpio \"exists init.real\" || abort \"! Cannot safely remove old KSU: init.real is missing\"\n\t\t# Use the original incompatible-LKM cleanup path.\n")
    value = replace_once(value, "if ${is_hyperos_fw_with_newer_adsp2}; then\n\tcp -f ${home}/_alt/",
                         "if false && ${is_hyperos_fw_with_newer_adsp2}; then\n\tcp -f ${home}/_alt/")
    value = replace_once(value, "elif ${is_hyperos_fw_with_new_adsp2}; then\n\tcp -f ${home}/_alt/",
                         "elif false && ${is_hyperos_fw_with_new_adsp2}; then\n\tcp -f ${home}/_alt/")
    for prompt in ("_LANG_SELECT_REAL_BATTERY", "_LANG_SELECT_DISGUISED_ADRENO730"):
        old = 'if keycode_select \\\n    "$' + prompt + '"'
        value = replace_once(value, old, 'if false && keycode_select \\\n    "$' + prompt + '"')
    old = "if [ -f /vendor/etc/displayconfig/display_id_4630946370515662721.xml ] || [ -f /vendor/etc/displayconfig/display_id_4630946480857061761.xml ]; then"
    value = replace_once(value, old, "if false && { [ -f /vendor/etc/displayconfig/display_id_4630946370515662721.xml ] || [ -f /vendor/etc/displayconfig/display_id_4630946480857061761.xml ]; }; then")
    value = replace_once(value, 'if [ "$(sha1 $ukee_dtb)" != "$(sha1 ${home}/dtb)" ]; then',
                         'if false && [ "$(sha1 $ukee_dtb)" != "$(sha1 ${home}/dtb)" ]; then')
    warning = ('ui_print " " "EXPERIMENTAL 5.15 DIAGNOSTIC KERNEL - NOT BOOT TESTED"\n'
               'ui_print "Display/GPU, audio, camera and WLAN are not complete. Black screen or boot failure is possible."\n'
               'ui_print "Recovery UI is not validated. Have an external bootloader restore path and the preserved backups."\n'
               'ui_print "This keeps the Bouquet partition/vbmeta workflow and removes incompatible KSU LKM wrappers."\n\n')
    return replace_once(value, "# Check firmware\n", warning + "# Check firmware\n").encode()


def payload_metadata(archive, entries):
    fd = os.memfd_create("bouquet-payload-metadata", os.MFD_CLOEXEC)
    try:
        os.write(fd, archive)
        os.lseek(fd, 0, 0)
        filename = "/proc/self/fd/" + str(fd)
        listing = subprocess.check_output(["7z", "l", "-slt", "-t7z", filename], pass_fds=(fd,), text=True)
        names = set(re.findall(r"^Path = (.*)$", listing, re.M))
        return {name: subprocess.check_output(["7z", "e", "-so", "-t7z", filename, name], pass_fds=(fd,))
                for name in entries if name in names}
    finally:
        os.close(fd)


def allocated_sections(data):
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (1, 183):
        raise RuntimeError("module is not AArch64 relocatable ELF")
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    if size != 64 or not count or names_index >= count or offset + size * count > len(data):
        raise RuntimeError("unsupported module section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size) for i in range(count)]
    names_header = sections[names_index]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    result = {}
    for item in sections:
        name = names[item[0]:names.index(b"\0", item[0])].decode()
        if item[2] & 2 or name in (".BTF", ".BTF.ext", ".modinfo", "__versions"):
            result[name] = (item[1], item[5], None if item[1] == 8 else hashlib.sha256(data[item[4]:item[4] + item[5]]).hexdigest())
    if ".BTF" not in result or ".modinfo" not in result:
        raise RuntimeError("module lacks expected BTF/modinfo")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate inputs without creating a ZIP")
    args = parser.parse_args()
    config = json.loads((ROOT / "port-5.15-sources.json").read_text())
    reference = config["packaging_reference"]
    template = (ROOT / reference["primary_zip"]).resolve()
    build = json.loads((OUT / "full-build-report.json").read_text())
    plan = json.loads((OUT / "boot-plan/plan.json").read_text())
    if build.get("status") != "passed" or plan["source_commit"] != build["source_commit"] or plan["module_count"] != 95:
        raise RuntimeError("matching validated build and reviewed 95-module plan required")
    if subprocess.check_output(["git", "-C", str(ROOT), "branch", "--show-current"], text=True).strip() != "port/waipio-5.15-20260907":
        raise RuntimeError("wrong packaging branch")
    if not args.check and subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"]):
        raise RuntimeError("commit packaging sources before assembling the ZIP")
    subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet", build["source_commit"], "--",
                    "Makefile", "Kconfig", "arch", "drivers", "include", "kernel", "mm", "net", "fs",
                    "block", "crypto", "certs", "init", "ipc", "lib", "scripts", "security", "sound", "usr", "virt"], check=True)
    if digest(template) != reference["primary_sha256"]:
        raise RuntimeError("working ZIP reference hash mismatch")
    image = OUT / "arch/arm64/boot/Image"
    if digest(image) != build["image_sha256"] or digest(OUT / ".config") != build["config_sha256"]:
        raise RuntimeError("stale kernel/config payload")
    if shutil.disk_usage(ROOT).free < 2 * 1024**3:
        raise RuntimeError("less than 2 GiB free")
    if OUT.resolve() != OUT or (OUT / "port-build.lock").is_symlink():
        raise RuntimeError("redirected build output")
    lock = (OUT / "port-build.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    subprocess.run(["python3", "-B", str(ROOT / "tools/port_515_geni.py"), "--build"], check=True)
    subprocess.run(["python3", "-B", str(ROOT / "tools/port_515_load_plan.py")], check=True)
    plan = json.loads((OUT / "boot-plan/plan.json").read_text())
    module_hashes = {entry["path"]: entry["sha256"] for entry in build["modules"]}
    for entry in plan["modules"]:
        path = OUT / entry["path"]
        if path.is_symlink() or not path.resolve().is_relative_to(OUT) or digest(path) != module_hashes[entry["path"]]:
            raise RuntimeError("module provenance mismatch")
        if subprocess.check_output(["modinfo", "-F", "signer", str(path)], text=True).strip():
            raise RuntimeError("signed module cannot be stripped without re-signing")
    release = build["kernel_release"]
    sha1 = digest(image, "sha1")
    with zipfile.ZipFile(template) as archive:
        for name, expected in reference["critical_tool_sha256"].items():
            if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                raise RuntimeError("installer tool pin mismatch: " + name)
        installer = patch_installer(archive.read("anykernel.sh"), sha1, release)
        subprocess.run(["bash", "-n"], input=installer, check=True)
        shell_fd = os.memfd_create("pinned-bouquet-busybox", os.MFD_CLOEXEC)
        try:
            os.write(shell_fd, archive.read("tools/busybox"))
            os.fchmod(shell_fd, 0o755)
            os.lseek(shell_fd, 0, 0)
            subprocess.run(["busybox", "ash", "-n"], executable="/proc/self/fd/" + str(shell_fd),
                           input=installer, pass_fds=(shell_fd,), check=True)
        finally:
            os.close(shell_fd)
        original_dtbo = payload_metadata(archive.read("_dtb.7z"), ["dtbo.img"])["dtbo.img"]
        header = struct.unpack_from(">8I", original_dtbo)
        if header[0] != 0xD7B7AB1E or header[2:6] != (32, 32, 1, 32) or header[7] != 0:
            raise RuntimeError("unexpected reference DTBO layout")
        entry = struct.unpack_from(">8I", original_dtbo, 32)
        if entry[1] != 64 or header[1] != entry[1] + entry[0] or header[1] > len(original_dtbo):
            raise RuntimeError("unexpected reference DTBO offsets")
        footer = struct.unpack_from(">4sIIQQQ", original_dtbo, len(original_dtbo) - 64)
        if len(original_dtbo) != 24 * 1024 * 1024 or footer[:3] != (b"AVBf", 1, 0) or footer[3] != header[1]:
            raise RuntimeError("unexpected reference DTBO partition/AVB layout")
        vbmeta = original_dtbo[footer[4]:footer[4] + footer[5]]
        if vbmeta[:4] != b"AVB0" or struct.unpack_from(">I", vbmeta, 28)[0] != 0:
            raise RuntimeError("reference DTBO AVB algorithm is not the expected NONE")
        subprocess.run(["avbtool", "version"], check=True)
        overlay = OUT / "dt-probe/marble-sm7475-pm8008-overlay.dtb"
        dtb = OUT / "dt-probe/ukee.dtb"
        old_fd = os.memfd_create("reference-overlay", os.MFD_CLOEXEC)
        try:
            os.write(old_fd, original_dtbo[entry[1]:entry[1] + entry[0]])
            os.lseek(old_fd, 0, 0)
            old_tree = tree(Path("/proc/self/fd/" + str(old_fd)))
        finally:
            os.close(old_fd)
        new_tree = tree(overlay)
        for key in ("qcom,msm-id", "qcom,board-id"):
            if old_tree["/"].get(key) != new_tree["/"].get(key):
                raise RuntimeError("DTBO selection identity changed: " + key)
        if text(tree(dtb)["/"]["model"]) != "Qualcomm Technologies, Inc. Ukee SoC":
            raise RuntimeError("base DTB model does not match Bouquet selection")
        metadata_names = [group + "/" + name for group in ("_vendor_boot_modules", "_vendor_dlkm_modules")
                          for name in ("modules.blocklist", "modules.options", "vertmp")]
        baseline_metadata = payload_metadata(archive.read("_modules_hyperos.7z"), metadata_names)
        if args.check:
            print("Packaging preflight passed: pinned installer/ash syntax, 95 coherent modules and DTBO identity; assembly path not yet exercised")
            return 0
        package_dir = ROOT / "consolidation/packages"
        scratch = ROOT / "consolidation/scratch"
        if package_dir.resolve() != package_dir or scratch.resolve() != scratch:
            raise RuntimeError("redirected package directory")
        package_dir.mkdir(exist_ok=True)
        scratch.mkdir(exist_ok=True)
        name = "Bouquet-" + release.split("-")[0] + "-marble-diagnostic-r1.zip"
        destination = package_dir / name
        report_path = package_dir / (name + ".json")
        if destination.exists() or destination.is_symlink() or report_path.exists() or report_path.is_symlink():
            raise RuntimeError("refusing to overwrite a package or report")
        work = Path(tempfile.mkdtemp(prefix="package-515-", dir=scratch))
        print("Packaging staging: " + str(work), flush=True)
        jobs = str(len(os.sched_getaffinity(0)))
        decoder = work / "7za"
        decoder.write_bytes(archive.read("tools/7za"))
        decoder.chmod(0o755)
        ash = work / "busybox"
        ash.write_bytes(archive.read("tools/busybox"))
        ash.chmod(0o755)
        subprocess.run([str(ash), "ash", "-n"], input=installer, check=True)
        common = work / "dep/lib/modules" / release
        common.mkdir(parents=True)
        rows = []
        for item in plan["modules"]:
            source = OUT / item["path"]
            target = common / source.name
            subprocess.run(["llvm-strip-18", "-S", str(source), "-o", str(target)], check=True)
            if allocated_sections(source.read_bytes()) != allocated_sections(target.read_bytes()):
                raise RuntimeError("strip changed runtime/BTF/version data: " + source.name)
            vermagic = subprocess.check_output(["modinfo", "-F", "vermagic", str(target)], text=True).strip()
            if vermagic.split()[0] != release:
                raise RuntimeError("wrong stripped module release")
            rows.append({"file": source.name, "source": item["path"], "sha256": digest(target), "vermagic": vermagic})
        load = "\n".join(item["file"] for item in rows) + "\n"
        (common / "modules.order").write_text(load)
        for name in ("modules.builtin", "modules.builtin.modinfo"):
            shutil.copyfile(OUT / name, common / name)
        for flag, symbols in (("-F", "System.map"), ("-E", "Module.symvers")):
            checked = subprocess.run(["depmod", "-C", "/dev/null", "-b", str(work / "dep"), "-e", flag,
                                      str(OUT / symbols), release], capture_output=True, text=True, check=True)
            if checked.stderr.strip():
                raise RuntimeError("depmod diagnostics: " + checked.stderr)
        modules_dir = work / "modules"
        (modules_dir / "_alt").mkdir(parents=True)
        names = {row["file"].removesuffix(".ko").replace("-", "_") for row in rows}
        for group, prefix in (("_vendor_boot_modules", "/lib/modules"), ("_vendor_dlkm_modules", "/vendor/lib/modules")):
            folder = modules_dir / group
            folder.mkdir()
            for row in rows:
                os.link(common / row["file"], folder / row["file"])
            dep = (common / "modules.dep").read_text()
            dep = re.sub(r"(^|[ :])([^/ :\n]+\.ko)", lambda match: match.group(1) + prefix + "/" + match.group(2), dep, flags=re.M)
            (folder / "modules.dep").write_text(dep)
            for key in ("modules.alias", "modules.softdep"):
                shutil.copyfile(common / key, folder / key)
            for key in ("modules.blocklist", "modules.options", "vertmp"):
                content = baseline_metadata.get(group + "/" + key)
                if content is None:
                    continue
                if key == "modules.blocklist":
                    blocked = {line.split()[1].replace("-", "_") for line in content.decode().splitlines() if line.startswith("blocklist ")}
                    if blocked & names:
                        raise RuntimeError("planned module is blocked by reference policy")
                if key == "modules.options":
                    for line in content.decode().splitlines():
                        if not line.strip() or line.startswith("#"):
                            continue
                        tokens = line.split()
                        if tokens[0] != "options" or tokens[1].replace("-", "_") not in names:
                            raise RuntimeError("unverified reference module option")
                        module = next(row["file"] for row in rows if row["file"].removesuffix(".ko").replace("-", "_") == tokens[1].replace("-", "_"))
                        parameters = subprocess.check_output(["modinfo", "-p", str(common / module)], text=True)
                        if any(token.split("=", 1)[0] not in {p.split(":", 1)[0] for p in parameters.splitlines()} for token in tokens[2:]):
                            raise RuntimeError("unsupported module parameter")
                (folder / key).write_bytes(content)
            (folder / "modules.load").write_text(load)
            if group == "_vendor_boot_modules":
                (folder / "modules.load.recovery").write_text(load)
        image_dir, dt_dir = work / "image", work / "dt"
        image_dir.mkdir()
        dt_dir.mkdir()
        os.link(image, image_dir / "Image")
        shutil.copyfile(dtb, dt_dir / "dtb")
        data = overlay.read_bytes()
        dtbo = struct.pack(">8I", header[0], 64 + len(data), 32, 32, 1, 32, header[6], header[7])
        dtbo += struct.pack(">8I", len(data), 64, *entry[2:]) + data
        (dt_dir / "dtbo.img").write_bytes(dtbo)
        subprocess.run(["avbtool", "add_hash_footer", "--algorithm", "NONE", "--partition_name", "dtbo", "--partition_size",
                        str(len(original_dtbo)), "--image", str(dt_dir / "dtbo.img")], check=True)
        subprocess.run(["avbtool", "verify_image", "--image", str(dt_dir / "dtbo.img")], check=True)
        dtbo = (dt_dir / "dtbo.img").read_bytes()
        if len(dtbo) != len(original_dtbo) or dtbo[-64:-60] != b"AVBf":
            raise RuntimeError("new DTBO does not preserve partition-size/footer layout")
        new_footer = struct.unpack_from(">4sIIQQQ", dtbo, len(dtbo) - 64)
        new_vbmeta = dtbo[new_footer[4]:new_footer[4] + new_footer[5]]
        if new_footer[:3] != footer[:3] or new_footer[3] != 64 + len(data) or new_vbmeta[:4] != b"AVB0" or struct.unpack_from(">I", new_vbmeta, 28)[0] != 0:
            raise RuntimeError("new DTBO AVB fields do not match the expected NONE layout")
        payloads = {}
        for filename, cwd, inputs, options in (
            ("Image.7z", image_dir, ["Image"], ["-ms=off"]),
            ("_dtb.7z", dt_dir, ["dtb", "dtbo.img"], ["-ms=on"]),
            ("_modules_hyperos.7z", modules_dir, ["_alt", "_vendor_boot_modules", "_vendor_dlkm_modules"], ["-m0=LZMA2:d=96m", "-ms=on"]),
        ):
            output = work / filename
            subprocess.run(["7z", "a", "-mmt=" + jobs, "-mx=9", "-mfb=273", *options, str(output), *inputs], cwd=cwd, check=True)
            subprocess.run(["7z", "t", str(output)], check=True)
            subprocess.run([str(decoder), "t", str(output)], check=True)
            payloads[filename] = output.read_bytes()
        verify = work / "verify-modules"
        subprocess.run(["7z", "x", "-o" + str(verify), str(work / "_modules_hyperos.7z")], check=True)
        expected_files = {str(path.relative_to(modules_dir)) for path in modules_dir.rglob("*") if path.is_file()}
        actual_files = {str(path.relative_to(verify)) for path in verify.rglob("*") if path.is_file()}
        if expected_files != actual_files:
            raise RuntimeError("module archive entry set mismatch")
        for relative in expected_files:
            if digest(modules_dir / relative) != digest(verify / relative):
                raise RuntimeError("module/metadata archive readback mismatch: " + relative)
        for filename, expected in (("Image.7z", {"Image": image}), ("_dtb.7z", {"dtb": dtb, "dtbo.img": dt_dir / "dtbo.img"})):
            recovered = payload_metadata(payloads[filename], list(expected))
            for entry_name, path in expected.items():
                if hashlib.sha256(recovered[entry_name]).hexdigest() != digest(path):
                    raise RuntimeError("payload archive readback mismatch: " + entry_name)
        manifest = ("EXPERIMENTAL DIAGNOSTIC PACKAGE - NOT BOOT TESTED\n"
                    "No complete display/GPU/audio/camera/WLAN stack. Recovery UI is unvalidated.\n"
                    "Existing Bouquet partition/vbmeta behavior is retained; external restore is required if boot fails.\n"
                    "kernelrelease=" + release + "\nimage_sha1=" + sha1 + "\nsource_commit=" + build["source_commit"] +
                    "\npackaging_commit=" + subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip() +
                    "\nmodule_set=95 diagnostic modules duplicated in original locations\n").encode()
        changed = {**payloads, "anykernel.sh": installer, "local-build-manifest.txt": manifest}
        private = work / "package.zip"
        with zipfile.ZipFile(private, "w", compression=zipfile.ZIP_STORED) as output:
            for info in archive.infolist():
                output.writestr(info, changed.get(info.filename, archive.read(info.filename)))
        with zipfile.ZipFile(private) as candidate:
            if candidate.testzip() is not None or candidate.namelist() != archive.namelist():
                raise RuntimeError("ZIP integrity or entry-set mismatch")
            for info in archive.infolist():
                new = candidate.getinfo(info.filename)
                if new.compress_type != info.compress_type or new.external_attr != info.external_attr:
                    raise RuntimeError("ZIP compression or mode changed: " + info.filename)
                if info.filename not in MUTABLE and candidate.read(info.filename) != archive.read(info.filename):
                    raise RuntimeError("immutable installer entry changed: " + info.filename)
        subprocess.run(["7z", "t", str(private)], check=True)
        report = {"status": "assembled_and_offline_verified_not_boot_tested", "zip": str(destination),
                  "sha256": digest(private), "bytes": private.stat().st_size,
                  "source_commit": build["source_commit"], "kernel_release": release,
                  "template_sha256": reference["primary_sha256"], "immutable_entries_equal": True,
                  "modules_per_location": len(rows), "modules": rows,
                  "dtbo_header": list(struct.unpack_from(">8I", dtbo)), "dtbo_sha256": hashlib.sha256(dtbo).hexdigest(),
                  "dtb_sha256": digest(dtb), "image_sha256": build["image_sha256"],
                  "recovery_validated": False, "hardware_validated": False,
                  "warning": "Installable diagnostic experiment, not a daily-driver/recovery guarantee. Original partition/vbmeta writes retained; no flashing performed by this tool."}
        private.chmod(0o644)
        os.link(private, destination)
        with report_path.open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        print(json.dumps({"zip": str(destination), "sha256": report["sha256"], "bytes": report["bytes"]}, indent=2))
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
