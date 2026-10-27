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
