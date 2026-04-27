#!/bin/bash
# ref-layers.sh - export each COLR v1 paint layer of TEXT as a separate PNG.
#
# Usage: scripts/ref-layers.sh "TEXT" /tmp/prefix
#        Produces /tmp/prefix_layer00.png, /tmp/prefix_layer01.png, ...

set -eu

if [ $# -lt 2 ]; then
	echo "Usage: $0 \"TEXT\" OUTPUT_PREFIX" >&2
	exit 1
fi

TEXT="$1"
PREFIX="$2"

cd "$(dirname "$0")/.."

if ! python3 -c "import blackrenderer" 2>/dev/null; then
	echo "ERROR: blackrenderer not installed. Run: pip install blackrenderer" >&2
	exit 1
fi

python3 scripts/colr_layers.py "$TEXT" "$PREFIX"
