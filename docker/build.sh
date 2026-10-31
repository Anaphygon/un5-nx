#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# The un5-nx builder's entry point: translates the game from your ISO, builds
# the NRO and lays out the folder to copy to the SD card.
#
#   build.sh [ISO (default /in/game.iso)] [out dir (default /out)]
#
# JOBS: parallel compile jobs (default: every core)
set -euo pipefail
ISO=${1:-/in/game.iso}
OUT=${2:-/out}
JOBS=${JOBS:-$(nproc)}
web=$PS2WEB
mesa=/un5/mesa/opt/devkitpro/portlibs/switch
portlibs=/opt/devkitpro/portlibs/switch

[ -f "$ISO" ] || { echo "No ISO at $ISO: mount yours with -v /path/to/your.iso:/in/game.iso:ro"; exit 1; }

/un5/tools/un5_from_iso.sh "$ISO"

echo "Building the NRO (this takes a while)..."
libs=(
	/un5/gs-pcsx2/build/libpcsx2gs.a "$mesa/lib/libEGL.a" "$mesa/lib/libglapi.a" "$portlibs/lib/libdrm_nouveau.a"
	"$portlibs/lib/libfreetype.a" "$portlibs/lib/libharfbuzz.a" "$portlibs/lib/libfreetype.a" "$portlibs/lib/libbz2.a"
	"$portlibs/lib/libpng16.a" "$portlibs/lib/libz.a" /opt/devkitpro/libnx/lib/libdeko3dd.a /un5/libuam-isolated.a
)
cmake -S "$web" -B "$web/build/switch-un5" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
	-DPS2_GENERATED="$web/generated/un5" -DPS2_NRO_NAME=un5-nx \
	-DPS2_EGL="$mesa/lib/libEGL.a" -DPS2_GLAPI="$mesa/lib/libglapi.a" -DPS2_EGL_INCLUDE="$portlibs/include" \
	-DPS2_DRM_NOUVEAU="$portlibs/lib/libdrm_nouveau.a" \
	-DPS2_EXTRA_SOURCES=/un5/gs-pcsx2/register/register_un5gs.cpp \
	-DPS2_EXTRA_LIBRARIES="$(IFS=';'; echo "${libs[*]}")" >/dev/null
ninja -C "$web/build/switch-un5" -j"$JOBS" ps2run_nro | tail -1

dest="$OUT/switch/un5-nx"
mkdir -p "$dest/resources"
cp "$web/build/switch-un5/ps2run.nro" "$dest/un5-nx.nro"
cp "$web/games/un5/disc/SLES_556.05" "$dest/game.elf"
cp -R /un5/pcsx2/bin/resources/shaders "$dest/resources/"
cp -R /un5/switch/shaders-dk "$dest/"
cat <<EOF
Done. Copy $(basename "$OUT")/switch/un5-nx to sdmc:/switch/un5-nx on the SD card,
and your ISO next to it as disc.iso. Start un5-nx from the homebrew menu
through title override (hold R while opening a game) for the full memory.
EOF
