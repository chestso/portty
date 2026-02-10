# bloom-terminal

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
- Sixel graphics protocol support
- Procedural box drawing and block element rendering (U+2500–U+257F)
- Text selection with clipboard support (Ctrl+Shift+C to copy, right-click copy/paste)
- Underline styles (single, double, curly, dotted, dashed) with SGR 58/59 color support
- Reverse video attribute rendering
- Nerd Fonts v2 to v3 codepoint translation
- Scrollback buffer with mouse wheel and Shift+PageUp/Down
- Terminal resize handling with optional reflow (`--reflow`)
- Custom terminfo entry (`TERM=bloom-terminal`) with truecolor, cursor style, bracketed paste, and strikethrough support

## Architecture

bloom-terminal uses a modular backend abstraction design:

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: libvterm (`terminal_backend_vt`)

- **Renderer Backend**: Handles graphics output and windowing
  - Current implementation: SDL3 (`renderer_backend_sdl3`)
  - Uses a texture atlas with shelf packing and FNV-1a hash-based lookup
  - LRU eviction occurs when the atlas fills

- **Font Backend**: Handles font loading, shaping, and glyph rasterization
  - Current implementation: FreeType/HarfBuzz (`font_backend_ft`)
  - Custom COLR v1 paint tree traversal implemented in `src/colr.c`
  - FreeType provides COLR v1 APIs for accessing paint data; recursive evaluation, affine transforms, and Porter-Duff compositing are implemented manually
  - Supports solid fills, linear/radial/sweep gradients, transforms, glyph masking, and basic composite modes
  - Some paint semantics (extend modes, all composite operators, transform edge-cases) are best-effort

- **Event Loop Backend**: Handles SDL event polling and PTY I/O integration
  - Current implementation: SDL3 (`event_loop_backend_sdl3`)

- **Font Resolver Backend**: Handles font discovery and selection
  - Current implementation: Fontconfig (`font_resolve_backend_fc`)

Each backend defines a standard interface (`TerminalBackend`, `RendererBackend`, `FontBackend`, `EventLoopBackend`, `FontResolveBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic.

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
- `--ref-layers TEXT PREFIX` - Export COLR layers for debugging (requires blackrenderer)
- `--prefix=PATH` - Set installation prefix (default: $HOME/.local)
- `--help` - Show help message

## Usage

```bash
# Run the terminal emulator
build/src/bloom-terminal

# Run with verbose output (useful to debug font/COLR/emoji handling)
build/src/bloom-terminal -v
```

## Configuration

bloom-terminal can be configured with an INI-style config file called `bloom.conf`. CLI flags always take precedence over config file values.

### File Locations

The first file found is used:

1. `./bloom.conf` (project-level, current working directory)
2. `$XDG_CONFIG_HOME/bloom/bloom.conf` (defaults to `~/.config/bloom/bloom.conf`)

### Example

```ini
# bloom.conf
[terminal]
font = Cascadia Code-14
geometry = 120x40
hinting = light
reflow = true
padding = false
verbose = false
```

### Available Keys

All keys are optional. Only the `[terminal]` section is recognized.

| Key        | Values                            | Default     | Description                                |
| ---------- | --------------------------------- | ----------- | ------------------------------------------ |
| `font`     | Fontconfig pattern                | `monospace` | Font family and size (e.g. `monospace-16`) |
| `geometry` | `COLSxROWS`                       | `80x24`     | Initial terminal dimensions                |
| `hinting`  | `none`, `light`, `normal`, `mono` | `light`     | FreeType hinting mode                      |
| `reflow`   | `true`/`false`                    | `false`     | Text reflow on resize                      |
| `padding`  | `true`/`false`                    | `false`     | Padding around terminal content            |
| `verbose`  | `true`/`false`                    | `false`     | Debug output                               |

Boolean values accept `true`/`false`, `yes`/`no`, or `1`/`0`. Lines starting with `#` or `;` are comments.

## Terminfo

bloom-terminal ships a custom terminfo entry (`bloom-terminal`) based on `xterm-256color`. It is compiled and installed automatically by `./build.sh --install` via `tic`. The child shell's `TERMINFO_DIRS` is set to `~/.local/share/terminfo` so the entry is found without system-wide installation.

If you SSH to a remote host that lacks the entry, the remote shell will fall back to a generic terminal type. You can copy the compiled entry to the remote host:

```bash
infocmp bloom-terminal | ssh remote-host 'tic -x -'
```

## Dependencies

- libvterm
- SDL3
- fontconfig
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz
- libpng

## Development

The project includes:

- `.clang-format` for code formatting
- `build.sh` for automated builds
- Example scripts demonstrating terminal features, including `examples/unicode/emoji.sh` which exercises COLR/emoji paths

Testing is currently manual using example scripts with the `-v` verbose flag. Automated visual tests for COLR/emoji rendering are not yet available.
