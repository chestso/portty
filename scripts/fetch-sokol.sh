#!/bin/sh
# fetch-sokol.sh — fetch the Sokol headers needed by the Sokol backend.
#
# Sokol has no version tags, so we pin to a specific commit hash.
# Headers are placed in third_party/sokol/ where configure.ac's
# SOKOL_CFLAGS (-I${srcdir}/third_party) picks them up.
#
# Usage: scripts/fetch-sokol.sh [DEST_DIR]
#   DEST_DIR defaults to third_party/sokol (relative to repo root)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="${1:-$SRC_DIR/third_party/sokol}"

# Pinned commit — update when bumping Sokol.
SOKOL_COMMIT="3743ea681fce95afd7bee41511cbe51480a046e5"
BASE_URL="https://raw.githubusercontent.com/floooh/sokol/${SOKOL_COMMIT}"

HEADERS="sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h sokol_time.h"

mkdir -p "$DEST"

for h in $HEADERS; do
	echo "Fetching $h ..."
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "${BASE_URL}/${h}" -o "${DEST}/${h}"
	elif command -v wget >/dev/null 2>&1; then
		wget -q "${BASE_URL}/${h}" -O "${DEST}/${h}"
	else
		echo "ERROR: Need curl or wget to fetch Sokol headers" >&2
		exit 1
	fi
done

echo "Done. Sokol headers placed in ${DEST}"
echo "Pinned to commit ${SOKOL_COMMIT}"
