#!/bin/sh
# Fetch PS2Recomp at the pinned commit, apply this project's patches on a
# branch named un5-nx, and build the recompiler and analyzer.
#
#   tools/setup_ps2recomp.sh        -> third_party/PS2Recomp (branch un5-nx)
#
# The patches in patches/ps2recomp are fixes found while porting this game
# (written to be sent upstream) and a few diagnostics; see docs/PHASE2.md.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REC=$ROOT/third_party/PS2Recomp
PIN=e563698
JOBS=${JOBS:-$(( $(sysctl -n hw.ncpu 2>/dev/null || nproc) / 2 ))}

if [ ! -d "$REC/.git" ]; then
    git clone https://github.com/ran-j/PS2Recomp "$REC"
fi
if ! git -C "$REC" rev-parse --verify -q un5-nx >/dev/null; then
    git -C "$REC" checkout -q -b un5-nx "$PIN"
    git -C "$REC" -c user.name=un5-nx -c user.email=un5-nx@localhost am -q "$ROOT"/patches/ps2recomp/*.patch
    echo "PS2Recomp $PIN + $(ls "$ROOT"/patches/ps2recomp/*.patch | wc -l | tr -d ' ') patches on branch un5-nx"
fi

cmake -S "$REC" -B "$REC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
ninja -C "$REC/build" -j"$JOBS" ps2_recomp ps2_analyzer
