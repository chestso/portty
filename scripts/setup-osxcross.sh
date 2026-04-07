#!/bin/bash
# Set up osxcross cross-compilation toolchain for building bloom-terminal on Linux.
#
# Usage:
#   ./scripts/setup-osxcross.sh                              # interactive
#   ./scripts/setup-osxcross.sh /path/to/Command_Line_Tools.dmg
#   ./scripts/setup-osxcross.sh /path/to/Xcode_*.xip
#   ./scripts/setup-osxcross.sh /path/to/MacOSX14.4.sdk.tar.xz
#
# The macOS SDK cannot be downloaded automatically (Apple ID required).
# Download one of these from https://developer.apple.com/download/all/:
#   - "Command Line Tools for Xcode 16" (.dmg) — smallest, ~700 MB
#   - "Xcode 16" (.xip) — larger, ~7 GB
#
# After setup, add osxcross to your PATH:
#   export PATH="$HOME/osxcross/target/bin:$PATH"

set -e
set -u

OSXCROSS_DIR="${OSXCROSS_DIR:-$HOME/osxcross}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

# --- Check prerequisites ---
check_prereqs() {
    local missing=()
    for cmd in git cmake clang clang++ make patch; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done

    if [ ${#missing[@]} -gt 0 ]; then
        error "Missing tools: ${missing[*]}"
        echo "Install with: sudo dnf install git cmake clang make patch"
        exit 1
    fi

    # osxcross needs these dev libraries
    local missing_pkgs=()
    for pkg in libxml2-devel openssl-devel; do
        rpm -q "$pkg" &>/dev/null || missing_pkgs+=("$pkg")
    done
    if [ ${#missing_pkgs[@]} -gt 0 ]; then
        error "Missing packages: ${missing_pkgs[*]}"
        echo "Install with: sudo dnf install ${missing_pkgs[*]}"
        exit 1
    fi
}

# --- Clone osxcross ---
clone_osxcross() {
    if [ -d "$OSXCROSS_DIR" ]; then
        info "osxcross already cloned: $OSXCROSS_DIR"
        return
    fi
    info "Cloning osxcross to $OSXCROSS_DIR..."
    git clone https://github.com/tpoechtrager/osxcross.git "$OSXCROSS_DIR"
}

# --- Extract SDK from .dmg / .xip / use existing tarball ---
prepare_sdk() {
    local input="$1"

    # Already have an SDK tarball in osxcross/tarballs/?
    local existing
    existing=$(find "$OSXCROSS_DIR/tarballs" -name "MacOSX*.sdk.tar.*" 2>/dev/null | head -1)
    if [ -n "$existing" ]; then
        info "SDK tarball already present: $existing"
        return
    fi

    if [ -z "$input" ]; then
        error "No SDK source provided."
        echo ""
        echo "Download one of these from https://developer.apple.com/download/all/ (free Apple ID):"
        echo "  - 'Command Line Tools for Xcode 16' (.dmg)  ~700 MB"
        echo "  - 'Xcode 16' (.xip)                         ~7 GB"
        echo ""
        echo "Then rerun:"
        echo "  $0 /path/to/downloaded/file"
        exit 1
    fi

    if [ ! -f "$input" ]; then
        error "File not found: $input"
        exit 1
    fi

    mkdir -p "$OSXCROSS_DIR/tarballs"

    case "$input" in
        *.dmg)
            info "Extracting SDK from Command Line Tools .dmg..."
            cd "$OSXCROSS_DIR"
            # gen_sdk_package_tools_dmg.sh extracts the SDK and places a tarball in tarballs/
            bash tools/gen_sdk_package_tools_dmg.sh "$input"
            # Move generated tarball to tarballs/
            local sdk_tar
            sdk_tar=$(ls MacOSX*.sdk.tar.* 2>/dev/null | head -1)
            if [ -n "$sdk_tar" ]; then
                mv "$sdk_tar" tarballs/
                info "SDK tarball: tarballs/$sdk_tar"
            fi
            cd - >/dev/null
            ;;
        *.xip)
            info "Extracting SDK from Xcode .xip (this takes a while)..."
            cd "$OSXCROSS_DIR"
            bash tools/gen_sdk_package_pbzx.sh "$input"
            local sdk_tar
            sdk_tar=$(ls MacOSX*.sdk.tar.* 2>/dev/null | head -1)
            if [ -n "$sdk_tar" ]; then
                mv "$sdk_tar" tarballs/
                info "SDK tarball: tarballs/$sdk_tar"
            fi
            cd - >/dev/null
            ;;
        *.tar.*)
            info "Copying SDK tarball..."
            cp "$input" "$OSXCROSS_DIR/tarballs/"
            ;;
        *)
            error "Unrecognized file type: $input"
            echo "Expected .dmg (Command Line Tools), .xip (Xcode), or .tar.* (pre-packaged SDK)"
            exit 1
            ;;
    esac

    # Verify we got a tarball
    existing=$(find "$OSXCROSS_DIR/tarballs" -name "MacOSX*.sdk.tar.*" 2>/dev/null | head -1)
    if [ -z "$existing" ]; then
        error "SDK extraction failed — no MacOSX*.sdk.tar.* found in tarballs/"
        exit 1
    fi
}

# --- Build osxcross ---
build_osxcross() {
    cd "$OSXCROSS_DIR"

    # Check if already built
    local target_bin="$OSXCROSS_DIR/target/bin"
    if ls "$target_bin"/*-clang &>/dev/null 2>&1; then
        info "osxcross already built"
        local cc
        cc=$(ls "$target_bin"/*-apple-darwin*-clang 2>/dev/null | head -1)
        info "Cross-compiler: $(basename "$cc")"
        cd - >/dev/null
        return
    fi

    info "Building osxcross (this takes a few minutes)..."
    UNATTENDED=1 ./build.sh

    if [ $? -ne 0 ]; then
        error "osxcross build failed"
        cd - >/dev/null
        exit 1
    fi

    local cc
    cc=$(ls "$target_bin"/*-apple-darwin*-clang 2>/dev/null | head -1)
    info "osxcross built successfully!"
    info "Cross-compiler: $(basename "$cc")"
    cd - >/dev/null
}

# --- Build compiler-rt (darwin builtins) ---
build_compiler_rt() {
    local rt_lib="$OSXCROSS_DIR/target/lib/darwin/libclang_rt.osx.a"
    if [ -f "$rt_lib" ]; then
        info "compiler-rt already built: $rt_lib"
        return
    fi

    cd "$OSXCROSS_DIR"
    if [ ! -f "build_compiler_rt.sh" ]; then
        error "build_compiler_rt.sh not found in osxcross"
        cd - >/dev/null
        exit 1
    fi

    info "Building compiler-rt (darwin builtins)..."
    ./build_compiler_rt.sh

    if [ $? -ne 0 ]; then
        error "compiler-rt build failed"
        cd - >/dev/null
        exit 1
    fi

    # Install to osxcross target (no sudo needed)
    mkdir -p "$OSXCROSS_DIR/target/lib/darwin"
    cp build/compiler-rt/compiler-rt/build/lib/darwin/libclang_rt.osx.a \
        "$OSXCROSS_DIR/target/lib/darwin/"
    info "compiler-rt installed: $rt_lib"
    cd - >/dev/null
}

# --- Main ---
main() {
    local sdk_input="${1:-}"

    info "Setting up osxcross cross-compilation toolchain"
    echo ""

    check_prereqs
    clone_osxcross
    prepare_sdk "$sdk_input"
    build_osxcross
    build_compiler_rt

    echo ""
    info "Setup complete!"
    echo ""
    echo "Add to your shell profile:"
    echo "  export PATH=\"$OSXCROSS_DIR/target/bin:\$PATH\""
    echo ""
    echo "Then cross-compile bloom-terminal:"
    echo "  ./build.sh --osxcross"
}

main "$@"
