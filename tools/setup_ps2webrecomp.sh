#!/usr/bin/env bash
# Fetch ps2-web-recomp at the pinned commit, apply this project's patches on a
# branch named un5-nx, translate your disc with tools/un5_from_iso.sh and build
# the native player (macOS).
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

# ps2-web-recomp's own builds take every core; ninja honours this wrapper.
BIN=$(mktemp -d)
printf '#!/bin/sh\nexec %s -j%s "$@"\n' "$(command -v ninja)" "$JOBS" > "$BIN/ninja"
chmod +x "$BIN/ninja"

(cd "$WEB" && PATH=$BIN:$PATH bash scripts/setup.sh --native-only)
PS2WEB=$WEB "$ROOT/tools/un5_from_iso.sh" "$ISO"
cd "$WEB"
. scripts/cxx.sh
cmake -S . -B build/native-un5 -G Ninja -DCMAKE_BUILD_TYPE=Release "${cxx_flags[@]}" -DPS2_GENERATED="$WEB/generated/un5" > /dev/null
PATH=$BIN:$PATH ninja -C build/native-un5 ps2run | tail -1
rm -rf "$BIN"
echo "Play it with tools/un5_play.sh"
