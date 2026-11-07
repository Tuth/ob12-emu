# Builds a replacement for the missing font sector at FF0000, in the format the Master reads
# (F0851F, F08565): u32 offsets per font at FF0002, relative to FF0002. Per font, from its base:
# +12 first char, +14 char count, +16 row width in words, +18 height, +1C advance,
# +1E offset of u16 column table, +20 offset of bitmap. A glyph spans columns table[i]..table[i+1];
# column x is bit 15 - (x & 15) of little endian word x >> 4. Row stride is the row width.
# The original font is lost. Height 8 uses a classic 5 x 7 dot matrix set, one column per byte,
# bit 0 on top, in a 6 x 8 cell: a blank column and a blank bottom row keep rows and letters
# apart. 7E and 7F are arrows, as on character LCDs; the firmware prints 7E after NEXT.
# Other heights are drawn from DejaVu Sans Mono.
# usage: make_font.py <out file> [height for each of 16 fonts, comma separated]
import struct, sys
import matplotlib
from PIL import Image, ImageDraw, ImageFont
import os

TTF = os.path.join(os.path.dirname(matplotlib.__file__), 'mpl-data', 'fonts', 'ttf', 'DejaVuSansMono.ttf')
FIRST, COUNT = 0x20, 0x60

DOTS = bytes.fromhex(
    '0000000000 00005F0000 0007000700 147F147F14 242A7F2A12 2313086462 3649552250 0005030000'
    '001C224100 0041221C00 14083E0814 08083E0808 0050300000 0808080808 0060600000 2010080402'
    '3E5149453E 00427F4000 4261514946 2141454B31 1814127F10 2745454539 3C4A494930 0171090503'
    '3649494936 064949291E 0036360000 0056360000 0814224100 1414141414 0041221408 0201510906'
    '324979413E 7E1111117E 7F49494936 3E41414122 7F4141221C 7F49494941 7F09090901 3E4149497A'
    '7F0808087F 00417F4100 2040413F01 7F08142241 7F40404040 7F020C027F 7F0408107F 3E4141413E'
    '7F09090906 3E4151215E 7F09192946 4649494931 01017F0101 3F4040403F 1F2040201F 3F4038403F'
    '6314081463 0708700807 6151494543 007F414100 0204081020 0041417F00 0402010204 4040404040'
    '0001020400 2054545478 7F48444438 3844444420 384444487F 3854545418 087E090102 0C5252523E'
    '7F08040478 00447D4000 2040443D00 7F10284400 00417F4000 7C04180478 7C08040478 3844444438'
    '7C14141408 081414187C 7C08040408 4854545420 043F444020 3C4040207C 1C2040201C 3C4030403C'
    '4428102844 0C5050503C 4464544C44 0008364100 00007F0000 0041360800 08082A1C08 081C2A0808'.replace(' ', ''))


def dot_glyphs():
    cells = []
    for i in range(COUNT):
        columns = DOTS[i * 5:i * 5 + 5] + b'\0'
        cells.append([[bool(columns[x] >> y & 1) for x in range(6)] for y in range(8)])
    return 6, cells


def glyphs(height):
    if height == 8:
        return dot_glyphs()
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
