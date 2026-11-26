#!/usr/bin/env python3
"""elf-align-data.py FILE... - in each object, or each object in a library
(.a), every .data section with data in it that is aligned to fewer than 4
bytes is set to 4, in place (32-bit little-endian ELF). Only those
sh_addralign fields change: 4 bytes for each section, nothing else.

UnixLib's assembler sources start .data without an alignment, so the
section comes out aligned to 1 byte and the linker can place its words at
an odd address. riscos-crossdev adds ".balign 4" after each ".data" when it
builds UnixLib; for a library built without that, raising the section's
alignment does the same, provided every symbol in it is at a multiple of
4 from the start, which this checks (and refuses the file if not). The
result is the same as riscos-crossdev's checks/check-data-align.py --fix.

Prints each section changed; exit status 1 if a file can't be fixed.
"""
import struct
import sys


def fix_object(data, base, label, changed):
    """Raises the .data alignments of the ELF object at DATA[BASE:]; False
    (and DATA unchanged) if one can't be"""
    if data[base:base + 4] != b"\x7fELF" or data[base + 4] != 1 or data[base + 5] != 1:
        print(f"{label}: not a 32-bit little-endian ELF object", file=sys.stderr)
        return False
    shoff, = struct.unpack_from("<I", data, base + 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, base + 0x2E)

    def section(i):
        return struct.unpack_from("<IIIIIIIIII", data, base + shoff + i * shentsize)

    names = base + section(shstrndx)[4]

    def name(off):
        return data[names + off:data.index(b"\0", names + off)].decode()

    symtabs = [section(i) for i in range(shnum) if section(i)[1] == 2]   # SHT_SYMTAB
    fields = []
    for i in range(shnum):
        sh_name, _, _, _, _, size, _, _, align, _ = section(i)
        if name(sh_name) != ".data" or align >= 4 or size == 0:
            continue
        for st in symtabs:
            st_off, st_size, entsize = st[4], st[5], st[9]
            for s in range(st_size // entsize):
                _, value, _, _, _, shndx = struct.unpack_from("<IIIBBH", data, base + st_off + s * entsize)   # Elf32_Sym
                if shndx == i and value % 4:
                    print(f"{label}: a symbol at .data+{value}, not a word boundary", file=sys.stderr)
                    return False
        fields.append((base + shoff + i * shentsize + 32, align))       # sh_addralign
    for at, align in fields:
        struct.pack_into("<I", data, at, 4)
        changed.append(f"{label}: .data aligned to {align}, now 4")
    return True


def members(data):
    """(offset, name) of each object in the ar archive DATA"""
    at, longnames = 8, b""
    while at + 60 <= len(data):
        name = data[at:at + 16].decode().rstrip()
        size = int(data[at + 48:at + 58])
        if name == "//":
            longnames = data[at + 60:at + 60 + size]
        elif name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            yield at + 60, longnames[off:longnames.index(b"/\n", off)].decode()
        elif name not in ("/", "/SYM64/"):
            yield at + 60, name.rstrip("/")
        at += 60 + size + (size & 1)


def fix(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    changed = []
    if data[:8] == b"!<arch>\n":
        ok = all([fix_object(data, at, f"{path}({name})", changed) for at, name in members(data)
                  if data[at:at + 4] == b"\x7fELF"])             # (not other files in it)
    else:
        ok = fix_object(data, 0, path, changed)
    if ok and changed:
        with open(path, "wb") as f:
            f.write(data)
        print("\n".join(changed))
    return ok


if __name__ == "__main__":
    sys.exit(0 if all([fix(p) for p in sys.argv[1:]]) else 1)
