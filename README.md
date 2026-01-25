# bloom-term

A universal terminal emulator with pluggable backends for terminal emulation, rendering, and fonts.

Currently ships with libvterm (terminal), SDL3 (renderer), and FreeType/HarfBuzz (fonts).

## Features

- Full terminal emulation using libvterm
- Rendering with SDL3
- Text shaping with HarfBuzz
- Font rasterization with FreeType
- Custom COLR v1 paint graph traversal (gradients, transforms, compositing)
- Variable-font support (MM_Var) and axis control
- Support for Unicode characters and emoji (COLR v1 color fonts supported; experimental)
- Configurable color schemes
- Proper terminal resize handling

## Architecture

bloom-term uses a modular backend abstraction design:

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: libvterm (`terminal_backend_vt`)

- **Renderer Backend**: Handles graphics output and windowing
  - Current implementation: SDL3 (`renderer_backend_sdl3`)
  - Uses a two-page texture atlas with shelf packing and FNV-1a hash-based lookup
  - Page 0 handles small glyphs (≤48px), page 1 handles large glyphs
  - LRU eviction occurs when pages fill

- **Font Backend**: Handles font loading, shaping, and glyph rasterization
  - Current implementation: FreeType/HarfBuzz (`font_backend_ft`)
  - Custom COLR v1 paint tree traversal implemented in `src/colr.c`
  - FreeType provides COLR v1 APIs for accessing paint data; recursive evaluation, affine transforms, and Porter-Duff compositing are implemented manually
  - Supports solid fills, linear/radial/sweep gradients, transforms, glyph masking, and basic composite modes
  - Some paint semantics (extend modes, all composite operators, transform edge-cases) are best-effort

Each backend defines a standard interface (`TerminalBackend`, `RendererBackend`, `FontBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic.

## Building

The project uses GNU Autotools. For convenience, a build script is provided:

```bash
chmod +x build.sh
./build.sh
```

This will:

1. Generate the configure script
2. Configure the build with appropriate flags
3. Build the project with parallel jobs
4. Create a `build/` directory with the compiled binary

### Build Script Options

The build script supports several options:

```bash
./build.sh --help
```

Available options:

- `--install` - Only install the project (skip build and run)
- `--bear` - Generate compile_commands.json using bear
- `--no-debug` - Disable debug build
- `--profiling` - Build with gprof, run benchmark, generate profile report
- `--format` - Format source files with clang-format, shfmt, and prettier
- `--ref-png TEXT OUT` - Generate reference PNG of text using hb-view
- `--prefix=PATH` - Set installation prefix (default: $HOME/.local)
- `--help` - Show help message

## Usage

```bash
# Run the terminal emulator
build/src/bloom-term

# Run with verbose output (useful to debug font/COLR/emoji handling)
build/src/bloom-term -v

# Process input from a file
build/src/bloom-term input.txt

# Process input from stdin
echo -e "\x1b[31mHello World\x1b[0m" | build/src/bloom-term -
```

## Dependencies

- libvterm
- SDL3
- fontconfig
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz

## Development

The project includes:

- `.clang-format` for code formatting
- `build.sh` for automated builds
- Example scripts demonstrating terminal features, including `examples/unicode/emoji.sh` which exercises COLR/emoji paths

Testing is currently manual using example scripts with the `-v` verbose flag. Automated visual tests for COLR/emoji rendering are not yet available.
