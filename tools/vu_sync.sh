#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# Translates the VU1 programs the Switch found with no translation. The game
# keeps each one in sdmc:/switch/un5-nx/vu1-new/ (the launcher turns
# PS2_VU_RECORD on); left to the interpreter they are about ten times slower.
# This pulls them (switch-devtools running, over USB or Wi-Fi), adds them to
# the game's VU1 programs, translates them with vurecomp, builds the NRO,
# installs it on the Switch and empties the folder.
#
#   tools/vu_sync.sh
#
# DEVTOOLS: switch-devtools' devtools.py (default ~/SwitchDev/nxdev/tools/devtools.py)
# PS2WEB:   the ps2-web-recomp checkout (default ~/SwitchDev/ps2-web-recomp)
# BUILD:    its Switch build folder (default $PS2WEB/build/switch-un5)
set -euo pipefail

devtools="${DEVTOOLS:-$HOME/SwitchDev/nxdev/tools/devtools.py}"
web="${PS2WEB:-$HOME/SwitchDev/ps2-web-recomp}"
build="${BUILD:-$web/build/switch-un5}"
programs="$web/games/un5/vu1"
remote="sdmc:/switch/un5-nx/vu1-new"

dt() { python3 "$devtools" "$@"; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Reading the new VU1 programs from the Switch..."
if ! dt ls "$remote" >"$work/list" 2>&1; then
	if grep -q "cannot reach" "$work/list"; then
		cat "$work/list"
		echo "Open switch-devtools on the Switch (and plug the cable in), then run this again."
		exit 1
	fi
	: >"$work/list"
fi
new=0
for name in $(grep -oE '[0-9A-F]{16}_[0-9A-F]{4}\.bin' "$work/list" | sort -u); do
	if [ ! -f "$programs/$name" ]; then
		dt get "$remote/$name" "$programs/$name" >/dev/null
		new=$((new + 1))
	fi
done
if [ "$new" -eq 0 ]; then
	echo "Nothing new: every VU1 program played so far is translated."
	exit 0
fi
echo "$new new VU1 program(s); translating..."
(cd "$web" && build/tools/vurecomp --rz "$programs" generated/un5 | tail -1)

echo "Building the NRO..."
export PATH="$HOME/SwitchDev/.tools:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH"
ninja -C "$build" -j3 ps2run_nro >"$work/ninja.log" 2>&1 || { tail -20 "$work/ninja.log"; exit 1; }

echo "Installing it on the Switch..."
dt -y put "$build/ps2run.nro" sdmc:/switch/un5-nx/un5-nx.nro | tail -1
dt -y rm -r "$remote" >/dev/null
echo "Done: $new program(s) translated. Start the game again from the homebrew menu."
