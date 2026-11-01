#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# Translates Naruto Shippuden: Ultimate Ninja 5 (PAL, SLES-55605) from the
# disc alone: the executable, its three overlays (PRG/BTL.BIN, ADV.BIN,
# ETC.BIN) and every VU1 program the executable uploads. Nothing has to be
# played first: functions are found by their layout (tools/un5_entries.py,
# tools/un5_seed.py) and VU1 programs from the upload blocks in the
# executable (tools/un5_vu1.py). The game's files are checked by SHA-256, so
# any other disc or release stops here.
#
#   tools/un5_from_iso.sh <your UN5 PAL ISO>
#
# PS2WEB: the ps2-web-recomp checkout on branch un5-nx (default
#         third_party/ps2-web-recomp, as tools/setup_ps2webrecomp.sh makes it,
#         or ~/SwitchDev/ps2-web-recomp when that exists)
# Writes $PS2WEB/games/un5/{disc,overlays-iso,entries-static.txt} and the
# translation in $PS2WEB/generated/un5. Build the NRO or the native player
# from there.
set -euo pipefail
[ $# -eq 1 ] || { sed -n '2,19p' "$0"; exit 1; }
ISO=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
if [ -n "${PS2WEB:-}" ]; then web=$PS2WEB
elif [ -d "$root/third_party/ps2-web-recomp/.git" ]; then web=$root/third_party/ps2-web-recomp
else web=$HOME/SwitchDev/ps2-web-recomp
fi
G="$web/games/un5"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Reading the disc..."
python3 -I "$web/scripts/disc.py" extract "$ISO" "$work/disc" SLES_556.05 SYSTEM.CNF 'PRG/BTL.BIN' 'PRG/ADV.BIN' 'PRG/ETC.BIN' >/dev/null
if command -v sha256sum >/dev/null; then sha() { sha256sum "$1" | cut -d' ' -f1; }
else sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi
check() {
	[ "$(sha "$work/disc/$1")" = "$2" ] || { echo "$1 is not the one of Naruto Shippuden: Ultimate Ninja 5 (Europe, SLES-55605)."; exit 1; }
}
check SLES_556.05 20a43677397731a2a20899336d1165ace5b436906b9b89be90fb10f4558dd19d
check PRG/BTL.BIN 7e8518da7bd4957af18cb0ababe67f0e9b37c42c6551375201b15997f0a3dfe3
check PRG/ADV.BIN 7e2af55362141bb1b055247cd7ef7edae290f3c0095701bc51467f096a2d00b8
check PRG/ETC.BIN bdb6bda1f9d335047586a263e478486c8e7924b91fa972b6f3e58caec5ea0778

mkdir -p "$G/disc" "$G/overlays-iso"
cp "$work/disc/SLES_556.05" "$work/disc/SYSTEM.CNF" "$G/disc/"
ln -sf "$ISO" "$G/disc.iso"
# The three overlays load at 0x6C6D00, the window scripts/windows.py names.
for kind in BTL ADV ETC; do cp "$work/disc/PRG/$kind.BIN" "$G/overlays-iso/006C6D00_$kind.bin"; done

echo "Finding the functions..."
python3 -I "$here/un5_entries.py" "$G/disc/SLES_556.05" "$G/overlays-iso" "$G/entries-static.txt"
grep -v '^#' "$root/ports/un5/entries.txt" >>"$G/entries-static.txt"
python3 -I "$here/un5_seed.py" "$G/disc/SLES_556.05" "$G/entries-static.txt" "$G/overlays-iso"

echo "Translating the EE code..."
(cd "$web" && build/tools/eerecomp "$G/disc/SLES_556.05" --entries "$G/entries-static.txt" --overlays "$G/overlays-iso" --out generated/un5 | tail -2)

echo "Translating the VU1 programs..."
python3 -I "$here/un5_vu1.py" "$G/disc/SLES_556.05" "$work/vu1"
# Programs the Switch recorded (tools/vu_sync.sh) join them: code an earlier,
# longer upload left in VU memory is out of the executable's reach.
if [ -d "$G/vu1" ]; then cp "$G"/vu1/*.bin "$work/vu1/" 2>/dev/null || true; fi
(cd "$web" && build/tools/vurecomp --rz --snapshots "$work/vu1" generated/un5 | tail -1)
# Hand-written stand-ins for the hottest VU1 programs, linked into the player.
ln -sf "$here/../ports/un5/vu1_native.cpp" "$web/generated/un5/native_vu1.cpp"
echo "Done: $web/generated/un5"
