# un5-nx

A native port of *Naruto Shippuden: Ultimate Ninja 5* (PlayStation 2, PAL,
`SLES-55605`) to the **Nintendo Switch, macOS and Windows**. The game's MIPS
code is statically recompiled to C++ with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), and the PS2 hardware it
talked to is replaced by native code instead of being emulated.

> [!IMPORTANT]
> This repository contains **no game files** (ISO, executable, overlays, IRX
> modules, data) and **no code generated from them**. You need your own copy
> of the game. Everything derived from your disc is built on your own
> computer and stays there.

**Status: experimental, phase 1 of 8.** The game recompiles with no errors
and links into a macOS arm64 executable. It starts executing but does not
reach the title screen yet. It is not playable.

## Read the plan

- [`docs/rancangan.html`](docs/rancangan.html): the whole plan and findings
  as a manga-style comic (Indonesian). Open it in a browser.
- [`docs/PLAN.md`](docs/PLAN.md): targets, architecture, phases and gates.
- [`docs/FEASIBILITY.md`](docs/FEASIBILITY.md): phase 0, what is on the disc
  and what PS2Recomp already covers.
- [`docs/PHASE1.md`](docs/PHASE1.md): phase 1, from 51,127 recompile errors
  to none, and the first run.

## What has been found so far

- The main ELF has no symbol table. Ghidra with the Emotion Engine processor
  module finds 8,709 functions.
- Battle and adventure code are Metrowerks `MWo3` overlays that always load
  at the same address (`0x6c6d00`). One hook in the game's post-load function
  is enough to know which overlay is live.
- All VU1 microcode sits in one region of the main ELF
  (`0x3D57B8`-`0x3E8EB8`); the overlays carry none.
- Audio goes through CRI ADX on the IOP (`cri_adxi.irx`, `sndbase.irx`),
  which upstream PS2Recomp cannot play yet.

## Building (phase 1, macOS arm64)

Requirements: Xcode command line tools, CMake, Ninja, Python 3, Ghidra 12.1.4
with [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
installed, and your own disc.

```sh
# 1. your disc: the ELF in disc/, the whole disc in build/game/
# 2. PS2Recomp, pinned
git clone https://github.com/ran-j/PS2Recomp third_party/PS2Recomp
git -C third_party/PS2Recomp checkout e563698
cmake -S third_party/PS2Recomp -B third_party/PS2Recomp/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
ninja -C third_party/PS2Recomp/build
# 3. function map, recompile, build
third_party/PS2Recomp/build/ps2xAnalyzer/ps2_analyzer disc/SLES_556.05 build/analysis/quick.toml
tools/ghidra_export.sh
python3 tools/fixmap.py build/ghidra/functions.csv build/analysis/quick.toml build/recomp
third_party/PS2Recomp/build/ps2xRecomp/ps2_recomp build/recomp/un5.toml
tools/build_mac.sh
```

Builds use half the CPU cores by default (`JOBS=` to change it).

## Tools

| Tool | What it does |
|---|---|
| `tools/elfinfo.py` | Segments, sections and symbol families of a PS2 ELF |
| `tools/xref.py` | R5900-aware address cross-references and disassembly (rabbitizer) |
| `tools/ghidra_export.sh` | Ghidra headless analysis and PS2Recomp function export |
| `tools/fixmap.py` | Cleans the Ghidra map: code ranges only, functions only |
| `tools/build_mac.sh` | Copies the recompiled output into the runtime and builds it |

## Credits

This project stands on other people's work:

- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** by **ran-j** and
  contributors: the static recompiler, analyzer, runtime and Ghidra export
  script everything here is built with.
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

GPL-3.0, the same as PS2Recomp. See [`LICENSE`](LICENSE).
