# Phase 1: recompile and link on macOS arm64

Date: 2026-10-25. **Done: the game recompiles with no errors and links into
a macOS arm64 executable.**

## Result

| | Analyzer only (phase 0) | Ghidra map (phase 1) |
|---|---|---|
| Function source | JAL scan | Ghidra + Emotion Engine module, cleaned |
| Functions | 6,266 | 8,709 |
| Recompile errors | 51,127 | **0** |
| Generated files | 6,293 (335 MB) | 8,762 (163 MB) |
| Recompile time | 3.4 s | 1.3 s |
| Build | not tried | `ps2EntryRunner` links, 96 MB, 8 min 44 s at 5 jobs |

## Pipeline

```
disc/SLES_556.05
  -> tools/ghidra_export.sh   Ghidra 12.1.4 headless, r5900:LE:32:default,
                              PS2Recomp ExportPS2Functions.java  (46 s)
  -> archive/ps2recomp/tools/fixmap.py          keep functions in code ranges, drop labels,
                              reuse the analyzer's SDK stubs and tables
  -> ps2_recomp               build/recomp/output, 8,762 files
  -> archive/ps2recomp/tools/build_mac.sh       copy into the runner, ninja -j5
```

## What it took

1. **Emotion Engine processor for Ghidra.** Stock Ghidra MIPS stops at the
   R5900's 128-bit loads and MMI instructions. `ghidra-emotionengine-reloaded`
   v2.1.38 has a build for 12.1.4; installed in
   `~/Library/ghidra/ghidra_12.1.4_PUBLIC/Extensions`.
2. **Headless answers.** `askFile()` reads a `.properties` file next to the
   script, keyed `"<title> <button text>"` (for example
   `Choose output CSV file Save`), not by the title alone.
3. **Code ranges.** Real MIPS code lives in three ranges; everything else is
   data or VU microcode:

   | Range | What |
   |---|---|
   | `0x100000`-`0x3D57B8` | Main code |
   | `0x40F848`-`0x410CE8` | Code that uses kernel addresses `0x8007xxxx` (likely a kernel patch the game installs) |
   | `0x5E3B50`-`0x5E6158` | Static initialisers |

4. **Functions only, no labels.** The export also lists 54,923 executable
   labels, each of which becomes its own C++ file (63,639 files, 411 MB).
   Dropping them leaves the recompiler's own fallback entries to handle jumps
   into the middle of a function, as God of War's map does.
5. **Generated headers on the include path.** The sources include
   `ps2_recompiled_functions.h` with angle brackets, so both headers go to
   `ps2xRuntime/include`, the way Tobiichi-Port's installer does it.

## First run (start of phase 2)

`build/game/` holds the full extracted disc (1.8 GB); the runtime reads files
next to the ELF, not the ISO. A 25-second run:

- opens a 640x448 window, OpenGL 4.1 Metal on the Apple M5;
- loads the ELF, registers code `0x100000`-`0x617a80`;
- starts at `0x100008` and reaches `SetupHeap`;
- then produces no further output.

`SetupHeap base=0x928900 size=0xffffffff runtimeEnd=0x928900`: a size of -1
means "the rest of RAM", but the runtime computed an empty heap. First thing
to look at in phase 2.

The recompiler also promoted 194,008 fallback entries for indirect jumps.
Their cost on performance is unknown until the game runs.

## Reference ports

The user pointed at the two projects Tobiichi-Port is built from:

| Project | License | What it adds over upstream PS2Recomp |
|---|---|---|
| [Waffle565/God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port) | **none** (look, don't copy) | The native runtime: GPU GS renderer, VU1 compiled ahead of time, IRX modules compiled to C++, sound engine driven by LIBSD calls, disc files at their original sectors |
| [KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp) | GPL-3.0 | Built on Waffle's runtime; most active (188 stars, CI tests) |
| [YYOzcan/Tobiichi-Port](https://github.com/YYOzcan/Tobiichi-Port) | GPL-3.0 | Ships a PS2Recomp fork with 58 more runtime files than upstream |

The generic parts of that fork are what phase 0 listed as missing for this
game, audio above all: `gs/gpu/*` (OpenGL GS backend, texture store,
threaded backend), `vu/ps2_vu1_compiled.inl` and `ps2_vu1_native.inl`,
`ps2xIOP/src/disc_source.cpp`, `ps2_audio_device.cpp`, and PCSX2's
reference EE and VU interpreters for diffing. The `gow_*` files are God of
War-specific and stay out.

PS2Recomp itself is GPL-3.0, so using the GPL-3.0 fork changes nothing about
this project's license.

## Next (phase 2)

1. Build the same recompiled output against the Tobiichi-Port runtime fork,
   minus `gow_*`, and compare with upstream.
2. Fix `SetupHeap`'s empty heap.
3. Trace until the title screen; record which runtime gaps the game hits.
