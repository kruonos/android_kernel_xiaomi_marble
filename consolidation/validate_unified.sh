#!/usr/bin/env bash
set -euo pipefail
stage=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel=$(dirname -- "$stage")
workspace=$(dirname -- "$kernel")
logfile="$stage/logs/unified-validation.log"
statusfile="$stage/logs/unified-validation.status"
phase=QUEUED

log() {
    printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$logfile"
}
finish() {
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        printf 'FAILED phase=%s exit=%s\n' "$phase" "$rc" > "$statusfile"
        log "FAILED phase=$phase exit=$rc; inspect the phase log. No device operations performed."
    fi
}
trap finish EXIT

# This is a resource dependency, not polling: no heavy work starts until the
# existing retirement/integrity job releases this same lock and reports PASS.
exec 9>"$stage/logs/retirement.lock"
printf 'QUEUED waiting for retirement/integrity lock\n' > "$statusfile"
log 'QUEUED: waiting for the existing integrity job; consuming no build workers.'
flock 9
[ "$(cat "$stage/logs/retirement.status")" = PASS ]
export TMPDIR="$stage/scratch/validation"
mkdir -p "$TMPDIR" "$stage/templates"
export PYTHONDONTWRITEBYTECODE=1

phase=SOURCE_TESTS
printf '%s\n' "$phase" > "$statusfile"
log 'START source preservation and userspace regression tests'
python3 -B "$stage/verify_snapshots.py" >> "$logfile" 2>&1
(
    cd "$kernel"
    python3 -B -m unittest discover -s tools/qca6490_cfr -p 'test_*.py'
    python3 -B -m unittest discover -s wlan_tools -p 'test_*.py'
    if [ ! -f "$workspace/nfc-infos/libsn100u_fw.so" ]; then
        printf 'NOTE: NFC firmware fixture unavailable; only source/CLI ABI checks will run.\n'
    fi
    python3 -B nfc_tools/test_qti_nfc_function.py --firmware "$workspace/nfc-infos/libsn100u_fw.so"
    bash -n tools/qca6490-inject/lab
) > "$stage/logs/unified-source-tests.log" 2>&1
log 'PASS source tests'

phase=TEMPLATE
template_zip="$workspace/Bouquet-5.10.258-Bouquet-v4.9-marble-local.zip"
expected=c3371caf732e8e22a378fbc6a58ec70aa265563be6fddd8cbf99673cd6734def
actual=$(sha256sum "$template_zip")
[ "${actual%% *}" = "$expected" ]
7z t "$template_zip" > "$stage/logs/template-integrity.log"
template="$stage/templates/known-good-ak3"
if [ -e "$template" ] || [ -L "$template" ]; then
    printf 'Template destination already exists; refusing to replace it\n' >&2
    exit 1
fi
unzip -q "$template_zip" -d "$template"
log 'PASS known-good template checksum and archive integrity'

phase=BUILD
printf '%s\n' "$phase" > "$statusfile"
log 'START unified build, four workers, no ccache; detailed output in unified-build.log'
(
    cd "$kernel"
    JOBS=4 ./build_bouquet.sh --noccache
) > "$stage/logs/unified-build.log" 2>&1
kernel_release=$(cat "$kernel/out/include/config/kernel.release")
log "PASS build: $kernel_release"
printf 'BUILD_PASSED %s\n' "$kernel_release" > "$stage/logs/unified-build.status"

phase=PACKAGE_GUARDS
printf '%s\n' "$phase" > "$statusfile"
log 'START packaging with existing provenance and module-change guards enabled'
(
    cd "$kernel"
    unset KERNEL_DIR RELEASE_DIR TEMPLATE_DIR OUTPUT_ZIP KERNEL_RELEASE
    unset ALLOW_MODULE_PAYLOAD_CHANGES UPDATED_VENDOR_BOOT_MODULES UPDATED_VENDOR_DLKM_MODULES UPDATED_ALT_MODULES
    export SKIP_RELEASE_PROVENANCE_GUARD=0 SKIP_PACKAGE_SIZE_GUARDS=0
    export PRESERVE_TEMPLATE_IMAGE=0 PRESERVE_TEMPLATE_DTB=0 PRESERVE_TEMPLATE_MODULES=0
    ./package_bouquet.sh
) > "$stage/logs/unified-package.log" 2>&1
package="$stage/packages/Bouquet-${kernel_release}-marble-unified.zip"
7z t "$package" >> "$stage/logs/unified-package.log" 2>&1
sha256sum "$package" > "$package.sha256"
printf 'PASS\n' > "$statusfile"
log 'PASS all validation stages; no device flashing performed.'
