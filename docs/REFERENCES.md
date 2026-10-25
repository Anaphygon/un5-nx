# Reference projects

Other PS2 ports worth learning from, gathered 2026-10-26. Licenses matter:
code from a GPL-3.0 or MIT project can be used here with credit; a project
with no license is read for ideas only.

## Closest to this game

| Project | Game | License | Why it matters |
|---|---|---|---|
| [z3xox/BT3-Recomp](https://github.com/z3xox/BT3-Recomp) | Dragon Ball Z Budokai Tenkaichi 3 | GPL-3.0 | `ps2xRuntime/src/iop_native/cri_adxi/`: **CRI's `cri_adxi.irx` recompiled to native code**, the same IOP module this game loads. Boots on an M1 Pro (OpenGL 4.1, Core Audio) per `docs/MACOS-PORT.md` |
| [nathanialf/ico-pc](https://github.com/nathanialf/ico-pc) | ICO | MIT | A complete software SPU2 (`port/audio/spu2.c`: voices, envelopes, reverb, AutoDMA, IRQ) and a libsd front end (`spu2_sd.c`), integer code with no platform dependency. Ships Windows, Linux, Android, iOS and Apple-silicon builds (a decompilation, not PS2Recomp) |

## Run on a Mac (built in CI or released)

| Project | Game | License | Notes |
|---|---|---|---|
| [Sinan-Karakaya/DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) | Dragon Quest VIII | GPL-3.0 | `macOS-arm64.zip` releases; the author's [PS2Recomp fork](https://github.com/Sinan-Karakaya/PS2Recomp) has native ARM64 fibers and a native IOP that renders sound in step with the device |
| [silentsudin/RoadTripAdventure-recomp](https://github.com/silentsudin/RoadTripAdventure-recomp) | Road Trip Adventure | GPL-3.0 | Signed macOS `.dmg`; Vulkan with Metal on the Mac; sound driver as native code |
| [danisandoval/fukami](https://github.com/danisandoval/fukami) | Ridge Racer V | GPL-3.0 | `macos-arm64.zip`; PCSX2's GS and SPU2 cores with sse2neon |
| [noahbaxter/GHClassic](https://github.com/noahbaxter/GHClassic) | Guitar Hero II | GPL-3.0 | `macos-arm64.dmg`; SPU2 after PCSX2 |
| [Scotho/socom-unzipped](https://github.com/Scotho/socom-unzipped) | SOCOM II | GPL-3.0 | macOS CI, experimental |

## Mac per README

| Project | Game | License | Notes |
|---|---|---|---|
| [HenryKun55/celosia](https://github.com/HenryKun55/celosia) | Shadow of the Colossus | GPL-3.0 | Native **Metal** renderer; SPU2 voices, ADSR, reverb and streaming; 60 FPS on an M4 Pro. Best reference for SPU2 and Metal together |
| [SirBraga/black-recomp](https://github.com/SirBraga/black-recomp) | Black | **none** (ideas only) | Native Metal GS checked against paraLLEl-GS; SPU2 with ADPCM, Gaussian interpolation, reverb |
| [catsandsoup/Road-trip-Adventure-PS2-Recompile-MacOS](https://github.com/catsandsoup/Road-trip-Adventure-PS2-Recompile-MacOS) | Road Trip Adventure | GPL-3.0 | Native LIBSD/SDRDRV through a software SPU2 to Core Audio (`docs/AUDIO_ENGINE.md`); its [PS2Recomp fork](https://github.com/catsandsoup/PS2Recomp) makes Metal the default GS on macOS |
| [soyjxck/PS2Recomp](https://github.com/soyjxck/PS2Recomp) | (fork) | GPL-3.0 | NEON fast path for VU1, and a commit that stops Clang fusing multiply-adds on Apple Silicon, which changes FPU rounding |

## Other CRI ADX handling

- [BigDaddyJRich/hctp-recomp](https://github.com/BigDaddyJRich/hctp-recomp): `HCTP_NATIVE_ADX=1` decodes ADX streams on the host.
- [matpdev/SaintSeiyaRecomp](https://github.com/matpdev/SaintSeiyaRecomp): CRI ADXI and the game's own sound driver on an IOP interpreter with an emulated SPU2.

## Already used or studied

[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp),
[YYOzcan/Tobiichi-Port](https://github.com/YYOzcan/Tobiichi-Port),
[Waffle565/God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port) (no license),
[KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp): see the README's credits.
