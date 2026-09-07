#!/bin/sh
# Keep all architecture flags while bounding LLVM's internal link parallelism.
threads=${PORT_515_LD_THREADS:-4}
case "$threads" in 1|2|3|4) ;; *) echo 'invalid linker worker count' >&2; exit 1;; esac
exec ld.lld-18 --threads="$threads" "$@"
