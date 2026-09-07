#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "$0")" && pwd -P)"
KERNEL_DIR="${KERNEL_DIR:-${BASE_DIR}}"
KERNEL_DIR="$(realpath -- "$KERNEL_DIR")"
RELEASE_DIR="${RELEASE_DIR:-${KERNEL_DIR}/release}"
TEMPLATE_DIR="${TEMPLATE_DIR:-${KERNEL_DIR}/consolidation/templates/known-good-ak3}"
export TMPDIR="$(realpath -m -- "${KERNEL_DIR}/consolidation/scratch/package")"
PACKAGE_DIR="$(realpath -m -- "${KERNEL_DIR}/consolidation/packages")"
for path in "$TMPDIR" "$PACKAGE_DIR"; do
	case "$path/" in "$KERNEL_DIR/"*) ;; *) echo "output escapes checkout: $path" >&2; exit 1;; esac
done
mkdir -p "$TMPDIR" "$PACKAGE_DIR"
KERNEL_RELEASE="${KERNEL_RELEASE:-}"
if [ -z "$KERNEL_RELEASE" ]; then
	KERNEL_RELEASE="$(tr -d '\n' < "${KERNEL_DIR}/out/include/config/kernel.release")"
fi
OUTPUT_ZIP="${OUTPUT_ZIP:-${PACKAGE_DIR}/Bouquet-${KERNEL_RELEASE}-marble-unified.zip}"
if [[ -e "$OUTPUT_ZIP" || -L "$OUTPUT_ZIP" ]]; then
	echo "refusing to replace existing package: $OUTPUT_ZIP" >&2
	exit 1
fi
OUTPUT_ZIP="$(realpath -m -- "$OUTPUT_ZIP")"
case "$OUTPUT_ZIP" in "$PACKAGE_DIR/"*) ;; *) echo 'ZIP must stay inside consolidation/packages' >&2; exit 1;; esac
if [[ -e "$OUTPUT_ZIP" || -L "$OUTPUT_ZIP" ]]; then
	echo "refusing to replace existing package: $OUTPUT_ZIP" >&2
	exit 1
fi
SEVENZIP="${SEVENZIP:-7z}"

require_file() {
	local path=$1
	[ -f "$path" ] || { echo "missing required file: $path" >&2; exit 1; }
}

require_dir() {
	local path=$1
	[ -d "$path" ] || { echo "missing required directory: $path" >&2; exit 1; }
}

require_cmd() {
	local cmd=$1
	command -v "$cmd" >/dev/null || { echo "missing required command: $cmd" >&2; exit 1; }
}

verify_release_against_out() {
	local out_image="${KERNEL_DIR}/out/arch/arm64/boot/Image"
	local tmp_dir module source_rel source_file stripped_file release_module release_dir
	declare -A module_sources=()

	[ "${SKIP_RELEASE_PROVENANCE_GUARD:-0}" = 1 ] && return 0

	require_file "$out_image"
	cmp -s "$out_image" "${RELEASE_DIR}/Image" || {
		echo "stale release Image: ${RELEASE_DIR}/Image does not match current out/ Image" >&2
		exit 1
	}

	while IFS= read -r source_rel; do
		[ -n "$source_rel" ] || continue
		source_file="${KERNEL_DIR}/out/${source_rel}"
		[ -f "$source_file" ] || continue
		module=$(basename "$source_rel")
		[ "$module" = qca6490.ko ] && module=qca_cld3_qca6490.ko
		if [ -n "${module_sources[$module]:-}" ] && \
		   [ "${module_sources[$module]}" != "$source_file" ]; then
			echo "ambiguous built module basename: $module" >&2
			exit 1
		fi
		module_sources[$module]="$source_file"
	done < "${KERNEL_DIR}/out/modules.order"

	tmp_dir=$(mktemp -d)
	for release_dir in vendor_boot_modules vendor_dlkm_modules alt_kernel_modules; do
		for release_module in "${RELEASE_DIR}/${release_dir}"/*.ko; do
			[ -f "$release_module" ] || continue
			module=$(basename "$release_module")
			source_file="${module_sources[$module]:-}"
			if [ -z "$source_file" ]; then
				echo "release module has no matching current build output: ${release_dir}/${module}" >&2
				rm -rf "$tmp_dir"
				exit 1
			fi
			stripped_file="${tmp_dir}/${module}"
			if [ ! -f "$stripped_file" ]; then
				llvm-strip -S "$source_file" -o "$stripped_file"
			fi
			if ! cmp -s "$stripped_file" "$release_module"; then
				echo "stale release module: ${release_dir}/${module} does not match current out/ build" >&2
				rm -rf "$tmp_dir"
				exit 1
			fi
		done
	done
	rm -rf "$tmp_dir"
}

archive_entry_size() {
	local archive=$1
	local entry=$2

	"$SEVENZIP" e -so "$archive" "$entry" | wc -c | awk '{print $1}'
}

file_size() {
	local path=$1

	wc -c < "$path" | awk '{print $1}'
}

guard_size_ratio() {
	local label=$1
	local current=$2
	local reference=$3
	local min_percent=$4
	local max_percent=$5

	if [ "$reference" -le 0 ]; then
		echo "invalid reference size for ${label}: ${reference}" >&2
		exit 1
	fi

	if [ $((current * 100)) -lt $((reference * min_percent)) ]; then
		echo "unsafe package size drift for ${label}: ${current} bytes, reference ${reference} bytes" >&2
		echo "set SKIP_PACKAGE_SIZE_GUARDS=1 only after manually validating the image" >&2
		exit 1
	fi

	if [ $((current * 100)) -gt $((reference * max_percent)) ]; then
		echo "unsafe package size drift for ${label}: ${current} bytes, reference ${reference} bytes" >&2
		echo "set SKIP_PACKAGE_SIZE_GUARDS=1 only after manually validating the image" >&2
		exit 1
	fi
}

guard_file_against_template() {
	local label=$1
	local current_file=$2
	local reference_file=$3
	local min_percent=$4
	local max_percent=$5

	[ "${SKIP_PACKAGE_SIZE_GUARDS:-0}" = 1 ] && return 0
	[ -f "$reference_file" ] || return 0

	guard_size_ratio "$label" "$(file_size "$current_file")" \
		"$(file_size "$reference_file")" "$min_percent" "$max_percent"
}

guard_archive_entry_against_template() {
	local label=$1
	local current_archive=$2
	local reference_archive=$3
	local entry=$4
	local min_percent=$5
	local max_percent=$6

	[ "${SKIP_PACKAGE_SIZE_GUARDS:-0}" = 1 ] && return 0
	[ -f "$reference_archive" ] || return 0

	guard_size_ratio "$label" \
		"$(archive_entry_size "$current_archive" "$entry")" \
		"$(archive_entry_size "$reference_archive" "$entry")" \
		"$min_percent" "$max_percent"
}

guard_module_archive_against_template() {
	local current_archive=$1
	local reference_archive=$2
	local allowed_changes=$3

	[ "${SKIP_PACKAGE_SIZE_GUARDS:-0}" = 1 ] && return 0
	[ -f "$reference_archive" ] || return 0

	python3 - "$current_archive" "$reference_archive" "$allowed_changes" "$SEVENZIP" <<'PY'
import subprocess
import sys

current_archive, reference_archive, allowed_arg, sevenzip = sys.argv[1:]
allowed = {item for item in allowed_arg.split() if item}

def parse_archive(path):
    out = subprocess.check_output([sevenzip, 'l', '-slt', path], text=True, errors='replace')
    entries = {}
    cur = None
    in_entries = False
    for line in out.splitlines():
        if line == '----------':
            in_entries = True
            continue
        if not in_entries:
            continue
        if line.startswith('Path = '):
            if cur and cur.get('Path') and not cur.get('Attributes', '').startswith('D'):
                entries[cur['Path']] = cur
            cur = {'Path': line[7:]}
        elif cur is not None and ' = ' in line:
            key, value = line.split(' = ', 1)
            cur[key] = value
    if cur and cur.get('Path') and not cur.get('Attributes', '').startswith('D'):
        entries[cur['Path']] = cur
    return entries

current = parse_archive(current_archive)
reference = parse_archive(reference_archive)
errors = []

for path in sorted(set(current) ^ set(reference)):
    if path not in allowed:
        errors.append(f'unexpected module entry add/remove: {path}')

for path in sorted(set(current) & set(reference)):
    c = current[path]
    r = reference[path]
    changed = c.get('Size') != r.get('Size') or c.get('CRC') != r.get('CRC')
    if changed and path not in allowed:
        errors.append(
            f'unexpected module entry change: {path} '
            f"size {r.get('Size')}->{c.get('Size')} crc {r.get('CRC')}->{c.get('CRC')}"
        )

if errors:
    print('unsafe module payload drift:', file=sys.stderr)
    for error in errors[:40]:
        print(error, file=sys.stderr)
    if len(errors) > 40:
        print(f'... {len(errors) - 40} more', file=sys.stderr)
    sys.exit(1)
PY
}

copy_named_modules() {
	local source_dir=$1
	local dest_dir=$2
	shift 2
	local module

	for module in "$@"; do
		[ -n "$module" ] || continue
		require_file "${source_dir}/${module}"
		cp "${source_dir}/${module}" "${dest_dir}/${module}"
	done
}

generate_modules_load() {
	local module_dir=$1
	local original_load=$2
	local output_load=$3
	local module name

	: > "$output_load"
	if [ -f "$original_load" ]; then
		while IFS= read -r module; do
			[ -n "$module" ] || continue
			case "$module" in \#*) continue;; esac
			name=$(basename "$module")
			if [ -f "${module_dir}/${name}" ] && ! grep -qxF "$name" "$output_load"; then
				printf '%s\n' "$name" >> "$output_load"
			fi
		done < "$original_load"
	fi

	while IFS= read -r module; do
		[ -n "$module" ] || continue
		name=$(basename "$module")
		if [ -f "${module_dir}/${name}" ] && ! grep -qxF "$name" "$output_load"; then
			printf '%s\n' "$name" >> "$output_load"
		fi
	done < "${KERNEL_DIR}/out/modules.order"
}

prefix_modules_dep() {
	local input_dep=$1
	local output_dep=$2
	local prefix=$3

	sed -E "s#(^|[ :])([^/ :]+\.ko)#\\1${prefix}/\\2#g" "$input_dep" > "$output_dep"
}

stage_module_metadata() {
	local module_dir=$1
	local original_dir=$2
	local dep_prefix=$3
	local include_recovery_load=$4
	local dep_root dep_modroot

	dep_root=$(mktemp -d)
	dep_modroot="${dep_root}/lib/modules/${KERNEL_RELEASE}"
	mkdir -p "$dep_modroot"
	cp "$module_dir"/*.ko "$dep_modroot"/
	depmod -b "$dep_root" "$KERNEL_RELEASE" >/dev/null 2>&1 || true

	require_file "${dep_modroot}/modules.dep"
	prefix_modules_dep "${dep_modroot}/modules.dep" "${module_dir}/modules.dep" "$dep_prefix"
	cp "${dep_modroot}/modules.alias" "${module_dir}/modules.alias"
	cp "${dep_modroot}/modules.softdep" "${module_dir}/modules.softdep"

	if [ -f "${original_dir}/modules.blocklist" ]; then
		cp "${original_dir}/modules.blocklist" "${module_dir}/modules.blocklist"
	else
		: > "${module_dir}/modules.blocklist"
	fi

	if [ -f "${original_dir}/modules.options" ]; then
		cp "${original_dir}/modules.options" "${module_dir}/modules.options"
	fi

	generate_modules_load "$module_dir" "${original_dir}/modules.load" "${module_dir}/modules.load"
	if [ "$include_recovery_load" = true ]; then
		generate_modules_load "$module_dir" "${original_dir}/modules.load.recovery" "${module_dir}/modules.load.recovery"
	fi

	rm -rf "$dep_root"
}

patch_anykernel_for_local_image() {
	local anykernel=$1
	local image_sha1=$2

	python3 - "$anykernel" "$image_sha1" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
sha1 = sys.argv[2]
text = path.read_text()
text = re.sub(r'^SHA1_STOCK="[^"]+"', f'SHA1_STOCK="{sha1}"', text, flags=re.M)
text = re.sub(r'^SHA1_KSU="[^"]+"', f'SHA1_KSU="{sha1}"', text, flags=re.M)
text = re.sub(r'^SHA1_SUSFS="[^"]+"', f'SHA1_SUSFS="{sha1}"', text, flags=re.M)

ksu_prefix = 'elif keycode_select ' + '\\' + '\n'
ksu_pattern = re.escape(ksu_prefix) + r'(\s+"\$_LANG_SELECT_KSU")'
ksu_replacement_prefix = 'elif false && keycode_select ' + '\\' + '\n'
text = re.sub(ksu_pattern, lambda match: ksu_replacement_prefix + match.group(1), text, count=1)

text = text.replace(
    'kernel.string=Bouquet Kernel by Pzqqt',
    'kernel.string=Bouquet Kernel local rebuild by Pzqqt',
    1,
)
path.write_text(text)
PY
}

require_cmd "$SEVENZIP"
require_cmd awk
require_cmd cmp
require_cmd depmod
require_cmd grep
require_cmd llvm-strip
require_cmd python3
require_cmd sed
require_cmd sha1sum
require_cmd wc
require_file "${RELEASE_DIR}/Image"
require_file "${RELEASE_DIR}/devicetree/ukee.dtb"
require_file "${RELEASE_DIR}/devicetree/dtbo.img"
require_file "${KERNEL_DIR}/out/modules.order"
require_dir "${RELEASE_DIR}/vendor_boot_modules"
require_dir "${RELEASE_DIR}/vendor_dlkm_modules"
require_dir "${RELEASE_DIR}/alt_kernel_modules"
require_dir "$TEMPLATE_DIR"
require_file "${TEMPLATE_DIR}/anykernel.sh"
require_file "${TEMPLATE_DIR}/Image.7z"
require_file "${TEMPLATE_DIR}/_modules_hyperos.7z"
require_file "${TEMPLATE_DIR}/_dtb.7z"

verify_release_against_out

WORK_DIR=$(mktemp -d)
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

TEMPLATE_WORK="${WORK_DIR}/ak3"
MODULES_ORIG="${WORK_DIR}/orig_modules"
MODULES_WORK="${WORK_DIR}/modules_work"
DTB_WORK="${WORK_DIR}/dtb_work"
read -r -a UPDATED_VENDOR_BOOT_MODULES_ARR <<< "${UPDATED_VENDOR_BOOT_MODULES:-}"
read -r -a UPDATED_VENDOR_DLKM_MODULES_ARR <<< "${UPDATED_VENDOR_DLKM_MODULES:-}"
read -r -a UPDATED_ALT_MODULES_ARR <<< "${UPDATED_ALT_MODULES:-}"
MODULE_PAYLOAD_ALLOWED="${ALLOW_MODULE_PAYLOAD_CHANGES:-}"
for module in "${UPDATED_VENDOR_BOOT_MODULES_ARR[@]}"; do
	[ -n "$module" ] && MODULE_PAYLOAD_ALLOWED="${MODULE_PAYLOAD_ALLOWED} _vendor_boot_modules/${module}"
done
for module in "${UPDATED_VENDOR_DLKM_MODULES_ARR[@]}"; do
	[ -n "$module" ] && MODULE_PAYLOAD_ALLOWED="${MODULE_PAYLOAD_ALLOWED} _vendor_dlkm_modules/${module}"
done
for module in "${UPDATED_ALT_MODULES_ARR[@]}"; do
	[ -n "$module" ] && MODULE_PAYLOAD_ALLOWED="${MODULE_PAYLOAD_ALLOWED} _alt/${module}"
done

cp -a "$TEMPLATE_DIR" "$TEMPLATE_WORK"
rm -f "${TEMPLATE_WORK}/_modules_hyperos.7z"
if [ "${PRESERVE_TEMPLATE_IMAGE:-0}" != 1 ]; then
	rm -f "${TEMPLATE_WORK}/Image.7z"
fi
if [ "${PRESERVE_TEMPLATE_DTB:-0}" != 1 ]; then
	rm -f "${TEMPLATE_WORK}/_dtb.7z"
fi

mkdir -p "$MODULES_ORIG" "$MODULES_WORK/_vendor_boot_modules" "$MODULES_WORK/_vendor_dlkm_modules" "$MODULES_WORK/_alt"
"$SEVENZIP" x -o"$MODULES_ORIG" "${TEMPLATE_DIR}/_modules_hyperos.7z" >/dev/null

if [ "${PRESERVE_TEMPLATE_MODULES:-0}" = 1 ]; then
	rm -rf "$MODULES_WORK/_vendor_boot_modules" "$MODULES_WORK/_vendor_dlkm_modules" "$MODULES_WORK/_alt"
	cp -a "${MODULES_ORIG}/_vendor_boot_modules" "$MODULES_WORK/_vendor_boot_modules"
	cp -a "${MODULES_ORIG}/_vendor_dlkm_modules" "$MODULES_WORK/_vendor_dlkm_modules"
	cp -a "${MODULES_ORIG}/_alt" "$MODULES_WORK/_alt"
	copy_named_modules "${RELEASE_DIR}/vendor_boot_modules" "${MODULES_WORK}/_vendor_boot_modules" "${UPDATED_VENDOR_BOOT_MODULES_ARR[@]}"
	copy_named_modules "${RELEASE_DIR}/vendor_dlkm_modules" "${MODULES_WORK}/_vendor_dlkm_modules" "${UPDATED_VENDOR_DLKM_MODULES_ARR[@]}"
	copy_named_modules "${RELEASE_DIR}/alt_kernel_modules" "${MODULES_WORK}/_alt" "${UPDATED_ALT_MODULES_ARR[@]}"
else
	cp "${RELEASE_DIR}/vendor_boot_modules"/*.ko "${MODULES_WORK}/_vendor_boot_modules"/
	cp "${RELEASE_DIR}/vendor_dlkm_modules"/*.ko "${MODULES_WORK}/_vendor_dlkm_modules"/
	cp "${RELEASE_DIR}/alt_kernel_modules"/*.ko "${MODULES_WORK}/_alt"/
fi

if [ -f "${MODULES_WORK}/_alt/qti_battery_charger_main.ko" ]; then
	cp "${MODULES_WORK}/_alt/qti_battery_charger_main.ko" "${MODULES_WORK}/_alt/NEW-qti_battery_charger_main.ko"
	cp "${MODULES_WORK}/_alt/qti_battery_charger_main.ko" "${MODULES_WORK}/_alt/NEW2-qti_battery_charger_main.ko"
fi
if [ -f "${MODULES_WORK}/_alt/msm_drm.ko" ]; then
	cp "${MODULES_WORK}/_alt/msm_drm.ko" "${MODULES_WORK}/_alt/msm_drm-2.ko"
fi

if [ "${PRESERVE_TEMPLATE_MODULES:-0}" != 1 ] || [ "${REGENERATE_MODULE_METADATA:-0}" = 1 ]; then
	stage_module_metadata "${MODULES_WORK}/_vendor_boot_modules" "${MODULES_ORIG}/_vendor_boot_modules" "/lib/modules" true
	stage_module_metadata "${MODULES_WORK}/_vendor_dlkm_modules" "${MODULES_ORIG}/_vendor_dlkm_modules" "/vendor/lib/modules" false
fi

if [ "${PRESERVE_TEMPLATE_IMAGE:-0}" = 1 ]; then
	require_file "${TEMPLATE_WORK}/Image.7z"
else
	cp "${RELEASE_DIR}/Image" "${TEMPLATE_WORK}/Image"
	(
		cd "$TEMPLATE_WORK"
		"$SEVENZIP" a -mmt=4 -mx=9 -mfb=273 -ms=off Image.7z Image >/dev/null
		rm -f Image
	)
	chmod 0644 "${TEMPLATE_WORK}/Image.7z"
	guard_archive_entry_against_template "Image" "${TEMPLATE_WORK}/Image.7z" \
		"${TEMPLATE_DIR}/Image.7z" Image 90 110
fi

(
	cd "$MODULES_WORK"
	"$SEVENZIP" a -mmt=4 -mx=9 -m0=LZMA2:d=64m -mfb=273 -ms=on "${TEMPLATE_WORK}/_modules_hyperos.7z" _alt _vendor_boot_modules _vendor_dlkm_modules >/dev/null
)
guard_module_archive_against_template "${TEMPLATE_WORK}/_modules_hyperos.7z" \
	"${TEMPLATE_DIR}/_modules_hyperos.7z" "$MODULE_PAYLOAD_ALLOWED"

if [ "${PRESERVE_TEMPLATE_DTB:-0}" = 1 ]; then
	require_file "${TEMPLATE_WORK}/_dtb.7z"
else
	mkdir -p "$DTB_WORK"
	cp "${RELEASE_DIR}/devicetree/ukee.dtb" "${DTB_WORK}/dtb"
	cp "${RELEASE_DIR}/devicetree/dtbo.img" "${DTB_WORK}/dtbo.img"
	(
		cd "$DTB_WORK"
		"$SEVENZIP" a -mmt=4 -mx=9 -mfb=273 -ms=on "${TEMPLATE_WORK}/_dtb.7z" dtb dtbo.img >/dev/null
	)
	guard_archive_entry_against_template "dtb" "${TEMPLATE_WORK}/_dtb.7z" \
		"${TEMPLATE_DIR}/_dtb.7z" dtb 90 110
	guard_archive_entry_against_template "dtbo.img" "${TEMPLATE_WORK}/_dtb.7z" \
		"${TEMPLATE_DIR}/_dtb.7z" dtbo.img 90 110
fi

IMAGE_SHA1=$("$SEVENZIP" e -so "${TEMPLATE_WORK}/Image.7z" Image | sha1sum | awk '{print $1}')
patch_anykernel_for_local_image "${TEMPLATE_WORK}/anykernel.sh" "$IMAGE_SHA1"

cat > "${TEMPLATE_WORK}/local-build-manifest.txt" <<EOF
kernelrelease=${KERNEL_RELEASE}
image_sha1=${IMAGE_SHA1}
source=${KERNEL_DIR}
release=${RELEASE_DIR}
template=${TEMPLATE_DIR}
EOF

PRIVATE_ZIP="${WORK_DIR}/package.zip"
(
	cd "$TEMPLATE_WORK"
	"$SEVENZIP" a -mmt=4 -tzip -mx=0 "$PRIVATE_ZIP" . >/dev/null
)
"$SEVENZIP" t "$PRIVATE_ZIP" >/dev/null
# Atomic, no-clobber publication on the same checkout filesystem.
ln -T -- "$PRIVATE_ZIP" "$OUTPUT_ZIP"

echo "$OUTPUT_ZIP"
