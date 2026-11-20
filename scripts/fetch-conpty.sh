#!/bin/sh
# fetch-conpty.sh - refresh portty's tracked conpty.dll + OpenConsole.exe
#
# Portty keeps the binaries in data/conpty/ (flat layout) so a clone builds
# offline.  This script downloads the pinned Microsoft.Windows.Console.ConPTY
# package from data/conpty/conpty.lock, verifies its SHA-256, and extracts
# the host architecture pair over the tracked files.
#
# Usage:
#   ./scripts/fetch-conpty.sh               # host architecture
#   ./scripts/fetch-conpty.sh --arch arm64
#
# Run it after bumping CONPTY_VERSION in the lock, then commit the binaries
# and VERSION.

set -eu

cd "$(dirname "$0")/.."

LOCK="data/conpty/conpty.lock"
DEST="data/conpty"
ARCH=host

while [ $# -gt 0 ]; do
    case "$1" in
    --arch)
        [ $# -ge 2 ] || { echo "fetch-conpty: --arch needs a value" >&2; exit 2; }
        ARCH=$2
        shift 2
        ;;
    --arch=*)
        ARCH=${1#--arch=}
        shift
        ;;
    -h | --help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "fetch-conpty: unknown option: $1" >&2
        exit 2
        ;;
    esac
done

[ -f "$LOCK" ] || { echo "fetch-conpty: missing $LOCK" >&2; exit 1; }
# shellcheck disable=SC1090
. "$LOCK"

case "$ARCH" in
host)
    case "$(uname -m)" in
    x86_64 | amd64) ARCH=x64 ;;
    aarch64 | arm64) ARCH=arm64 ;;
    i386 | i486 | i586 | i686) ARCH=x86 ;;
    *) echo "fetch-conpty: unsupported host architecture" >&2; exit 1 ;;
    esac
    ;;
x64 | arm64 | x86) ;;
*) echo "fetch-conpty: unsupported architecture: $ARCH" >&2; exit 2 ;;
esac

EXTRACTOR=""
if command -v unzip >/dev/null 2>&1; then
    EXTRACTOR=unzip
elif command -v bsdtar >/dev/null 2>&1; then
    EXTRACTOR=bsdtar
elif command -v 7z >/dev/null 2>&1; then
    EXTRACTOR=7z
elif command -v python >/dev/null 2>&1; then
    EXTRACTOR=python
elif command -v python3 >/dev/null 2>&1; then
    EXTRACTOR=python3
else
    echo "fetch-conpty: need unzip, bsdtar, 7z, or python" >&2
    exit 1
fi

extract_member() {
    archive=$1
    member=$2
    case "$EXTRACTOR" in
    unzip) unzip -p "$archive" "$member" ;;
    bsdtar) bsdtar -xOf "$archive" "$member" ;;
    7z) 7z x -so "$archive" "$member" 2>/dev/null ;;
    python | python3)
        "$EXTRACTOR" -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$archive" "$member"
        ;;
    esac
}

if command -v sha256sum >/dev/null 2>&1; then
    sha_file() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    sha_file() { shasum -a 256 "$1" | awk '{print $1}'; }
else
    echo "fetch-conpty: need sha256sum or shasum" >&2
    exit 1
fi

CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/portty/conpty"
mkdir -p "$CACHE" "$DEST"
NUPKG="$CACHE/$CONPTY_PACKAGE.$CONPTY_VERSION.nupkg"

if [ ! -f "$NUPKG" ]; then
    echo "fetch-conpty: downloading $CONPTY_PACKAGE $CONPTY_VERSION"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$NUPKG.part" "$CONPTY_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$NUPKG.part" "$CONPTY_URL"
    else
        echo "fetch-conpty: need curl or wget" >&2
        exit 1
    fi
    mv "$NUPKG.part" "$NUPKG"
fi

GOT=$(sha_file "$NUPKG")
if [ "$GOT" != "$CONPTY_NUPKG_SHA256" ]; then
    echo "fetch-conpty: package checksum mismatch" >&2
    echo "  expected: $CONPTY_NUPKG_SHA256" >&2
    echo "  got:      $GOT" >&2
    exit 1
fi

DLL="$DEST/conpty.dll"
HOST="$DEST/OpenConsole.exe"

echo "fetch-conpty: extracting $ARCH"
extract_member "$NUPKG" "runtimes/win-$ARCH/native/conpty.dll" >"$DLL.part"
extract_member "$NUPKG" "build/native/runtimes/$ARCH/OpenConsole.exe" >"$HOST.part"
mv "$DLL.part" "$DLL"
mv "$HOST.part" "$HOST"

cat >"$DEST/VERSION" <<EOF
package=$CONPTY_PACKAGE
version=$CONPTY_VERSION
arch=$ARCH
url=$CONPTY_URL
nupkg_sha256=$CONPTY_NUPKG_SHA256
source=https://github.com/microsoft/terminal
license=MIT
EOF

echo "fetch-conpty: $ARCH ready ($(sha_file "$DLL") / $(sha_file "$HOST"))"
