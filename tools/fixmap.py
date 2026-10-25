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
    (0x005E3B50, 0x005E6158, "static initialisers"),
]


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


def write_toml(src, dst, elf, out_dir, csv_path):
    text = open(src).read()
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
    csv_out = os.path.join(out, "functions.csv")
    with open(csv_out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["Name", "Start", "End", "Size"])
        w.writeheader()
        w.writerows(kept)
    root = os.path.dirname(os.path.dirname(os.path.abspath(out)))
    write_toml(toml_in, os.path.join(out, "un5.toml"), os.path.join(root, "disc", "SLES_556.05"),
               os.path.join(os.path.abspath(out), "output"), os.path.abspath(csv_out))
    labels = sum(1 for r in rows if LABEL.match(r["Name"]))
    print(f"{len(rows)} records: {labels} labels skipped, kept {len(kept)} functions, "
          f"dropped {dropped} outside code, clipped {clipped}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
