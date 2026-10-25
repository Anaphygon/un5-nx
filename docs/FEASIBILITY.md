# Feasibility study (phase 0)

Date: 2026-10-25. Disc: *Naruto Shippuden: Ultimate Ninja 5*, PAL, `SLES-55605`,
`VER = 1.00`. PS2Recomp pinned at `e563698` (2026-10-22).

**Verdict: feasible, go to phase 1.** Nothing found blocks a recompiled port.
The overlays turned out to be the easy kind, all VU1 microcode is in one known
place, and FMV is already covered. Two things stay open and only phase 2 can
answer them: how fast PS2Recomp's GS and VU1 replacements run on ARM, and
how much work audio needs.

## Findings

### Main executable

| | |
|---|---|
| File | `SLES_556.05`, MIPS-III (R5900), entry `0x100008` |
| Loaded image | `0x100000`-`0x617a80` (5.1 MB), BSS up to `0x6c6d00` |
| Symbols | **none.** `file` says "not stripped" only because section headers remain; `.symtab` is absent |
| Functions | **6,266** found by the analyzer's JAL scan (God of War: ~6,400) |
| SDK functions recognised | 266 with a runtime handler already (sceGs, sceCd, scePad, sceMc, sceSif, sceDma, sceMpeg, ...) |
| Recompile | 6,293 C++ files in **3.4 s**, 335 MB of source |

All 51,127 recompile errors come from **8 functions** whose end the JAL scan
over-estimated, running into strings, constant tables and VU microcode (raw
words like `0x756f7365` are ASCII). They are boundary errors, not unsupported
instructions. Ghidra-exported function boundaries (the workflow PS2Recomp
recommends for stripped games) fix this class of error.

### Overlays: fixed-address, so straightforward

`PRG/*.BIN` are Metrowerks CodeWarrior overlays with an `MWo3` header:

| Overlay | id | Load address | Code | Data | BSS |
|---|---|---|---|---|---|
| BTL (battle) | 1 | `0x6c6d00` | 0x1e5b40 | 0x40600 | 0x6e80 |
| ADV (adventure) | 2 | `0x6c6d00` | 0x157540 | 0x9d180 | 0x400 |
| ETC | 3 | `0x6c6d00` | 0x262c0 | 0x3c00 | 0 |
| TEXT{ENG,FRN,GER,ITA,SPA} | 4 | `0x8f3d00` | data only | | |

Header: `+0 "MWo3"`, `+4 id`, `+8 load address`, `+0xC code size`,
`+0x10 data size`, `+0x14 BSS size`, `+0x18/+0x1C` static constructor table
start/end, `+0x20` name. The ELF itself reserves these regions as empty
`PT_LOAD` segments, one per overlay.

How the game loads them (all addresses in `SLES_556.05`):

- `func_1FA5D0(id)` checks the current overlay and calls the loader if it differs.
- `func_1C3150(slot, name)` builds `cdrom0:\PRG\<name>`, opens it, and reads the
  whole file to `slot_buffer[slot]` (table at `0x60FEE0`: `0x100000`,
  `0x6c6d00`, `0x8f3d00`). The header is loaded too; code starts at `+0x40`.
- `func_100270(buf, size)` then calls `FlushCache(0)`, `FlushCache(2)`, clears
  the BSS after the image, and runs the constructors (`func_11A0B0`).

So overlays are **not relocated**: each one always runs at the same
addresses. Each is recompiled on its own as code at `0x6c6d40`, and an
override of `func_100270` tells the runtime which overlay's function table is
live (by the id at `0x6c6d04`). No change to game code, and no general
overlay support in PS2Recomp needed.

### VU1 microcode: one known region

All VU microcode is in the main ELF, `0x3D57B8`-`0x3E8EB8`, five blocks,
~9,000 instruction pairs (~72 KB), found by VU `NOP` pairs
(`0x8000033C`/`0x000002FF`) and 87 end-bit markers. **The overlays carry
none.** The set of VU1 programs is fixed and can be compiled ahead of time,
as Tobiichi-Port does for God of War. These ranges must also be excluded from
EE recompilation.

### Middleware

From version strings in the ELF:

| Library | Version | Side |
|---|---|---|
| CRI ADXT / ADXF / ADXM | 9.69 / 7.43 | EE (recompiled with the game) |
| CRI AHX | 1.55 | EE |
| CRI CVFS (reads `data.cvm`) | 2.53 | EE, over `sceCd` |
| CRI DVCI | 2.73 | EE |
| `cri_adxi.irx` | | IOP: ADX decode to the SPU2 |
| `sndbase.irx` | | IOP: the game's sound engine (VAG voices) |
| Sony modules in `MODULES.BIN` | | padman, mcman, mcserv, libsd, sdrdrv, cdvdman, sio2man |

The CRI file layer runs on the EE and only needs `sceCd` reads, which the
runtime has. Every Sony module the game loads is in PS2Recomp's built-in
module list.

### Runtime coverage (PS2Recomp)

| Area | State |
|---|---|
| CD/DVD, file I/O, pads, memory card, SIF, DMA, kernel | Implemented |
| FMV (`sceMpeg`, IPU) | Implemented with ffmpeg (2,573 lines); PSS should work |
| IOP | An IOP emulator runs real IRX modules with HLE kernel imports (sysmem, ioman, cdvd, intrman, timrman, vblank, ...) |
| Audio | `libsd` exists only as an EE-side RPC service. **No IOP-side libsd imports and no SPU2 voice mixing**, which `cri_adxi.irx` and `sndbase.irx` need |
| GS / VU1 | Present; upstream says "performance is very bad for VU and GS" |
| macOS arm64 | Supported (sse2neon path); recompiler and analyzer build in 10 s |

## Work per phase

| Phase | Work | Size |
|---|---|---|
| 1 Recompile on Mac | Ghidra function export; exclude VU/data ranges; build runtime + generated code on arm64 | Days |
| 2 Boot on Mac | Debug runtime gaps until the title screen; measure GS/VU1 cost | Weeks, the most uncertain |
| 3 Overlays | Recompile BTL/ADV/ETC as images at `0x6c6d40`; override `func_100270` | About a week |
| 4 Switch build | devkitA64 toolchain, Mesa GL, applet vs title-takeover memory | One to two weeks |
| 5 Performance | Depends entirely on phase 2's measurements | Unknown |
| 6 Audio, FMV, saves | IOP libsd imports + SPU2 mixer (or HLE of ADX/sndbase); FMV likely free | Weeks |

Audio is the largest known gap. The game can be brought up silent and gain
audio last.

## Risks, updated

| Risk | Before | Now |
|---|---|---|
| Overlays unsupported | High | **Low**: fixed address, one hook point |
| No symbols | Unknown | Medium: 6,266 functions, Ghidra export needed |
| VU1 microcode | Unknown | **Low-medium**: one region, five blocks, AOT-compilable |
| GS/VU1 speed on Switch | High | High, unmeasured until phase 2 |
| Audio (CRI ADX, sndbase) | Medium | **Medium-high**: SPU2 side missing in the runtime |
| FMV | Medium | **Low**: runtime already decodes PS2 MPEG |

## Next steps (phase 1)

1. Run Ghidra headless on `SLES_556.05` with PS2Recomp's
   `ExportPS2Functions.java` (R5900 needs the Emotion Engine processor module)
   and regenerate the config with its function map.
2. Mark `0x3D57B8`-`0x3E8EB8` and the string/constant ranges as data.
3. Build `ps2xRuntime` on macOS arm64 and link the generated code, at `-j5`.

## Tools written for this study

- `tools/elfinfo.py`: segments, sections, symbol families of a PS2 ELF.
- `tools/xref.py`: R5900-aware address cross-references (lui pairs and
  `$gp`-relative) and disassembly, via rabbitizer. Capstone cannot be used:
  it stops at the first MMI instruction, which in this ELF is `_start`'s
  first instruction.
