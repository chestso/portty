#!/bin/bash
# ref-png.sh - generate a reference PNG of TEXT using hb-view (Noto COLRv1).
#
# Usage: scripts/ref-png.sh "TEXT" output.png

set -eu

if [ $# -lt 2 ]; then
	echo "Usage: $0 \"TEXT\" output.png" >&2
	exit 1
fi

TEXT="$1"
OUTPUT="$2"
FONT="/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"

if [ ! -f "$FONT" ]; then
	echo "ERROR: Reference font not found: $FONT" >&2
	exit 1
fi
if ! command -v hb-view >/dev/null 2>&1; then
	echo "ERROR: hb-view not found. Install harfbuzz-utils." >&2
	exit 1
fi

hb-view --font-file="$FONT" \
	--font-size=128 \
	--output-format=png \
	--background=00000000 \
	--margin=0 \
	--output-file="$OUTPUT" \
	"$TEXT"

echo "Reference PNG written to $OUTPUT"
