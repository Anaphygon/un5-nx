"""Inventory of a PS2 ELF: segments, sections, symbols, and the PS2 library
families its functions belong to. Usage: elfinfo.py disc/SLES_556.05"""
import collections
import re
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


def main(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        print(f"entry 0x{elf.header.e_entry:08x}  machine {elf.header.e_machine}  flags 0x{elf.header.e_flags:x}")
        print("\nsegments")
        for s in elf.iter_segments():
            print(f"  {s.header.p_type:10} vaddr 0x{s.header.p_vaddr:08x} filesz 0x{s.header.p_filesz:07x} memsz 0x{s.header.p_memsz:07x}")
        print("\nsections")
        for s in elf.iter_sections():
            if s.header.sh_size:
                print(f"  {s.name:16} 0x{s.header.sh_addr:08x} size 0x{s.header.sh_size:07x}")
        syms = []
        for s in elf.iter_sections():
            if isinstance(s, SymbolTableSection):
                syms += [x for x in s.iter_symbols() if x.name]
        funcs = [x for x in syms if x["st_info"]["type"] == "STT_FUNC"]
        objs = [x for x in syms if x["st_info"]["type"] == "STT_OBJECT"]
        print(f"\nsymbols {len(syms)}  functions {len(funcs)}  objects {len(objs)}")
        fam = collections.Counter()
        for fn in funcs:
            m = re.match(r"(sce[A-Z][a-z]+|ADX|adx|AFS|afs|CVM|cvm|cri|Cri|_?[a-z]+_)", fn.name)
            fam[m.group(1) if m else "(game/other)"] += 1
        print("\nfunction families (top 40)")
        for k, v in fam.most_common(40):
            print(f"  {v:5}  {k}")
        size = sum(fn["st_size"] for fn in funcs)
        print(f"\ncode in named functions: {size/1024:.0f} KiB")


if __name__ == "__main__":
    main(sys.argv[1])
