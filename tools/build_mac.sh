#!/bin/sh
# Build the game for macOS arm64 from the recompiled output:
#   1. copy build/recomp/output into PS2Recomp's runner folder and its two
#      headers onto the runtime include path (only files that changed, so
#      the build stays incremental),
#   2. configure and build ps2EntryRunner.
#
#   tools/build_mac.sh            -> build/mac/ps2xRuntime/ps2EntryRunner
#
# JOBS defaults to half the cores: a full build of ~8,800 generated files
# at full speed overheats the Mac.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REC=$ROOT/third_party/PS2Recomp
GEN=$ROOT/build/recomp/output
RUNNER=$REC/ps2xRuntime/src/runner
BUILD=$ROOT/build/mac
JOBS=${JOBS:-$(( $(sysctl -n hw.ncpu) / 2 ))}

[ -d "$GEN" ] || { echo "no $GEN; run ps2_recomp first" >&2; exit 1; }

[ "$(git -C "$REC" rev-parse --abbrev-ref HEAD)" = un5-nx ] ||
    { echo "PS2Recomp is not on branch un5-nx; run tools/setup_ps2recomp.sh" >&2; exit 1; }

mkdir -p "$RUNNER"
changed=0
copy() { if ! cmp -s "$1" "$2"; then cp "$1" "$2"; changed=$((changed + 1)); fi; }
for f in "$GEN"/*.cpp; do copy "$f" "$RUNNER/$(basename "$f")"; done
# The generated sources include these with <...>, so they go on the
# runtime's include path rather than next to the sources.
before=$changed
for f in "$GEN"/*.h; do copy "$f" "$REC/ps2xRuntime/include/$(basename "$f")"; done
# Those headers are in the runner's precompiled header, and ninja does not
# rebuild the PCH when they change ("modified since the precompiled header
# was built"), so drop it and let it be rebuilt.
if [ "$changed" -gt "$before" ]; then
    rm -f "$BUILD"/ps2xRuntime/CMakeFiles/ps2EntryRunner.dir/cmake_pch.hxx.pch
fi
rm -f "$RUNNER"/ps2_recompiled_*.h
# Drop runner files the recompiler no longer produces (a function that was
# split or removed would otherwise still be compiled and registered).
for f in "$RUNNER"/*_0x*.cpp; do
    [ -e "$GEN/$(basename "$f")" ] || rm -f "$f"
done
echo "$changed generated files updated in the runner"

if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$REC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
        -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=8
fi
ninja -C "$BUILD" -j"$JOBS" ps2EntryRunner
