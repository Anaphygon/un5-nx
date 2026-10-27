# Phase 3: overlays, on ps2-web-recomp (done on macOS)

Phase 2 ended with the game loading its first overlay, `PRG/BTL.BIN`, and
stopping on its first constructor: PS2Recomp translates the executable once,
and UN5 runs battle, adventure and menu code from Metrowerks `MWo3` overlays
loaded over the same addresses. Recompiling overlays meant building, inside
PS2Recomp, what [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp)
by Samir Nuri already has: it captures every image the game writes into a
code window, translates each as its own module, and picks the one in place by
its code words at run time. So phase 3 moved to it.

ps2-web-recomp also brings a GPU GS renderer (compute shaders through
wgpu-native, Metal on macOS), VU1 programs translated ahead of time, SPU2
sound, IPU movies, save states, and a browser build that this project does
not use.

## Result

On a MacBook Air (Apple M5), from your own disc: intro movies, title screen,
menus, character select, battles and Master Mode with its story dialogue,
with sound, mostly at the PAL 50 frames per second but with drops now and
then that heat does not explain. `PS2_SCALE=2` or `3` raises the internal
resolution.

## What it took

| Stop | Cause | Fix |
|---|---|---|
| Black screen after the intro | The sound driver serves its RPC queue with `sceSifGetNextRequest` and `sceSifExecRequest` and sleeps between requests; the runtime had neither, so the game's bind to it waited forever | patch 0001 |
| First overlay constructor at `0x8EA080` | Code windows were one 16 KiB unit each, so the 2.2 MB overlays were captured in pieces and code past the first unit was never translated | patch 0003: windows from the ELF segments, `0x6C6D00` (up to 0x22D000) and `0x8F3D00` (up to 0x34C00) |
| Master Mode and battles hang after loading, `file close error!` forever | SNDBASE checks that `close(fd)` returns `fd`; the runtime returned 0 | patch 0002 |
| One overlay function per run | Overlays are called through vtables in the main executable's data, constructor tables inside the overlay (ADV's 11 at `0x8BB380`) and `lui`/`addiu` pairs; functions are padded to 16 bytes | `tools/un5_seed.py` seeds every pointer that lands on a function start |
| Static constructors one per run | The 19 constructors are reached only through the table at `0x5E7210` | `ports/un5/entries.txt` |

Patch 0004 adds F5/F9 quick save and load to the native window, used to
capture a battle and replay it headless with both GS renderers.

## Patches

`patches/ps2-web-recomp/` applies to ps2-web-recomp `79d8eab` with `git am`
(`tools/setup_ps2webrecomp.sh` does it). Each is meant to go upstream.

| Patch | Change | Test |
|---|---|---|
| 0001 | IOP `sceSifGetNextRequest`/`sceSifExecRequest`; a request wakes a sleeping queue thread | none: the IOP side has no test harness yet; checked by reaching the title |
| 0002 | IOP `close` answers with the closed descriptor | none, as above; checked in battles and Master Mode |
| 0003 | Code windows sized from the ELF segments; captures keep empty units inside an image | `tests/windows_segments.py`, fails without the change |
| 0004 | F5/F9 quick save/load in the native window | manual |

## Open

- After some jutsu (Rasengan against Guy, Kyuubi chakra) the picture
  sometimes turns striped and ghosted while the game runs on. Not reproduced
  yet; a quick save taken while it shows will let the GPU and software GS
  renderers be compared on it.
- Frame rate drops now and then, with the laptop cool. Not measured yet;
  `PS2_GPU_PROFILE` and `PS2_TIME` in the runtime are the first things to try.
- The game writes data next to its overlay code, so a later capture of the
  same overlay can differ in words the translation counts as code and become
  a new image to translate. `tools/un5_play.sh` handles it by capturing again;
  the real fix is to leave words that differ between captures of one overlay
  out of the check.
- Nothing is verified on Switch or Windows. ps2-web-recomp renders through
  wgpu, which has no Switch backend: phase 4 needs a GS renderer for the
  Switch GPU (deko3d or OpenGL).
