#!/bin/bash
# make-dist-zip.sh — assemble a portable ZIP of portty.
#
# On Windows, produces a self-contained ZIP with the exe, DLLs, terminfo,
# and a launcher .cmd that runs without MSYS2 installed.
#
# On macOS, produces a ZIP containing a .app bundle with the binary,
# bundled dylibs (via otool/install_name_tool), terminfo, resources, and
# an Info.plist. Extract and double-click to run.
#
# Usage (invoked from the top-level Makefile):
#   make dist-portable
#
# Or directly:
#   scripts/make-dist-zip.sh [BUILD_DIR] [OUTPUT_DIR] [BACKEND]
#
# BUILD_DIR defaults to "build", OUTPUT_DIR defaults to ".", BACKEND defaults to "sdl3".
# On Windows: produces portty-<version>-windows-<arch>-<backend>.zip
# On macOS:   produces portty-<version>-macos-<arch>-<backend>.zip

set -eu

# Resolve paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$SRC_DIR/build}"
OUTPUT_DIR="$(cd "${2:-$SRC_DIR}" && pwd)"
BACKEND="${3:-sdl3}"

# Get version from the same script configure uses
VERSION="$("$SRC_DIR/build-aux/git-version.sh" "$SRC_DIR" 2>/dev/null || echo "0.0.0-unknown")"

# ── Platform detection ──────────────────────────────────────────────

if [ "$(uname -s)" = "Darwin" ]; then
	IS_MACOS=1
else
	IS_MACOS=0
fi

if [ "$IS_MACOS" -eq 1 ]; then
	# --- macOS .app bundle ---------------------------------------------------

	REAL_EXE="$BUILD_DIR/src/portty"
	if [ -f "$BUILD_DIR/src/.libs/portty" ]; then
		REAL_EXE="$BUILD_DIR/src/.libs/portty"
	fi

	ARCH=$(file "$REAL_EXE" 2>/dev/null | grep -oq 'arm64\|aarch64' && echo "arm64" || echo "x86_64")
	PKG_NAME="portty-${VERSION}-macos-${ARCH}-${BACKEND}"
	STAGE_DIR="$OUTPUT_DIR/$PKG_NAME"
	APP_DIR="$STAGE_DIR/Portty.app"
	ZIP_FILE="$OUTPUT_DIR/${PKG_NAME}.zip"

	echo "==> Packaging portty $VERSION (macOS $ARCH, $BACKEND)"

	rm -rf "$STAGE_DIR"
	mkdir -p "$APP_DIR/Contents/MacOS" \
		"$APP_DIR/Contents/Resources/share/icons/hicolor/scalable/apps" \
		"$APP_DIR/Contents/Resources/share/emacs/site-lisp" \
		"$APP_DIR/Contents/Resources/share/terminfo"

	# --- Binary ---
	echo "==> Copying portty binary (from $REAL_EXE)"
	cp "$REAL_EXE" "$APP_DIR/Contents/MacOS/portty"

	# --- Bundle dylibs ---
	echo "==> Bundling dylibs"
	DYLIB_DIR="$APP_DIR/Contents/Frameworks"
	mkdir -p "$DYLIB_DIR"

	SYSTEM_LIBS='libSystem\.|libc\.\|libobjc\.\|libiconv\.\|libcharset\.'

	bundle_dylib() {
		local lib_path="$1"
		local base
		base="$(basename "$lib_path")"

		# Skip system libraries
		if echo "$base" | grep -qE "^($SYSTEM_LIBS)"; then
			return
		fi

		# Skip if already bundled
		if [ -f "$DYLIB_DIR/$base" ]; then
			return
		fi

		echo "  bundling: $base"
		cp "$lib_path" "$DYLIB_DIR/$base"
		chmod 755 "$DYLIB_DIR/$base"

		# Fix install name in the dylib itself so it finds its siblings
		install_name_tool -id "@rpath/$base" "$DYLIB_DIR/$base" 2>/dev/null || true

		# Recurse into the dylib's own dependencies
		local deps
		deps=$(otool -L "$lib_path" 2>/dev/null | tail -n +2 | awk '{print $1}')
		for dep in $deps; do
			[ -f "$dep" ] || continue
			bundle_dylib "$dep"
		done
	}

	# Process the main binary's dependencies
	MAIN_DEPS=$(otool -L "$REAL_EXE" 2>/dev/null | tail -n +2 | awk '{print $1}')
	for dep in $MAIN_DEPS; do
		[ -f "$dep" ] || continue
		bundle_dylib "$dep"
	done

	# Fix install names in the main binary to point to @rpath
	for dep in $(otool -L "$REAL_EXE" 2>/dev/null | tail -n +2 | awk '{print $1}'); do
		base="$(basename "$dep")"
		if [ -f "$DYLIB_DIR/$base" ]; then
			install_name_tool -change "$dep" "@rpath/$base" "$APP_DIR/Contents/MacOS/portty" 2>/dev/null || true
		fi
	done

	# Add LC_RPATH pointing to the Frameworks directory
	install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP_DIR/Contents/MacOS/portty" 2>/dev/null || true

	# --- Data files ---
	echo "==> Copying data files (icons, emacs)"
	cp "$SRC_DIR/data/icons/hicolor/scalable/apps/portty.svg" \
		"$APP_DIR/Contents/Resources/share/icons/hicolor/scalable/apps/portty.svg"
	if [ -f "$SRC_DIR/data/portty.el" ]; then
		cp "$SRC_DIR/data/portty.el" \
			"$APP_DIR/Contents/Resources/share/emacs/site-lisp/portty.el"
	fi

	# --- Terminfo ---
	echo "==> Compiling terminfo"
	tic -x -o "$APP_DIR/Contents/Resources/share/terminfo" "$SRC_DIR/data/portty.ti" ||
		echo "  WARNING: tic failed — terminfo not included" >&2

	# --- Info.plist ---
	echo "==> Writing Info.plist"
	cat >"$APP_DIR/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>Portty</string>
    <key>CFBundleDisplayName</key>
    <string>Portty</string>
    <key>CFBundleIdentifier</key>
    <string>so.chestso.portty</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleExecutable</key>
    <string>portty</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>portty.icns</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
PLIST

	# Generate .icns from SVG if tools are available, else skip
	if command -v rsvg-convert >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
		echo "==> Generating app icon"
		ICONSET="$APP_DIR/Contents/Resources/icon.iconset"
		mkdir -p "$ICONSET"
		for size in 16 32 64 128 256 512; do
			rsvg-convert -w "$size" -h "$size" "$SRC_DIR/data/icons/hicolor/scalable/apps/portty.svg" \
				-o "$ICONSET/icon_${size}x${size}.png"
		done
		iconutil -c icns "$ICONSET" -o "$APP_DIR/Contents/Resources/portty.icns"
		rm -rf "$ICONSET"
	else
		echo "  WARNING: rsvg-convert or iconutil not found — skipping icon"
	fi

	# --- Docs ---
	echo "==> Copying documentation"
	for doc in README.md COPYING; do
		if [ -f "$SRC_DIR/$doc" ]; then
			cp "$SRC_DIR/$doc" "$STAGE_DIR/$doc"
		fi
	done

	# --- PkgInfo ---
	echo "APPL????" >"$APP_DIR/Contents/PkgInfo"

	# --- Launcher wrapper ---
	# The executable in Contents/MacOS/ is the real binary. We create a
	# wrapper script that sets TERMINFO_DIRS so child programs (vim, etc.)
	# find the bundled terminfo, then launches the real binary.
	REAL_BIN="$APP_DIR/Contents/MacOS/portty"
	mv "$REAL_BIN" "$REAL_BIN.real"
	cat >"$REAL_BIN" <<WRAPPER
#!/bin/bash
RES_DIR="\$(dirname "\$0")/../Resources"
export TERMINFO_DIRS="\$RES_DIR/share/terminfo:\${HOME}/.terminfo:\${TERMINFO_DIRS:-}"
exec "\$(dirname "\$0")/portty.real" "\$@"
WRAPPER
	chmod 755 "$REAL_BIN"

	# Fix rpath on the renamed binary too
	install_name_tool -add_rpath "@executable_path/../Frameworks" "$REAL_BIN.real" 2>/dev/null || true

	# --- README ---
	echo "==> Writing README-PORTABLE.txt"
	cat >"$STAGE_DIR/README-PORTABLE.txt" <<'README'
Portty (macOS Portable)
=======================

This is a self-contained .app bundle of portty.

Quick start:
  1. Extract the ZIP.
  2. Drag Portty.app to /Applications (or anywhere).
  3. Double-click Portty.app.

The bundle includes all required dylibs and terminfo. No Homebrew
or external dependencies needed.

To uninstall:
  Delete Portty.app. Config is stored in ~/Library/Application Support/portty/.
README

	# --- Create ZIP ---
	echo "==> Creating ZIP: $(basename "$ZIP_FILE")"
	rm -f "$ZIP_FILE"
	(cd "$OUTPUT_DIR" && zip -r -9 "$(basename "$ZIP_FILE")" "$PKG_NAME" >/dev/null)

	rm -rf "$STAGE_DIR"

	echo "==> Done: $ZIP_FILE"
	echo "    Size: $(du -h "$ZIP_FILE" | cut -f1)"

else
	# ── Windows (MSYS2) ───────────────────────────────────────────────
	# Original Windows portable ZIP logic below.

	# --- Binary ----------------------------------------------------------------
	# libtool wraps the real binary in .libs/ — use that if it exists.
	REAL_EXE="$BUILD_DIR/src/portty.exe"
	if [ -f "$BUILD_DIR/src/.libs/portty.exe" ]; then
		REAL_EXE="$BUILD_DIR/src/.libs/portty.exe"
	fi

	# Detect target architecture from the built binary
	ARCH=$(file "$REAL_EXE" 2>/dev/null | grep -oq 'ARM64\|aarch64' && echo "aarch64" || echo "x86_64")
	PKG_NAME="portty-${VERSION}-windows-${ARCH}-${BACKEND}"
	STAGE_DIR="$OUTPUT_DIR/$PKG_NAME"
	ZIP_FILE="$OUTPUT_DIR/${PKG_NAME}.zip"

	echo "==> Packaging portty $VERSION"

	# Fresh staging directory
	rm -rf "$STAGE_DIR"
	mkdir -p "$STAGE_DIR/share/icons/hicolor/scalable/apps" \
		"$STAGE_DIR/share/emacs/site-lisp"

	echo "==> Copying portty.exe (from $REAL_EXE)"
	cp "$REAL_EXE" "$STAGE_DIR/portty.exe"

	# --- Data files ------------------------------------------------------------
	echo "==> Copying data files (icons, emacs)"
	cp "$SRC_DIR/data/icons/hicolor/scalable/apps/portty.svg" \
		"$STAGE_DIR/share/icons/hicolor/scalable/apps/portty.svg"
	# Emacs integration
	if [ -f "$SRC_DIR/data/portty.el" ]; then
		cp "$SRC_DIR/data/portty.el" \
			"$STAGE_DIR/share/emacs/site-lisp/portty.el"
	fi

	# --- Docs ------------------------------------------------------------------
	echo "==> Copying documentation"
	for doc in README.md COPYING; do
		if [ -f "$SRC_DIR/$doc" ]; then
			cp "$SRC_DIR/$doc" "$STAGE_DIR/$doc"
		fi
	done

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
		(cd "$OUTPUT_DIR" && zip -r -9 "$(basename "$ZIP_FILE")" "$PKG_NAME" >/dev/null)
	else
		echo "ERROR: Neither 7z nor zip found — cannot create ZIP" >&2
		echo " staged directory left at: $STAGE_DIR" >&2
		exit 1
	fi

	# Clean up staging dir
	rm -rf "$STAGE_DIR"

	echo "==> Done: $ZIP_FILE"
	echo "    Size: $(du -h "$ZIP_FILE" | cut -f1)"
fi
