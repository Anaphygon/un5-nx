#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0+
# Adds the shaders the deko3d renderer found missing, and any the OpenGL
# renderer harvested, to the deko3d pack in one go: pulls
# sdmc:/switch/un5-nx/shaders-dk-missing/ and the new files of shaders-gl/ from
# the Switch (switch-devtools running, over USB or Wi-Fi), compiles them with
# uam (tools/dk_shaders.py), pushes the new .dksh files to shaders-dk/ and
# empties the missing folder. Takes effect the next time the game starts.
#
#   tools/dk_sync.sh
#
# DEVTOOLS: switch-devtools' devtools.py (default ~/SwitchDev/nxdev/tools/devtools.py)
# DK_PACK:  the local pack, kept as the master copy
#           (default ~/SwitchDev/un5-nx-switch-bench/shaders-dk)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
devtools="${DEVTOOLS:-$HOME/SwitchDev/nxdev/tools/devtools.py}"
pack="${DK_PACK:-$HOME/SwitchDev/un5-nx-switch-bench/shaders-dk}"
sources="$(dirname "$pack")/shaders-dk-sources"
remote_missing="sdmc:/switch/un5-nx/shaders-dk-missing"
remote_pack="sdmc:/switch/un5-nx/shaders-dk"
remote_gl="sdmc:/switch/un5-nx/shaders-gl"

dt() { python3 "$devtools" "$@"; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$pack" "$sources"

echo "Reading the missing shaders from the Switch..."
mkdir -p "$work/missing"
if ! dt ls "$remote_missing" >"$work/list" 2>&1; then
	if grep -q "cannot reach" "$work/list"; then
		cat "$work/list"
		echo "Open switch-devtools on the Switch (and plug the cable in), then run this again."
		exit 1
	fi
	: >"$work/list"
fi
if grep -qE '\.(vert|frag)$' "$work/list"; then
	dt get "$remote_missing" "$work/missing" >/dev/null
fi

# Programs the OpenGL renderer harvested (shaders-gl/) that the pack lacks:
# playing a new mode with OpenGL fills the pack without the frozen picture
# deko3d shows while shaders are missing.
dt ls "$remote_gl" >"$work/gl" 2>/dev/null || : >"$work/gl"
for name in $(grep -oE '[0-9a-f]{16}\.(vert|frag)' "$work/gl" || true); do
	if [ ! -f "$pack/${name%.*}.dksh" ] && ! grep -qx "$name" "$sources/uncompilable.txt" 2>/dev/null; then
		dt get "$remote_gl/$name" "$work/missing/$name" >/dev/null
	fi
done

count=$(find "$work/missing" -type f \( -name '*.vert' -o -name '*.frag' \) | wc -l | tr -d ' ')
if [ "$count" -eq 0 ]; then
	echo "Nothing new: the pack already has every shader played so far."
	exit 0
fi
echo "$count missing shader(s) found; compiling..."
cp "$work/missing"/*.vert "$work/missing"/*.frag "$sources"/ 2>/dev/null || true
python3 "$here/dk_shaders.py" "$work/missing" -o "$pack" || true

sent=0
for src in "$work/missing"/*.vert "$work/missing"/*.frag; do
	[ -e "$src" ] || continue
	name="$(basename "${src%.*}")"
	if [ -f "$pack/$name.dksh" ]; then
		dt -y put "$pack/$name.dksh" "$remote_pack/$name.dksh" >/dev/null
		sent=$((sent + 1))
	else
		basename "$src" >>"$sources/uncompilable.txt"
	fi
done
if grep -qE '\.(vert|frag)$' "$work/list"; then
	dt -y rm -r "$remote_missing" >/dev/null
fi
echo "Done: $sent shader(s) added to the pack on the Switch. Restart the game to use them."
