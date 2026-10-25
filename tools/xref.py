"""Find code that forms an address: lui+addiu/ori/load/store pairs and
$gp-relative accesses. Uses rabbitizer, which knows the R5900's MMI and
128-bit instructions (capstone stops at the first one).

Usage:
  xref.py <elf> addr <0xADDR> [...]       code that forms these addresses (+-8 bytes)
  xref.py <elf> str <text> [...]          the same, for where these strings live
  xref.py <elf> dis <0xADDR> [count]      disassemble from an address
"""
import struct
import sys

import rabbitizer
from elftools.elf.elffile import ELFFile

CAT = rabbitizer.InstrCategory.R5900


def load(path):
    with open(path, "rb") as f:
        seg = next(ELFFile(f).iter_segments())
        return seg.data(), seg.header.p_vaddr


def instrs(data, base, start=None, count=None):
    lo = (start - base) if start is not None else 0
    hi = len(data) & ~3 if count is None else lo + 4 * count
    for off in range(lo, hi, 4):
        w = struct.unpack_from("<I", data, off)[0]
        yield rabbitizer.Instruction(w, vram=base + off, category=CAT)


LUI, ADDIU, ORI, DADDIU = 0x0F, 0x09, 0x0D, 0x19
# Loads and stores, including the R5900's 128-bit lq/sq and the FPU ones.
MEM = {0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37, 0x28, 0x29, 0x2B, 0x3F, 0x1E, 0x1F, 0x31, 0x39}
J, JAL = 0x02, 0x03


def fields(w):
    imm = w & 0xFFFF
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, imm - 0x10000 if imm & 0x8000 else imm


def find_gp(data, base):
    """_start loads $gp with lui then addiu/ori before calling anything."""
    hi = None
    for off in range(8, 8 + 4 * 400, 4):
        op, rs, rt, imm = fields(struct.unpack_from("<I", data, off)[0])
        if rt != 28:
            continue
        if op == LUI:
            hi = (imm & 0xFFFF) << 16
        elif hi is not None and op in (ADDIU, ORI):
            return (hi + (imm & 0xFFFF if op == ORI else imm)) & 0xFFFFFFFF
    return None


def scan(data, base, targets, slack=8):
    gp = find_gp(data, base)
    print(f"$gp = {gp:#010x}" if gp is not None else "$gp not found")
    his = {}
    for off in range(0, len(data) & ~3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        op, rs, rt, imm = fields(w)
        if op == LUI:
            his[rt] = (imm & 0xFFFF) << 16
            continue
        if op not in MEM and op not in (ADDIU, ORI, DADDIU):
            continue
        if rs == 28 and gp is not None:
            v = (gp + imm) & 0xFFFFFFFF
        elif rs in his:
            v = (his[rs] + (imm & 0xFFFF if op == ORI else imm)) & 0xFFFFFFFF
        else:
            continue
        for t, label in targets.items():
            if t - slack <= v <= t + slack:
                ins = rabbitizer.Instruction(w, vram=base + off, category=CAT)
                print(f"  {base + off:08x}  {ins.disassemble():40} -> {v:#010x} {label}")


def main(argv):
    data, base = load(argv[1])
    mode = argv[2]
    if mode == "dis":
        start = int(argv[3], 16)
        for ins in instrs(data, base, start, int(argv[4]) if len(argv) > 4 else 40):
            print(f"{ins.vram:08x}  {ins.disassemble()}")
        return
    targets = {}
    if mode == "addr":
        targets = {int(a, 16): a for a in argv[3:]}
    elif mode == "str":
        for s in argv[3:]:
            off = data.find(s.encode() + b"\0")
            while off != -1:
                targets[base + off] = repr(s)
                off = data.find(s.encode() + b"\0", off + 1)
    for t, label in targets.items():
        print(f"target {t:#010x} {label}")
    scan(data, base, targets)


if __name__ == "__main__":
    main(sys.argv)
