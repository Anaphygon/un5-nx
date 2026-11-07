#!/usr/bin/env zsh
# SPDX-License-Identifier: GPL-3.0+
# The deko3d shader harvest on the Mac. PCSX2's GS with the Switch's deko3d
# device (nothing behind deko3d: gs-pcsx2/host, src/host/) is linked into the
# Mac runner as its external GS; the runner plays UN5 headless, unpaced, and
# the device writes the source of every shader its pack lacks. Those sources
# are the Switch's own (same GS, same device code, same PCSX2 resources), so
# tools/dk_shaders.py builds them into the pack with uam. The Switch is not
# used.
#
#   tools/dk_harvest.sh [UPSCALE,FILTER,BLEND,FIX ...]
#
# Each setting set (the launcher's: resolution 1/1.5/2/3, filter 0-2,
# blending 0 minimum or 1, upscale fix 0-6; default 1.5,2,0,6) gets the
# characters and stages sweeps (tools/dk_harvest_rounds.py), JOBS processes
# each. It needs, in STATES: harvest-select.state, stage-select.state and the
# all-unlocked save's card full-memcard.ps2 (made on the Mac from your game;
# not in this repository).
#
# PS2WEB: ps2-web-recomp with the game translated (default ~/SwitchDev/ps2-web-recomp)
# STATES: the states and card (default ~/SwitchDev/un5-nx-switch-bench/replay)
# PACK:   the shader pack, read and added to (default ~/SwitchDev/un5-nx-switch-bench/shaders-dk)
# JOBS:   processes at once (default 6; each takes one core)
set -euo pipefail
here=${0:A:h}
gs=$here/../gs-pcsx2
web=${PS2WEB:-$HOME/SwitchDev/ps2-web-recomp}
states=${STATES:-$HOME/SwitchDev/un5-nx-switch-bench/replay}
pack=${PACK:-$HOME/SwitchDev/un5-nx-switch-bench/shaders-dk}
jobs=${JOBS:-6}
resources=${PCSX2_SRC:-$HOME/SwitchDev/third_party_src/pcsx2}/bin/resources
(( $# )) || set -- 1.5,2,0,6

echo "Building PCSX2's GS for the Mac..."
cmake -S $gs/host -B $gs/build-host -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C $gs/build-host -j$jobs | tail -1
cmake $web/build/native-un5 -DPS2_EXTRA_SOURCES=$gs/register/register_un5gs.cpp \
	"-DPS2_EXTRA_LIBRARIES=$gs/build-host/libpcsx2gs_host.a;$(brew --prefix freetype)/lib/libfreetype.dylib" >/dev/null
ninja -C $web/build/native-un5 -j$jobs ps2run | tail -1

work=$(mktemp -d)
trap 'rm -rf $work' EXIT
mkdir -p $work/new
for set in "$@"; do
	IFS=, read -r up filter blend fix <<<"$set"
	echo "Settings $set: characters and stages..."
	for sweep in characters stages; do
		state=$states/$([[ $sweep == characters ]] && echo harvest-select || echo stage-select).state
		for (( i = 0; i < jobs; ++i )); do
			(
				d=$work/$set-$sweep-$i
				mkdir -p $d
				cp $states/full-memcard.ps2 $d/card.ps2
				args=("${(@f)$(python3 $here/dk_harvest_rounds.py $sweep --part $i --parts $jobs)}")
				cd $web
				env PS2_EXTERNAL_GS=1 PS2_UNPACED=1 PS2_ONE_THREAD=1 UN5GS_RENDERER=dk UN5GS_NATIVE=0 UN5_SHOW_FPS=0 \
					UN5GS_UPSCALE=$up UN5GS_FILTER=$filter UN5GS_BLEND=$blend UN5GS_FIX=$fix \
					UN5DK_PACK=$pack UN5DK_MISSING=$d/missing UN5GS_RESOURCES=$resources \
					build/native-un5/ps2run games/un5/disc/SLES_556.05 --disc games/un5/disc.iso --window \
					--memcard $d/card.ps2 --load-state $state $args >$d/log.txt 2>&1 || true
				# It ends at its frame limit; the GS's teardown then crashes, harmlessly.
				grep -q 'frame limit reached' $d/log.txt || echo "  $sweep part $i did not finish: $d/log.txt"
				found=($d/missing/*(N))
				(( ! $#found )) || cp -n $found $work/new/
			) &
		done
		wait
	done
done
echo "$(ls $work/new | wc -l | tr -d ' ') new shader sources; building them..."
python3 $here/dk_shaders.py $work/new -o $pack | tail -1
echo "Pack: $(ls $pack/*.dksh | wc -l | tr -d ' ') shaders in $pack"
