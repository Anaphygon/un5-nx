# un5-nx: Naruto Shippuden Ultimate Ninja 5, native

A native port of *Naruto Shippuden: Ultimate Ninja 5* (PS2, PAL, `SLES-55605`)
to three systems from one codebase:

| Target | Output | CPU | Graphics |
|---|---|---|---|
| Nintendo Switch (Atmosphère) | `un5.nro` homebrew | AArch64 (Cortex-A57) | OpenGL via Mesa |
| macOS | `UN5.app` | AArch64 (Apple Silicon) | OpenGL 4.1 (deprecated but present) |
| Windows | `un5.exe` | x86-64 | OpenGL 4.x |

The game's MIPS code is statically
recompiled to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and the
PS2 hardware is replaced by native code, the way
[Tobiichi-Port](https://github.com/YYOzcan/Tobiichi-Port) does it for God of War.

No game file, and no code generated from one, is ever committed or
distributed. A release ships the tooling and runtime only: the user points
the installer at their own disc image, and it recompiles and builds the game
on their own computer. For the Switch, the installer runs on a Mac or
Windows PC and produces the NRO to copy to the SD card.

## Why recompile instead of emulate

The game already runs in NetherSX2 on the Switch, but stutters in battle and
drops to 7 FPS on saves and cutscenes. An emulator pays for interpreting or
JIT-translating every MIPS instruction and for emulating the GS and VUs
cycle by cycle. A recompiled game runs as ordinary AArch64 code, and the GS
can be drawn with the Switch GPU directly.

## What the disc holds

| File | What it is |
|---|---|
| `SLES_556.05` (5.3 MB) | Main ELF, MIPS-III (R5900), no symbol table; VU1 microcode embedded |
| `PRG/BTL.BIN` (2.3 MB) | Battle code, Metrowerks `MWo3` overlay at a fixed address |
| `PRG/ADV.BIN` (2.0 MB) | Adventure-mode code, loaded at run time (overlay) |
| `PRG/ETC.BIN`, `PRG/TEXT*.BIN` | Smaller overlay and per-language text |
| `MODULES/*.IRX`, `IOPRP300.IMG` | IOP modules: CRI ADX audio, sound base, IOP replacement image |
| `DATA/*.CVM`, `*.AFS`, `*.BIN` | Game data in CRI containers (1.1 GB) |
| `PSS/` | FMV, PS2 MPEG-2 streams (680 MB) |

## Architecture

```
your ISO --extract--> disc/ --ps2xRecomp--> generated/*.cpp --+
                         |                                    |
                         +-- overlays (BTL/ADV/ETC) ----------+--> CMake --+--> un5.nro  (devkitA64)
                                                              |            +--> UN5.app  (clang arm64)
             ps2xRuntime (EE, GS, VU1, IOP, SPU2 replacements)+            +--> un5.exe  (clang-cl / MSVC x64)
             src/ (game overrides, overlay loader)
             platform/ (switch, macos, windows: window, input, audio out, paths)
```

The generated code and the runtime are the same on all three; only
`platform/` differs, and it is kept thin: create a GL context and window,
read pads, hand audio frames to the OS, and say where the disc and saves
live. Two architectures matter for correctness: AArch64 (Switch and Mac,
where SSE becomes NEON through sse2neon) and x86-64 (Windows, native SSE).

- **EE code**: the main ELF and each overlay are recompiled ahead of time.
  An overlay is code that the game copies into RAM at a fixed address; the
  runtime has to dispatch calls into whichever overlay is loaded there.
- **GS**: PS2Recomp's GS replacement, drawn through OpenGL (Mesa on the
  Switch). This is the main performance risk.
- **VU1**: microcode recompiled ahead of time where possible.
- **IOP**: PS2Recomp's IOP emulator runs the game's own IRX modules with
  high-level kernel imports; Sony modules are replaced outright.
- **FMV**: PSS decoded with ffmpeg (devkitPro's port on the Switch).

## Phases and decision gates

Each phase ends with a go/no-go. Stopping at any gate is a valid outcome.

| # | Phase | Done when | Gate question |
|---|---|---|---|
| 0 | **Feasibility study** | Report in `docs/FEASIBILITY.md` | Is the work bounded and the risk acceptable? |
| 1 | Recompile on the Mac | Main ELF recompiles and links with PS2Recomp on macOS arm64 (**done**, `docs/PHASE1.md`) | Does the generated code build cleanly? |
| 2 | Boot on the Mac | Reaches the title screen | Is the runtime complete enough for this game? |
| 3 | Overlays | Enters a battle (BTL.BIN) and adventure mode (ADV.BIN) | Can overlays be handled without rewriting PS2Recomp? |
| 4 | Switch build | Same NRO reaches the title screen on hardware | Does it fit in memory, and what FPS? |
| 5 | Windows build | Same code reaches a battle on Windows x64 | Do x86 and AArch64 behave the same? |
| 6 | Performance | Battle at full speed on the Switch (with sys-clk if needed) | Is it better than NetherSX2? |
| 7 | Audio, FMV, saves | ADX audio, cutscenes, memory card on all three | |
| 8 | Installer and release | One installer per host turns an ISO into the game; CI builds all three | |

The Mac is both the development machine and a target, and it is arm64 like
the Switch, so phases 1-3 flush out most ARM-specific problems before the
Switch is involved. Windows comes after the Switch because the Switch is the
harder target; x86-64 is PS2Recomp's most travelled path, so it should cost
the least. Windows builds and tests run in GitHub Actions (`windows-latest`);
playing on Windows needs a Windows PC.

From phase 1 on, every change builds for all three in CI, so a platform does
not silently fall behind.

## Runtime base

Upstream PS2Recomp lacks what this game needs most (IOP-side sound, a fast
GS). The God of War ports already solved those generically: Waffle565's
native runtime, carried in Tobiichi-Port's GPL-3.0 PS2Recomp fork. Phase 2
builds this game against that fork without its `gow_*` files and keeps
whichever runtime gets further. Waffle565's own repository has no license,
so its code is studied, never copied directly.

## Phase 0: feasibility study

1. Extract the ELF, overlays and IOP modules from the ISO into `disc/`.
2. Inventory the ELF: sections, symbol count, function count, entry points.
3. Find how overlays are loaded: their load addresses, which code reads
   `BTL.BIN`/`ADV.BIN`, and whether they are raw code or a relocatable format.
4. Build PS2Recomp on macOS and run it on the main ELF; count functions that
   fail to translate and runtime calls that are stubbed.
5. List which PS2 libraries the game calls (sceGs, sceVu, sceCd, sceSd, CRI),
   and which of them PS2Recomp's runtime already implements.
6. Write `docs/FEASIBILITY.md`: findings, size of the work per phase, risks,
   and a recommendation.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| GS and VU1 run on the CPU and PS2Recomp calls them "very slow" | Could make the Switch build unplayable | Measure early on the Mac; render GS through the GPU; overclock with sys-clk |
| PS2Recomp has no overlay support | Battles never load | Recompile each overlay as its own image and dispatch by loaded overlay |
| CRI middleware (ADX, AFS, CVM) | No audio, or data not found | High-level replacements; CRI formats are well documented |
| Switch memory in applet mode (~440 MB) | Out of memory | Run in title-takeover mode (launch holding R over a game) |
| PS2Recomp changes upstream | Rebuild breaks | Pin a commit; update deliberately |
| Mac heat during builds (~thousands of files) | Builds stopped | Cap parallel jobs at half the cores |

## Constraints

- No game data, generated code or keys in the repository.
- Builds use at most half the Mac's cores.
- The Naruto Senki port (`ns-nx`) stays paused; this does not replace it.
