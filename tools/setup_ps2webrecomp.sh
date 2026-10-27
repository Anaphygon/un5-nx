#!/bin/sh
# Fetch ps2-web-recomp at the pinned commit, apply this project's patches on a
# branch named un5-nx, and port your disc with it (native build only).
#
#   tools/setup_ps2webrecomp.sh <your UN5 PAL ISO>
#                                   -> third_party/ps2-web-recomp (branch un5-nx)
#
# The patches in patches/ps2-web-recomp are fixes found while porting this game,
# written to be sent upstream; see docs/PHASE3.md. Set PS2WEB to use a checkout
# somewhere else.
set -eu
[ $# -eq 1 ] || { sed -n '2,10p' "$0"; exit 1; }
ISO=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")/.."
ROOT=$(pwd)
WEB=${PS2WEB:-$ROOT/third_party/ps2-web-recomp}
PIN=79d8eab
JOBS=${JOBS:-$(( $(sysctl -n hw.ncpu 2>/dev/null || nproc) / 2 ))}

if [ ! -d "$WEB/.git" ]; then
    git clone https://github.com/snuri00/ps2-web-recomp "$WEB"
fi
if ! git -C "$WEB" rev-parse --verify -q un5-nx >/dev/null; then
    git -C "$WEB" checkout -q -b un5-nx "$PIN"
    git -C "$WEB" -c user.name=un5-nx -c user.email=un5-nx@localhost am -q "$ROOT"/patches/ps2-web-recomp/*.patch
    echo "ps2-web-recomp $PIN + $(ls "$ROOT"/patches/ps2-web-recomp/*.patch | wc -l | tr -d ' ') patches on branch un5-nx"
fi

# The static constructors are reached only through a table, so seed them
# before the first exploring run.
mkdir -p "$WEB/games/un5"
grep -v '^#' "$ROOT/ports/un5/entries.txt" > "$WEB/games/un5/entries.txt"

# ps2-web-recomp's own builds take every core; ninja honours this wrapper.
BIN=$(mktemp -d)
printf '#!/bin/sh\nexec %s -j%s "$@"\n' "$(command -v ninja)" "$JOBS" > "$BIN/ninja"
chmod +x "$BIN/ninja"

cd "$WEB"
PATH=$BIN:$PATH bash scripts/setup.sh --native-only
presses=""
f=600
while [ $f -lt 9000 ]; do
    presses="$presses --press ${f}:cross --press $((f + 150)):start"
    f=$((f + 300))
done
# shellcheck disable=SC2086
PATH=$BIN:$PATH bash scripts/port.sh un5 "$ISO" --native-only --frames 9000 --rounds 16 $presses
rm -rf "$BIN"
echo "Play it with tools/un5_play.sh"
