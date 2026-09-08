#!/usr/bin/env python3
"""Guarded native 5.15 core/module build. Never packages or accesses a device."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import resource
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="validate tools, config and BTF encoding without a kernel build")
    args = parser.parse_args()
    jobs = len(os.sched_getaffinity(0))
    root = Path(__file__).resolve().parents[1]
    out = root / "out-5.15-probe"
    scratch = root / "consolidation/scratch/port-515"
    gib = 1024**3
    reserve = 2 * gib
    for path in (out, scratch):
        if not path.is_dir() or path.resolve() != path:
            raise RuntimeError("missing or redirected prepared directory: " + str(path))
    for name in ("port-build.lock", "full-build-report.json", "full-build-report.json.tmp",
                 "btf-tool-probe.o"):
        if (out / name).is_symlink():
            raise RuntimeError("refusing redirected build metadata: " + name)
    lock = (out / "port-build.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(root), *arguments], text=True).strip()

    def digest(path):
        result = hashlib.sha256()
        with Path(path).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                result.update(chunk)
        return result.hexdigest()

    def allocated():
        return int(subprocess.check_output(["du", "-sk", str(out)]).split()[0]) * 1024

    if git("branch", "--show-current") != "port/waipio-5.15-20260907":
        raise RuntimeError("full builds are restricted to the dedicated port branch")
    source = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=all"))
    if dirty and not args.check:
        raise RuntimeError("commit the source batch before a full build")
    makefile = (root / "Makefile").read_text()
    if not re.search(r"^VERSION = 5$", makefile, re.M) or not re.search(r"^PATCHLEVEL = 15$", makefile, re.M):
        raise RuntimeError("not a 5.15 source tree")
    start_free = shutil.disk_usage(root).free
    start_size = allocated()
    if start_free <= reserve:
        raise RuntimeError("full build requires more than 2 GiB free")
    free_floor = reserve
    for limit in (resource.RLIMIT_FSIZE, resource.RLIMIT_AS):
        _, hard = resource.getrlimit(limit)
        resource.setrlimit(limit, (hard, hard))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    inventory = json.loads((root / "port-5.15-sources.json").read_text())
    tool = inventory["native_toolchain"]["project_pahole"]
    pahole = root / tool["path"]
    package = root / tool["package"]
    for path, expected in ((pahole, tool["sha256"]), (package, tool["package_sha256"])):
        if not path.resolve().is_relative_to(root) or digest(path) != expected:
            raise RuntimeError("unverified local BTF tool: " + str(path))
    linker = root / "tools/port_515_ld.sh"
    if not os.access(pahole, os.X_OK) or not os.access(linker, os.X_OK):
        raise RuntimeError("pahole and linker wrapper must be executable")
    subprocess.run([str(pahole), "--jobs=1", "--version"], check=True)
    subprocess.run([str(linker), "--version"], check=True)

    env = dict(os.environ, TMPDIR=str(scratch), LLVM_OBJCOPY="llvm-objcopy-18")
    for key in ("KBUILD_OUTPUT", "KBUILD_MIXED_TREE", "KBUILD_EXTMOD", "KCONFIG_CONFIG",
                "KCONFIG_ALLCONFIG", "MAKEFLAGS", "MFLAGS", "MAKELEVEL", "KCFLAGS",
                "KCPPFLAGS", "KAFLAGS", "CFLAGS_KERNEL", "CFLAGS_MODULE", "AFLAGS_KERNEL",
                "AFLAGS_MODULE", "LDFLAGS_MODULE", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
        env.pop(key, None)
    env["PAHOLE"] = str(pahole)
    pahole_base_flags = subprocess.check_output(
        ["sh", str(root / "scripts/pahole-flags.sh")], env=env, text=True
    ).strip()
    pahole_flags = pahole_base_flags + " --jobs=" + str(jobs)
    assignments = [value for value in inventory["native_toolchain"]["make_assignments"]
                   if not value.startswith("LD=")]
    assignments += ["LD=" + str(linker), "PAHOLE=" + str(pahole),
                    "PAHOLE_FLAGS=" + pahole_flags]
    command = ["make", "-C", str(root), "O=" + str(out), *assignments, "-j" + str(jobs)]
    manifest = {
        "status": "running", "check_only": args.check, "source_commit": source,
        "source_clean_at_start": not dirty, "completed_phases": [], "phase": "preflight",
        "config_before_sha256": digest(out / ".config"),
        "bounds": {"compile_jobs": jobs, "core_linker_threads": jobs, "core_pahole_jobs": jobs,
                   "core_lto_partitions": jobs,
                   "parallel_module_jobs": jobs, "module_linker_threads": 1, "module_pahole_jobs": 1,
                   "initial_output_allowance_bytes": start_size + start_free - reserve,
                   "minimum_free_bytes": reserve, "event_free_floor_bytes": free_floor,
                   "per_file_limit": resource.getrlimit(resource.RLIMIT_FSIZE)[0],
                   "per_process_address_space_limit": resource.getrlimit(resource.RLIMIT_AS)[0],
                   "core_dumps": False,
                   "note": "Use available host resources with a 2 GiB free-space stop; no added memory, per-file or fixed output cap. Negative rlimit values mean unlimited. Not a filesystem quota."},
        "pahole_sha256": digest(pahole), "pahole_flags": pahole_flags,
        "linker_wrapper_sha256": digest(linker), "llvm_tools": {},
        "limits": "Build validation only; no packaging, flashing, hardware or proprietary userspace validation.",
    }
    for name in ("clang-18", "ld.lld-18", "llvm-ar-18", "llvm-nm-18", "llvm-objcopy-18",
                 "llvm-objdump-18", "llvm-readelf-18", "llvm-strip-18"):
        path = shutil.which(name)
        if path is None:
            raise RuntimeError("missing native tool: " + name)
        manifest["llvm_tools"][name] = {"path": path, "sha256": digest(path)}

    def record():
        temporary = out / "full-build-report.json.tmp"
        temporary.write_text(json.dumps(manifest, indent=2) + "\n")
        temporary.replace(out / "full-build-report.json")

    def capacity():
        if shutil.disk_usage(root).free < free_floor:
            raise RuntimeError("full-build storage safety limit reached; no cleanup attempted")

    def refresh(names):
        removed = []
        for name in names:
            path = out / name
            if path.is_symlink() or not path.resolve().is_relative_to(out):
                raise RuntimeError("refusing redirected final output: " + str(path))
            if path.exists():
                if not path.is_file():
                    raise RuntimeError("final output is not a regular file: " + str(path))
                path.unlink()
                removed.append(name)
        stamp = (time.time_ns() // 1_000_000_000) * 1_000_000_000
        manifest.setdefault("refreshed_outputs", []).append({"started_ns": stamp, "removed": removed})
        record()
        return stamp

    def fresh(path, stamp):
        if path.is_symlink() or not path.resolve().is_relative_to(out) or not path.is_file() or path.stat().st_size == 0 or path.stat().st_mtime_ns < stamp:
            raise RuntimeError("missing, empty or stale final output: " + str(path))

    def interrupt(signum, frame):
        raise KeyboardInterrupt("signal %d" % signum)

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)

    def run(phase, argv, input_data=None):
        capacity()
        manifest["phase"] = phase
        record()
        print("PHASE " + phase + ": " + shlex.join(argv), flush=True)
        phase_env = dict(env, PORT_515_LD_THREADS="1" if phase == "modules" else str(jobs))
        process = subprocess.Popen(argv, cwd=root, env=phase_env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, process_group=0,
                                   stdin=subprocess.PIPE if input_data is not None else None)
        try:
            if input_data is not None:
                process.stdin.write(input_data)
                process.stdin.close()
            for line in process.stdout:
                print(line, end="", flush=True)
                if shutil.disk_usage(root).free < free_floor:
                    raise RuntimeError("free-space guard stopped " + phase)
            result = process.wait()
            if result:
                raise RuntimeError("%s failed with exit %d" % (phase, result))
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            process.stdout.close()
        manifest["children_maxrss_kib"] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        capacity()
        manifest["completed_phases"].append(phase)
        record()

    def config():
        values = dict(line.split("=", 1) for line in (out / ".config").read_text().splitlines()
                      if line.startswith("CONFIG_") and "=" in line)
        required = {item["config"] for item in inventory["target_boot_module_sources"]["modules"].values()}
        for name in required | {"MSM_GPUCC_WAIPIO"}:
            if values.get("CONFIG_" + name) != "m":
                raise RuntimeError("required module gate lost: " + name)
        for name in ("CFI_CLANG", "SHADOW_CALL_STACK", "LTO_CLANG_FULL", "MODVERSIONS", "DEBUG_INFO_BTF"):
            if values.get("CONFIG_" + name) != "y":
                raise RuntimeError("required build/security gate lost: " + name)
        if values.get("CONFIG_CFI_PERMISSIVE") == "y":
            raise RuntimeError("permissive CFI is forbidden")
        return values

    def verify_elf(path, allowed_types, btf=False):
        with path.open("rb") as stream:
            header = stream.read(20)
        if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01":
            raise RuntimeError("not ELF64 little endian: " + str(path))
        kind, machine = struct.unpack_from("<HH", header, 16)
        if kind not in allowed_types or machine != 183:
            raise RuntimeError("wrong ELF type/architecture: " + str(path))
        if btf:
            sections = subprocess.check_output(["llvm-readelf-18", "-SW", str(path)], text=True)
            match = re.search(r"\]\s+\.BTF\s+\S+\s+\S+\s+\S+\s+([0-9a-fA-F]+)", sections)
            if not match or int(match.group(1), 16) == 0:
                raise RuntimeError("missing or empty BTF: " + str(path))

    started = time.time()
    record()
    try:
        config()
        probe = out / "btf-tool-probe.o"
        run("btf-compile-probe", ["clang-18", "-g", "-gdwarf-4", "-c", "-x", "c", "-", "-o", str(probe)],
            input_data="struct port_btf_probe { int value; }; struct port_btf_probe probe;\n")
        run("btf-tool-probe", [str(pahole), "-J", *shlex.split(pahole_flags), str(probe)])
        verify_elf(probe, {1}, btf=True)
        if args.check:
            manifest["status"] = "preflight_passed"
            print("PREFLIGHT PASSED: native BTF encoding and build safeguards verified", flush=True)
            return 0
        run("olddefconfig", [*command, "olddefconfig"])
        values = config()
        manifest["config_sha256"] = digest(out / ".config")
        core_stamp = refresh(["vmlinux", "vmlinux.symvers", "System.map"])
        run("vmlinux", [*command, "vmlinux"])
        fresh(out / "vmlinux", core_stamp)
        verify_elf(out / "vmlinux", {2, 3}, btf=True)
        for name in ("vmlinux.symvers", "System.map"):
            fresh(out / name, core_stamp)
        module_stamp = refresh(["Module.symvers", "modules.order"] +
                               [str(path.relative_to(out)) for path in out.rglob("*.ko")])
        run("modules", [*command, "PAHOLE_FLAGS=" + pahole_base_flags + " --jobs=1", "modules"])
        fresh(out / "modules.order", module_stamp)
        fresh(out / "Module.symvers", module_stamp)
        order = (out / "modules.order").read_text().splitlines()
        expected = {item["directory"] + "/" + item["object"][:-2] + ".ko"
                    for item in inventory["target_boot_module_sources"]["modules"].values()}
        expected.add("drivers/clk/qcom/gpucc-waipio.ko")
        if not expected.issubset(order):
            raise RuntimeError("required modules missing from modules.order: " + repr(expected - set(order)))
        if (out / "Module.symvers").stat().st_size == 0:
            raise RuntimeError("empty module symbol table")
        release = (out / "include/config/kernel.release").read_text().strip()
        module_records = []
        for name in order:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or path.suffix != ".ko":
                raise RuntimeError("invalid module output path: " + name)
            artifact = out / name
            fresh(artifact, module_stamp)
            verify_elf(artifact, {1}, btf=values.get("CONFIG_DEBUG_INFO_BTF_MODULES") == "y")
            vermagic = subprocess.check_output(["modinfo", "-F", "vermagic", str(artifact)], text=True).strip()
            if not vermagic or vermagic.split()[0] != release:
                raise RuntimeError("wrong module release: " + name)
            module_records.append({"path": name, "sha256": digest(artifact), "vermagic": vermagic})
        image_stamp = refresh(["arch/arm64/boot/Image"])
        run("Image", [*command, "Image"])
        image = out / "arch/arm64/boot/Image"
        fresh(image, image_stamp)
        with image.open("rb") as stream:
            if stream.read(64)[56:60] != b"ARM\x64":
                raise RuntimeError("invalid arm64 Image header")
        if git("rev-parse", "HEAD") != source or git("status", "--porcelain", "--untracked-files=all"):
            raise RuntimeError("source changed during the build; artifacts not validated")
        if digest(out / ".config") != manifest["config_sha256"]:
            raise RuntimeError("configuration changed during the build")
        manifest.update(status="passed", kernel_release=release, modules=module_records,
                        module_count=len(module_records), image_sha256=digest(image),
                        vmlinux_sha256=digest(out / "vmlinux"),
                        module_symvers_sha256=digest(out / "Module.symvers"))
        print("FULL BUILD PASSED; artifacts remain experimental and unpackaged", flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        manifest.update(status="failed", error=str(error))
        print("FULL BUILD STOPPED: " + str(error), file=sys.stderr, flush=True)
        return 1
    finally:
        manifest.update(elapsed_seconds=round(time.time() - started, 1),
                        output_bytes=allocated(), free_bytes=shutil.disk_usage(root).free,
                        children_maxrss_kib=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
        record()
        lock.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Full-build preflight refused: " + str(error), file=sys.stderr)
        sys.exit(1)
