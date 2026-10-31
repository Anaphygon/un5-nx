#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# Opens UN5's DATA/DATA.CVM for study: decrypts its system area with the game's
# own code (tools/cvm_decrypt.cpp) and extracts every file, gzip-compressed CCS
# assets for the most part, into the output folder. Everything it writes comes
# from your disc: keep it on your machine.
#
#   tools/cvm_decrypt.sh <out dir>
#
# Needs the native build (tools/setup_ps2webrecomp.sh or tools/un5_from_iso.sh
# plus build/native-un5). PS2WEB: the ps2-web-recomp checkout.
set -euo pipefail
[ $# -eq 1 ] || { sed -n '2,11p' "$0"; exit 1; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
here="$(cd "$(dirname "$0")" && pwd)"
web=${PS2WEB:-$HOME/SwitchDev/ps2-web-recomp}
G=$web/games/un5
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$web"

echo "Booting the game to mount the volume and keep its key..."
PS2_DUMP=$work/ram.bin build/native-un5/ps2run "$G/disc/SLES_556.05" --disc "$G/disc.iso" --memcard "$work/card.ps2" --frames 1200 >/dev/null 2>&1

echo "Building the decrypter..."
clang++ -std=c++20 -O2 -Iruntime/include "$here/cvm_decrypt.cpp" build/native-un5/libps2game.a build/native-un5/libps2runtime.a \
	third_party/wgpu-native/lib/libwgpu_native.a -framework Metal -framework QuartzCore -framework Foundation \
	-framework CoreGraphics -framework IOKit -framework IOSurface -o "$work/cvm_decrypt"

echo "Decrypting..."
python3 -I scripts/disc.py extract "$G/disc.iso" "$work/disc" 'DATA/DATA.CVM' >/dev/null
# SLES-55605's volume: descriptors and directories end before sector 117,
# where the first file starts.
"$work/cvm_decrypt" "$work/ram.bin" "$work/disc/DATA/DATA.CVM" "$work/data.iso" 117 2>&1 | tail -1
python3 -I scripts/disc.py extract "$work/data.iso" "$out" '*' | tail -1
