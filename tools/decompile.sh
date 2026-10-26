#!/bin/sh
# Decompile the functions containing the given addresses (hex, no 0x) from
# the phase 1 Ghidra project into one C file, e.g.
#   tools/decompile.sh out.c 1e6b20 3d0c60
set -e
cd "$(dirname "$0")/.."
GHIDRA=${GHIDRA_INSTALL_DIR:-$HOME/ghidra/ghidra_12.1.4_PUBLIC}
OUT=$1
shift
case $OUT in /*) ;; *) OUT=$PWD/$OUT ;; esac
MAXMEM=4G "$GHIDRA/support/analyzeHeadless" build/ghidra/project un5 \
    -process -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript DecompileAt.java "$OUT" "$@" >/dev/null 2>&1
echo "$OUT"
