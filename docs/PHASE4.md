# Phase 4: the Switch (in progress)

## Where it stands

The game builds into a Switch NRO and runs in an emulator: language select,
input and in game, with sound. It is far too slow to play, because the GS is
drawn in software on one CPU core. Nothing has run on a Switch yet.

| Step | State |
|---|---|
| Runtime and generated game code build with devkitA64 (GCC 16.1) | done, patch 0006 |
| NRO packaging; finds its files next to itself on the SD card | done, patches 0006 and 0008 |
| Boots in an emulator (Astris 1.0.14, Ryujinx-based, Apple silicon) | done: language select, in game |
| Runs on a Switch | not tried |
| GS on the Switch GPU (OpenGL 4.3 compute) | shaders translated (patch 0007), backend not written |
| Render and VU threads on cores of their own | blocked, see below |

## Building

```sh
export DEVKITPRO=/opt/devkitpro
cd third_party/ps2-web-recomp       # after tools/setup_ps2webrecomp.sh
cmake -S . -B build/switch-un5 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release -DPS2_GENERATED=$PWD/generated/un5 -DPS2_NRO_NAME=un5-nx
ninja -C build/switch-un5 -j3       # about 15 minutes on an M5 MacBook Air
```

Copy to the SD card, in `sdmc:/switch/un5-nx/`:

- `build/switch-un5/ps2run.nro` as `un5-nx.nro`
- `games/un5/disc/SLES_556.05` as `game.elf`
- your ISO as `disc.iso` (1.9 GB, fits FAT32)

The NRO writes `ps2run.log` and `memcard.ps2` there. Start it through title
override (hold R while opening a game) for the full memory: it needs about
144 MB for its code and data before the heap.

## What it took

- `[[clang::musttail]]`, on 6,875 calls in the generated code, is honoured by
  devkitA64's GCC 16: it emits a branch, not a call, even at `-O0`.
- `vu_ops.hpp` used clang's `__builtin_elementwise_min` and
  `__builtin_reduce_or`; GCC gets equivalent fallbacks.
- wgpu-native has no Switch build, so the Switch build skips it and the GS
  renders in software; `getrusage` and `/proc` are skipped.
- Astris starts an NRO with `argc = 0`. Found with a probe NRO that writes its
  arguments to the SD card; the folder now falls back to
  `sdmc:/switch/<PS2_NRO_NAME>/`.
- Astris writes no log of its own here, so the NRO's `ps2run.log` (stderr,
  unbuffered) and probe NROs were the only window into it.

## Measurements (M5 MacBook Air, a battle replayed headless)

`PS2_TIME=7500,200` from a quick save at the battle's VS screen, with the
same scripted input each time:

| Configuration | ms per frame | slowest frame |
|---|---|---|
| Software GS | 93 | 288 |
| Software GS, no primitives drawn (`PS2_GS_NULL`) | 93 | 430 |
| WebGPU GS | 20.5 | 64 |
| WebGPU GS, battle VU1 programs translated | **12.5** | 57 |
| Software GS, battle VU1 programs translated | 85 | 305 |

- Only 7 VU1 programs (intro and title) had been recorded; a battle uses 39.
  Left to the interpreter they cost the WebGPU run 40% of its frame time.
  `tools/un5_play.sh` now records new VU1 programs every session and
  translates them.
- In the software GS, drawing costs little. Two thirds of the time is the
  textures UN5 uploads again every frame, written pixel by pixel into GS
  memory (`dmac → vif → gif::render → write_pixel`). The WebGPU GS skips
  uploads whose content it already holds; the software GS has no such cache.
- The WebGPU GS spends 77% of its GPU time in `raster`, the compute shader
  that rasterises every primitive. A Switch GPU is roughly a tenth of the
  M5's, so the same design would land around 4 to 10 frames per second
  there. That is an estimate, not a measurement.

Checked in Astris (the NRO's ARM code runs on the M5's CPU there): the
software-GS bench gives 90 ms per frame, as on the Mac, so the bench itself
works end to end; only a Switch gives the numbers that decide.

To run it on a Switch, copy into `sdmc:/switch/un5-nx/` the NRO, `game.elf`,
`disc.iso`, the quick save `battle.state` and one of the two argument files
as `args.txt` (`args-full.txt` first, then `args-null.txt`), start the NRO
through title override and wait until `ps2run.log` ends in `stopped after
8100 frames`. The `[time]` lines are the result. Without `args.txt` the NRO
starts the game normally.

### On a Switch (2026-10-27 evening)

HOS 23.0.0, Atmosphère 1.12.0, stock clocks, title override, one thread,
the bench above with `PS2_TIME=6880,20` (every 20 frames from the quick save):

| Frames | Software GS | No drawing (`PS2_GS_NULL`) |
|---|---|---|
| 6880-7180 (VS screen, loading) | 80-500 ms | 4-100 ms |
| 7200-7680 (fighting) | **1,050-1,700 ms** (~0.7 fps) | **96-128 ms** (~9 fps) |

- On the Switch the software rasteriser is ~90% of the frame; on the M5 it
  was negligible. The GS has to move to the GPU.
- The rest of the machine (EE, IOP, VU1 code, texture uploads) costs about
  110 ms per frame on one A57 core, 13-14 times the M5. Full speed needs
  20 ms: about 5x. The render and VU threads on other cores could give
  1.5-2x; the rest has to come from optimisation (an upload cache like the
  WebGPU GS has, the VU1 and EE hot paths), none of it measured yet. 20-30
  fps is a realistic aim after that work; 50 fps is not assured.
- No crash in either run.

**Decision (2026-10-27):** measure on a Switch before choosing the renderer.
The NRO reads `args.txt` next to it, so the two runs below need no rebuild:
the first times the whole machine with the software GS, the second without
drawing. If the second is already slower than 20 ms per frame, the CPU side
has to get faster before any renderer matters; if not, a hardware-rasterising
GL renderer (PCSX2-style) is the way to full speed.

## Open

- **Threads after SDL.** In the emulator, the first `std::thread` (or 32 MB
  allocation) after SDL has brought up the GPU never returns, while a small
  NRO creates libnx and `std::thread` threads fine, and the game runs
  headless (no SDL) at about 60 frames per second there. Every stall was in an
  allocation, which points at the allocator lock and a thread SDL or mesa
  started. For now the Switch runs on one thread (`PS2_ONE_THREAD`). Next:
  create the render and VU threads before SDL starts, or with libnx on other
  cores.
- **GS on the GPU.** `scripts/glsl.sh` turns the nine compute shaders into
  GLSL 4.30 that passes glslangValidator; `gpu.cpp`'s wgpu calls sit in about
  seven functions (buffers, dispatch, submit, read back, present), so an
  OpenGL device beside the wgpu one is the plan.
- Emulator frame rates say nothing about the Switch: Astris runs the NRO's
  ARM code directly on the Mac's CPU.
