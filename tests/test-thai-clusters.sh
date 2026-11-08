#!/bin/sh
# Thai base+mark clusters must render ink in every SGR style. Regression
# test: with a primary font lacking Thai, a non-NORMAL style accepted an
# all-.notdef shaped run and the cluster vanished.

case "$(uname -s 2>/dev/null)" in
MSYS* | MINGW* | CYGWIN*) exit 77 ;;
esac
command -v python3 >/dev/null 2>&1 || exit 77
if command -v fc-list >/dev/null 2>&1; then
	[ -n "$(fc-list :lang=th 2>/dev/null)" ] || exit 77
fi

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

tmp=${TMPDIR:-/tmp}/portty-thai.$$
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp" || exit 77

# The binary must start in dummy video mode at all.
printf 'quit\n' >"$tmp/quit.sh"
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/quit.sh" >/dev/null 2>&1 || exit 77

# Every grapheme is a base+mark cluster, so a vanished cluster removes
# the whole cell's ink instead of only a mark's.
thai='วัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวัวั'
base_ink=""
fail=0

for sgr in 0 1 3 1\;3 7 1\;7; do
	cat >"$tmp/scene.sh" <<EOF
resize 80 24
emit \\e[2J\\e[H
emit \\e[${sgr}m${thai}\\e[0m\\r\\n
wait 1
renderdump $tmp/out.png
quit
EOF
	SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/scene.sh" >/dev/null 2>&1 ||
		{
			echo "FAIL: portty run failed for style $sgr" >&2
			exit 1
		}
	ink=$(
		python3 - "$tmp/out.png" <<'PY'
import collections, struct, sys, zlib

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
counts = collections.Counter()
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
	for x in range(w):
		counts[tuple(line[x * bpp:(x + 1) * bpp])] += 1
	prev = line
bg = counts.most_common(1)[0][0]
print(sum(n for px, n in counts.items() if px != bg))
PY
	) || {
		echo "FAIL: PNG decode failed for style $sgr" >&2
		exit 1
	}
	echo "style $sgr: ink=$ink"
	if [ -z "$base_ink" ]; then
		base_ink=$ink
	else
		# ink(style) >= 0.75 * ink(0)
		if [ $((ink * 4)) -lt $((base_ink * 3)) ]; then
			echo "FAIL: style $sgr ink=$ink < 0.75 * ink(0)=$base_ink" >&2
			fail=1
		fi
	fi
done

[ $fail -eq 0 ] && echo "PASS"
exit $fail
