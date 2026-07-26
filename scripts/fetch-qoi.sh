#!/bin/sh
# fetch-qoi.sh — fetch the QOI single-file library needed by the frame recorder.
#
# QOI has no version tags, so we pin to a specific commit hash.
# The header is placed in third_party/qoi/ where Makefile.am's
# -I$(top_srcdir)/third_party/qoi picks it up.
#
# Usage: scripts/fetch-qoi.sh [DEST_DIR]
#   DEST_DIR defaults to third_party/qoi (relative to repo root)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="${1:-$SRC_DIR/third_party/qoi}"

# Pinned commit — update when bumping QOI.
QOI_COMMIT="4ab68bbd20618a663255625160c40875713f5485"
BASE_URL="https://raw.githubusercontent.com/phoboslab/qoi/${QOI_COMMIT}"

HEADER="qoi.h"

mkdir -p "$DEST"

echo "Fetching $HEADER ..."
if command -v curl >/dev/null 2>&1; then
	curl -fsSL "${BASE_URL}/${HEADER}" -o "${DEST}/${HEADER}"
elif command -v wget >/dev/null 2>&1; then
	wget -q "${BASE_URL}/${HEADER}" -O "${DEST}/${HEADER}"
else
	echo "ERROR: Need curl or wget to fetch QOI header" >&2
	exit 1
fi

echo "Done. QOI header placed in ${DEST}"
echo "Pinned to commit ${QOI_COMMIT}"
