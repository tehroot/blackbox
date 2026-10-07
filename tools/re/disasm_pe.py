#!/usr/bin/env python3
"""Disassemble the code sections of a 32-bit x86 PE file (for the small vendor drivers).

Usage: python3 -I disasm_pe.py <file.sys|file.exe> > out.asm
Needs: pip install capstone pefile   (use a virtual environment)

Prints the import table, then each instruction of the .text and INIT sections.
A call or jump through an import slot gets the import name as a comment.
Run with -I and keep untrusted vendor files in their own directory.
"""
import sys

import capstone
import pefile


def main(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    imports = {}
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in d.imports:
            name = i.name.decode() if i.name else str(i.ordinal)
            imports[i.address] = f"{d.dll.decode()}!{name}"
    for addr, name in imports.items():
        print(f"IMP {addr:#x} {name}")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for s in pe.sections:
        if s.Name.startswith(b".text") or s.Name.startswith(b"INIT"):
            print("== section", s.Name.rstrip(b"\0").decode(), hex(base + s.VirtualAddress))
            for ins in md.disasm(s.get_data(), base + s.VirtualAddress):
                note = next((f"  ; {n}" for a, n in imports.items() if f"{a:#x}" in ins.op_str), "")
                print(f"{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}{note}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
