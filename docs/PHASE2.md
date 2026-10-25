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
   runs**: the VSync callback wakes the frame thread every frame;
6. issues its first GS draws (textured sprites).

Frames are still black and no disc data has been read yet (no `sceCdRead`):
the game appears to wait on CRI ADX, whose IOP module calls `libsd`
functions the runtime does not implement (4 `sceSdInit`, 5
`sceSdSetParam`, 18 `sceSdBlockTrans`, 26, 28). That is the next thing to
solve, most likely with the Tobiichi-Port runtime fork's sound engine.

## What it took

| Stop | Cause | Fix |
|---|---|---|
| `missing-target 0x5e3a00` from `func_11A0B0` | Static constructors are only reached through the table at `0x5E7210`; the code range there was set too small (`0x5E3B50`) | Range is `0x5E3A00`-`0x5E7210`; the constructor table is read and seeded |
| `J 0x1a1bc0`, `JALR 0x1a1da0`, `JALR 0x122b60` | Tail calls and callbacks into code Ghidra left as labels, which phase 1 dropped | Ghidra's 53,000 labels plus 344 pointer/jump seeds go back in as **entry points**: inside a known function each becomes a cheap resume entry, outside one a bounded function. 10,285 files, 0 errors |
| Loop on "Libcdvd bind err CdSearchFile" | `0x1733C0` is `sceCdLayerSearchFile`; the runtime only has `sceCdSearchFile`, so the game's own libcdvd ran and waited for cdvdfsv's RPC server `0x80000597` | Bound to the `sceCdSearchFile` handler (one-layer disc, same arguments) in `tools/fixmap.py` |
| All EE threads asleep, nobody wakes them | The analyzer saw one ELF section spanning code and data and **replaced 32 stores with NOPs as "potential self-modifying code"**; they write globals. One held the idle thread's priority, so its value stayed 0 and the idle busy loop starved every other thread | `tools/fixmap.py` keeps such a patch only if the store's target is inside a real code range: all 32 dropped |
| `CreateThread` refused with -403 | The runtime rejects priority 0 (`< 1`), the IOP's rule; the EE takes 0-127 and runs its own main thread at 0 | `patches/ps2recomp/0003-ee-priority-zero.patch` |
| Precompiled header errors on rebuild | The generated headers are in the runner's PCH and ninja does not rebuild it | `tools/build_mac.sh` drops the PCH when they change |

Seeds come from: the constructor table, data words that point at a function
start (a stack prologue, or right after a `jr $ra`/`j` and padding), slots
next to known function pointers (vtables), `lui`/`addiu` pairs that build a
function address in code, and `J`/`JAL` targets outside their own function.

## Tools added

Runtime patches in `patches/ps2recomp/`, applied in order by
`tools/build_mac.sh`:

| Patch | What |
|---|---|
| `0001-frame-dump.patch` | `UN5_FRAME_DUMP=<absolute dir>` writes every 30th presented frame as a PPM |
| `0002-thread-diagnostics.patch` | Each `run:tick` also logs every EE thread's status, wait reason and PC, semaphores with waiters, and `UN5_WATCH=0xADDR,...` guest words; refused `CreateThread` calls are logged |
| `0003-ee-priority-zero.patch` | EE thread priority 0 is valid |

Two findings are worth reporting upstream: the analyzer's self-modifying
code patches on single-section ELFs, and the EE priority range.

## Next

1. Build against the Tobiichi-Port runtime fork, whose `libsd`-driven sound
   engine is aimed at exactly this gap, and compare.
2. Otherwise implement the five `libsd` imports CRI ADX calls, enough for it
   to report its transfers complete.
