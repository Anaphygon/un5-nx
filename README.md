# un5-nx

A native port of *Naruto Shippuden: Ultimate Ninja 5* (PlayStation 2, PAL,
`SLES-55605`) to the **Nintendo Switch, macOS and Windows**. The game's MIPS
code is statically recompiled to C++ with
[ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp), and the PS2
hardware it talked to is replaced by native code instead of being emulated.
Phases 1 and 2 were done on [PS2Recomp](https://github.com/ran-j/PS2Recomp)
and are kept in [`archive/ps2recomp`](archive/ps2recomp).

> [!IMPORTANT]
> This repository contains **no game files** (ISO, executable, overlays, IRX
> modules, data) and **no code generated from them**. You need your own copy
> of the game. Everything derived from your disc is built on your own
> computer and stays there.

**Status: experimental, phase 4 of 8.** Playable on macOS (Apple silicon):
intro movies, title, menus, battles and Master Mode with its story dialogue,
with sound, mostly at 50 frames per second. Some jutsu sometimes leave the
picture striped, and the frame rate drops now and then. Switch and Windows
are not started. See [`docs/PHASE3.md`](docs/PHASE3.md).

## Read the plan

- [`docs/rancangan.html`](docs/rancangan.html): the whole plan and findings
  as a manga-style comic (Indonesian). Open it in a browser.
- [`docs/PLAN.md`](docs/PLAN.md): targets, architecture, phases and gates.
- [`docs/FEASIBILITY.md`](docs/FEASIBILITY.md): phase 0, what is on the disc
  and what PS2Recomp already covers.
- [`docs/PHASE1.md`](docs/PHASE1.md): phase 1, from 51,127 recompile errors
  to none, and the first run.
- [`docs/PHASE2.md`](docs/PHASE2.md): phase 2, from a black screen to the
  title screen, on PS2Recomp.
- [`docs/PHASE3.md`](docs/PHASE3.md): phase 3, the overlays, on
  ps2-web-recomp: the game plays on macOS.

## What has been found so far

- The main ELF has no symbol table. Ghidra with the Emotion Engine processor
  module finds 8,709 functions.
- Battle (`BTL`), adventure (`ADV`) and menu (`ETC`) code are Metrowerks
  `MWo3` overlays of up to 2.2 MB that load at the same address
  (`0x6c6d00`), with smaller ones after them at `0x8f3d00`. Most of their
  functions are called through vtables in the main executable.
- All VU1 microcode sits in one region of the main ELF
  (`0x3D57B8`-`0x3E8EB8`); the overlays carry none.
- Audio goes through CRI ADX on the IOP (`cri_adxi.irx`, `sndbase.irx`).
  The sound driver runs its own RPC server loop and insists that `close`
  return the descriptor it closed.

## Building (macOS, Apple silicon)

Requirements: Xcode command line tools, CMake, Ninja, Python 3, and your own
disc of the PAL release. About 3 GB of disk.

```sh
# ps2-web-recomp at the pinned commit with this project's patches, then a
# first headless run that captures the overlays and translates the game
tools/setup_ps2webrecomp.sh "/path/to/your/Naruto Shippuden - Ultimate Ninja 5.iso"
# play; when it stops on code nobody has reached yet, it translates that code
# and asks you to play again from there
tools/un5_play.sh
PS2_SCALE=2 tools/un5_play.sh   # 2x internal resolution (up to 3)
tools/un5_play.sh --ntsc        # 60 Hz instead of PAL's 50
```

The game counts everything in frames, so at 60 Hz it also runs 20% faster
than the PAL release; that may be the original Japanese speed, which is not
checked yet.

Keyboard: Z cross, X circle, A square, S triangle, Enter start, arrows for
the D-pad; gamepads work too. F5 saves a quick state and F9 loads it.
Builds use half the CPU cores by default (`JOBS=` to change it).

## Tools

| Tool | What it does |
|---|---|
| `tools/setup_ps2webrecomp.sh` | Clones ps2-web-recomp at the pinned commit, applies `patches/ps2-web-recomp` with `git am`, ports your disc |
| `tools/un5_play.sh` | Plays the game; when it halts on untranslated code, captures the overlays, seeds entries and rebuilds |
| `tools/un5_seed.py` | Finds overlay functions reached through pointers: vtables, constructor tables, `lui`/`addiu` |
| `tools/elfinfo.py` | Segments, sections and symbol families of a PS2 ELF |
| `tools/xref.py` | R5900-aware address cross-references and disassembly (rabbitizer) |
| `tools/ghidra_export.sh` | Ghidra headless analysis of the executable (and the PS2Recomp function export) |
| `tools/decompile.sh` | Decompiles the functions containing given addresses from that Ghidra project |

## Fixes to ps2-web-recomp

In `patches/ps2-web-recomp/`, to be sent upstream (details in
[`docs/PHASE3.md`](docs/PHASE3.md)):

- The IOP had no `sceSifGetNextRequest`/`sceSifExecRequest`, so a module that
  runs its own RPC server loop was never served.
- The IOP's `close` returned 0 instead of the closed descriptor.
- Code windows were cut into 16 KiB pieces instead of following the
  executable's segments, so large overlays were never fully translated.
- The native window gets F5/F9 quick save and load.
- Native frames were paced by the audio queue and came in bursts, which
  showed as stutter at a full frame rate.

## Fixes to PS2Recomp (phases 1 and 2)

Bugs found while porting this game on PS2Recomp, fixed with tests in
[`archive/ps2recomp/patches`](archive/ps2recomp/patches) (all PS2Recomp
tests pass), ready to send upstream:

- The analyzer replaced ordinary "store a global, then call" code with NOPs
  as self-modifying code whenever one ELF section holds code and data.
- The EE kernel's thread priority 0 was rejected, and `ChangeThreadPriority`
  did not return the old priority.
- `sceCdLayerSearchFile` had no runtime handler.
- Async callback stacks were carved over the automatic main stack and
  overwrote its saved return addresses.
- VIF1 swallowed a DIRECT command that carried the rest of a GS image, and
  lost sync with the command stream.
- Thread calls returned 0 instead of the thread ID.
- The IOP refused disc reads into buffers no allocation had touched, so the
  sound driver never loaded.
- Every EE thread started with VU0's VF0 zeroed instead of (0, 0, 0, 1), so
  the screen matrix was empty and nothing was drawn.
- The GS renderer wrote depth with the Z buffer switched off (`ZTE = 0`),
  erasing the frame while it was presented.
- The pad never reported pressure mode (ID `0x79`), so the game kept
  restarting its pad setup and ignored every button.
- The game's `malloc` took the whole heap, leaving the runtime unable to
  pass movie audio to the game, so the intro movie never started.
- DMA left `MADR` stale after a chain stopped, so the game's DMA interrupt
  handler took its error path and froze the movie.
- Every EE thread switch was a C++ exception; threads now run on fibers,
  ported from [Sinan Karakaya's PS2Recomp fork](https://github.com/Sinan-Karakaya/PS2Recomp).

## Contributors

<table>
  <tr>
    <td align="center"><a href="https://github.com/Lisvindanu"><img src="https://github.com/Lisvindanu.png?size=100" width="100" alt="Lisvindanu"/><br/><b>Lisvindanu</b></a><br/>Project manager and product owner: direction, priorities, and the go/no-go at each phase gate</td>
    <td align="center"><a href="https://github.com/claude"><img src="https://github.com/claude.png?size=100" width="100" alt="claude"/><br/><b>Claude (Anthropic)</b></a><br/>Executor: research, reverse engineering, tooling, builds and documentation</td>
  </tr>
</table>

### Built on the work of

<table>
  <tr>
    <td align="center"><a href="https://github.com/snuri00"><img src="https://github.com/snuri00.png?size=100" width="100" alt="snuri00"/><br/><b>Samir Nuri</b></a><br/><a href="https://github.com/snuri00/ps2-web-recomp">ps2-web-recomp</a>: the recompiler, overlay capture, GPU GS, VU1, sound and runtime the game now runs on</td>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/ran-j"><img src="https://github.com/ran-j.png?size=100" width="100" alt="ran-j"/><br/><b>ran-j</b></a><br/><a href="https://github.com/ran-j/PS2Recomp">PS2Recomp</a>: the static recompiler, analyzer, runtime and Ghidra export everything here is built with</td>
    <td align="center"><a href="https://github.com/YYOzcan"><img src="https://github.com/YYOzcan.png?size=100" width="100" alt="YYOzcan"/><br/><b>YYOzcan</b></a><br/><a href="https://github.com/YYOzcan/Tobiichi-Port">Tobiichi-Port</a>: the God of War port that showed the way, its installer model and runtime fork</td>
    <td align="center"><a href="https://github.com/Waffle565"><img src="https://github.com/Waffle565.png?size=100" width="100" alt="Waffle565"/><br/><b>Waffle565</b></a><br/><a href="https://github.com/Waffle565/God-Of-War-Native-Port">God-Of-War-Native-Port</a>: the native GS, VU1, IOP and sound runtime (studied, not copied)</td>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/KIexster"><img src="https://github.com/KIexster.png?size=100" width="100" alt="KIexster"/><br/><b>KIexster</b></a><br/><a href="https://github.com/KIexster/god-of-war-recomp">god-of-war-recomp</a>: God of War recompilation on Waffle565's runtime, with CI tests</td>
    <td align="center"><a href="https://github.com/chaoticgd"><img src="https://github.com/chaoticgd.png?size=100" width="100" alt="chaoticgd"/><br/><b>chaoticgd</b></a><br/><a href="https://github.com/chaoticgd/ghidra-emotionengine-reloaded">ghidra-emotionengine-reloaded</a>: the Emotion Engine processor module for Ghidra</td>
    <td align="center"><a href="https://github.com/Decompollaborate"><img src="https://github.com/Decompollaborate.png?size=100" width="100" alt="Decompollaborate"/><br/><b>Decompollaborate</b></a><br/><a href="https://github.com/Decompollaborate/rabbitizer">rabbitizer</a>: the R5900-aware disassembler behind tools/xref.py</td>
    <td align="center"><a href="https://github.com/Sinan-Karakaya"><img src="https://github.com/Sinan-Karakaya.png?size=100" width="100" alt="Sinan-Karakaya"/><br/><b>Sinan Karakaya</b></a><br/><a href="https://github.com/Sinan-Karakaya/PS2Recomp">PS2Recomp fork</a> and <a href="https://github.com/Sinan-Karakaya/DQ8-Recompiled">DQ8-Recompiled</a>: the fiber scheduler ported here (patch 0020)</td>
  </tr>
</table>

## Credits

This project stands on other people's work:

- **[ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp)** by
  **Samir Nuri** (snuri00), MIT: the static recompiler with overlay capture,
  the GPU GS renderer, VU1 translation, SPU2 sound, IPU movies and the native
  runner the game plays on since phase 3.
- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** by **ran-j** and
  contributors: the static recompiler, analyzer, runtime and Ghidra export
  script phases 1 and 2 were built with.
- **[Tobiichi-Port](https://github.com/YYOzcan/Tobiichi-Port)** by
  **YYOzcan**: the God of War port that showed the way, including the
  installer model (your disc, built on your computer) and how the generated
  code is wired into the runtime.
- **[God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port)**
  by **Waffle565**: the native runtime (GPU GS renderer, compiled VU1 and IRX
  modules, LIBSD-driven sound) that Tobiichi-Port builds on. Studied for
  approach; no code from it is copied here.
- **[god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp)** by
  **KIexster**: a God of War recompilation on Waffle565's runtime, with CI
  tests; one of the two projects Tobiichi-Port draws on.
- **[ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)**
  by **chaoticgd**: the Emotion Engine processor module that makes Ghidra
  read R5900 code.
- **[PS2Recomp fork](https://github.com/Sinan-Karakaya/PS2Recomp)** by
  **Sinan Karakaya**, used by
  [DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled): its
  fiber-based EE scheduler (`EeFiber` and tests, GPL-3.0) is ported in
  patch `0020`; its notes on movie playback pointed at the same bottleneck.
- **[rabbitizer](https://github.com/Decompollaborate/rabbitizer)** by
  **Decompollaborate**: the R5900-aware disassembler behind `tools/xref.py`.
- **[Ghidra](https://github.com/NationalSecurityAgency/ghidra)** by the NSA.
- **[PCSX2](https://github.com/PCSX2/pcsx2)**: the reference for how the PS2
  behaves, and the source of the reference interpreters in the runtime forks
  above.

*Naruto Shippuden: Ultimate Ninja 5* is © Masashi Kishimoto / Shueisha,
TV Tokyo, Pierrot, and Namco Bandai Games; developed by CyberConnect2. This
project is not affiliated with or endorsed by any of them, nor by Sony or
Nintendo. CRI ADX is a product of CRI Middleware.

## License

GPL-3.0, the same as PS2Recomp. ps2-web-recomp is MIT, which GPL-3.0 code may build on. See [`LICENSE`](LICENSE).
