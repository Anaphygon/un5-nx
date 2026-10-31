#!/usr/bin/env python3
"""Turn a run's code misses into entries for ports/un5/entries.txt.

A miss in the overlay window (0x6C6D00-0x8F3D00) is named after the overlay
the RAM held when the game stopped, by its header's kind (1 BTL, 2 ADV,
3 ETC), as tools/un5_from_iso.sh names the overlay images; the rest stay plain
addresses. Address 0, a jump through a null pointer, is no entry.

  tools/un5_misses.py <RAM dump> <misses file>   (entries on stdout)
"""
import struct
import sys

BASE, END = 0x6C6D00, 0x8F3D00
KINDS = {1: 'BTL', 2: 'ADV', 3: 'ETC'}


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ram = open(sys.argv[1], 'rb').read()
    kind = KINDS.get(struct.unpack_from('<I', ram, BASE + 4)[0])
    for token in open(sys.argv[2]).read().split():
        if ':' in token:
            print(token)
            continue
        pc = int(token, 16)
        if pc == 0:
            continue
        print('%08X_%s:%x' % (BASE, kind, pc - BASE) if kind and BASE <= pc < END else '%x' % pc)


if __name__ == '__main__':
    main()
