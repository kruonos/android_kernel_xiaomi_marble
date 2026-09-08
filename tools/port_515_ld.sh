#!/bin/sh
# Keep all architecture flags while bounding LLVM's internal link parallelism.
available=$(nproc)
threads=${PORT_515_LD_THREADS:-$available}
case "$threads" in ''|*[!0-9]*) echo 'invalid linker worker count' >&2; exit 1;; esac
if [ "$threads" -lt 1 ] || [ "$threads" -gt "$available" ]; then
	echo 'linker worker count exceeds available CPUs' >&2
	exit 1
fi
exec ld.lld-18 --threads="$threads" --thinlto-jobs="$threads" "$@"
