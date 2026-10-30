"""Lists the callers (JAL sites) of the given addresses in the main program and the overlays, with the function each
call sits in (from the symbol-table maps).
  python -I tools/find_calls.py <address> [address...]
"""
import bisect
import csv
import struct
import sys


def load_map(path):
    rows = sorted((int(r['Start'], 16), int(r['End'], 16), r['Name']) for r in csv.DictReader(open(path)))
    return rows, [r[0] for r in rows]


def owner(rows, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    return rows[i][2] if i >= 0 and addr < rows[i][1] else '?'


targets = {int(a, 16) for a in sys.argv[1:]}
elf = open('game/SCUS_971.11', 'rb').read()
images = [('main', 0x100000, elf[0x100:0x100 + 0x1A2380], 'recomp/maps/dc_main.csv'),
          ('title', 0x1DABD00, open('game/TITLE.BIN', 'rb').read(), 'recomp/maps/dc_title.csv'),
          ('dun', 0x1DABD00, open('game/DUN.BIN', 'rb').read(), 'recomp/maps/dc_dun.csv')]
for name, base, image, mp in images:
    rows, starts = load_map(mp)
    for off in range(0, len(image) - 3, 4):
        w = struct.unpack_from('<I', image, off)[0]
        if w >> 26 == 3:
            addr = base + off
            t = (addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if t in targets:
                print('%s %#x -> %#x in %s' % (name, addr, t, owner(rows, starts, addr)))
