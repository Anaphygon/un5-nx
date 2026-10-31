#!/usr/bin/env python3
"""List the chunks of a CyberConnect2 CCS file (UN5's asset container).

A CCS file is gzip; inside, chunks of `u32 type (0xCCCCxxxx), u32 size in
words, payload`. The index chunk (0x0002) names the source files and every
object; most other chunks start with their object's index in that list.

  tools/ccs_dump.py <file.CCS> [type filter, e.g. 0800]
"""
import gzip
import struct
import sys

TYPES = {
    0x0001: 'header', 0x0002: 'index', 0x0005: 'stream', 0x0100: 'object', 0x0200: 'material',
    0x0300: 'texture', 0x0400: 'clut', 0x0500: 'camera', 0x0600: 'light', 0x0700: 'anime',
    0x0800: 'model', 0x0900: 'clump', 0x0A00: 'external', 0x0B00: 'hit', 0x0C00: 'bbox',
    0x0D00: 'particle', 0x0E00: 'effect', 0xFF01: 'end',
}


def load(path: str) -> bytes:
    data = open(path, 'rb').read()
    return gzip.decompress(data) if data[:2] == b'\x1f\x8b' else data


def index_names(u: bytes, at: int) -> list[str]:
    nf, no = struct.unpack_from('<II', u, at)
    p = at + 8 + nf * 32
    return [u[p + i * 32:p + i * 32 + 30].split(b'\0')[0].decode('cp932', errors='replace') for i in range(no)]


def chunks(u: bytes):
    pos = 0
    while pos + 8 <= len(u):
        t, size = struct.unpack_from('<II', u, pos)
        if t >> 16 != 0xCCCC:
            # Stray words between chunks: skip to the next chunk marker.
            pos += 4
            continue
        yield pos, t & 0xFFFF, size
        if t & 0xFFFF == 0xFF01:
            return
        pos += 8 + size * 4


def main() -> None:
    u = load(sys.argv[1])
    want = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    names: list[str] = []
    for pos, t, size in chunks(u):
        if t == 0x0002:
            names = index_names(u, pos + 8)
        if want is not None and t != want:
            continue
        first = struct.unpack_from('<I', u, pos + 8)[0] if size else None
        label = names[first] if names and first is not None and first < len(names) else ''
        print('%06X %-9s %5d words  obj %-5s %s' % (pos, TYPES.get(t, '%04X' % t), size, first, label))


if __name__ == '__main__':
    main()
