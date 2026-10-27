#!/bin/sh
# Play UN5 on the ps2-web-recomp build, and learn from where it stops.
#
#   tools/un5_play.sh [runner options, e.g. --widescreen]
#
# The run records its misses and a RAM dump. If the game halts on code that is
# not translated yet, the overlay images in the dump are captured, the misses
# and every function pointer into an overlay are added as entries
# (tools/un5_seed.py), and the game is translated and built again; play again
# to continue. F5 and F9 save and load quicksave.state. PS2_SCALE=2 or 3 raises
# the internal resolution.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
WEB=${PS2WEB:-$ROOT/third_party/ps2-web-recomp}
JOBS=${JOBS:-$(( $(sysctl -n hw.ncpu 2>/dev/null || nproc) / 2 ))}
G=$WEB/games/un5
ELF=$G/disc/SLES_556.05

cd "$WEB"
rm -f "$G/misses.txt" "$G/play_ram.bin"
PS2_DUMP=$G/play_ram.bin build/native-un5/ps2run "$ELF" --disc "$G/disc.iso" --window \
    --memcard "$G/play.ps2" --misses "$G/misses.txt" "$@" 2>&1 | tee "$G/play.log" | grep -v '^\[loadfile\] buffer' || true

grep -q '^\[ee\] halted: missing code' "$G/play.log" || exit 0
echo "== stopped on untranslated code; translating what this run reached"
python3 -I scripts/windows.py capture "$ELF" "$G/play_ram.bin" "$G/overlays" "$G/misses.txt"
python3 -I "$ROOT/tools/un5_seed.py" "$ELF" "$G/entries.txt" "$G/overlays" "$G/misses.txt"
build/tools/eerecomp "$ELF" --entries "$G/entries.txt" --overlays "$G/overlays" --out generated/un5 | tail -1
ninja -C build/native-un5 -j"$JOBS" ps2run > build/native-un5/ninja.log 2>&1 || { tail -20 build/native-un5/ninja.log; exit 1; }
echo "== built; run tools/un5_play.sh again"
