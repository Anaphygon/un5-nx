#!/bin/sh
# Play UN5 on the ps2-web-recomp build, and learn from where it goes.
#
#   tools/un5_play.sh [runner options, e.g. --widescreen]
#
# The game is translated from the disc (tools/un5_from_iso.sh), so this should
# never stop on missing code. If it does, the misses join ports/un5/entries.txt
# (tools/un5_misses.py), VU1 programs the run recorded join the ones built from
# the executable, the game is translated again and the runner rebuilt.
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
    echo "== stopped on untranslated code; adding it to ports/un5/entries.txt"
    python3 -I "$ROOT/tools/un5_misses.py" "$G/play_ram.bin" "$G/misses.txt" >>"$ROOT/ports/un5/entries.txt"
    rebuild=1
fi
vu_after=$(ls "$G/vu1" | wc -l)
if [ "$vu_after" -gt "$vu_before" ]; then
    echo "== $((vu_after - vu_before)) VU1 programs ran interpreted; adding them"
    rebuild=1
fi
[ "$rebuild" = 1 ] && PS2WEB=$WEB "$ROOT/tools/un5_from_iso.sh" "$G/disc.iso" | tail -1
[ "$rebuild" = 1 ] || exit 0
ninja -C build/native-un5 -j"$JOBS" ps2run > build/native-un5/ninja.log 2>&1 || { tail -20 build/native-un5/ninja.log; exit 1; }
echo "== built; run tools/un5_play.sh again"
