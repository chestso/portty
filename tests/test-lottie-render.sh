#!/bin/sh
# Lottie vector frames render once, at their exact device-pixel size.

case "$(uname -s 2>/dev/null)" in
MSYS* | MINGW* | CYGWIN*) exit 77 ;;
esac
command -v python3 >/dev/null 2>&1 || exit 77
[ "${PORTTY_LOTTIE:-yes}" = yes ] || exit 77

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
		exit 77
	fi
fi
[ -x "$bin" ] || exit 77

tmp=${TMPDIR:-/tmp}/portty-lottie.$$
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp" || exit 77

printf 'quit\n' >"$tmp/quit.sh"
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/quit.sh" >/dev/null 2>&1 || exit 77

python3 - "$tmp" <<'PY'
import base64
import json
import sys
from pathlib import Path

tmp = Path(sys.argv[1])
lottie = {
    "v": "5.6.0", "fr": 30, "ip": 0, "op": 1, "w": 40, "h": 40,
    "ddd": 0, "assets": [],
    "layers": [{
        "ddd": 0, "ind": 1, "ty": 4, "nm": "solid", "sr": 1,
        "ks": {
            "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
            "p": {"a": 0, "k": [20, 20, 0]},
            "a": {"a": 0, "k": [0, 0, 0]},
            "s": {"a": 0, "k": [100, 100, 100]},
        },
        "ao": 0,
        "shapes": [{
            "ty": "gr",
            "it": [
                {"ty": "rc", "d": 1, "s": {"a": 0, "k": [40, 40]},
                 "p": {"a": 0, "k": [0, 0]}, "r": {"a": 0, "k": 0}},
                {"ty": "fl", "c": {"a": 0, "k": [1, 0, 0, 1]},
                 "o": {"a": 0, "k": 100}, "r": 1},
                {"ty": "tr", "p": {"a": 0, "k": [0, 0]},
                 "a": {"a": 0, "k": [0, 0]},
                 "s": {"a": 0, "k": [100, 100]},
                 "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}},
            ],
        }],
        "ip": 0, "op": 1, "st": 0, "bm": 0,
    }],
}
command = {
    "cmd": "load", "id": 1, "lottie": lottie,
    "fit": "none", "scale": 1.0,
    "placement": {"row": 2, "col": 2},
    "play": {"autostart": False},
}
payload = base64.b64encode(json.dumps(command, separators=(",", ":")).encode()).decode()
(tmp / "load.apc").write_bytes(b"\x1b_" + payload.encode() + b"\x1b\\")
(tmp / "scene.sh").write_text(f"""wait 0.5
sendln clear; cat {tmp}/load.apc; echo ZZLO''ADED
wait-for ZZLOADED 20
renderdump {tmp}/one.png
display-scale 2
wait 1
sendln clear; cat {tmp}/load.apc; echo ZZRE''LOADED
wait-for ZZRELOADED 20
renderdump {tmp}/two.png
quit
""")
PY

SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/scene.sh" >"$tmp/run.log" 2>&1 || {
	cat "$tmp/run.log" >&2
	exit 1
}

python3 - "$tmp/one.png" "$tmp/two.png" <<'PY'
import struct
import sys
import zlib


def red_extent(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
        pos += length + 12
    assert depth == 8 and ctype in (2, 6)
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    off = 0
    points = []
    for y in range(h):
        filter_type = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if filter_type == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + (left + prev[i]) // 2) & 255
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        for x in range(w):
            r, g, b = line[x * bpp:x * bpp + 3]
            if (r, g, b) == (255, 0, 0):
                points.append((x, y))
        prev = line
    assert points, f"no Lottie pixels in {path}"
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return len(points), (min(xs), min(ys), max(xs) + 1, max(ys) + 1)


for path in sys.argv[1:]:
    count, box = red_extent(path)
    w = box[2] - box[0]
    h = box[3] - box[1]
    if count != 40 * 40 or w != 40 or h != 40:
        raise SystemExit(f"{path}: red pixels={count}, box={box}, expected a 40x40 raster")
print("PASS")
PY
