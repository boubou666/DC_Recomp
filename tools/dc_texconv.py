#!/usr/bin/env python3
"""Offline conversion of Dark Cloud's 8-bit textures into a native store, from the user's own disc files.

DC enters its textures from IMG archives (CTextureManager::EnterIMGFile, 0x132BA0): "IMG\\0", a count, then 0x30-byte
entries (name, + 0x20 the offset of a TIM2 picture). Twice as many sit in the same directory under "IM2\\0" (the map
pieces' textures among them). EnterTexture (0x1313B0) keeps 8-bit pictures in GS memory order
(Conv8to32: the game sends them as a CT32 image of half the width), so the native adapter does not decode those at
runtime (BaldHuman's rule: no GS layout function in the game; texels in GS order only through an offline conversion).
This tool reads the same pictures from the disc (TIM2: 8-bit indices, a 256-colour CT32 palette): plain in IMG
archives; in GS memory order in IM2 ones (EnterIMGFile tells EnterTexture they are already converted, and it copies
them unchanged), which tools/gs_layout.py resolves here, offline. It writes them to a store the adapter loads at
startup, keyed by the record's name, size and palette.

  tools/dc_texconv.py <game dir with DATA.HD2 / DATA.DAT> <output file, e.g. build/textures/dc_textures.bin>

Store: "DCTX", version 1, count; per texture: name (32 bytes), width, height (u16), palette hash (u32, FNV-1a 32 of
the 1024 palette bytes as the disc holds them, as EnterTexture copies them into the record), offset (u32), then at
the offset the palette as RGBA8 (256 x 4, alpha 0x80 = 255, CSM1 entry order resolved) and width x height indices.
Generated from the user's disc: never committed.
"""
import struct
import sys

from gs_layout import untwiddle8


def fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def csm1(index):
    # The GS reads a 256-colour CT32 palette stored as a 16 x 16 image with entries 8-15 and 16-23 of each 32 swapped.
    return (index & 0xE7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)


def archives(blob):
    """Offsets of the "IMG\0" and "IM2\0" magics in a file."""
    for magic in (b'IMG\x00', b'IM2\x00'):
        pos = 0
        while True:
            k = blob.find(magic, pos)
            if k < 0:
                break
            pos = k + 4
            yield k


def pictures(blob):
    """Each (name, TIM2 picture offset) of every IMG / IM2 archive in a file (standalone or embedded).

    IM2 ("IM2\0") is the same directory under another magic; the map pieces' textures (s140101...) are kept in them."""
    for k in sorted(archives(blob)):
        if k % 16 or k + 0x10 > len(blob):
            continue
        count = struct.unpack_from('<I', blob, k + 4)[0]
        if not 0 < count < 2000 or k + 0x10 + count * 0x30 > len(blob):
            continue
        for e in range(count):
            entry = k + 0x10 + e * 0x30
            name = blob[entry:entry + 0x20].split(b'\x00')[0]
            picture = k + struct.unpack_from('<I', blob, entry + 0x20)[0]
            if name and picture + 0x30 <= len(blob) and blob[picture:picture + 4] == b'TIM2':
                yield name, picture, blob[k:k + 4] == b'IM2\x00'


def convert(blob, picture):
    """An 8-bit TIM2 picture as (width, height, palette hash, RGBA8 palette, indices), or None."""
    header = picture + 0x10
    image_size = struct.unpack_from('<I', blob, header + 0x08)[0]
    header_size = struct.unpack_from('<H', blob, header + 0x0C)[0]
    clut_colours = struct.unpack_from('<H', blob, header + 0x0E)[0]
    clut_type, image_type = blob[header + 0x12], blob[header + 0x13]
    width, height = struct.unpack_from('<HH', blob, header + 0x14)
    if image_type != 5 or clut_colours < 256 or (clut_type & 0x3F) != 3 or width == 0 or height == 0:
        return None
    pixels = header + header_size
    clut = pixels + image_size
    if clut + 1024 > len(blob) or pixels + width * height > len(blob):
        return None
    raw_palette = blob[clut:clut + 1024]
    palette = bytearray(1024)
    for i in range(256):
        r, g, b, a = raw_palette[csm1(i) * 4:csm1(i) * 4 + 4]
        palette[i * 4:i * 4 + 4] = bytes((r, g, b, 255 if a >= 0x80 else a * 2))
    return width, height, fnv1a32(raw_palette), bytes(palette), blob[pixels:pixels + width * height]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    game, out = sys.argv[1], sys.argv[2]
    index = open(f'{game}/DATA.HD2', 'rb').read()
    files = struct.unpack_from('<I', index, 0)[0] // 32
    store, seen = [], set()
    with open(f'{game}/DATA.DAT', 'rb') as dat:
        for i in range(files):
            _, _, _, _, offset, size, _, _ = struct.unpack_from('<8I', index, i * 32)
            if size == 0 or size > 64 * 1024 * 1024:
                continue
            dat.seek(offset)
            blob = dat.read(size)
            for name, picture, twiddled in pictures(blob):
                texture = convert(blob, picture)
                if not texture:
                    continue
                width, height, palette_hash, palette, indices = texture
                if twiddled:
                    # IM2: the texels are in GS memory order on the disc (EnterIMGFile passes EnterTexture its "already
                    # converted" flag for that magic, and EnterTexture copies them instead of running Conv8to32).
                    indices = untwiddle8(indices, width, height)
                key = (name, width, height, palette_hash, fnv1a32(indices[:4096]))
                if key in seen:
                    continue
                seen.add(key)
                store.append((name, width, height, palette_hash, palette, indices))
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sII', b'DCTX', 1, len(store)))
        offset = 12 + len(store) * 44
        for name, width, height, palette_hash, palette, indices in store:
            f.write(struct.pack('<32sHHII', name, width, height, palette_hash, offset))
            offset += 1024 + width * height
        for _, _, _, _, palette, indices in store:
            f.write(palette)
            f.write(indices)
    print(f'{len(store)} 8-bit textures -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
