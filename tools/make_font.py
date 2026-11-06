# Builds a replacement for the missing font sector at FF0000, in the format the Master reads
# (F0851F, F08565): u32 offsets per font at FF0002, relative to FF0002. Per font, from its base:
# +12 first char, +14 char count, +16 row width in words, +18 height, +1C advance,
# +1E offset of u16 column table, +20 offset of bitmap. A glyph spans columns table[i]..table[i+1];
# column x is bit 15 - (x & 15) of little endian word x >> 4. Row stride is the row width.
# The glyphs are drawn from DejaVu Sans Mono, not the original font, which is lost here.
# usage: make_font.py <out file> [height for each of 16 fonts, comma separated]
import struct, sys
import matplotlib
from PIL import Image, ImageDraw, ImageFont
import os

TTF = os.path.join(os.path.dirname(matplotlib.__file__), 'mpl-data', 'fonts', 'ttf', 'DejaVuSansMono.ttf')
FIRST, COUNT = 0x20, 0x60

def glyphs(height):
    # Baseline one row above the bottom, for descenders; capitals fill the rest.
    size = round((height - 1) / 0.73)
    font = ImageFont.truetype(TTF, size)
    advance = max(4, int(round(font.getlength('M'))))
    cells = []
    for code in range(FIRST, FIRST + COUNT):
        image = Image.new('L', (advance * 3, height * 3), 0)
        ImageDraw.Draw(image).text((advance, height + height - 1), ('O' if code == 0x30 else chr(code)) if code < 0x7f else ' ', font=font, fill=255, anchor='ls')
        cells.append([[image.getpixel((advance + x, height + y)) > 120 for x in range(advance)] for y in range(height)])
    return advance, cells

def font(height):
    advance, cells = glyphs(height)
    width = advance * COUNT
    words = (width + 15) // 16
    columns = [i * advance for i in range(COUNT + 1)]
    bitmap = bytearray(words * 2 * height)
    for i, cell in enumerate(cells):
        for y in range(height):
            for x in range(advance):
                if cell[y][x]:
                    c = i * advance + x
                    at = y * words * 2 + (c >> 4) * 2
                    word = struct.unpack_from('<H', bitmap, at)[0] | (0x8000 >> (c & 15))
                    struct.pack_into('<H', bitmap, at, word)
    header = bytearray(0x24)
    struct.pack_into('<H', header, 0x12, FIRST)
    struct.pack_into('<H', header, 0x14, COUNT)
    struct.pack_into('<H', header, 0x16, words)
    struct.pack_into('<H', header, 0x18, height)
    struct.pack_into('<H', header, 0x1a, advance)
    struct.pack_into('<H', header, 0x1c, advance)
    struct.pack_into('<h', header, 0x1e, 0x24)
    struct.pack_into('<h', header, 0x20, 0x24 + len(columns) * 2)
    return bytes(header) + b''.join(struct.pack('<H', c) for c in columns) + bytes(bitmap)

heights = [int(h) for h in sys.argv[2].split(',')] if len(sys.argv) > 2 else [8] * 16
table = 16 * 4
body = bytearray()
offsets = []
made = {}
for h in heights:
    if h not in made:
        if len(body) & 1:
            body.append(0)
        made[h] = table + len(body)
        body += font(h)
    offsets.append(made[h])
sector = b'\xff\xff' + b''.join(struct.pack('<I', o) for o in offsets) + bytes(body)
assert len(sector) < 0xa000
open(sys.argv[1], 'wb').write(sector)
print('bytes', len(sector), 'heights', sorted(made))
