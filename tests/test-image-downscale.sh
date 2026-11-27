#!/bin/sh
# Inline images must be area-filtered when they are shrunk, not left to the
# GPU sampler's fixed 2x2 tap. A 128x128 raster of 1px horizontal lines
# displayed at 24x16 must come out as a flat mid-grey; a bare sampler tap
# (or nearest sampling, as a software renderer does) keeps the lines at full
# contrast. Regression test for the CPU box reduction in the image path
# (rend_downscale_rgba / image_get_texture).

case "$(uname -s 2>/dev/null)" in
MSYS* | MINGW* | CYGWIN*) exit 77 ;;
esac
command -v python3 >/dev/null 2>&1 || exit 77

srcdir=${srcdir:-.}
bin=${PORTTY_BIN:-}
if [ -z "$bin" ]; then
        if [ -x "$srcdir/../src/portty" ]; then
                bin=$srcdir/../src/portty
        elif [ -x src/portty ]; then
                bin=src/portty
        elif [ -x ../src/portty ]; then
                bin=../src/portty
        else
                echo "FAIL: portty binary not found (set PORTTY_BIN)" >&2
                exit 77
        fi
fi
[ -x "$bin" ] || exit 77

tmp=${TMPDIR:-/tmp}/portty-img-scale.$$
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp" || exit 77

# The binary must start in dummy video mode at all.
printf 'quit\n' >"$tmp/quit.sh"
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/quit.sh" >/dev/null 2>&1 || exit 77

# Build the raster and the scene script. The image is iTerm2 inline with
# explicit PIXEL dimensions, so the display box does not depend on the cell
# size the machine's font happens to produce.
#
# The raster is 1px horizontal lines (period 2) at 256x256, shown at 48x48.
# The shrink is 5.33x, deliberately not integral: a 2x2 sampler tap on a 2px
# period walks through every interpolation phase and swings between black,
# white and the greys in between, while an area filter averages whole pairs
# of lines and stays in the middle of the range.
python3 - "$tmp" <<'PY' || exit 1
import base64
import struct
import sys
import zlib

tmp = sys.argv[1]
W = H = 256
raw = bytearray()
for y in range(H):
    v = 255 if y & 1 else 0
    raw += bytes([0]) + bytes((v, v, v)) * W


def chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


png = (b"\x89PNG\r\n\x1a\n" +
       chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
       chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
       chunk(b"IEND", b""))
b64 = base64.b64encode(png).decode()

with open(tmp + "/scene.txt", "w") as f:
    f.write("emit \\e[2J\\e[H\n")
    f.write("emit \\e]1337;File=inline=1;width=48px;height=48px:" + b64 +
            "\\x07\n")
    f.write("wait 1\n")
    f.write("screendump " + tmp + "/out.png\n")
    f.write("quit\n")
PY

SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/scene.txt" -d "" >"$tmp/log" 2>&1 || {
        echo "FAIL: portty run failed" >&2
        cat "$tmp/log" >&2
        exit 1
}
[ -f "$tmp/out.png" ] || {
        echo "FAIL: no screendump written" >&2
        cat "$tmp/log" >&2
        exit 1
}

python3 - "$tmp/out.png" <<'PY'
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n"
pos = 8
idat = b""
w = h = depth = ctype = None
while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
                w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
                idat += chunk
        elif typ == b"IEND":
                break
        pos += 12 + ln
assert depth == 8 and ctype in (2, 6), (depth, ctype)

bpp = 3 if ctype == 2 else 4
raw = zlib.decompress(idat)
stride = w * bpp
prev = bytearray(stride)
off = 0
rows = []
for y in range(h):
        f = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if f == 1:
                for i in range(bpp, stride):
                        line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
                for i in range(stride):
                        line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
                for i in range(stride):
                        a = line[i - bpp] if i >= bpp else 0
                        line[i] = (line[i] + (a + prev[i]) // 2) & 0xFF
        elif f == 4:
                for i in range(stride):
                        a = line[i - bpp] if i >= bpp else 0
                        b = prev[i]
                        c = prev[i - bpp] if i >= bpp else 0
                        p = a + b - c
                        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                        pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                        line[i] = (line[i] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line

# The image is anchored at the top-left cell: its box is exactly 48x48 px.
bw, bh = 48, 48
assert w >= bw and h >= bh, (w, h)
total = mid = 0
for y in range(bh):
        line = rows[y]
        for x in range(bw):
                px = line[x * bpp:(x + 1) * bpp]
                lum = sum(px[:3]) / 3.0
                total += 1
                # A correct area filter keeps every pixel mid-grey; the
                # broken path swings between black, grey and white.
                if 64.0 <= lum <= 192.0:
                        mid += 1
if mid * 100 < total * 99:
        print("FAIL: downscaled image is aliased: %d/%d pixels mid-grey" %
              (mid, total))
        sys.exit(1)
print("downscaled image: %d/%d pixels mid-grey" % (mid, total))
PY
