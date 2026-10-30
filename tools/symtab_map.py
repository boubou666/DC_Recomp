"""Function maps (the CSV ps2_recomp reads: Name,Start,End,Size) for Dark Cloud from the boot ELF's own symbol table.

SCUS_971.11 is not stripped: CodeWarrior kept every FUNC symbol with its size, for the main program (section 4) and
for the two code overlays whose sections are empty in the ELF (title, section 6: TITLE.BIN; dun, section 8: DUN.BIN;
both load at 0x01DABD00). No Ghidra pass is needed.

The FUNC symbols leave a few spans of code uncovered (crt0's _start, hand-written SDK assembly with NOTYPE labels):
each run of non-zero words between two functions inside a code range becomes a function, named after the NOTYPE
label at its start when there is one.

  python -I tools/symtab_map.py <SCUS_971.11> main  <out.csv>
  python -I tools/symtab_map.py <SCUS_971.11> title <out.csv> <TITLE.BIN>
  python -I tools/symtab_map.py <SCUS_971.11> dun   <out.csv> <DUN.BIN>
"""
import struct
import sys

SECTIONS = {'main': 4, 'title': 6, 'dun': 8}
# Code ranges of the main program: .text up to the VU microcode (.vutext at 0x245300), and the static initializers
# CodeWarrior put after the data (__sinit_*, 0x29FE80-0x2A1010).
MAIN_CODE = [(0x100000, 0x245300), (0x29FE80, 0x2A1010)]


def elf_symbols(elf):
    shoff, = struct.unpack_from('<I', elf, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', elf, 0x2E)
    shdrs = [struct.unpack_from('<10I', elf, shoff + i * shentsize) for i in range(shnum)]
    symtab = next(s for s in shdrs if s[1] == 2)
    strtab = shdrs[symtab[6]]
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', elf, off)
        end = elf.index(b'\0', strtab[4] + name)
        yield elf[strtab[4] + name:end].decode('latin-1'), value, size, info & 15, shndx


def main():
    elf_path, which, out = sys.argv[1:4]
    elf = open(elf_path, 'rb').read()
    shndx_want = SECTIONS[which]
    funcs, labels = {}, {}
    for name, value, size, kind, shndx in elf_symbols(elf):
        if shndx != shndx_want or not name:
            continue
        if kind == 2 and size:  # STT_FUNC
            if value not in funcs or size > funcs[value][1]:
                funcs[value] = (name, size)
        elif kind == 0 and not name.startswith(('gcc2_compiled', '__gnu_compiled')):
            labels.setdefault(value, name)

    if which == 'main':
        base, image = 0x100000, elf[0x100:0x100 + 0x1A2380]  # the LOAD segment's file image
        code = MAIN_CODE
    else:
        image = open(sys.argv[4], 'rb').read()
        base, = struct.unpack_from('<I', image, 8)
        text, = struct.unpack_from('<I', image, 12)
        # Code ranges: runs of functions less than 0x400 bytes apart (.text, then the static initializers after the
        # data); the data between them is left out.
        code = []
        for addr in sorted(funcs):
            end = addr + funcs[addr][1]
            if code and addr - code[-1][1] <= 0x400:
                code[-1][1] = max(code[-1][1], end)
            else:
                code.append([addr, end])
        print('%s: load %#x, header text size %#x, code %s' %
              (which, base, text, ', '.join('%#x-%#x' % tuple(r) for r in code)))

    def word(addr):
        off = addr - base
        return struct.unpack_from('<I', image, off)[0] if 0 <= off <= len(image) - 4 else 0

    gaps = 0
    for lo, hi in code:
        starts = sorted(a for a in funcs if lo <= a < hi)
        bounds = [lo] + [b for a in starts for b in (a, a + funcs[a][1])] + [hi]
        for g_lo, g_hi in zip(bounds[0::2], bounds[1::2]):  # (lo, f1), (f1 end, f2), ..., (fn end, hi)
            addr = g_lo
            while addr < g_hi:
                while addr < g_hi and word(addr) == 0:
                    addr += 4
                if addr >= g_hi:
                    break
                start = addr
                while addr < g_hi and (word(addr) != 0 or any(word(a) for a in range(addr, min(addr + 16, g_hi), 4))):
                    addr += 4
                name = labels.get(start, 'gap_%08x' % start)
                funcs[start] = (name, addr - start)
                gaps += 1

    with open(out, 'w', newline='\n') as f:
        f.write('Name,Start,End,Size\n')
        for addr in sorted(funcs):
            name, size = funcs[addr]
            f.write('%s,0x%08X,0x%08X,%d\n' % (name, addr, addr + size, size))
    print('%s: %d functions (%d from uncovered code) -> %s' % (which, len(funcs), gaps, out))


main()
