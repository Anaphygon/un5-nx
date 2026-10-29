#!/bin/sh
# Play UN5 on the ps2-web-recomp build, and learn from where it goes.
#
#   tools/un5_play.sh [runner options, e.g. --widescreen]
#
# The run records its misses, a RAM dump and the VU1 programs it uploads. If
# the game halts on code that is not translated yet, the overlay images in the
# dump are captured and the misses and every function pointer into an overlay
# are added as entries (tools/un5_seed.py). Either way, code and VU1 programs
# seen for the first time are translated and the runner is built again: VU1
# programs left to the interpreter cost a battle about 40% of its frame time.
# F5 and F9 save and load quicksave.state. PS2_SCALE=2 or 3 raises the
# internal resolution.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
WEB=${PS2WEB:-$ROOT/third_party/ps2-web-recomp}
JOBS=${JOBS:-$(( $(sysctl -n hw.ncpu 2>/dev/null || nproc) / 2 ))}
G=$WEB/games/un5
ELF=$G/disc/SLES_556.05

cd "$WEB"
mkdir -p "$G/vu1"
vu_before=$(ls "$G/vu1" | wc -l)
rm -f "$G/misses.txt" "$G/play_ram.bin"
PS2_DUMP=$G/play_ram.bin PS2_VU_RECORD=$G/vu1 build/native-un5/ps2run "$ELF" --disc "$G/disc.iso" --window \
    --memcard "$G/play.ps2" --misses "$G/misses.txt" "$@" 2>&1 | tee "$G/play.log" | grep -v '^\[loadfile\] buffer' || true

rebuild=0
if grep -q '^\[ee\] halted: missing code' "$G/play.log"; then
    echo "== stopped on untranslated code; translating what this run reached"
    python3 -I scripts/windows.py capture "$ELF" "$G/play_ram.bin" "$G/overlays" "$G/misses.txt"
    python3 -I "$ROOT/tools/un5_seed.py" "$ELF" "$G/entries.txt" "$G/overlays" "$G/misses.txt"
    build/tools/eerecomp "$ELF" --entries "$G/entries.txt" --overlays "$G/overlays" --out generated/un5 | tail -1
    rebuild=1
fi
vu_after=$(ls "$G/vu1" | wc -l)
if [ "$vu_after" -gt "$vu_before" ]; then
    echo "== $((vu_after - vu_before)) new VU1 programs; translating them"
    build/tools/vurecomp --rz "$G/vu1" generated/un5 | tail -1
    rebuild=1
fi
[ "$rebuild" = 1 ] || exit 0
ninja -C build/native-un5 -j"$JOBS" ps2run > build/native-un5/ninja.log 2>&1 || { tail -20 build/native-un5/ninja.log; exit 1; }
echo "== built; run tools/un5_play.sh again"
