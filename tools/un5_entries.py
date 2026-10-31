#!/usr/bin/env python3
"""Find every function of UN5 from the disc alone, as ps2-web-recomp entries.

UN5's code is the main executable and three overlays loaded at 0x6C6D00
(PRG/BTL.BIN, ADV.BIN, ETC.BIN). An overlay starts with a header: "MWo3", its
kind, its address, then the size of its text. Functions are laid out one
after another, so every function starts either with a stack frame being
opened or at the first word after the previous one's jr ra and delay slot,
past any padding. This lists all those places in the executable's single
load segment and in each overlay's text. eerecomp rejects a seed that is not
code (data after a jr ra), so the list may be generous.

  tools/un5_entries.py <elf> <overlays dir> <entries out>

The overlays dir holds the images as <base>_<KIND>.bin (tools/iso_overlays.sh).
"""
import os
import struct
import sys

JR_RA = 0x03E00008


def function_starts(words: list[int], begin: int = 0) -> set[int]:
    starts = set()
    for j in range(begin, len(words) - 2):
        w = words[j]
        if w == JR_RA:
            i = j + 2
            while i < len(words) and words[i] == 0:
                i += 1
            if i < len(words):
                starts.add(i)
        elif w >> 16 == 0x27BD and w & 0x8000:  # addiu sp, sp, -n
            k = j - 1
            while k > begin and words[k] == 0:
                k -= 1
            if JR_RA in (words[k], words[k - 1]):
                starts.add(j)
    return starts


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf_path, overlays, out_path = sys.argv[1:4]
    elf = open(elf_path, 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 28)
    _, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', elf, phoff)
    words = list(struct.unpack_from('<%dI' % (filesz // 4), elf, offset))
    tokens = ['%x' % (vaddr + 4 * i) for i in sorted(function_starts(words))]
    for fn in sorted(os.listdir(overlays)):
        if not fn.endswith('.bin') or '_' not in fn:
            continue
        image = open(os.path.join(overlays, fn), 'rb').read()
        if image[:4] != b'MWo3':
            continue
        text, = struct.unpack_from('<I', image, 12)
        code = list(struct.unpack_from('<%dI' % (min(text, len(image)) // 4), image))
        tokens += ['%s:%x' % (fn[:-4], 4 * i) for i in sorted(function_starts(code, 0x20))]
    with open(out_path, 'w') as out:
        out.writelines(t + '\n' for t in tokens)
    print('%d entries' % len(tokens))


if __name__ == '__main__':
    main()
