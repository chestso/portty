#!/bin/bash
# Build Script for bloom-terminal terminal emulator

set -e  # Exit on any error
set -u  # Treat unset variables as errors

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

# Check if running on Fedora 43
check_os() {
    log_info "Checking operating system..."
    if [ -f /etc/fedora-release ]; then
        FEDORA_VERSION=$(grep -oP 'Fedora release \K\d+' /etc/fedora-release)
        if [ "$FEDORA_VERSION" -eq 43 ]; then
            log_info "Running on Fedora 43"
        else
            log_warn "Running on Fedora $FEDORA_VERSION (expected Fedora 43)"
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
    
    # Note: --enable-debug and --enable-tests are not recognized by configure
    # but we'll keep them for compatibility with BUILD.md
    
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

# Generate a bench script for profiling (scripted key events)
generate_bench_script() {
    local output_file="$1"
    log_info "Generating bench script..."

    cat > "$output_file" <<'SCRIPT'
# Bench script for profiling: exercises typing, scrolling, special keys
# Format: [delay_ms] <key_spec>

# Type a command with color output
0 echo -e "\e[31mRed\e[32mGreen\e[34mBlue\e[0m"\n
50 echo -e "\e[1;33mBold Yellow\e[0m and \e[4;36mUnderline Cyan\e[0m"\n

# Type lines to fill the screen and exercise scrollback
10 for i in $(seq 1 50); do echo "Line $i: The quick brown fox jumps over the lazy dog"; done\n

# Truecolor gradient
50 for i in $(seq 0 5 255); do printf "\e[38;2;${i};$((255-i));128m#\e[0m"; done; echo\n

# Emoji
50 echo "Emoji: 😀🎉🚀💻🔥✨🌍🎨📚🔧"\n

# Box drawing
50 echo "┌────────────────────┐"\n
0 echo "│ Box drawing test   │"\n
0 echo "└────────────────────┘"\n

# More scrolling
10 for i in $(seq 1 100); do echo "Scroll $i: Lorem ipsum dolor sit amet"; done\n

# Arrow keys and backspace
100 Hello\b\b\b\b\bWorld
50 \n

# Exit
100 \C-q
SCRIPT

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
    local bench_script="bench-script.txt"
    generate_bench_script "$bench_script"

    # Run benchmark with scripted events (window opens, script plays, Ctrl+Q exits)
    log_info "Running benchmark (--bench $bench_script)..."
    rm -f gmon.out
    ./"$BUILD_DIR"/src/"$PROJECT_NAME" --bench="$bench_script"
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
    gprof ./"$BUILD_DIR"/src/"$PROJECT_NAME" gmon.out > "$report_file"

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
    if command -v clang-format &> /dev/null; then
        log_info "Formatting C/C++ files with clang-format..."
        find src/ -name "*.c" -o -name "*.h" | xargs clang-format -i
    else
        log_warn "clang-format not found. Skipping C/C++ formatting."
    fi
    
    # Format shell scripts with shfmt
    if command -v shfmt &> /dev/null; then
        log_info "Formatting shell scripts with shfmt..."
        find examples/ -name "*.sh" | xargs shfmt -w
    else
        log_warn "shfmt not found. Skipping shell script formatting."
    fi
    
    # Format markdown files with prettier
    if command -v prettier &> /dev/null; then
        log_info "Formatting markdown files with prettier..."
        find . -name "*.md" | xargs prettier --write
    else
        log_warn "prettier not found. Skipping markdown formatting."
    fi
    
    log_info "Formatting completed."
}

# Main execution
main() {
    log_info "Starting build process for $PROJECT_NAME"
    
    # Parse command line arguments
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
            --profiling)
                run_profiling
                exit 0
                ;;
            --format)
                format_sources
                exit 0
                ;;
            --ref-png)
                # Usage: --ref-png "emoji" output.png
                if [[ $# -lt 3 ]]; then
                    log_error "--ref-png requires TEXT and OUTPUT_PATH arguments"
                    echo "Usage: $0 --ref-png \"emoji_text\" output.png"
                    exit 1
                fi
                REF_TEXT="$2"
                REF_OUTPUT="$3"
                REF_FONT="/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"
                if [ ! -f "$REF_FONT" ]; then
                    log_error "Reference font not found: $REF_FONT"
                    exit 1
                fi
                if ! command -v hb-view &> /dev/null; then
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
                exit 0
                ;;
            --ref-layers)
                # Usage: --ref-layers "emoji" output_prefix
                if [[ $# -lt 3 ]]; then
                    log_error "--ref-layers requires TEXT and OUTPUT_PREFIX arguments"
                    echo "Usage: $0 --ref-layers \"emoji\" /tmp/prefix"
                    exit 1
                fi
                REF_TEXT="$2"
                REF_PREFIX="$3"

                # Check for blackrenderer
                if ! python3 -c "import blackrenderer" 2>/dev/null; then
                    log_error "blackrenderer not installed. Run: pip install blackrenderer"
                    exit 1
                fi

                log_info "Exporting COLR layers for \"$REF_TEXT\" -> ${REF_PREFIX}_layer*.png"
                python3 scripts/colr_layers.py "$REF_TEXT" "$REF_PREFIX"
                exit 0
                ;;
            --help)
                echo "Usage: $0 [options]"
                echo "Options:"
                echo "  --install         Only install the project (skip build and run)"
                echo "  --bear            Generate compile_commands.json using bear"
                echo "  --no-debug        Disable debug build"
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
    
    # Check OS
    check_os
    
    # If --install flag is used, skip build and run steps
    if [ "${INSTALL_ONLY:-false}" = true ]; then
        log_info "Installing project only (--install flag used)"
        
        # Generate configure script
        if ! generate_configure; then
            exit 1
        fi
        
        # Configure build
        if ! configure_build; then
            exit 1
        fi
        
        # Install project (skip build)
        log_info "Installing project to $INSTALL_PREFIX"
        if ! install_project; then
            log_error "Installation failed"
            exit 1
        fi
    else
        # Generate configure script
        if ! generate_configure; then
            exit 1
        fi
        
        # Configure build
        if ! configure_build; then
            exit 1
        fi
        
        # Build project with bear if requested
        if [ "${USE_BEAR:-false}" = true ]; then
            # Build project with bear
            if ! build_project true; then
                exit 1
            fi
        else
            # Build project normally
            if ! build_project; then
                exit 1
            fi
        fi
    fi
    
    log_info "Build process completed successfully!"
    log_info "Build directory: $BUILD_DIR"
    log_info "To run the application: ./$BUILD_DIR/src/$PROJECT_NAME"
}

# Run main function with all arguments
main "$@"
