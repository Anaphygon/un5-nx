#!/bin/sh
# Analyse the main ELF in Ghidra (headless) with the Emotion Engine processor
# module, then run PS2Recomp's ExportPS2Functions.java to write function
# boundaries as a CSV plus a PS2Recomp TOML.
#
#   tools/ghidra_export.sh            -> build/ghidra/{functions.csv,ghidra.toml}
#
# Needs ghidra-emotionengine-reloaded installed in Ghidra's user Extensions
# folder (r5900:LE:32:default). GHIDRA_INSTALL_DIR overrides the Ghidra path.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
GHIDRA=${GHIDRA_INSTALL_DIR:-$HOME/ghidra/ghidra_12.1.4_PUBLIC}
ELF=$ROOT/disc/SLES_556.05
OUT=$ROOT/build/ghidra
SCRIPTS=$ROOT/third_party/PS2Recomp/ps2xRecomp/tools/ghidra

mkdir -p "$OUT/project"
# The export script asks for its two output files; headless mode answers
# askFile() from a .properties file next to the script, keyed by
# "<title> <button text>".
cat > "$SCRIPTS/ExportPS2Functions.properties" <<EOF
Choose output TOML config file Save = $OUT/ghidra.toml
Choose output CSV file Save = $OUT/functions.csv
EOF

# Keep the Mac cool: Ghidra's analysis is mostly single-threaded anyway, and
# the heap is capped so it does not swap.
MAXMEM=6G "$GHIDRA/support/analyzeHeadless" "$OUT/project" un5 \
    -import "$ELF" -overwrite \
    -processor r5900:LE:32:default \
    -analysisTimeoutPerFile 3600 \
    -scriptPath "$SCRIPTS" \
    -postScript ExportPS2Functions.java \
    -log "$OUT/ghidra.log" -scriptlog "$OUT/script.log"

echo "functions: $(($(wc -l < "$OUT/functions.csv") - 1))"
