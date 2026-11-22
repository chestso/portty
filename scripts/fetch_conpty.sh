#!/bin/sh
# fetch_conpty.sh - download and extract the pinned Microsoft.Windows.Console.ConPTY
#
# Usage:
#   ./scripts/fetch_conpty.sh                 # host architecture
#   ./scripts/fetch_conpty.sh --arch all      # x64, arm64, x86
#   ./scripts/fetch_conpty.sh --arch arm64    # one architecture
#   ./scripts/fetch_conpty.sh --dest DIR      # override output directory
#   ./scripts/fetch_conpty.sh --force         # re-extract even if present
#
# The lock file (data/conpty/conpty.lock) pins the package version and the
# SHA-256 of the .nupkg.  Downloads are cached under
# ${XDG_CACHE_HOME:-$HOME/.cache}/conpty.  The binaries are extracted
# as <arch>/conpty.dll and <arch>/OpenConsole.exe, next to a SHA256SUMS file.
#
# This script is intentionally POSIX sh + curl/wget + unzip + sha256sum.
# It runs in MSYS2 UCRT64 on Windows and on Linux/macOS for `make dist`.

set -eu

cd "$(dirname "$0")/.."

DEST="data/conpty"
ARCH="host"
FORCE=0

while [ $# -gt 0 ]; do
	case "$1" in
	--arch)
		[ $# -ge 2 ] || {
			echo "fetch_conpty: --arch needs a value" >&2
			exit 2
		}
		ARCH=$2
		shift 2
		;;
	--arch=*)
		ARCH=${1#--arch=}
		shift
		;;
	--dest)
		[ $# -ge 2 ] || {
			echo "fetch_conpty: --dest needs a value" >&2
			exit 2
		}
		DEST=$2
		shift 2
		;;
	--dest=*)
		DEST=${1#--dest=}
		shift
		;;
	--force)
		FORCE=1
		shift
		;;
	-h | --help)
		sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "fetch_conpty: unknown option: $1" >&2
		exit 2
		;;
	esac
done

LOCK="$DEST/conpty.lock"
if [ ! -f "$LOCK" ]; then
	# Allow running from an arbitrary --dest: lock still lives in the tree.
	LOCK="data/conpty/conpty.lock"
fi
if [ ! -f "$LOCK" ]; then
	echo "fetch_conpty: missing $LOCK" >&2
	exit 1
fi

# shellcheck disable=SC1090
. "$LOCK"

if [ -z "${CONPTY_VERSION:-}" ] || [ -z "${CONPTY_URL:-}" ]; then
	echo "fetch_conpty: $LOCK does not define CONPTY_VERSION/CONPTY_URL" >&2
	exit 1
fi

host_arch() {
	case "$(uname -m)" in
	x86_64 | amd64) echo x64 ;;
	aarch64 | arm64) echo arm64 ;;
	i386 | i486 | i586 | i686) echo x86 ;;
	*)
		echo "fetch_conpty: unsupported host architecture: $(uname -m)" >&2
		return 1
		;;
	esac
}

case "$ARCH" in
host) ARCH=$(host_arch) ;;
x64 | arm64 | x86) ;;
all) : ;;
*)
	echo "fetch_conpty: unsupported architecture: $ARCH (use x64, arm64, x86, all or host)" >&2
	exit 2
	;;
esac

need() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "fetch_conpty: missing required tool: $1" >&2
		echo "  MSYS2: pacman -S --needed curl unzip coreutils" >&2
		exit 1
	}
}

# Pick an extractor. MSYS2 does not install `unzip` by default, but bsdtar
# and 7z are common; Python's zipfile is the last resort.
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
	echo "fetch_conpty: need unzip, bsdtar, 7z, or python" >&2
	echo "  MSYS2: pacman -S --needed curl libarchive" >&2
	exit 1
fi

# extract_member ARCHIVE MEMBER > OUT
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

SHA_TOOL=""
if command -v sha256sum >/dev/null 2>&1; then
	SHA_TOOL="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
	SHA_TOOL="shasum -a 256"
else
	echo "fetch_conpty: need sha256sum or shasum" >&2
	exit 1
fi

FETCH_TOOL=""
if command -v curl >/dev/null 2>&1; then
	FETCH_TOOL="curl -fsSL"
elif command -v wget >/dev/null 2>&1; then
	FETCH_TOOL="wget -qO-"
else
	echo "fetch_conpty: need curl or wget" >&2
	exit 1
fi

mkdir -p "$DEST"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/conpty"
mkdir -p "$CACHE"
NUPKG="$CACHE/$CONPTY_PACKAGE.$CONPTY_VERSION.nupkg"

sha_file() {
	$SHA_TOOL "$1" | awk '{print $1}'
}

if [ ! -f "$NUPKG" ]; then
	echo "fetch_conpty: downloading $CONPTY_PACKAGE $CONPTY_VERSION"
	if [ "$FETCH_TOOL" = "wget -qO-" ]; then
		wget -q -O "$NUPKG.part" "$CONPTY_URL"
	else
		curl -fsSL --retry 3 -o "$NUPKG.part" "$CONPTY_URL"
	fi
	mv "$NUPKG.part" "$NUPKG"
fi

GOT_SHA=$(sha_file "$NUPKG")
if [ "$GOT_SHA" != "$CONPTY_NUPKG_SHA256" ]; then
	echo "fetch_conpty: package checksum mismatch" >&2
	echo "  expected: $CONPTY_NUPKG_SHA256" >&2
	echo "  got:      $GOT_SHA" >&2
	echo "  file:     $NUPKG" >&2
	exit 1
fi

fetch_one() {
	arch=$1
	dir="$DEST/$arch"
	dll="$dir/conpty.dll"
	host="$dir/OpenConsole.exe"
	sums="$dir/SHA256SUMS"

	if [ "$FORCE" -eq 0 ] && [ -f "$dll" ] && [ -f "$host" ] && [ -f "$sums" ]; then
		if (cd "$dir" && $SHA_TOOL -c SHA256SUMS >/dev/null 2>&1); then
			echo "fetch_conpty: $arch up to date"
			return 0
		fi
	fi

	echo "fetch_conpty: extracting $arch"
	mkdir -p "$dir"
	if ! extract_member "$NUPKG" "runtimes/win-$arch/native/conpty.dll" >"$dll.part" 2>/dev/null; then
		echo "fetch_conpty: package is missing runtimes/win-$arch/native/conpty.dll" >&2
		rm -f "$dll.part"
		return 1
	fi
	if ! extract_member "$NUPKG" "build/native/runtimes/$arch/OpenConsole.exe" >"$host.part" 2>/dev/null; then
		echo "fetch_conpty: package is missing build/native/runtimes/$arch/OpenConsole.exe" >&2
		rm -f "$host.part" "$dll.part"
		return 1
	fi
	mv "$dll.part" "$dll"
	mv "$host.part" "$host"
	(
		cd "$dir"
		$SHA_TOOL conpty.dll OpenConsole.exe >SHA256SUMS
	)
	echo "fetch_conpty: $arch ready ($(sha_file "$dll") / $(sha_file "$host"))"
}

if [ "$ARCH" = "all" ]; then
	for a in x64 arm64 x86; do
		fetch_one "$a"
	done
else
	fetch_one "$ARCH"
fi

VERSION_WANTED="package=$CONPTY_PACKAGE
version=$CONPTY_VERSION
url=$CONPTY_URL
nupkg_sha256=$CONPTY_NUPKG_SHA256
source=https://github.com/microsoft/terminal
license=MIT"

VERSION_HAVE=
if [ -f "$DEST/VERSION" ]; then
	VERSION_HAVE=$(cat "$DEST/VERSION")
fi
if [ "$VERSION_HAVE" != "$VERSION_WANTED" ]; then
	# Do not rewrite an up-to-date stamp: a read-only dist tree (distcheck)
	# has the bundles and VERSION already, and mkdir/cat there would fail.
	printf '%s\n' "$VERSION_WANTED" >"$DEST/VERSION"
fi

echo "fetch_conpty: $CONPTY_PACKAGE $CONPTY_VERSION installed in $DEST"
