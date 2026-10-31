#!/usr/bin/env python3
"""List the chunks of a CyberConnect2 CCS file (UN5's asset container).

A CCS file is gzip; inside, chunks of `u32 type (0xCCCCxxxx), u32 size in
words, payload` (setup 3 and stream 5 sometimes without the CCCC tag). The index chunk (0x0002) names the source files and every
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


def a4(x: int) -> int:
    return (x + 3) & ~3


def is_chunk(u: bytes, q: int) -> bool:
    if q + 8 > len(u):
        return False
    t = struct.unpack_from('<I', u, q)[0]
    if t in (3, 5):     # setup and stream chunks, sometimes written untagged
        return True
    return t >> 16 == 0xCCCC and ((t & 0xFF) < 3 and (t >> 8) & 0xFF < 0x30 or t & 0xFFFF in (3, 5, 0xFF01))


def model_size(u: bytes, p: int) -> int:
    """The payload size of a model chunk: its size field is not reliable."""
    stated = struct.unpack_from('<I', u, p - 4)[0] * 4
    _, _, typ, nprim, _, _, gif = struct.unpack_from('<IfHHHHI', u, p)
    q = p + 28
    try:
        if typ & 2:
            return stated
        if typ & 4:
            q += a4(gif & 0xFF)
            for _ in range(nprim):
                _, n, entries = struct.unpack_from('<3I', u, q)
                if entries == 0:
                    q = a4(q + 16 + n * 6) + n * 8
                else:
                    q += 12 + entries * 12 + n * 4
            return q - p
        if typ & 8:
            for _ in range(nprim):
                nv, ni = struct.unpack_from('<2I', u, q)
                q = a4(q + 8 + nv * 6) + ni * 4
            return q - p
        for _ in range(nprim):
            n = struct.unpack_from('<I', u, q + 8)[0]
            q = a4(q + 12 + n * 6) + n * 4
            if typ & 0x600 != 0x600:
                q += n * 4 if typ & 0x400 else n * 8
        return q - p
    except struct.error:
        return stated


def payload_size(u: bytes, pos: int) -> int:
    """Bytes of the chunk at pos after its 8-byte head. Texture and model
    size fields are not reliable (a texture's counts 50 words more than its
    data), so those two are measured from their contents."""
    t, size = struct.unpack_from('<II', u, pos)
    if t & 0xFFFF == 0x0300:
        return (7 + struct.unpack_from('<I', u, pos + 32)[0]) * 4
    if t & 0xFFFF == 0x0800:
        return model_size(u, pos + 8)
    return size * 4


def chunks(u: bytes):
    """(position, type, payload bytes) of every chunk. Stray words between
    chunks are skipped; trailing zero padding ends the walk."""
    pos = 0
    while pos + 8 <= len(u):
        if not any(u[pos:pos + 256]) and not any(u[pos:]):
            return
        if not is_chunk(u, pos):
            pos += 4
            continue
        t = struct.unpack_from('<I', u, pos)[0] & 0xFFFF
        size = payload_size(u, pos)
        yield pos, t, size
        if t == 0xFF01:
            return
        pos += 8 + size


def main() -> None:
    u = load(sys.argv[1])
    want = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    names: list[str] = []
    for pos, t, size in chunks(u):
        if t == 0x0002:
            names = index_names(u, pos + 8)
        if want is not None and t != want:
            continue
        first = struct.unpack_from('<I', u, pos + 8)[0] if size >= 4 else None
        label = names[first] if names and first is not None and first < len(names) else ''
        print('%06X %-9s %5d words  obj %-5s %s' % (pos, TYPES.get(t, '%04X' % t), size // 4, first, label))


if __name__ == '__main__':
    main()
