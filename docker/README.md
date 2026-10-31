# Building un5-nx from your own disc

The builder turns your copy of **Naruto Shippuden: Ultimate Ninja 5**
(Europe, SLES-55605) into the Switch homebrew. It contains no game data and
no translated game code: both come from your ISO when you run it, and only
on your machine.

## What you need

- Docker (Docker Desktop, Colima or Linux), about 8 GB of disk for the image
- Your own ISO of the game, dumped from your disc. The builder checks its
  files by SHA-256 and stops on any other release.

## Build

```sh
git clone https://github.com/Anaphygon/un5-nx && cd un5-nx
docker build -t un5-nx-builder -f docker/Dockerfile .
docker run --rm -v "/path/to/your.iso:/in/game.iso:ro" -v "$PWD/out:/out" un5-nx-builder
```

The image build fetches devkitPro's toolchain, Mesa 20.1, uam, PCSX2's GS
sources and ps2-web-recomp, each at a pinned version, and builds them once.
The run translates the game (seconds) and compiles the NRO (the longest step;
`-e JOBS=4` caps the cores it uses).

## Install

Copy `out/switch/un5-nx` to `sdmc:/switch/un5-nx` on the SD card and put
your ISO in that folder as `disc.iso`. Start **un5-nx** from the homebrew menu
through title override (hold R while opening a game) so it gets the full
memory.

## How the game is translated

`tools/un5_from_iso.sh` does it from the disc alone; nothing is played first.
The executable and the three overlays in `PRG/` are recompiled to C++ by
ps2-web-recomp's `eerecomp`, from every function start their layout gives
(`tools/un5_entries.py`, `tools/un5_seed.py`), and the VU1 microprograms are
rebuilt from the upload blocks in the executable (`tools/un5_vu1.py`) and
compiled by `vurecomp`.
