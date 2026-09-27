"""Turn the boot logo into something the kernel can carry.

The kernel is loaded into the board's RAM over a 115200 baud serial line, so
every kilobyte of it is a second of waiting.  The logo is therefore stored as
16 bit pixels - the framebuffer is 32 bit, but the difference does not show on
a photograph - and squeezed with the smallest compressor that is worth having:
LZSS, a flag bit per item, literals as themselves and matches as a twelve bit
distance - one less than the real one, so that the whole window fits - and a
four bit length.  That is about a hundred lines of Python here
and twenty of C at the other end, and it takes the picture from 533K to 118K.

    python3 mklogo.py logo.png ../pc/bootlogo.c

Deflate would do better, but only by carrying an inflater into the kernel.
"""
import sys
from PIL import Image

WIDTH, HEIGHT = 640, 427          # fits 640x480 with the aspect ratio kept
WINDOW, MAX_MATCH, MIN_MATCH = 4096, 18, 3

def rgb565(path):
    im = Image.open(path).convert("RGB").resize((WIDTH, HEIGHT), Image.LANCZOS)
    px = im.load()
    out = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v & 0xFF, v >> 8))
    return bytes(out)

def compress(src):
    out = bytearray()
    i = 0
    while i < len(src):
        flags_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= len(src):
                break
            start = max(0, i - WINDOW)
            best_len = 0
            for length in range(min(MAX_MATCH, len(src) - i), MIN_MATCH - 1, -1):
                at = src.rfind(src[i:i + length], start, i + length - 1)
                if at >= 0:
                    best_len, best_off = length, i - at
                    break
            if best_len >= MIN_MATCH:
                flags |= 1 << bit
                stored = best_off - 1           # 1..4096 fits in twelve bits
                out += bytes(((stored >> 4) & 0xFF,
                              ((stored & 0x0F) << 4) | (best_len - MIN_MATCH)))
                i += best_len
            else:
                out.append(src[i])
                i += 1
        out[flags_at] = flags
    return bytes(out)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = rgb565(src)
    packed = compress(raw)
    with open(dst, "w", encoding="ascii", newline="\n") as f:
        f.write("/*\n * The boot logo, generated - do not edit.\n *\n"
                " *   python3 platform/circle/tools/mklogo.py <logo.png> "
                "platform/circle/pc/bootlogo.c\n *\n"
                " * %d by %d pixels, 16 bit, LZSS packed: %d bytes from %d.\n */\n"
                % (WIDTH, HEIGHT, len(packed), len(raw)))
        f.write('#include "bootlogo.h"\n\n')
        f.write("const unsigned short bootlogo_width = %d;\n" % WIDTH)
        f.write("const unsigned short bootlogo_height = %d;\n" % HEIGHT)
        f.write("const unsigned int bootlogo_packed_size = %d;\n\n" % len(packed))
        f.write("const unsigned char bootlogo_packed[] = {")
        for n, byte in enumerate(packed):
            f.write(("\n\t" if n % 16 == 0 else "") + "0x%02x," % byte)
        f.write("\n};\n")
    print("%s: %d bytes packed from %d" % (dst, len(packed), len(raw)))

if __name__ == "__main__":
    main()
