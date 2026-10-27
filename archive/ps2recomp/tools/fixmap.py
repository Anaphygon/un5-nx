"""Turn Ghidra's function export into the map PS2Recomp recompiles from.

Ghidra's export also lists every executable label, and gives each one an end
at the next record. Past the last real code that end runs on through strings,
constant tables and VU microcode (one "function" came out 1.5 MB long), and
the recompiler then translates data as MIPS. This keeps only records that
start inside the ELF's code ranges and clips each one to its range.

It also keeps Ghidra's functions only, not the ~55,000 labels: each label
becomes its own C++ file (63,000 files, 411 MB), while jumps into the middle
of a function are already handled by the recompiler's own fallback entries.
God of War's playable Tobiichi-Port map has functions only, too.

The config is the analyzer's (it names 266 SDK functions by address and has
the MMIO, jump-table and patch sections) pointed at the cleaned map.

Usage: fixmap.py <ghidra functions.csv> <analyzer toml> <out dir>
"""
import csv
import os
import re
import struct
import sys

# Names the export gives labels rather than functions: anonymous entries,
# switch cases and Ghidra's own label prefixes.
LABEL = re.compile(r"^(entry_[0-9a-fA-F]{8}|caseD_|default|switchD_|LAB_)")

# Where SLES_556.05 has MIPS code, found from Ghidra's function density and by
# disassembling the outliers. Everything else in the image is data, and
# 0x3D57B8-0x3E8EB8 is VU1 microcode.
CODE_RANGES = [
    (0x00100000, 0x003D57B8, "main code"),
    (0x0040F848, 0x00410CE8, "code using kernel addresses 0x8007xxxx"),
    (0x005E3A00, 0x005E7210, "static initialisers"),
]

# The table of static constructors the game's func_11A0B0 calls at boot
# (a0..a1 at that call). Ghidra finds no caller for these, so they are
# seeded explicitly: a missing one stalls the game before its first frame.
CTOR_TABLE = (0x005E7210, 0x005E725C)


def load_image(elf_path):
    """The first PT_LOAD segment: (bytes, base address)."""
    with open(elf_path, "rb") as f:
        data = f.read()
    phoff, = struct.unpack_from("<I", data, 0x1C)
    off, vaddr, _, filesz = struct.unpack_from("<IIII", data, phoff + 4)
    return data[off:off + filesz], vaddr


def looks_like_start(image, base, t):
    """A stack-allocating prologue, or the first instruction after another
    function's last jump (jr $ra, or a j tail call) and its delay slot and
    any nop padding."""
    def word(addr):
        return struct.unpack_from("<I", image, addr - base)[0]
    if word(t) >> 16 == 0x27BD and word(t) & 0x8000:         # addiu $sp, $sp, -N
        return True
    a = t - 4
    while a > t - 64 and word(a) == 0:                      # nop padding
        a -= 4
    last = word(a - 4)                                       # a is the delay slot
    return last == 0x03E00008 or last >> 26 == 0x02          # jr $ra, or j


def code_pointer_seeds(image, base, known):
    """Function addresses built in code with lui + addiu/ori and then used as
    callbacks (passed in a register, called through jalr)."""
    seeds = set()
    for lo, hi, _ in CODE_RANGES:
        his = {}
        for a in range(lo, hi, 4):
            w = struct.unpack_from("<I", image, a - base)[0]
            op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 0x0F:                                   # lui
                his[rt] = imm << 16
                continue
            if op not in (0x09, 0x0D) or rs not in his:      # addiu, ori
                continue
            low = imm if op == 0x0D else (imm - 0x10000 if imm & 0x8000 else imm)
            t = (his[rs] + low) & 0xFFFFFFFF
            if t & 3 or t in known or not range_of(t):
                continue
            if looks_like_start(image, base, t):
                seeds.add(t)
    return seeds


def pointer_seeds(image, base, known):
    """Entry points reached only through pointers stored in data: the
    constructor table, plus any data word that points at an unmapped
    address which starts a function (a stack-allocating prologue, or the
    instruction after a previous function's jr $ra and padding)."""
    def word(addr):
        return struct.unpack_from("<I", image, addr - base)[0]

    def starts_function(t):
        return looks_like_start(image, base, t)

    seeds = set(struct.unpack_from(f"<{(CTOR_TABLE[1] - CTOR_TABLE[0]) // 4}I", image,
                                   CTOR_TABLE[0] - base))
    for off in range(0, len(image) - 3, 4):
        src = base + off
        if range_of(src):
            continue                                         # code, not a data word
        t = word(src)
        if t & 3 or t in known or not range_of(t):
            continue
        # A slot in a table of function pointers (a vtable): a neighbouring
        # word is a known function start.
        in_table = any(word(src + d) in known for d in (-4, 4)
                       if not range_of(src + d) and base <= src + d < base + len(image) - 3)
        if in_table or starts_function(t):
            seeds.add(t)
    return seeds - known


def jump_seeds(image, base, records):
    """Targets of J/JAL that land outside the function the jump is in and
    are not a function start yet: tail calls into the middle of another
    function. Ghidra exports these as labels, which are not kept, and the
    runtime stops at any jump whose target it cannot find."""
    spans = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in records)
    starts = {s for s, _ in spans}
    seeds = set()
    for lo, hi in spans:
        for a in range(lo, hi, 4):
            w = struct.unpack_from("<I", image, a - base)[0]
            if w >> 26 not in (0x02, 0x03):                  # J, JAL
                continue
            t = (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if lo <= t < hi or t in starts or not range_of(t):
                continue
            seeds.add(t)
    return seeds


def range_of(addr):
    for lo, hi, _ in CODE_RANGES:
        if lo <= addr < hi:
            return lo, hi
    return None


def clean(rows):
    kept, dropped, clipped = [], 0, 0
    for r in rows:
        if LABEL.match(r["Name"]):
            continue
        start, end = int(r["Start"], 16), int(r["End"], 16)
        rng = range_of(start)
        if rng is None:
            dropped += 1
            continue
        if end > rng[1]:
            end = rng[1]
            clipped += 1
        kept.append({"Name": r["Name"], "Start": f"0x{start:08X}", "End": f"0x{end:08X}",
                     "Size": str(end - start)})
    return kept, dropped, clipped


def write_toml(src, dst, elf, out_dir, csv_path, entries=()):
    text = open(src).read()
    if entries:
        # Ghidra's labels go back in as entry points: an entry inside a known
        # function becomes a cheap resume entry of that function rather than
        # a file of its own, so every callback, vtable slot and tail-call
        # target is reachable without 54,000 extra C++ files.
        extra = "".join(f'  "entry_{a:08x}@0x{a:08X}",\n' for a in sorted(entries))
        text, n = re.subn(r"^(entry_points\s*=\s*\[\n)", lambda m: m.group(1) + extra, text,
                          count=1, flags=re.M)
        if n != 1:
            sys.exit(f"{src}: no 'entry_points = [' list")
    for key, value in (("input", elf), ("output", out_dir), ("ghidra_output", csv_path)):
        text, n = re.subn(rf'^{key}\s*=\s*".*"$', f'{key} = "{value}"', text, count=1, flags=re.M)
        if n != 1:
            sys.exit(f"{src}: no '{key} =' line in [general]")
    with open(dst, "w") as f:
        f.write(text)


def main(csv_in, toml_in, out):
    os.makedirs(out, exist_ok=True)
    with open(csv_in) as f:
        rows = list(csv.DictReader(f))
    kept, dropped, clipped = clean(rows)
    image, base = load_image(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(out))),
                                          "disc", "SLES_556.05"))
    known = {int(r["Start"], 16) for r in kept}
    seeds = (pointer_seeds(image, base, known) | code_pointer_seeds(image, base, known)
             | jump_seeds(image, base, kept))
    # A seed outside every known body is a function Ghidra missed: give it a
    # record bounded by the next start or the end of its code range (left to
    # the recompiler, the last one in a range runs on into data). A seed
    # inside a body stays an entry point below, so it cannot split it.
    spans = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in kept)
    def inside(a):
        return any(lo < a < hi for lo, hi in spans)
    loose = sorted(a for a in seeds if not inside(a))
    starts = sorted(known | set(loose))
    for a in loose:
        nxt = next((b for b in starts if b > a), None)
        end = min(nxt, range_of(a)[1]) if nxt else range_of(a)[1]
        kept.append({"Name": f"sub_{a:08X}", "Start": f"0x{a:08X}", "End": f"0x{end:08X}"})
    kept.sort(key=lambda r: int(r["Start"], 16))
    known |= set(loose)
    for r in kept:
        r["Size"] = str(int(r["End"], 16) - int(r["Start"], 16))
    csv_out = os.path.join(out, "functions.csv")
    with open(csv_out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["Name", "Start", "End", "Size"])
        w.writeheader()
        w.writerows(kept)
    root = os.path.dirname(os.path.dirname(os.path.abspath(out)))
    # Seeds and Ghidra's labels both go in as entry points, never as new
    # function records: an entry inside a known function becomes a resume
    # entry and leaves that function whole (a wrong seed cannot split it),
    # and one outside every function is synthesised by the recompiler,
    # bounded by the next known function.
    labels_in = {int(r["Start"], 16) for r in rows
                 if r["Name"].startswith("entry_") and range_of(int(r["Start"], 16))}
    entries = (labels_in | seeds) - known
    write_toml(toml_in, os.path.join(out, "un5.toml"), os.path.join(root, "disc", "SLES_556.05"),
               os.path.join(os.path.abspath(out), "output"), os.path.abspath(csv_out), entries)
    print(f"{len(entries)} entry points ({len(seeds)} from pointers and jumps, "
          f"{len(labels_in - known)} Ghidra labels)")
    labels = sum(1 for r in rows if LABEL.match(r["Name"]))
    print(f"{len(rows)} records: {labels} labels skipped, kept {len(kept)} functions, "
          f"dropped {dropped} outside code, clipped {clipped}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
