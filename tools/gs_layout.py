"""GS memory layouts for the offline texture conversion (tools/dc_texconv.py): never used by the game at runtime.

DC's IM2 archives keep 8-bit pictures in GS memory order: the data is the PSMCT32 image (half the width and height)
whose bytes, read as PSMT8, are the picture (EnterTexture copies them unchanged; IMG pictures it swizzles itself with
Conv8to32). untwiddle8 lays that CT32 image out in a GS memory model and reads the PSMT8 picture back, linear.
"""

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
           [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN32 = [0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15,
            16, 17, 20, 21, 24, 25, 28, 29, 18, 19, 22, 23, 26, 27, 30, 31,
            32, 33, 36, 37, 40, 41, 44, 45, 34, 35, 38, 39, 42, 43, 46, 47,
            48, 49, 52, 53, 56, 57, 60, 61, 50, 51, 54, 55, 58, 59, 62, 63]
BLOCK8 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
          [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN8 = [
    0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54,
    8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62,
    33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23,
    41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31,
    96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86,
    104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94,
    65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119,
    73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127,
    128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182,
    136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190,
    161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151,
    169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159,
    224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214,
    232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222,
    193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247,
    201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]


def addr32(x, y, bw):
    """Byte address of a PSMCT32 pixel; bw in 64-pixel units."""
    page = (y >> 5) * bw + (x >> 6)
    block = BLOCK32[(y >> 3) & 3][(x >> 3) & 7]
    return ((page << 11) + (block << 6) + COLUMN32[((y & 7) << 3) | (x & 7)]) << 2


def addr8(x, y, bw):
    """Byte address of a PSMT8 texel; bw in 64-texel units (even)."""
    page = (y >> 6) * (bw >> 1) + (x >> 7)
    block = BLOCK8[(y >> 4) & 3][(x >> 4) & 7]
    return (page << 13) + (block << 8) + COLUMN8[((y & 15) << 4) | (x & 15)]


def untwiddle8(data, width, height):
    """The linear PSMT8 picture (width x height bytes) from its GS-order data: the PSMCT32 image of width / 2 x
    height / 2 pixels the game sends, laid out in GS memory and read back as PSMT8."""
    bw8 = max(2, (width + 127) // 128 * 2)
    bw32 = bw8 // 2
    cw, ch = width // 2, height // 2
    pages = ((height + 63) // 64) * (bw8 // 2)
    memory = bytearray(pages * 8192 + 8192)
    for cy in range(ch):
        row = cy * cw * 4
        for cx in range(cw):
            a = addr32(cx, cy, bw32)
            memory[a:a + 4] = data[row + cx * 4:row + cx * 4 + 4]
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            out[y * width + x] = memory[addr8(x, y, bw8)]
    return bytes(out)
