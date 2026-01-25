# bloom-term

A terminal emulator built with SDL3, libvterm, FreeType and HarfBuzz.

## Features

- Full terminal emulation using libvterm
- Rendering with SDL3
- Text shaping with HarfBuzz
- Font rasterization and COLR v1 paint traversal using FreeType
- Variable-font support (MM_Var) and axis control
- Support for Unicode characters and emoji (COLR v1 color fonts supported; experimental)
- Configurable color schemes
- Proper terminal resize handling

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
- `--format` - Format source files with clang-format, shfmt, and prettier
- `--prefix=PATH` - Set installation prefix (default: $HOME/.local)

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

Note: Cairo is not required for the renderer anymore; COLR v1 paint traversal and shaping are handled by the FreeType/HarfBuzz backend.

## Development

The project includes:

- `.clang-format` for code formatting
- `build.sh` for automated builds
- Example scripts demonstrating terminal features, including `examples/unicode/emoji.sh` which exercises COLR/emoji paths

## Notes and current limitations

- COLR v1 support is implemented in the FreeType backend and covers many common paint types (solid, linear/radial/sweep gradients, transforms, glyph masking and basic composite modes), but several paint semantics (extend modes, all composite operators, some transform edge-cases) are still best-effort and may need refinement.
- Renderer currently uploads per-glyph textures and uses a simple LRU cache. A texture atlas / batched GPU upload path is a planned optimization.
- Automated visual tests for COLR/emoji rendering are not yet available; use the provided examples with `-v` to inspect behavior.
