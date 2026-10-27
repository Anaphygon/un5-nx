# Phase 4: the Switch (in progress)

## Where it stands

**PCSX2's hardware GS runs on the Switch.** UN5 boots through its intros,
plays battles and Master Mode (story dialogue) without crashing, drawn by
PCSX2's OpenGL renderer on the Switch GPU. It is not yet full speed: the
sound crackles because emulation falls behind real time, and the frame rate
is below 30 (not yet measured; the NRO now logs `[speed]` every 5 s).

| Step | State |
|---|---|
| Runtime and generated game code build with devkitA64 (GCC 16.1) | done, patch 0006 |
| NRO packaging; finds its files next to itself on the SD card | done, patches 0006 and 0008 |
| Threads on the Switch | done, patch 0010 |
| ps2-web-recomp's compute GS on the Switch GPU (OpenGL 4.3) | works, patch 0011: 2.5-3.3 fps in battle, too heavy |
| **PCSX2's hardware GS on the Switch** (`gs-pcsx2/`, patches 0012-0013) | **runs: intro, battle, Master Mode** |
| Full speed | not yet: CPU side, shader compile stutter, audio |

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

## PCSX2's GS (gs-pcsx2)

- A probe NRO (`glprobe`) showed the Switch's Mesa 20.1 GL 4.3 core context
  has every extension PCSX2's OpenGL device needs; it reports 4.3 only.
- `gs-pcsx2/` builds PCSX2's GS (core, HW renderer, texture cache, OpenGL
  device, ImGui, common) for the Switch with two local patches: a Switch EGL
  context in `GLContext::Create`, and no ImGui overlay without a context.
- ps2-web-recomp gets a generic external-GS hook (patch 0012, MIT): raw GIF
  packets, privileged registers, vsync, GS-to-host reads; the window host
  leaves the screen to it. PCSX2's code stays in un5-nx (GPL-3.0).
- What it took on the Switch: no dlopen (EGL entry points through
  eglGetProcAddress), no way to mirror heap memory (`svcMapProcessMemory`
  fails with 0xD401, so GS memory is a plain block), the render window must
  not be `Surfaceless` (PCSX2 then skips every present), SetGsCrt must set
  SMODE1 (patch 0013; with it 0 PCSX2 thinks the screen is VESA).
- Shaders are read from `sdmc:/switch/un5-nx/resources/shaders/{opengl,common}`.
- **Speed on a Switch (2026-10-27 night, CPU 1785 / GPU 921 / mem 1600 MHz,
  the clocks the game is judged at; 1224 is not playable).** Every 5 s the
  log prints fps, the share of time the render thread spends in the GS, new
  shaders and the time spent getting programs, and PCSX2's per-frame draws,
  draw calls, uploads, copies, readbacks and barriers. Menus run at 50 fps;
  the intro movie at 26-28; battle at 31-44 with the GS thread 92-98% busy
  on 2 000 to 25 000 GL draw calls a frame: the CPU side of the driver, not
  the GPU, limits it. `mesa_glthread` (on) gains ~10%; lighter GS settings
  (`UN5GS_PRELOAD/BLEND/MIPMAP/DITHER`) and presenting without vsync did not
  help. Next: why the GS splits UN5's frames into so many draws.
- **Shader cache.** A new shader costs ~12 ms to compile, ~22 ms with its
  binary saved; jutsu bursts spent up to 1.6 s of every 5 s compiling.
  devkitPro's Mesa (horizon) has no disk cache and so reports no program
  binary format, which turns PCSX2's GLShaderCache off. `patches/switch-mesa/`
  rebuilds Mesa with a format reported and a fixed driver sha1 (the stub left
  it unset, so every binary was rejected on load). With it a cached program
  loads in ~2 ms: boot shaders 3.4 s to 0.6 s, a 70-shader burst 1.3 s to
  0.14 s. The cache is `sdmc:/switch/un5-nx/cache/gl_programs.{idx,bin}`.
- **Stripes and ghosting (fixed, patch 0014).** The present read the field
  and the display registers when the lagging render thread got to it, so
  PCSX2 deinterlaced with the wrong field and showed another frame's buffer.

## Open

- **Threads (fixed, patch 0010).** The "stall after SDL" was `std::thread::detach`
  throwing: libnx has no `pthread_detach`, the exception went uncaught and the
  runner aborted. Astris showed that as an endless "Starting game", a Switch
  as an Atmosphère crash report (main thread: `run_window → detach →
  __cxa_throw → abort`). The Switch now runs the threaded runner: a battle
  shows at 1-8 fps with the software GS; overclocking the CPU to 1785 MHz
  barely changes that, since rasterising dominates.
- **Audio on the Switch**: the speed line reports 0 ms of queued sound; not
  looked at yet.
- **GS on the GPU (patch 0011).** The compute shaders run on the Switch GPU
  (NV120, Mesa 20.1, GL 4.3 core) through a surfaceless EGL context; the
  picture is correct. In battle: 2.5-3.3 fps with the GPU busy ~58% of the
  time, i.e. ~190 ms of GPU work per frame (loading screens reach ~24 fps).
  The compute rasteriser (77% of GPU time on the M5) is too heavy for this
  GPU at any level of polish; a hardware-rasterising renderer, as PCSX2's
  and AetherSX2's, turning GS primitives into GL triangles, is the way to a
  playable frame rate. The EGL context, buffers and loader carry over.
- Emulator frame rates say nothing about the Switch: Astris runs the NRO's
  ARM code directly on the Mac's CPU.
