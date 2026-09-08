#!/usr/bin/env bash
# Boot-module compile/prelink and diagnostic DT probes. No kernel Image/package.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
jobs=$(nproc)
OUT="$ROOT/out-5.15-probe"
export TMPDIR="$ROOT/consolidation/scratch/port-515"
unset KBUILD_OUTPUT KBUILD_MIXED_TREE KBUILD_EXTMOD KCONFIG_CONFIG KCONFIG_ALLCONFIG

if ! grep -qx 'VERSION = 5' "$ROOT/Makefile" ||
   ! grep -qx 'PATCHLEVEL = 15' "$ROOT/Makefile"; then
	echo 'refusing compile preparation outside the 5.15 source' >&2
	exit 1
fi
for path in "$OUT" "$TMPDIR"; do
	if [[ -L "$path" || "$(realpath -m -- "$path")" != "$path" ]]; then
		echo "refusing redirected output: $path" >&2
		exit 1
	fi
done
read -r free_blocks block_size < <(stat -f -c '%a %S' "$ROOT")
if (( free_blocks * block_size < 20 * 1024 * 1024 * 1024 )); then
	echo 'at least 20 GiB free is required before compile preparation' >&2
	exit 1
fi
mkdir -p "$OUT" "$TMPDIR"
[[ ! -L "$OUT/port-build.lock" ]] || { echo 'refusing redirected build lock' >&2; exit 1; }
exec 9>"$OUT/port-build.lock"
flock -n 9 || { echo 'another 5.15 build owns the output directory' >&2; exit 1; }
ulimit -f 1048576

make_args=( -C "$ROOT" O="$OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1
	CC=clang-18 HOSTCC=clang-18 HOSTCXX=clang++-18 LD=ld.lld-18
	AR=llvm-ar-18 NM=llvm-nm-18 OBJCOPY=llvm-objcopy-18
	OBJDUMP=llvm-objdump-18 READELF=llvm-readelf-18 STRIP=llvm-strip-18 )

cd -- "$TMPDIR"
bash "$ROOT/scripts/kconfig/merge_config.sh" -m -O "$OUT" \
	"$ROOT/arch/arm64/configs/gki_defconfig" \
	"$ROOT/arch/arm64/configs/vendor/waipio_GKI.config" \
	"$ROOT/arch/arm64/configs/vendor/marble_515_bringup.config"
make "${make_args[@]}" -j"$jobs" olddefconfig

targets_text="$(python3 -B - "$ROOT/port-5.15-sources.json" "$OUT/.config" <<'PY'
import json
from pathlib import Path, PurePosixPath
import sys

inventory = json.loads(Path(sys.argv[1]).read_text())
config = dict(line.split("=", 1) for line in Path(sys.argv[2]).read_text().splitlines()
              if line.startswith("CONFIG_") and "=" in line)
modules = inventory["target_boot_module_sources"]["modules"]
if len(modules) != 25:
    raise SystemExit("Expected the reviewed 25-module probe set")
required = {entry["config"] for entry in modules.values()} | {"MSM_GPUCC_WAIPIO"}
missing = sorted(symbol for symbol in required
                 if config.get("CONFIG_" + symbol) != "m")
for symbol in ("CFI_CLANG", "SHADOW_CALL_STACK", "LTO_CLANG_THIN"):
    if config.get("CONFIG_" + symbol) != "y":
        missing.append(symbol)
if config.get("CONFIG_CFI_PERMISSIVE") == "y":
    missing.append("CFI_PERMISSIVE must remain disabled")
if missing:
    raise SystemExit("Required configuration did not survive Kconfig: " + ", ".join(missing))
targets = [str(PurePosixPath(entry["directory"]) / entry["object"])
           for entry in modules.values()]
targets.append("drivers/clk/qcom/gpucc-waipio.o")
if len(set(targets)) != 26:
    raise SystemExit("Duplicate probe targets")
for target in targets:
    path = PurePosixPath(target)
    if path.is_absolute() or ".." in path.parts or path.suffix != ".o" or path.parts[0] not in ("drivers", "kernel"):
        raise SystemExit("Invalid probe target: " + target)
print("\n".join(targets))
PY
)"
mapfile -t boot_targets <<< "$targets_text"
[[ ${#boot_targets[@]} -eq 26 ]] || { echo 'invalid probe target count' >&2; exit 1; }
echo 'Required module gates and CFI/SCS/ThinLTO survived Kconfig resolution'

make "${make_args[@]}" -j"$jobs" modules_prepare
make "${make_args[@]}" -j"$jobs" "${boot_targets[@]}"
prelinks=()
link_limits=()
for target in "${boot_targets[@]}"; do
	prelink="${target%.o}.lto.o"
	prelinks+=( "$prelink" )
	link_limits+=( "LDFLAGS_${prelink##*/}=--threads=1" )
done
make "${make_args[@]}" -j"$jobs" "${link_limits[@]}" "${prelinks[@]}"

python3 -B - "$OUT" "${boot_targets[@]}" <<'PY'
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

out = Path(sys.argv[1])
records = []
for target in sys.argv[2:]:
    path = out / (target[:-2] + ".lto.o")
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (1, 183):
        raise SystemExit("Expected native AArch64 relocatable ELF: " + str(path))
    symbols = subprocess.check_output(
        ["llvm-nm-18", "--undefined-only", "--format=posix", str(path)], text=True
    )
    records.append({"target": target, "prelink": str(path.relative_to(out)),
                    "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    "undefined_symbols": [line.split()[0] for line in symbols.splitlines() if line.strip()]})
report = {"kernel_release": (out / "include/config/kernel.release").read_text().strip(),
          "config_sha256": hashlib.sha256((out / ".config").read_bytes()).hexdigest(),
          "count": len(records), "format": "AArch64 ELF ET_REL",
          "limits": "Module-sized native LTO prelinks only; unresolved kernel imports allowed. Not modpost, final .ko, BTF or runtime validation.",
          "modules": records}
(out / "boot-link-report.json").write_text(json.dumps(report, indent=2) + "\n")
print("Validated %d native AArch64 module prelinks; kernel imports remain unresolved" % len(records))
PY

dt_out="$OUT/dt-probe"
if [[ -L "$dt_out" || "$(realpath -m -- "$dt_out")" != "$dt_out" ]]; then
	echo "refusing redirected DT output: $dt_out" >&2
	exit 1
fi
mkdir -p "$dt_out"
for name in ukee marble-sm7475-pm8008-overlay; do
	clang-18 -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I "$ROOT/include" -I "$ROOT/arch/arm64/boot/dts/vendor/qcom" \
		"$ROOT/arch/arm64/boot/dts/vendor/qcom/$name.dts" | \
		"$OUT/scripts/dtc/dtc" -@ -I dts -O dtb -o "$dt_out/$name.dtb"
done
fdtoverlay -i "$dt_out/ukee.dtb" -o "$dt_out/marble-composed.dtb" \
	"$dt_out/marble-sm7475-pm8008-overlay.dtb"
tlmm_path="$(fdtget -t s "$dt_out/marble-composed.dtb" /__symbols__ tlmm)"
if [[ "$(fdtget -t s "$dt_out/marble-composed.dtb" "$tlmm_path" compatible)" != qcom,cape-pinctrl ||
      "$(fdtget -t x "$dt_out/marble-composed.dtb" "$tlmm_path" interrupts)" != '0 d0 4' ]]; then
	echo 'composed DT does not satisfy the reviewed physical Cape TLMM contract' >&2
	exit 1
fi
echo 'Diagnostic base/overlay DTs compiled and composed; physical TLMM IRQ contract verified.'
read -r output_kib _ < <(du -sk -- "$OUT")
if (( output_kib > 2 * 1024 * 1024 )); then
	echo 'compile preparation exceeded its 2 GiB output budget; stop before further builds' >&2
	exit 1
fi
make "${make_args[@]}" -s kernelrelease
echo "Compile preparation complete; output uses $output_kib KiB. No image or package built."
