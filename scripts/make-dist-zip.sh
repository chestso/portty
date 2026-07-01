#!/bin/bash
# make-dist-zip.sh — assemble a portable Windows ZIP of portty.
#
# Produces a self-contained ZIP that runs without MSYS2 installed.
# Layout inside the ZIP:
#
#   portty-<version>/
#     portty.exe               — main binary
#     portty.cmd               — launcher (sets TERMINFO_DIRS, then runs exe)
#     README-PORTABLE.txt      — quick-start instructions
#     bin/portty.exe           — symlink-free copy for shortcut targets
#     share/
#       icons/hicolor/256x256/apps/portty.png
#       icons/hicolor/scalable/apps/portty.svg
#       icons/portty.ico
#       terminfo/70/portty-256color     (compiled by tic; 70 = hex 'p')
#       terminfo/70/portty-vty-256color
#       terminfo/70/portty
#       emacs/site-lisp/portty.el
#     *.dll                    — all runtime DLL dependencies
#
# Usage (invoked from the top-level Makefile):
#   make dist-portable
#
# Or directly:
#   scripts/make-dist-zip.sh [BUILD_DIR] [OUTPUT_DIR]
#
# BUILD_DIR defaults to "build", OUTPUT_DIR defaults to ".".
# The resulting file is portty-<version>-windows-x86_64.zip.

set -eu

# Resolve paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$SRC_DIR/build}"
OUTPUT_DIR="${2:-$SRC_DIR}"

# Get version from the same script configure uses
VERSION="$("$SRC_DIR/build-aux/git-version.sh" "$SRC_DIR" 2>/dev/null || echo "0.0.0-unknown")"

# --- Binary ----------------------------------------------------------------
# libtool wraps the real binary in .libs/ — use that if it exists.
REAL_EXE="$BUILD_DIR/src/portty.exe"
if [ -f "$BUILD_DIR/src/.libs/portty.exe" ]; then
	REAL_EXE="$BUILD_DIR/src/.libs/portty.exe"
fi

# Detect target architecture from the built binary
ARCH=$(file "$REAL_EXE" 2>/dev/null | grep -oq 'ARM64\|aarch64' && echo "aarch64" || echo "x86_64")
PKG_NAME="portty-${VERSION}-windows-${ARCH}"
STAGE_DIR="$OUTPUT_DIR/$PKG_NAME"
ZIP_FILE="$OUTPUT_DIR/${PKG_NAME}.zip"

echo "==> Packaging portty $VERSION"

# Fresh staging directory
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/share/icons/hicolor/256x256/apps" \
	"$STAGE_DIR/share/icons/hicolor/scalable/apps" \
	"$STAGE_DIR/share/emacs/site-lisp"

echo "==> Copying portty.exe (from $REAL_EXE)"
cp "$REAL_EXE" "$STAGE_DIR/portty.exe"
cp "$REAL_EXE" "$STAGE_DIR/bin/portty.exe"

# --- Data files ------------------------------------------------------------
echo "==> Copying data files (icons, emacs)"
cp "$SRC_DIR/data/icons/hicolor/256x256/apps/portty.png" \
	"$STAGE_DIR/share/icons/hicolor/256x256/apps/portty.png"
cp "$SRC_DIR/data/icons/hicolor/scalable/apps/portty.svg" \
	"$STAGE_DIR/share/icons/hicolor/scalable/apps/portty.svg"
# Copy .ico if it exists (may not if gen-ico wasn't run)
if [ -f "$SRC_DIR/data/icons/portty.ico" ]; then
	cp "$SRC_DIR/data/icons/portty.ico" \
		"$STAGE_DIR/share/icons/portty.ico"
fi
# Emacs integration
if [ -f "$SRC_DIR/data/portty.el" ]; then
	cp "$SRC_DIR/data/portty.el" \
		"$STAGE_DIR/share/emacs/site-lisp/portty.el"
fi

# --- Terminfo --------------------------------------------------------------
echo "==> Compiling terminfo"
mkdir -p "$STAGE_DIR/share/terminfo"
tic -x -o "$STAGE_DIR/share/terminfo" "$SRC_DIR/data/portty.ti" ||
	echo "  WARNING: tic failed — terminfo not included" >&2

# --- Runtime DLLs ----------------------------------------------------------
echo "==> Bundling runtime DLLs"
# Recursively resolve DLL dependencies via ldd, filtering out Windows system
# DLLs (those live in System32 and are always present on the user's machine).
# We use a worklist: start with the exe, run ldd on each binary, collect any
# non-system DLLs we haven't seen yet, then process those too.
SYSTEM_DLLS='ADVAPI32|KERNEL32|msvcrt|USER32|GDI32|SHELL32|ole32|OLE32|COMDLG32|SHLWAPI|WS2_32|WINSPOOL|VERSION|WINMM|IMM32|powrprof|PSAPI|DNSAPI|IPHLPAPI|WINHTTP|Secur32|SSPICLI|BCRYPT|NCRYPT|NTDLL|RPCRT4|SETUPAPI|CFGMG32|DEVOBJ|dwmapi|dwrite|D3D|DXGI|windows.storage|processthreadsapi|kernelbase|ucrtbase|msvcp|api-ms-win|win32u|combase|SHCORE|clbcat|propsys|MMDevApi|USERENV|AUTHZ|cryptbase|wldap32|wkscli|netutils|SAMLIB|secur32|SensApi|dpapi|apphelp|sechost|gdi32full|OLEAUT32|USP10|cfgmgr32|schannel|msvcp_win|gpapi|devobj|xtajit'

# Use temp files for the queue and seen-set to avoid subshell variable loss
QUEUE="$(mktemp)"
SEEN="$(mktemp)"
echo "$REAL_EXE" >"$QUEUE"

while [ -s "$QUEUE" ]; do
	current="$(head -1 "$QUEUE")"
	# Remove first line from queue
	tail -n +2 "$QUEUE" >"$QUEUE.tmp" && mv "$QUEUE.tmp" "$QUEUE"

	[ -z "$current" ] && continue
	[ ! -f "$current" ] && continue

	# Process all DLL dependencies of the current binary
	# timeout prevents ldd from hanging on certain DLLs (ARM64)
	echo "  ldd: $(basename "$current")"
	LDD_OUT="$(timeout 10 ldd "$current" 2>&1)"
	LDD_RC=$?
	if [ "$LDD_RC" -ne 0 ]; then
		echo "  WARNING: ldd timed out or failed (rc=$LDD_RC) on: $current"
		continue
	fi
	for dll in $(echo "$LDD_OUT" |
		grep -i '\.dll' | awk '{print $3}'); do
		[ -z "$dll" ] && continue
		[ ! -f "$dll" ] && continue
		base="$(basename "$dll")"
		# Skip Windows system DLLs
		if echo "$base" | grep -iqE "^($SYSTEM_DLLS)"; then
			continue
		fi
		# Skip if already seen
		if grep -qxF "$base" "$SEEN" 2>/dev/null; then
			continue
		fi
		echo "$base" >>"$SEEN"
		cp "$dll" "$STAGE_DIR/"
		echo "  bundled: $base"
		# Enqueue for recursive resolution
		echo "$dll" >>"$QUEUE"
	done
done

rm -f "$QUEUE" "$SEEN"

# --- Launcher script -------------------------------------------------------
echo "==> Writing launcher script"
cat >"$STAGE_DIR/portty.cmd" <<'LAUNCHER'
@echo off
:: portty.cmd — launcher for portable portty
:: Sets TERMINFO_DIRS so MSYS2/ncurses programs find our terminfo,
:: then starts portty.exe. All paths are relative to this script.
setlocal
set "HERE=%~dp0"
set "TERMINFO_DIRS=%HERE%share\terminfo;%HOME%\.terminfo;"
for %%I in ("%HERE%portty.exe") do set "PORTTY_EXE=%%~fI"
start "" /b "%PORTTY_EXE%" %*
endlocal
LAUNCHER

# --- README ----------------------------------------------------------------
echo "==> Writing README-PORTABLE.txt"
cat >"$STAGE_DIR/README-PORTABLE.txt" <<'README'
Portty (Portable)
=================

This is a self-contained build of portty — no MSYS2 installation required.

Quick start:
  1. Extract the ZIP anywhere.
  2. Double-click portty.cmd (or portty.exe directly).

portty.cmd sets up TERMINFO_DIRS so that MSYS2/ncurses programs
(find, vim, etc.) find portty's terminfo definitions. If you run
portty.exe directly, terminfo may not be found by child programs.

To create a Start Menu shortcut:
  Right-click portty.cmd > Create shortcut
  Move the shortcut to: %AppData%\Microsoft\Windows\Start Menu\Programs\

To uninstall:
  Delete the folder. portty stores no data outside its own directory
  (config: %AppData%\portty\portty.conf is the only exception).
README

# --- Create ZIP ------------------------------------------------------------
echo "==> Creating ZIP: $(basename "$ZIP_FILE")"
rm -f "$ZIP_FILE"
# 7z is a native Windows binary — it needs Windows-style paths. zip (if
# present) is an MSYS2 binary and prefers POSIX paths. Convert as needed.
if command -v 7z >/dev/null 2>&1; then
	win_zip="$(cygpath -w "$ZIP_FILE" 2>/dev/null || echo "$ZIP_FILE")"
	# cd to OUTPUT_DIR so 7z stores relative paths (just the pkg name)
	win_pkg="$(cygpath -w "$PKG_NAME" 2>/dev/null || echo "$PKG_NAME")"
	(cd "$OUTPUT_DIR" && 7z a -tzip -mx=9 "$win_zip" "$win_pkg" >/dev/null)
elif command -v zip >/dev/null 2>&1; then
	(cd "$OUTPUT_DIR" && zip -r -9 "$ZIP_FILE" "$PKG_NAME" >/dev/null)
else
	echo "ERROR: Neither 7z nor zip found — cannot create ZIP" >&2
	echo " staged directory left at: $STAGE_DIR" >&2
	exit 1
fi

# Clean up staging dir
rm -rf "$STAGE_DIR"

echo "==> Done: $ZIP_FILE"
echo "    Size: $(du -h "$ZIP_FILE" | cut -f1)"
