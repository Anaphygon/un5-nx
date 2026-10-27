#!/usr/bin/env python3
"""Add overlay functions the explorer cannot see to ps2-web-recomp's entries.

UN5 calls most of its overlay code through pointers: vtables in the main
executable's data, constructor tables inside each overlay, and addresses built
with lui/addiu. Exploring finds those one per run, as the game halts on each,
so this seeds every pointer that lands on a function start in a captured
window image.

  tools/un5_seed.py <elf> <entries.txt> <overlays dir> [<misses.txt>]

The misses of the last run are added first, normalised as port.sh writes them.
"""
import os
import struct
import sys
from typing import Iterable

JR_RA = 0x03E00008


def words(data: bytes) -> list[int]:
    return list(struct.unpack_from('<%dI' % (len(data) // 4), data))


def elf_data_words(elf: bytes) -> set[int]:
    phoff, = struct.unpack_from('<I', elf, 28)
    _, offset, _, _, filesz, _, _, _ = struct.unpack_from('<8I', elf, phoff)
    return set(words(elf[offset:offset + filesz]))


def lui_addiu_targets(code: list[int]) -> set[int]:
    targets, high = set(), {}
    for i, w in enumerate(code):
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x0F:
            high[rt] = (imm << 16, i)
        elif op in (0x09, 0x0D) and rs in high and i - high[rs][1] < 16:
            low = imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm
            targets.add((high[rs][0] + low) & 0xFFFFFFFF)
    return targets


def is_function_start(code: list[int], index: int) -> bool:
    # A stack frame being opened, or the first word after the previous
    # function's jr ra and delay slot, past nops padding it to 16 bytes.
    if index < 4 or index >= len(code) or code[index] == 0:
        return False
    w = code[index]
    if w >> 16 == 0x27BD and w & 0x8000:
        return True
    k = index - 1
    while k >= index - 4 and code[k] == 0:
        k -= 1
    return k >= 1 and JR_RA in (code[k], code[k - 1])


def seeds(name: str, base: int, image: bytes, pointers: Iterable[int]) -> set[str]:
    code = words(image)
    candidates = set(pointers) | set(code) | lui_addiu_targets(code)
    return {'%s:%x' % (name, v - base) for v in candidates
            if base <= v < base + len(image) and not v & 3 and is_function_start(code, (v - base) // 4)}


def token(t: str) -> str:
    name, _, at = t.rpartition(':')
    return (name + ':' if name else '') + '%x' % int(at, 16)


def main() -> None:
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    elf_path, entries_path, overlays = sys.argv[1:4]
    misses = sys.argv[4] if len(sys.argv) > 4 else None
    pointers = elf_data_words(open(elf_path, 'rb').read())
    known = {token(t) for t in open(entries_path).read().split()} if os.path.exists(entries_path) else set()
    found = {token(t) for t in open(misses).read().split()} if misses and os.path.exists(misses) else set()
    for fn in sorted(os.listdir(overlays)):
        if fn.endswith('.bin') and '_' in fn:
            name = fn[:-4]
            found |= seeds(name, int(name.split('_')[0], 16), open(os.path.join(overlays, fn), 'rb').read(), pointers)
    new = sorted(found - known)
    with open(entries_path, 'a') as out:
        out.writelines(t + '\n' for t in new)
    print('%d new entry points' % len(new))


if __name__ == '__main__':
    main()
