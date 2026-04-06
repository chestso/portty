#!/bin/bash
# Build Script for bloom-terminal terminal emulator

set -e # Exit on any error
set -u # Treat unset variables as errors

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
PROJECT_NAME="bloom-terminal"
BUILD_DIR="build"
INSTALL_PREFIX="$HOME/.local"
ENABLE_DEBUG=true
PARALLEL_JOBS=$(nproc)

# VM configuration
VM_DIR="vm"
VM_DISK="$VM_DIR/win11.qcow2"
VM_ISO="$VM_DIR/win11-ltsc-eval.iso"
VM_VIRTIO="$VM_DIR/virtio-win.iso"
VM_OVMF_VARS="$VM_DIR/OVMF_VARS.fd"
VM_TPM_DIR="$VM_DIR/tpm"
VM_TRANSFER="$VM_DIR/transfer.img"
VM_AUTOUNATTEND="$VM_DIR/autounattend.img"
OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd"

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running on a supported Fedora version
check_os() {
    log_info "Checking operating system..."
    if [ -f /etc/fedora-release ]; then
        FEDORA_VERSION=$(grep -oP 'Fedora release \K\d+' /etc/fedora-release)
        if [ "$FEDORA_VERSION" -ge 41 ]; then
            log_info "Running on Fedora $FEDORA_VERSION"
        else
            log_warn "Running on Fedora $FEDORA_VERSION (Fedora 41+ recommended)"
        fi
    else
        log_warn "Not running on Fedora. Some dependencies may differ."
    fi
}

# Generate configure script
generate_configure() {
    log_info "Generating configure script..."

    # Clean up any existing generated files
    log_info "Cleaning up generated files..."
    rm -f configure aclocal.m4
    rm -rf autom4te.cache

    if [ -f configure ]; then
        log_info "Removing existing configure script"
        rm -f configure
    fi

    autoreconf -fvi

    if [ $? -ne 0 ]; then
        log_error "Failed to generate configure script"
        return 1
    fi

    log_info "Configure script generated successfully"
    return 0
}

# Configure the build
configure_build() {
    log_info "Configuring build..."

    # Create build directory if it doesn't exist
    if [ -d "$BUILD_DIR" ]; then
        log_info "Removing existing build directory"
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Build configure command
    CONFIGURE_CMD="../configure --prefix=$INSTALL_PREFIX"

    if [ "$ENABLE_DEBUG" = true ]; then
        CONFIGURE_CMD="$CONFIGURE_CMD CFLAGS='-O0 -g3 -DDEBUG'"
    fi

    log_info "Running: $CONFIGURE_CMD"
    eval $CONFIGURE_CMD

    if [ $? -ne 0 ]; then
        log_error "Configure failed"
        if [ -f "config.log" ]; then
            log_error "Last 50 lines of config.log:"
            tail -50 config.log
        fi
        cd ..
        return 1
    fi

    cd ..
    log_info "Build configured successfully"
    return 0
}

# Build the project
build_project() {
    local use_bear=${1:-false}

    log_info "Building project..."

    cd "$BUILD_DIR"

    if [ "$use_bear" = true ]; then
        log_info "Generating compile_commands.json with bear..."
        bear --output ../compile_commands.json -- make -j"$PARALLEL_JOBS"
    else
        log_info "Running make with $PARALLEL_JOBS parallel jobs"
        make -j"$PARALLEL_JOBS"
    fi

    if [ $? -ne 0 ]; then
        log_error "Build failed"
        cd ..
        return 1
    fi

    cd ..
    log_info "Project built successfully"
    return 0
}

# Install the project (optional)
install_project() {
    log_info "Installing project to $INSTALL_PREFIX..."

    cd "$BUILD_DIR"

    if ! make install; then
        log_error "Installation failed"
        cd ..
        return 1
    fi

    cd ..
    log_info "Project installed successfully"
    return 0
}

# Run the application
run_application() {
    log_info "Running application..."

    if [ -f "$BUILD_DIR/src/$PROJECT_NAME" ]; then
        cd "$BUILD_DIR"
        log_info "Starting $PROJECT_NAME..."
        ./src/$PROJECT_NAME &
        APP_PID=$!
        cd ..

        log_info "Application started with PID $APP_PID"
        log_info "Press Ctrl+C to stop the application"

        # Wait for user interrupt
        wait $APP_PID
    else
        log_error "Application not found at $BUILD_DIR/src/$PROJECT_NAME"
        return 1
    fi

    return 0
}

# Generate a bench script for profiling (bash commands that exercise rendering)
generate_bench_script() {
    local output_file="$1"
    log_info "Generating bench script..."

    cat >"$output_file" <<'SCRIPT'
#!/bin/bash
# Bench script for profiling: exercises colors, scrollback, emoji, box drawing

# SGR color output
echo -e "\e[31mRed\e[32mGreen\e[34mBlue\e[0m"
echo -e "\e[1;33mBold Yellow\e[0m and \e[4;36mUnderline Cyan\e[0m"

# Fill screen and exercise scrollback
for i in $(seq 1 50); do echo "Line $i: The quick brown fox jumps over the lazy dog"; done

# Truecolor gradient
for i in $(seq 0 5 255); do printf "\e[38;2;${i};$((255-i));128m#\e[0m"; done; echo

# Emoji
echo "Emoji: 😀🎉🚀💻🔥✨🌍🎨📚🔧"

# Box drawing
echo "┌────────────────────┐"
echo "│ Box drawing test   │"
echo "└────────────────────┘"

# Heavy scrolling
for i in $(seq 1 200); do echo "Scroll $i: Lorem ipsum dolor sit amet"; done
SCRIPT

    chmod +x "$output_file"
    log_info "Bench script written to $output_file"
}

# Run profiling build, benchmark, and report
run_profiling() {
    log_info "Building with profiling instrumentation (-pg)..."

    # Generate configure script
    if ! generate_configure; then
        exit 1
    fi

    # Configure with profiling flags
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    eval "../configure --prefix=$INSTALL_PREFIX CFLAGS='-O2 -g -pg' LDFLAGS='-pg'"
    if [ $? -ne 0 ]; then
        log_error "Configure with profiling flags failed"
        cd ..
        exit 1
    fi
    cd ..

    # Build
    cd "$BUILD_DIR"
    make -j"$PARALLEL_JOBS"
    if [ $? -ne 0 ]; then
        log_error "Profiling build failed"
        cd ..
        exit 1
    fi
    cd ..

    # Generate bench script
    local bench_script="bench-script.sh"
    generate_bench_script "$bench_script"

    # Run benchmark (terminal exits when bash script finishes)
    log_info "Running benchmark (-- bash $bench_script)..."
    rm -f gmon.out
    ./"$BUILD_DIR"/src/"$PROJECT_NAME" -- bash "$bench_script"
    if [ $? -ne 0 ]; then
        log_error "Benchmark run failed"
        rm -f "$bench_script"
        exit 1
    fi

    # Check gmon.out was generated
    if [ ! -f gmon.out ]; then
        log_error "gmon.out not generated (profiling data missing)"
        rm -f "$bench_script"
        exit 1
    fi

    # Generate report
    log_info "Generating gprof report..."
    local report_file="profile-report.txt"
    gprof ./"$BUILD_DIR"/src/"$PROJECT_NAME" gmon.out >"$report_file"

    echo ""
    echo "=== Top 20 Functions by Cumulative Time ==="
    head -30 "$report_file" | tail -20
    echo ""

    log_info "Full report saved to: $report_file"

    # Cleanup
    rm -f "$bench_script"
}

# Format source files
format_sources() {
    log_info "Formatting source files..."

    # Format C/C++ files with clang-format
    if command -v clang-format &>/dev/null; then
        log_info "Formatting C/C++ files with clang-format..."
        find src/ -name "*.c" -o -name "*.h" | xargs clang-format -i
    else
        log_warn "clang-format not found. Skipping C/C++ formatting."
    fi

    # Format shell scripts with shfmt
    if command -v shfmt &>/dev/null; then
        log_info "Formatting shell scripts with shfmt..."
        find examples/ -name "*.sh" | xargs shfmt -w
    else
        log_warn "shfmt not found. Skipping shell script formatting."
    fi

    # Format markdown files with prettier
    if command -v prettier &>/dev/null; then
        log_info "Formatting markdown files with prettier..."
        find . -name "*.md" | xargs prettier --write
    else
        log_warn "prettier not found. Skipping markdown formatting."
    fi

    log_info "Formatting completed."
}

# Cross-compile for Windows using mingw64
build_mingw64() {
    log_info "Cross-compiling for Windows (mingw64)..."

    local MINGW_BUILD_DIR="build-mingw64"
    local MINGW_HOST="x86_64-w64-mingw32"
    local MINGW_CC="${MINGW_HOST}-gcc"
    local MINGW_AR="${MINGW_HOST}-ar"
    local MINGW_PKG_CONFIG="${MINGW_HOST}-pkg-config"
    local MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"
    local DEPS_DIR="deps"
    local VTERM_VERSION="0.3.3"
    local VTERM_URL="https://www.leonerd.org.uk/code/libvterm/libvterm-${VTERM_VERSION}.tar.gz"
    local VTERM_DIR="${DEPS_DIR}/libvterm-${VTERM_VERSION}"
    local VTERM_TARBALL="${DEPS_DIR}/libvterm-${VTERM_VERSION}.tar.gz"

    # Check for cross-compiler
    if ! command -v "$MINGW_CC" &>/dev/null; then
        log_error "mingw64 cross-compiler not found: $MINGW_CC"
        log_error "Install with: sudo dnf install mingw64-gcc"
        exit 1
    fi

    # Check for required mingw64 packages
    local missing_pkgs=()
    for pkg in mingw64-SDL3 mingw64-freetype mingw64-harfbuzz mingw64-libpng; do
        if ! rpm -q "$pkg" &>/dev/null; then
            missing_pkgs+=("$pkg")
        fi
    done
    if [ ${#missing_pkgs[@]} -gt 0 ]; then
        log_error "Missing mingw64 packages: ${missing_pkgs[*]}"
        log_error "Install with: sudo dnf install ${missing_pkgs[*]}"
        exit 1
    fi

    # --- Cross-compile libvterm ---
    mkdir -p "$DEPS_DIR"

    if [ ! -f "$VTERM_TARBALL" ]; then
        log_info "Downloading libvterm ${VTERM_VERSION}..."
        curl -L -o "$VTERM_TARBALL" "$VTERM_URL"
        if [ $? -ne 0 ]; then
            log_error "Failed to download libvterm"
            rm -f "$VTERM_TARBALL"
            exit 1
        fi
    fi

    if [ ! -d "$VTERM_DIR" ]; then
        log_info "Extracting libvterm..."
        tar -xzf "$VTERM_TARBALL" -C "$DEPS_DIR"
    fi

    local VTERM_BUILD="${VTERM_DIR}/build-mingw64"
    if [ ! -f "${VTERM_BUILD}/libvterm.a" ]; then
        log_info "Cross-compiling libvterm for mingw64..."
        mkdir -p "$VTERM_BUILD"
        for f in "${VTERM_DIR}"/src/*.c; do
            local base
            base=$(basename "$f" .c)
            "$MINGW_CC" -Wall -I"${VTERM_DIR}/include" -c "$f" \
                -o "${VTERM_BUILD}/${base}.o"
        done
        "$MINGW_AR" rcs "${VTERM_BUILD}/libvterm.a" "${VTERM_BUILD}"/*.o
        log_info "libvterm cross-compiled: ${VTERM_BUILD}/libvterm.a"
    else
        log_info "Using cached libvterm: ${VTERM_BUILD}/libvterm.a"
    fi

    local VTERM_ABS
    VTERM_ABS=$(cd "$VTERM_DIR" && pwd)

    # --- Generate configure script ---
    if ! generate_configure; then
        exit 1
    fi

    # --- Configure for cross-compilation ---
    if [ -d "$MINGW_BUILD_DIR" ]; then
        rm -rf "$MINGW_BUILD_DIR"
    fi
    mkdir -p "$MINGW_BUILD_DIR"
    cd "$MINGW_BUILD_DIR"

    local CONFIGURE_CMD="../configure"
    CONFIGURE_CMD="$CONFIGURE_CMD --host=${MINGW_HOST}"
    CONFIGURE_CMD="$CONFIGURE_CMD --prefix=${MINGW_SYSROOT}"
    CONFIGURE_CMD="$CONFIGURE_CMD PKG_CONFIG=${MINGW_PKG_CONFIG}"
    CONFIGURE_CMD="$CONFIGURE_CMD VTERM_CFLAGS=-I${VTERM_ABS}/include"
    CONFIGURE_CMD="$CONFIGURE_CMD VTERM_LIBS=\"-L${VTERM_ABS}/build-mingw64 -lvterm\""

    if [ "$ENABLE_DEBUG" = true ]; then
        CONFIGURE_CMD="$CONFIGURE_CMD CFLAGS='-O0 -g3 -DDEBUG'"
    fi

    log_info "Running: $CONFIGURE_CMD"
    eval "$CONFIGURE_CMD"

    if [ $? -ne 0 ]; then
        log_error "Cross-compilation configure failed"
        if [ -f "config.log" ]; then
            log_error "Last 50 lines of config.log:"
            tail -50 config.log
        fi
        cd ..
        exit 1
    fi

    cd ..

    # --- Build ---
    cd "$MINGW_BUILD_DIR"
    log_info "Building with $PARALLEL_JOBS parallel jobs..."
    make -j"$PARALLEL_JOBS"

    if [ $? -ne 0 ]; then
        log_error "Cross-compilation build failed"
        cd ..
        exit 1
    fi
    cd ..

    # --- Collect DLLs alongside the binary ---
    log_info "Collecting runtime DLLs..."
    local DLL_DIR="${MINGW_SYSROOT}/bin"
    local EXE_DIR="${MINGW_BUILD_DIR}/src/.libs"
    local DLLS=(
        SDL3.dll
        libfreetype-6.dll
        libharfbuzz-0.dll
        libpng16-16.dll
        zlib1.dll
        libbz2-1.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
        libglib-2.0-0.dll
        libintl-8.dll
        libpcre2-8-0.dll
        iconv.dll
    )

    for dll in "${DLLS[@]}"; do
        if [ -f "${DLL_DIR}/${dll}" ]; then
            cp "${DLL_DIR}/${dll}" "${EXE_DIR}/"
        else
            log_warn "DLL not found: ${DLL_DIR}/${dll}"
        fi
    done

    log_info "Cross-compilation complete!"
    log_info "Binary and DLLs: ${MINGW_BUILD_DIR}/src/.libs/"
}

# --- Windows VM (QEMU/KVM) functions ---

# Download ISOs, create disk image, UEFI vars, autounattend floppy
vm_setup() {
    log_info "Setting up Windows VM..."

    # Check prerequisites
    for cmd in qemu-system-x86_64 swtpm qemu-img mcopy mkfs.fat; do
        if ! command -v "$cmd" &>/dev/null; then
            log_error "$cmd not found"
            log_error "Install with: sudo dnf install qemu-system-x86 swtpm qemu-img mtools dosfstools"
            exit 1
        fi
    done
    if [ ! -f "$OVMF_CODE" ]; then
        log_error "OVMF Secure Boot firmware not found: $OVMF_CODE"
        log_error "Install with: sudo dnf install edk2-ovmf"
        exit 1
    fi

    mkdir -p "$VM_DIR" "$VM_TPM_DIR"

    # Download Windows 11 LTSC evaluation ISO
    if [ ! -f "$VM_ISO" ]; then
        log_info "Downloading Windows 11 LTSC evaluation ISO (~4.7 GB)..."
        curl -L -o "$VM_ISO" \
            'https://go.microsoft.com/fwlink/?linkid=2289029&clcid=0x409&culture=en-us&country=us'
    else
        log_info "Windows ISO already exists: $VM_ISO"
    fi

    # Download virtio-win drivers ISO
    if [ ! -f "$VM_VIRTIO" ]; then
        log_info "Downloading virtio-win drivers ISO..."
        curl -L -o "$VM_VIRTIO" \
            'https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso'
    else
        log_info "Virtio-win ISO already exists: $VM_VIRTIO"
    fi

    # Create disk image
    if [ ! -f "$VM_DISK" ]; then
        log_info "Creating 40GB disk image..."
        qemu-img create -f qcow2 "$VM_DISK" 40G
    else
        log_info "Disk image already exists: $VM_DISK"
    fi

    # Copy UEFI variables
    if [ ! -f "$VM_OVMF_VARS" ]; then
        log_info "Copying UEFI variables (Secure Boot)..."
        cp /usr/share/edk2/ovmf/OVMF_VARS.secboot.fd "$VM_OVMF_VARS"
    fi

    # Create autounattend floppy (bypasses TPM/SecureBoot/storage checks)
    if [ ! -f "$VM_AUTOUNATTEND" ]; then
        log_info "Creating autounattend floppy image..."
        local XML_FILE="$VM_DIR/autounattend.xml"
        cat >"$XML_FILE" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend"
          xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
    <settings pass="windowsPE">
        <component name="Microsoft-Windows-Setup" processorArchitecture="amd64"
                   publicKeyToken="31bf3856ad364e35" language="neutral"
                   versionScope="nonSxS">
            <RunSynchronous>
                <RunSynchronousCommand wcm:action="add">
                    <Order>1</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
                <RunSynchronousCommand wcm:action="add">
                    <Order>2</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
                <RunSynchronousCommand wcm:action="add">
                    <Order>3</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassStorageCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
            </RunSynchronous>
        </component>
    </settings>
</unattend>
XML
        dd if=/dev/zero of="$VM_AUTOUNATTEND" bs=1440k count=1 2>/dev/null
        mkfs.fat "$VM_AUTOUNATTEND" >/dev/null
        mcopy -i "$VM_AUTOUNATTEND" "$XML_FILE" ::/autounattend.xml
        rm -f "$XML_FILE"
    fi

    # Create transfer disk image with MBR partition table
    if [ ! -f "$VM_TRANSFER" ]; then
        log_info "Creating 256MB transfer disk image..."
        dd if=/dev/zero of="$VM_TRANSFER" bs=1M count=256 2>/dev/null
        printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$VM_TRANSFER" >/dev/null 2>&1
        mkfs.fat -F 32 --offset 2048 "$VM_TRANSFER" >/dev/null
    fi

    log_info "VM setup complete!"
    log_info "Next: ./build.sh --vm-install  (install Windows)"
}

# Launch the VM (install mode or normal boot)
vm_launch() {
    local mode="$1"

    if [ ! -f "$VM_DISK" ]; then
        log_error "VM disk not found. Run: ./build.sh --vm-setup"
        exit 1
    fi

    # Start software TPM 2.0
    mkdir -p "$VM_TPM_DIR"
    swtpm socket \
        --tpmstate dir="$VM_TPM_DIR" \
        --ctrl type=unixio,path="$VM_TPM_DIR/swtpm-sock" \
        --tpm2 \
        --daemon

    local QEMU_ARGS=(
        -enable-kvm
        -cpu host
        -m 4G
        -smp 4
        -machine q35,smm=on
        -global driver=cfi.pflash01,property=secure,value=on
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$VM_OVMF_VARS"
        -drive "file=$VM_DISK,format=qcow2,if=virtio"
        -chardev "socket,id=chrtpm,path=$VM_TPM_DIR/swtpm-sock"
        -tpmdev emulator,id=tpm0,chardev=chrtpm
        -device tpm-tis,tpmdev=tpm0
        -device virtio-net-pci,netdev=net0
        -netdev user,id=net0
        -vga virtio
        -display sdl,gl=on
    )

    if [ "$mode" = "install" ]; then
        if [ ! -f "$VM_ISO" ]; then
            log_error "Windows ISO not found. Run: ./build.sh --vm-setup"
            exit 1
        fi
        log_info "Booting VM in install mode..."
        log_info "When installer asks for disk driver: Load driver -> Browse -> F: -> viostor/w11/amd64"
        cp /usr/share/edk2/ovmf/OVMF_VARS.secboot.fd "$VM_OVMF_VARS"
        QEMU_ARGS+=(
            -device qemu-xhci
            -device usb-storage,drive=install
            -drive "id=install,file=$VM_ISO,media=cdrom,readonly=on,if=none"
            -device usb-storage,drive=virtio
            -drive "id=virtio,file=$VM_VIRTIO,media=cdrom,readonly=on,if=none"
            -drive "file=$VM_AUTOUNATTEND,format=raw,index=0,if=floppy"
            -boot order=d
        )
    else
        log_info "Booting VM..."
        QEMU_ARGS+=(
            -device qemu-xhci
            -device usb-storage,drive=transfer
            -drive "id=transfer,file=$VM_TRANSFER,format=raw,if=none,readonly=on"
        )
    fi

    exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
}

# Cross-compile and write exe + DLLs to transfer disk
vm_deploy() {
    build_mingw64

    local EXE_DIR="build-mingw64/src/.libs"

    # Recreate transfer image with MBR partition table
    log_info "Writing files to transfer disk..."
    dd if=/dev/zero of="$VM_TRANSFER" bs=1M count=256 2>/dev/null
    printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$VM_TRANSFER" >/dev/null 2>&1
    # Format the partition (starts at sector 2048 = offset 1048576)
    mkfs.fat -F 32 --offset 2048 "$VM_TRANSFER" >/dev/null

    local MTOOLS_OFFSET="@@1048576"
    for f in "${EXE_DIR}"/*.exe "${EXE_DIR}"/*.dll; do
        mcopy -i "${VM_TRANSFER}${MTOOLS_OFFSET}" "$f" ::/
    done

    log_info "Transfer disk contents:"
    mdir -i "${VM_TRANSFER}${MTOOLS_OFFSET}" ::/
    log_info "Next: ./build.sh --vm  (boot VM, files on second drive)"
}

# Default build action
do_build() {
    check_os

    if [ "${INSTALL_ONLY:-false}" = true ]; then
        log_info "Installing project only (--install flag used)"

        if ! generate_configure; then
            exit 1
        fi

        if ! configure_build; then
            exit 1
        fi

        log_info "Installing project to $INSTALL_PREFIX"
        if ! install_project; then
            log_error "Installation failed"
            exit 1
        fi
    else
        if ! generate_configure; then
            exit 1
        fi

        if ! configure_build; then
            exit 1
        fi

        if [ "${USE_BEAR:-false}" = true ]; then
            if ! build_project true; then
                exit 1
            fi
        else
            if ! build_project; then
                exit 1
            fi
        fi
    fi

    log_info "Build process completed successfully!"
    log_info "Build directory: $BUILD_DIR"
    log_info "To run the application: ./$BUILD_DIR/src/$PROJECT_NAME"
}

# Main execution
main() {
    local ACTION="build"

    # Parse all arguments first, then dispatch
    while [[ $# -gt 0 ]]; do
        case $1 in
            --install)
                INSTALL_ONLY=true
                shift
                ;;
            --bear)
                USE_BEAR=true
                shift
                ;;
            --no-debug)
                ENABLE_DEBUG=false
                shift
                ;;
            --prefix=*)
                INSTALL_PREFIX="${1#*=}"
                shift
                ;;
            --mingw64)
                ACTION=mingw64
                shift
                ;;
            --vm-setup)
                ACTION=vm_setup
                shift
                ;;
            --vm-install)
                ACTION=vm_install
                shift
                ;;
            --vm)
                ACTION=vm
                shift
                ;;
            --vm-deploy)
                ACTION=vm_deploy
                shift
                ;;
            --profiling)
                ACTION=profiling
                shift
                ;;
            --format)
                ACTION=format
                shift
                ;;
            --ref-png)
                ACTION=ref_png
                if [[ $# -lt 3 ]]; then
                    log_error "--ref-png requires TEXT and OUTPUT_PATH arguments"
                    echo "Usage: $0 --ref-png \"emoji_text\" output.png"
                    exit 1
                fi
                REF_TEXT="$2"
                REF_OUTPUT="$3"
                shift 3
                ;;
            --ref-layers)
                ACTION=ref_layers
                if [[ $# -lt 3 ]]; then
                    log_error "--ref-layers requires TEXT and OUTPUT_PREFIX arguments"
                    echo "Usage: $0 --ref-layers \"emoji\" /tmp/prefix"
                    exit 1
                fi
                REF_TEXT="$2"
                REF_PREFIX="$3"
                shift 3
                ;;
            --help)
                echo "Usage: $0 [options]"
                echo "Options:"
                echo "  --install         Only install the project (skip build and run)"
                echo "  --bear            Generate compile_commands.json using bear"
                echo "  --no-debug        Disable debug build"
                echo "  --mingw64         Cross-compile for Windows using mingw64"
                echo "  --vm-setup        Download ISOs and create Windows VM disk images"
                echo "  --vm-install      Boot VM from ISO for Windows installation"
                echo "  --vm              Boot the Windows VM"
                echo "  --vm-deploy       Cross-compile and write files to VM transfer disk"
                echo "  --profiling       Build with gprof, run benchmark, generate profile report"
                echo "  --format          Format source files with clang-format, shfmt, and prettier"
                echo "  --ref-png T OUT   Generate reference PNG of text T using hb-view"
                echo "  --ref-layers T P  Export COLR layers of text T with prefix P (requires blackrenderer)"
                echo "  --prefix=PATH     Set installation prefix (default: $HOME/.local)"
                echo "  --help            Show this help message"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
    done

    # Dispatch action
    case "$ACTION" in
        build)
            log_info "Starting build process for $PROJECT_NAME"
            do_build
            ;;
        mingw64)
            build_mingw64
            ;;
        vm_setup)
            vm_setup
            ;;
        vm_install)
            vm_launch install
            ;;
        vm)
            vm_launch run
            ;;
        vm_deploy)
            vm_deploy
            ;;
        profiling)
            run_profiling
            ;;
        format)
            format_sources
            ;;
        ref_png)
            REF_FONT="/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"
            if [ ! -f "$REF_FONT" ]; then
                log_error "Reference font not found: $REF_FONT"
                exit 1
            fi
            if ! command -v hb-view &>/dev/null; then
                log_error "hb-view not found. Install harfbuzz-utils."
                exit 1
            fi
            log_info "Generating reference PNG for \"$REF_TEXT\" -> $REF_OUTPUT"
            hb-view --font-file="$REF_FONT" \
                --font-size=128 \
                --output-format=png \
                --background=00000000 \
                --margin=0 \
                --output-file="$REF_OUTPUT" \
                "$REF_TEXT"
            if [ $? -eq 0 ]; then
                log_info "Reference PNG written to $REF_OUTPUT"
            else
                log_error "hb-view failed"
                exit 1
            fi
            ;;
        ref_layers)
            if ! python3 -c "import blackrenderer" 2>/dev/null; then
                log_error "blackrenderer not installed. Run: pip install blackrenderer"
                exit 1
            fi
            log_info "Exporting COLR layers for \"$REF_TEXT\" -> ${REF_PREFIX}_layer*.png"
            python3 scripts/colr_layers.py "$REF_TEXT" "$REF_PREFIX"
            ;;
    esac
}

# Run main function with all arguments
main "$@"
