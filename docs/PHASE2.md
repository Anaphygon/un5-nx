# Phase 2: boot on the Mac (in progress)

Goal: reach the title screen on macOS arm64 and measure GS/VU1 cost.

## Where it stands (2026-10-26)

On upstream PS2Recomp (`e563698`) plus three local patches, the game:

1. runs all 19 static constructors;
2. initialises SIF RPC, programs the GS and installs its VSync callback;
3. **loads its own IOP modules in the IOP emulator**: `SNDBASE.IRX`
   ("sound driver version 0.2.0") and `CRI_ADXI.IRX` ("CRI ADX Driver
   Ver.9.69");
4. **finds every file on the disc** through `sceCdSearchFile`;
5. creates its eight threads with the right priorities, and **its main loop
   runs**: the VSync callback wakes the frame thread every frame, and the
   game-logic thread (`0x12E9E8`, priority 122) starts;
6. issues its first GS draws (textured sprites).

Frames are still black. The IOP side no longer has unhandled imports: a
minimal libsd keeps CRI ADX's block transfers draining and its transfer
interrupt firing (no sound yet). The game-logic thread now waits at
`0x12EADC`; that is the next stop to trace. (`sceCdRead` is not logged by
the runtime, so "no reads" cannot be told from the log alone.)

## What it took

| Stop | Cause | Fix |
|---|---|---|
| `missing-target 0x5e3a00` from `func_11A0B0` | Static constructors are only reached through the table at `0x5E7210`; the code range there was set too small (`0x5E3B50`) | Range is `0x5E3A00`-`0x5E7210`; the constructor table is read and seeded |
| `J 0x1a1bc0`, `JALR 0x1a1da0`, `JALR 0x122b60` | Tail calls and callbacks into code Ghidra left as labels, which phase 1 dropped | Ghidra's 53,000 labels plus 344 pointer/jump seeds go back in as **entry points**: inside a known function each becomes a cheap resume entry, outside one a bounded function. 10,285 files, 0 errors |
| Loop on "Libcdvd bind err CdSearchFile" | `0x1733C0` is `sceCdLayerSearchFile`; the runtime only has `sceCdSearchFile`, so the game's own libcdvd ran and waited for cdvdfsv's RPC server `0x80000597` | Runtime handler, patch `0003` |
| All EE threads asleep, nobody wakes them | The analyzer saw one ELF section spanning code and data and **replaced 32 stores with NOPs as "potential self-modifying code"**; they write globals. One held the idle thread's priority, so its value stayed 0 and the idle busy loop starved every other thread | Analyzer fix, patch `0002` |
| Game-logic thread never started; main thread "restored" to priority 0 | `ChangeThreadPriority` returned `KE_OK` (0); the EE kernel returns the old priority, which the game saves and restores around a critical section | Patch `0001` |
| CRI ADX waits on SPU2 transfers forever | No libsd for IRX modules: `sceSdBlockTrans` never completes, the transfer interrupt never fires | Patch `0004`: transfers drain at 48 kHz stereo rate, the handler from `sceSdSetTransIntrHandler` fires per half buffer |
| `CreateThread` refused with -403 | The runtime rejects priority 0 (`< 1`), the IOP's rule; the EE takes 0-127 and runs its own main thread at 0 | Patch `0001` |
| Precompiled header errors on rebuild | The generated headers are in the runner's PCH and ninja does not rebuild it | `tools/build_mac.sh` drops the PCH when they change |

Seeds come from: the constructor table, data words that point at a function
start (a stack prologue, or right after a `jr $ra`/`j` and padding), slots
next to known function pointers (vtables), `lui`/`addiu` pairs that build a
function address in code, and `J`/`JAL` targets outside their own function.

## Changes to PS2Recomp

Kept as a commit series on branch `un5-nx` of `third_party/PS2Recomp`,
exported with `git format-patch` to `patches/ps2recomp/` and applied by
`tools/setup_ps2recomp.sh`. Each fix has a test; the full suite (448 tests)
passes, and the new tests fail without their fix.

| Patch | What |
|---|---|
| `0001` fix(runtime) | EE thread priority 0 is valid; `ChangeThreadPriority` returns the old priority |
| `0002` fix(analyzer) | A "self-modifying" store must write into the function its jump enters; before, 32 ordinary stores to globals became NOPs |
| `0003` feat(runtime) | `sceCdLayerSearchFile` handler (forwards to `sceCdSearchFile`) |
| `0004` feat(iop) | Minimal libsd for IRX modules: no sound, but the SPU2 transfer clock and interrupts |
| `0005` tools | `UN5_FRAME_DUMP=<absolute dir>` writes every 30th presented frame as a PPM |
| `0006` tools | Thread, semaphore and `UN5_WATCH=0xADDR,...` memory diagnostics on each run tick |

With the analyzer and runtime fixed, `tools/fixmap.py` no longer filters
the analyzer's patches or binds `sceCdLayerSearchFile` itself; the
recompiled output is identical apart from that call now using its own name.

The first three are meant for upstream; the GitHub account is flagged at
the moment, so they wait here.

## Next

1. Trace the game-logic thread's wait at `0x12EADC`.
2. Look at [z3xox/BT3-Recomp](https://github.com/z3xox/BT3-Recomp)'s native
   `cri_adxi` (same IOP module, GPL-3.0) and
   [ico-pc](https://github.com/nathanialf/ico-pc)'s software SPU2 (MIT) for
   real sound. More leads in [`REFERENCES.md`](REFERENCES.md).
