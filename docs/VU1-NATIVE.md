# VU1 programs in native code: stage 0

Measured on a Switch on 2026-10-29. Battle replay, CPU 1785, GPU 921, memory 1600, `PS2_VU_PROFILE=1`.

## Why the VU thread

glthread now runs on the Switch, and the GS thread takes about 20 ms per frame. That is close to what 50 fps needs, and the GS thread is idle about a quarter of the time. The VU thread is the limit. During a 49 s battle it is busy 93% of the time:

| Work on the VU thread | Time | Share |
|---|---|---|
| Compiled VU1 programs | 31.7 s | 69% |
| …of which XGKICK | 1.9 s | 4% |
| VIF unpack | 6.1 s | 13% |
| Ring queues, other VIF commands, copies | about 8 s | 18% |

Several cheaper fixes were measured and rejected:

- **Sticky status kept only where it is read; flags built from all lanes at once; single-precision DIV/SQRT.** Each gives the same bits as before. Together they add about 1 fps.
- **VF and ACC registers held in locals, written back only at exit.** No change: 31.6 s against 31.7 s.
- **Higher clocks.** Raising the CPU from 1785 to 1963 MHz makes the programs only 3% faster. Raising memory from 1600 to 1996 MHz adds about 1.5 fps. The CPU clock was measured at 1956 MHz, so the setting is real.

A line profile of the hottest program puts its time in one loop of about 35 instruction pairs per vertex. The translated code runs at about 8 host cycles per VU pair. That is close to the limit for translating one instruction at a time.

## The hot programs are three families

| Family | Programs (code similarity) | Time | Share of VU1 program time |
|---|---|---|---|
| A | DC7A318002C3DD86, 80671E32D6E94206, F79A23C9F43A7278 (0.99-1.00) | 13.8 s | 44% |
| B | 100FC1A0F9434D58, D6416A0D9B442AB1 (0.87) | 6.1 s | 19% |
| C | 2D0D95D002273606, FC4AD0B4293C8A44 (0.95) | 5.1 s | 16% |

Family A (DC7A) does the following:

- reads vertices from the VIF-unpacked buffer: 12.4 fixed-point positions, converted with ITOF12;
- transforms them by the 4x4 matrix in VF1-VF4;
- divides by w (DIV, then MUL by Q) and converts to screen coordinates with FTOI4;
- tests clipping through MAC flags against a mask in VI13;
- writes XYZ, ST and RGBA into a GIF packet and sends it with XGKICK.

A second section (from L_07C8) takes vertex differences, probably for triangle clipping or culling. It jumps through JR to routines that the runtime dispatches.

## Plan

**Go**, one family at a time, largest first. For each family:

1. Write a native C++ version of the per-vertex loop using NEON. It processes four vertices per step and computes only the flag bits the program reads, which are the clip tests.
2. Keep every result bit-exact. Use the same per-lane float helpers (`vfix`, `vmuln`, `fp::div`) so the GIF packets come out identical. Verify with the Mac battle replay, which must stay byte-identical, and with `PS2_VU_CHECK` against the interpreter.
3. Register the native version under the family's program keys, ahead of the translated code. The translated code stays as the fallback.
4. Measure it with one Switch run.

Expected gains are estimates, not measurements. If family A runs three times faster, VU1 program time falls from 31.7 s to about 22.5 s and the VU thread's load drops by about a fifth. With families B and C done as well, the VU thread stops being the limit, and the GS thread (about 20 ms per frame, so about 50 fps) takes over.

Other options considered, and why they are not first:

- **VIF unpack on another core.** The ceiling is 13% of the VU thread, and the work needs exact synchronisation with VU memory double-buffering.
- **VU1 on the GPU** (as DBT-NX does with Tenkaichi 3). PCSX2's GS consumes GIF packets and has no path for geometry that the GPU has already transformed, so this would need a renderer of its own. It is left for the deko3d work.

## Stage 1: what the measurements said, and what was done

PS2_VU_BENCH timed DC7A on its first battle call, on a Mac, in three versions. Two of them are wrong on purpose and only show how much could be gained:

| Version | µs per call | Speed |
|---|---|---|
| Compiled, as vurecomp makes it | 2.24 | 1.00x |
| Same, with no flag work at all | 2.08 | 1.08x |
| Same, also without the per-result `vfix` | 0.94 | 2.39x |

Flags cost little. The cost is `vfix`, which runs after every operation to give PS2 float behaviour: no infinities and no denormals.

**Done:** `vurecomp --rz` (engine patch 0023).
- Compiled programs run with the host set to round-toward-zero and flush-to-zero. The runtime switches FPCR (or MXCSR on x86-64) around each compiled program.
- In that mode the hardware does what `vfix` did to results, and round-toward-zero is the VU's own rounding. Loads still go through `vfix`.
- Checked against the interpreter in the same mode: only the unread flags differ, as before. A battle replay changes by at most 24/255 per channel, in a few pixels, and was confirmed clean on the Switch (no spiky polygons).

**Result** on a Switch at stock clocks (1785/768/1600): a battle went from ~37 to **~43 fps**, and DC7A from 25.7 to 22.2 µs per call. The VU thread is still the limit.

**A native family-A rewrite is not done for now.** Flags were the only overhead left to remove, and removing them is worth about 1.08x. The next lever is the GPU or deko3d work.
