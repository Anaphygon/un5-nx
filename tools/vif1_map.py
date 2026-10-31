#!/usr/bin/env python3
"""Maps what UN5 sends VU1 in a frame: the VIF1 stream rebuilt from a DMA
trace, cut into batches (the unpacks before each MSCAL/MSCNT), with where
each batch's data lives and which GS registers the direct packets write.

  PS2_DUMP=ram.bin <ps2run ... --save-state F:s.state --frames F+2>
  PS2_TRACE_DMA1=1 <ps2run ... --load-state s.state --frames F+2> 2> dma1.log
  tools/vif1_map.py ram.bin dma1.log [frames in the trace, default 2]

The dump must be taken at the traced frames (the data the tags point at).
Everything it reads comes from your disc: keep its output local.
"""
import bisect
import collections
import re
import struct
import sys

TAG = re.compile(r'ch1 tag at 0x(\w+) id (\d) qwc (\d+) irq \d addr 0x(\w+)')
ELEMENT = {0: 4, 1: 2, 2: 1}            # bytes per element by vl; vl 3 is 2 bytes a vector
GS_REG = {0x00: 'PRIM', 0x06: 'TEX0_1', 0x14: 'TEX1_1', 0x08: 'CLAMP_1', 0x3F: 'TEXFLUSH', 0x42: 'ALPHA_1',
          0x47: 'TEST_1', 0x4C: 'FRAME_1', 0x4E: 'ZBUF_1', 0x50: 'BITBLTBUF', 0x51: 'TRXPOS', 0x52: 'TRXREG',
          0x53: 'TRXDIR', 0x3B: 'TEXA', 0x1C: 'TEXCLUT', 0x3D: 'FOGCOL', 0x40: 'SCISSOR_1', 0x45: 'DTHE'}


def vif_stream(ram: bytes, log: str) -> tuple[bytes, list]:
    """The bytes VIF1 received, tag by tag: each tag's 128 bits with the low
    64 zeroed (its two VIF codes, as with CHCR.TTE), then its data. Returns
    the stream and (stream offset, source address, bytes) runs."""
    out = bytearray()
    runs = []
    for line in open(log, errors='replace'):
        m = TAG.search(line)
        if not m:
            continue
        at, tag_id, qwc, addr = int(m[1], 16), m[2], int(m[3]), int(m[4], 16)
        out += bytes(8) + ram[at + 8:at + 16]
        data = addr if tag_id in '034' else at + 16     # REFE/REF/REFS point; the rest follow the tag
        runs.append((len(out), data, qwc * 16))
        out += ram[data:data + qwc * 16]
    return bytes(out), runs


def vif_codes(data: bytes):
    """(offset, command, num, imm, payload) of each VIF code."""
    p = 0
    while p + 4 <= len(data):
        w = struct.unpack_from('<I', data, p)[0]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        p += 4
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            size = (2 * n if vl == 3 else (vn + 1) * ELEMENT[vl] * n + 3) & ~3
            yield p - 4, cmd, num, imm, data[p:p + size]
            p += size
        elif cmd in (0x50, 0x51):
            p = (p + 15) & ~15
            n = (imm or 65536) * 16
            yield p, cmd, num, imm, data[p:p + n]
            p += n
        elif cmd == 0x4A:
            p = (p + 7) & ~7
            yield p, cmd, num, imm, data[p:p + (num or 256) * 8]
            p += (num or 256) * 8
        elif cmd in (0x30, 0x31):
            yield p - 4, cmd, num, imm, data[p:p + 16]
            p += 16
        elif cmd == 0x20:
            yield p - 4, cmd, num, imm, data[p:p + 4]
            p += 4
        else:
            yield p - 4, cmd, num, imm, b''


def gs_writes(packet: bytes) -> collections.Counter:
    """GS registers written by the A+D GIF packets of a direct transfer."""
    regs: collections.Counter = collections.Counter()
    p = 0
    while p + 16 <= len(packet):
        lo, hi = struct.unpack_from('<QQ', packet, p)
        nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, (lo >> 60) & 15 or 16
        p += 16
        if not (flg == 0 and nreg == 1 and hi & 15 == 0xE):
            break               # image data or a non A+D packet: stop reading registers
        for i in range(nloop):
            regs[packet[p + i * 16 + 8]] += 1
        p += nloop * 16
    return regs


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ram = open(sys.argv[1], 'rb').read()[:32 << 20]
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    stream, runs = vif_stream(ram, sys.argv[2])
    starts = [s for s, _, _ in runs]
    kicks: collections.Counter = collections.Counter()
    mpg: collections.Counter = collections.Counter()
    regs: collections.Counter = collections.Counter()
    shapes: collections.Counter = collections.Counter()
    verts: collections.Counter = collections.Counter()
    unpacks: list = []
    program = None
    for off, cmd, num, imm, payload in vif_codes(stream):
        if cmd >= 0x60:
            k = bisect.bisect_right(starts, off + 4) - 1
            s, addr, _ = runs[k]
            unpacks.append((cmd, num or 256, addr + off + 4 - s))
        elif cmd in (0x14, 0x15, 0x17):
            if cmd != 0x17:
                program = imm
            kicks['MSCNT' if cmd == 0x17 else 'MSCAL %d' % imm] += 1
            positions = [(c, n, a) for c, n, a in unpacks if c & 0x6F in (0x68, 0x69, 0x6D)]
            if positions:
                c, n, a = positions[0]
                key = (' '.join('%02X' % u[0] for u in unpacks), program, a >> 20)
                shapes[key] += 1
                verts[key] += n
            unpacks = []
        elif cmd == 0x4A:
            mpg[imm] += 1
        elif cmd in (0x50, 0x51):
            regs.update(gs_writes(payload))
    print('per frame, over %d traced frames' % frames)
    print('  VU1 kicks:', ', '.join('%s %d' % (k, v // frames) for k, v in kicks.most_common()))
    print('  microcode uploads (VU address: count):', ', '.join('0x%X: %s' % (k, v / frames) for k, v in sorted(mpg.items())))
    print('  GS register writes by direct packets:', ', '.join('%s %d' % (GS_REG.get(k, hex(k)), v // frames) for k, v in regs.most_common(12)))
    print('  batches drawing vertices (unpacks | program | position data in MB):')
    for key, n in sorted(shapes.items(), key=lambda kv: -verts[kv[0]]):
        print('    %5d batches %6d verts  %-24s prog %-4s from %2d MB' % (n // frames, verts[key] // frames, key[0], key[1], key[2]))


if __name__ == '__main__':
    main()
