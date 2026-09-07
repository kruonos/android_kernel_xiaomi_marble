#!/usr/bin/env bash
set -euo pipefail

stage=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(dirname -- "$stage")
logfile="$stage/logs/verify-archives-serial.log"
statusfile="$stage/logs/verify-archives-serial.status"

# The process manager owns job lifetime; flock prevents overlapping invocations.
exec 9>"$stage/logs/verify-archives-serial.lock"
flock -n 9 || { printf 'Another serial verification is already running.\n' >&2; exit 1; }

log() {
    printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$logfile"
}

finish() {
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        printf 'PASS\n' > "$statusfile"
        log 'PASS: all archive comparisons completed; no source trees changed.'
    else
        printf 'FAILED exit=%s\n' "$rc" > "$statusfile"
        log "FAILED: exit $rc. No cleanup is authorized by this run. See $logfile"
    fi
}
trap finish EXIT

printf 'RUNNING\n' > "$statusfile"
log 'Starting strictly serial archive-to-original comparisons.'
for label in baseline csi mglru wifi; do
    case "$label" in
        baseline) tree="$workspace/android_kernel_xiaomi_marble-bouquet" ;;
        csi) tree="$workspace/CSI-PATCHED/android_kernel_xiaomi_marble" ;;
        mglru) tree="$workspace/MGLRU-EXPERIMENT/android_kernel_xiaomi_marble" ;;
        wifi) tree="$workspace/WIFI-MONITOR-EXPERIMENT" ;;
    esac
    for kind in source out; do
        archive="$stage/archives/$label-$kind.tar.zst"
        log "START $label/$kind"
        printf 'RUNNING %s/%s\n' "$label" "$kind" > "$statusfile"
        tar --use-compress-program='zstd -T1' --compare --file="$archive" \
            --directory="$tree" </dev/null >> "$logfile" 2>&1
        log "PASS $label/$kind"
    done
done
