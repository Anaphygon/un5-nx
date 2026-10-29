# Plan D: a deko3d GS backend

Written 2026-10-29. Goal: 50 fps in battle on the Switch, including big jutsu.

## Where the time goes now

Measured on a Switch at the Fast preset (CPU 1963, GPU 921, RAM 1996 MHz), 2x resolution, in a Tsunade battle. Each row is a 5 s window of the log.

| Moment | fps | GS thread per frame | of which PCSX2 `Draw()` | VU thread busy | GPU still busy at present |
|---|---|---|---|---|---|
| Normal exchange | 41-44 | 17-20 ms | 10-12 ms | 82-87% | up to 50% |
| Big jutsu | 25-37 | 23-38 ms | 15-31 ms | 50-87% | about 50% |

At 50 fps a frame has 20 ms. In a big jutsu the GS thread alone takes more than that. In a normal exchange the GS thread and the VU thread are both close to the limit.

The GS thread's time has three parts:

1. **PCSX2's own work.** Texture cache lookups, vertex conversion and draw state. This stays with any backend.
2. **The OpenGL driver.** Mesa 20.1 nouveau validates state on every draw and emits the commands. Part of it runs on the glthread worker (core 1), and the rest waits for that worker.
3. **Waiting on the GPU.** Buffer waits (`nouveau_bo_wait`), fences, and cache barriers that drain the GPU.

deko3d is the Switch's native low-level graphics API (fincs; devkitPro ships `libdeko3d` and the `uam` shader compiler). It removes part 2 almost entirely: a draw becomes a few words written into a command buffer. It also lets part 3 be managed by hand: our own memory, our own fences, barriers only where they are needed. It does nothing for part 1, and nothing for the VU thread.

## What deko3d can and cannot reach

- **Can:** cut the GS thread from 17-38 ms toward PCSX2's own work alone. Stage 0 measures how far that is.
- **Cannot:** the VU thread. At 82-87% busy and 42 fps, it would cap a normal exchange at roughly 48-50 fps, even with an infinitely fast GS. Reaching 50 in every jutsu needs VU work as well (see "After deko3d").
- **Bonus:** no shader compile stutter after the first play. Compiled deko3d shaders (DKSH) are kept on the SD card, and a pre-built pack can ship with the release.

## Stage 0: measure before building (one self-driving run)

This is a decision gate. It is one Switch run that uses the existing battle replay (`battle.state` with `args-bench.txt`) and records three things:

- **Sampler split.** The GS thread and the glthread worker, sampled with `PS2_SAMPLE` and grouped by module: Mesa (`nvc0_*`, `nouveau_*`, `st_*`, `_mesa_*`, `glthread`), PCSX2 GS, and libc/other.
- **GPU time per frame.** Measured with `GL_TIME_ELAPSED` queries around each frame, to tell how much the GPU itself is the limit.
- **Wait time on the GS thread.** Time spent in `nouveau_bo_wait`, fence waits and glthread syncs.

**Go** if Mesa plus the waits are at least about 6 ms of the GS thread's frame in a normal exchange. That would bring the GS thread under 14 ms, below the VU thread's limit.

**No go** if most of the time is PCSX2's own `Draw()` work. In that case deko3d would not pay, and the better lever is reducing that CPU work: fewer draws per frame, a lighter texture cache path, or moving GS work to another core.

## Stage 1: a deko3d device that presents

- **New device.** Add `GSDeviceDK` / `GSTextureDK` in `gs-pcsx2/src/deko3d/`, modelled on PCSX2's Vulkan device (`GSDeviceVK`, 6800 lines). The Vulkan device is the closest model because it also uses explicit command buffers and explicit memory.
- **Our files only.** The new files live in our tree. The PCSX2 source stays pristine, and selecting the device is a local patch in `gs-pcsx2/patches/`.
- **Window and present.** The window comes from `nwindowGetDefault`. Presenting uses a swapchain of 3 images (`dkQueuePresentImage`).
- **Fixed shaders.** `convert`, `present`, `merge`, `interlace` and `imgui` are compiled with `uam` at build time into DKSH, from PCSX2's OpenGL GLSL. `uam` takes OpenGL-style bindings.
- **Done when** the launcher, the ImGui menu and a cleared frame show on the Switch through deko3d.

## Stage 2: the game's shaders

PCSX2 builds the main vertex, geometry and pixel shaders from a selector of many bits at run time, so they cannot all be compiled ahead of time.

- **On the device.** The `uam` compiler (Mesa's GLSL front end plus the nouveau code generator, the same code the NRO already carries in Mesa) is built as a library into the NRO. Each new selector is compiled once and stored as DKSH on the SD card.
- **Pre-built pack.** The selectors UN5 uses are recorded during play (the log already counts "new shaders"), compiled on the Mac, and shipped as a pack. Most players then never compile at all.
- **Check first:** the `uam` license must fit GPL-3.0.

## Stage 3: drawing

- **Textures and targets.** `GSTextureDK` provides render targets, depth, the texture cache's uploads and copies, and readbacks.
- **Buffers.** Vertex, index and uniform data go through ring buffers in deko3d memory blocks, guarded by our own fence ring. There are no driver buffer waits.
- **State.** Blend, depth and stencil state are recorded straight into the command buffer. Barriers are emitted only where PCSX2 asks for one (texture barriers and feedback loops).
- **Submission.** Command buffers are submitted at the same points where the GL backend flushes today. Recording can later move to another core.
- **Done when** a full battle draws correctly.

## Stage 4: check against OpenGL

- **Same frames.** The battle replay is deterministic, so both backends draw the same frames.
- **Compare.** A run dumps chosen frames with OpenGL and with deko3d, and a pixel diff on the Mac must show no differences beyond rounding.
- **Where it can run.** deko3d also runs in the Ryujinx-based emulators, so most checking can start there before the Switch.
- **Known risk areas:** the "purple flash" in Master Mode's forest, the half-pixel and native-scaling fixes at 2x, and the intro movie.

## Stage 5: ship

- **Default.** deko3d becomes the default backend.
- **Fallback.** OpenGL stays as a launcher option, "Renderer: deko3d / OpenGL", for anything that breaks.
- **Benchmark.** The self-driving clock bench is run again at every preset, and `PHASE4.md` gets the new table.

## After deko3d: the VU thread

The VU thread is still the limit at around 48-50 fps in heavy scenes. Two levers remain:

- **Split VIF1 unpack from VU1.** Unpack is 13% of the VU thread, and moving it to its own core needs exact double-buffer synchronisation.
- **Run the hot VU1 program families on the GPU** with deko3d compute. With deko3d in place, the transformed vertices can stay on the GPU instead of going back through GIF packets. This is the path DBT-NX took with Tenkaichi 3, and it needs a GS path for pre-transformed geometry.

## Effort and order

| Stage | Size |
|---|---|
| 0, measure | one build and one run |
| 1, present | days |
| 2, shaders | about a week (the `uam` library build is the unknown) |
| 3, drawing | one to two weeks |
| 4, compare | days |
| 5, ship | a day |

Each stage ends with a Switch run. Stage 0 decides whether stages 1-5 happen at all.
