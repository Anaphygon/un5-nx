#!/usr/bin/env python3
"""Build UN5's VU1 programs from the executable alone, for vurecomp --snapshots.

The executable carries every VU1 microprogram the game uploads, as VIF MPG
blocks in one area of its file (offsets 0x2D0000-0x2F0000 in SLES_556.05).
VU1 memory holds three layers: a library uploaded once at 0x300-0x7EC (one
block per address), slot A at 0 and slot B at 0x100, which renderers replace.
So every program the game can run lives in one of library + slot A + slot B.
This writes one memory image per pair of slot blocks (and per A block alone),
with its entries in a sidecar: 0, each block's start, the instruction after
every end (E bit and its delay slot), every address loaded with IADDIU vi, vi0
(vurecomp's own jump-target rule), and every library address the EE code
names as a byte offset (addiu/ori immediates; UN5 passes MSCAL addresses
that way).

  tools/un5_vu1.py <SLES_556.05> <out dir>

Not covered: code left in VU memory from an earlier, longer upload behind a
shorter one. The runtime interprets a program it has no translation for.
"""
import os
import struct
import sys

AREA = (0x2D0000, 0x2F0000)
LIBRARY = 0x300  # first instruction of the library layer
SIZE = 16384


def mpg_blocks(elf: bytes) -> list[tuple[int, bytes]]:
    blocks = []
    for off in range(AREA[0] + 4, AREA[1], 8):  # MPG data is 64-bit aligned
        w, = struct.unpack_from('<I', elf, off)
        if (w >> 24) & 0x7F != 0x4A:
            continue
        num, addr = (w >> 16) & 0xFF or 256, w & 0xFFFF
        if addr + num > SIZE // 8 or off + 4 + num * 8 > len(elf):
            continue
        body = elf[off + 4:off + 4 + num * 8]
        if any(body):
            blocks.append((addr, body))
    return blocks


def ee_byte_addresses(elf: bytes) -> set[int]:
    found = set()
    for off in range(0, len(elf) - 4, 4):
        w, = struct.unpack_from('<I', elf, off)
        if w >> 26 in (0x09, 0x0D) and (w & 0xFFFF) % 8 == 0 and (w & 0xFFFF) < SIZE:
            found.add((w & 0xFFFF) // 8)
    return found


def entries(mem: bytes, starts: set[int], library_entries: set[int]) -> list[int]:
    found = {0} | starts | library_entries
    for i in range(SIZE // 8):
        lo, hi = struct.unpack_from('<II', mem, i * 8)
        if hi >> 30 & 1 and i + 2 < SIZE // 8:
            found.add(i + 2)
        if not hi & 0x80000000 and lo >> 25 == 0x08 and (lo >> 11) & 15 == 0:  # IADDIU vi, vi0, imm
            found.add(((((lo >> 10) & 0x7800) | (lo & 0x7FF)) * 8 & (SIZE - 1)) // 8)
    return sorted(i for i in found if any(mem[i * 8:i * 8 + 8]))


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    elf = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    blocks = mpg_blocks(elf)
    library = bytearray(SIZE)
    for addr, body in blocks:
        if addr >= LIBRARY:
            library[addr * 8:addr * 8 + len(body)] = body
    library_entries = {i for i in ee_byte_addresses(elf) if i >= LIBRARY}
    slot_a = [b for b in blocks if b[0] == 0]
    slot_b = [b for b in blocks if 0 < b[0] < LIBRARY]
    os.makedirs(out, exist_ok=True)
    for ai, a in enumerate(slot_a):
        for bi, b in enumerate(slot_b + [None]):
            mem = bytearray(library)
            starts = set()
            for block in (a, b):
                if block:
                    addr, body = block
                    mem[addr * 8:addr * 8 + len(body)] = body
                    starts.add(addr)
            name = os.path.join(out, 'A%02dB%02d' % (ai, bi))
            open(name + '.bin', 'wb').write(mem)
            open(name + '.entries', 'w').write(' '.join('%X' % (i * 8) for i in entries(mem, starts, library_entries)) + '\n')
    print('%d library blocks, %d slot A, %d slot B: %d images' % (
        sum(1 for b in blocks if b[0] >= LIBRARY), len(slot_a), len(slot_b), len(slot_a) * (len(slot_b) + 1)))


if __name__ == '__main__':
    main()
