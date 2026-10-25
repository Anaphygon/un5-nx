# Phase 2: boot on the Mac (in progress)

Goal: reach the title screen on macOS arm64 and measure GS/VU1 cost.

## Where it stands (2026-10-26)

The game boots further each step. On upstream PS2Recomp (`e563698`) it now:

1. runs all 19 static constructors;
2. initialises SIF RPC, programs the GS and installs its VSync callback;
3. **loads its own IOP modules in the IOP emulator**: `SNDBASE.IRX`
   ("sound driver version 0.2.0") and `CRI_ADXI.IRX` ("CRI ADX Driver
   Ver.9.69");
4. **finds every file on the disc** through `sceCdSearchFile`: `FLIST.DIR`,
   the five AFS archives, `DATA.CVM` and the three overlays, with the right
   LSNs and sizes;
5. **sends its first GS draws** (textured sprites, primitive type 6) with
   four EE threads running.

It then stops making progress about 120 ticks (about two seconds of game
time) in: every EE thread waits, while the VSync callback and the IOP keep
running. Frames presented so far are black.

Likely cause, not yet confirmed: the IOP side of audio. The game's IRX
modules call `libsd` functions the runtime does not implement (4
`sceSdInit`, 5 `sceSdSetParam`, 18 `sceSdBlockTrans`, 26, 28), so a sound
transfer that the EE waits on never completes.

## What it took

| Stop | Cause | Fix |
|---|---|---|
| `missing-target 0x5e3a00` from `func_11A0B0` | Static constructors are only reached through the table at `0x5E7210`; the code range there was set too small (`0x5E3B50`) | Range is `0x5E3A00`-`0x5E7210`; the constructor table is read and seeded |
| `J 0x1a1bc0`, `JALR 0x1a1da0`, `JALR 0x122b60` | Tail calls and callbacks into code Ghidra left as labels, which phase 1 dropped | Ghidra's 53,000 labels plus 344 pointer/jump seeds go back in as **entry points**: inside a known function each becomes a cheap resume entry, outside one a bounded function. 10,285 files, 0 errors |
| Loop on "Libcdvd bind err CdSearchFile" | `0x1733C0` is `sceCdLayerSearchFile`; the runtime only has `sceCdSearchFile`, so the game's own libcdvd ran and waited for cdvdfsv's RPC server `0x80000597` | Bound to the `sceCdSearchFile` handler (one-layer disc, same arguments) in `tools/fixmap.py` |
| Precompiled header errors on rebuild | The generated headers are in the runner's PCH and ninja does not rebuild it | `tools/build_mac.sh` drops the PCH when they change |

Seeds come from: the constructor table, data words that point at a function
start (a stack prologue, or right after a `jr $ra`/`j` and padding), slots
next to known function pointers (vtables), `lui`/`addiu` pairs that build a
function address in code, and `J`/`JAL` targets outside their own function.

## Tools added

- `UN5_FRAME_DUMP=<absolute dir>`: every 30th presented frame is written as
  a PPM. Kept as `patches/ps2recomp/0001-frame-dump.patch`, applied by
  `tools/build_mac.sh`.

## Next

1. See which semaphore or RPC the EE threads wait on (the runtime's thread
   snapshot, or a log of `WaitSema`/`SifCallRpc`).
2. Build against the Tobiichi-Port runtime fork, whose `libsd`-driven sound
   engine is aimed at exactly this gap, and compare.
