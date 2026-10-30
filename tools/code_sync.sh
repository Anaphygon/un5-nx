#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# Translates the game code the Switch reached with no translation. When the
# game halts on such code (the shop's overlay, say, never played on the Mac),
# the runner leaves its RAM and the missed entries in
# sdmc:/switch/un5-nx/code-miss/. This pulls them (switch-devtools running),
# captures the overlay images in the RAM and seeds their entries as
# tools/un5_play.sh does after a halt on the Mac, translates with eerecomp,
# builds the NRO, installs it and empties the folder.
#
#   tools/code_sync.sh
#
# DEVTOOLS: switch-devtools' devtools.py (default ~/SwitchDev/nxdev/tools/devtools.py)
# PS2WEB:   the ps2-web-recomp checkout (default ~/SwitchDev/ps2-web-recomp)
# BUILD:    its Switch build folder (default $PS2WEB/build/switch-un5)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
devtools="${DEVTOOLS:-$HOME/SwitchDev/nxdev/tools/devtools.py}"
web="${PS2WEB:-$HOME/SwitchDev/ps2-web-recomp}"
build="${BUILD:-$web/build/switch-un5}"
G="$web/games/un5"
ELF="$G/disc/SLES_556.05"
remote="sdmc:/switch/un5-nx/code-miss"

dt() { python3 "$devtools" "$@"; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Reading what the Switch missed..."
if ! dt ls "$remote" >"$work/list" 2>&1; then
	if grep -q "cannot reach" "$work/list"; then
		cat "$work/list"
		echo "Open switch-devtools on the Switch (and plug the cable in), then run this again."
		exit 1
	fi
	: >"$work/list"
fi
if ! grep -q "ram.bin" "$work/list"; then
	echo "Nothing missed: the game has not halted on untranslated code."
	exit 0
fi
dt get "$remote/ram.bin" "$work/ram.bin" >/dev/null
dt get "$remote/misses.txt" "$work/misses.txt" >/dev/null 2>&1 || : >"$work/misses.txt"
# A jump through a null pointer reports address 0, which is no entry.
grep -vx '0' "$work/misses.txt" >"$work/misses" || true
echo "Missed: $(tr '\n' ' ' <"$work/misses")"

cd "$web"
python3 -I scripts/windows.py capture "$ELF" "$work/ram.bin" "$G/overlays" "$work/misses"
python3 -I "$here/un5_seed.py" "$ELF" "$G/entries.txt" "$G/overlays" "$work/misses"
build/tools/eerecomp "$ELF" --entries "$G/entries.txt" --overlays "$G/overlays" --out generated/un5 | tail -1

echo "Building the NRO..."
export PATH="$HOME/SwitchDev/.tools:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH"
ninja -C "$build" -j3 ps2run_nro >"$work/ninja.log" 2>&1 || { tail -20 "$work/ninja.log"; exit 1; }

echo "Installing it on the Switch..."
dt -y put "$build/ps2run.nro" sdmc:/switch/un5-nx/un5-nx.nro | tail -1
dt -y rm -r "$remote" >/dev/null
echo "Done. Start the game again from the homebrew menu and go back where it stopped."
