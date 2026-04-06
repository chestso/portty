# bloom-terminal

A terminal emulator with pluggable backends for terminal emulation, rendering, platform windowing, and fonts.

Currently ships with libvterm (terminal), SDL3 (renderer/platform), FreeType/HarfBuzz (fonts), and an optional GTK4/libadwaita platform backend for native GNOME integration.

## Features

- Full terminal emulation using libvterm
- Rendering with SDL3
- Text shaping with HarfBuzz
- Font rasterization with FreeType
- Custom COLR v1 paint graph traversal (gradients, transforms, compositing)
- Bold, italic, and bold-italic font styles (variable font axes, fontconfig resolution, synthetic fallback)
- Variable-font support (MM_Var) and axis control
- Dynamic font fallback via Fontconfig (up to 8 runtime fallback fonts with codepoint cache)
- Support for Unicode characters and emoji (COLR v1 color fonts supported; experimental)
- Sixel graphics protocol support
- Procedural box drawing and block element rendering (U+2500–U+257F)
- Text selection with clipboard support (Ctrl+C or Ctrl+Shift+C to copy, right-click copy/paste)
- Soft-wrap aware word selection and copy
- Underline styles (single, double, curly, dotted, dashed) with SGR 58/59 color support
- Reverse video attribute rendering
- Nerd Fonts v2 to v3 codepoint translation
- Scrollback buffer with mouse wheel and Shift+PageUp/Down
- Terminal resize handling with optional reflow (`--reflow`)
- HiDPI support (pixel density scaling for underlines and UI elements)
- Window title via OSC 2
- Custom terminfo entry (`TERM=bloom-terminal-256color`) with truecolor, cursor style, and bracketed paste (pasted text is distinguished from typed input so shells don't execute it prematurely)
- Optional GTK4/libadwaita backend (`--gtk4`) for native client-side decorations on GNOME/Wayland

## Architecture

bloom-terminal uses a modular backend abstraction design:

- **Platform Backend**: Handles windowing, input events, clipboard, and the main event loop
  - Default: SDL3 (`platform_backend_sdl3`) — uses libdecor for Wayland decorations
  - Optional: GTK4/libadwaita (`platform_backend_gtk4`) — built as a dlopen plugin, provides native CSD with AdwHeaderBar. Uses zero-copy DMA-BUF rendering (GBM → EGL → `glBlitFramebuffer` → `GdkDmabufTexture`) when EGL/GBM are available, with `SDL_RenderReadPixels` fallback.

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: libvterm (`terminal_backend_vt`)

- **Renderer Backend**: Handles graphics output
  - Current implementation: SDL3 (`renderer_backend_sdl3`)
  - Uses a texture atlas with shelf packing and FNV-1a hash-based lookup
  - LRU eviction occurs when the atlas fills

- **Font Backend**: Handles font loading, shaping, and glyph rasterization
  - Current implementation: FreeType/HarfBuzz (`font_backend_ft`)
  - Custom COLR v1 paint tree traversal implemented in `src/colr.c`
  - FreeType provides COLR v1 APIs for accessing paint data; recursive evaluation, affine transforms, and Porter-Duff compositing are implemented manually
  - Supports solid fills, linear/radial/sweep gradients, transforms, glyph masking, and basic composite modes
  - Some paint semantics (extend modes, all composite operators, transform edge-cases) are best-effort

- **Font Resolver Backend**: Handles font discovery and selection
  - Current implementation: Fontconfig (`font_resolve_backend_fc`)

Each backend defines a standard interface (`PlatformBackend`, `TerminalBackend`, `RendererBackend`, `FontBackend`, `FontResolveBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic.

### GTK4 Plugin

The GTK4 backend is compiled as a separate shared library (`bloom-terminal-gtk4.so`) and loaded via `dlopen` only when `--gtk4` is passed. This avoids symbol conflicts between GTK4 and libdecor's GTK3 plugin, which both export identically-named symbols (`gtk_init`, `gtk_widget_get_type`, etc.).

```
build/src/bloom-terminal                          # Main binary (no GTK4 symbols)
build/src/.libs/bloom-terminal-gtk4.so            # Plugin (dev build)

$PREFIX/bin/bloom-terminal                        # Installed binary
$PREFIX/lib/bloom-terminal/bloom-terminal-gtk4.so # Installed plugin
```

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

If GTK4 and libadwaita are available, the plugin is built automatically. Use `--disable-gtk4` to skip it.

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
- `--mingw64` - Cross-compile for Windows using mingw64
- `--prefix=PATH` - Set installation prefix (default: $HOME/.local)
- `--help` - Show help message

## Usage

```bash
# Run the terminal emulator
build/src/bloom-terminal

# Run with verbose output (useful to debug font/COLR/emoji handling)
build/src/bloom-terminal -v

# Run with GTK4/libadwaita backend (native GNOME decorations)
build/src/bloom-terminal --gtk4

# Run a specific command instead of the default shell
build/src/bloom-terminal -- htop

# Display text without spawning a shell (for testing)
build/src/bloom-terminal --demo "Hello, world!"

# Render text to a PNG file
build/src/bloom-terminal -P "😀" output.png
```

### CLI Flags

| Flag                  | Description                                                     |
| --------------------- | --------------------------------------------------------------- |
| `-h`                  | Show help message                                               |
| `-v`                  | Verbose output (font resolution, COLR, atlas events)            |
| `-f PATTERN`          | Font via fontconfig pattern (e.g. `-f "Cascadia Code-14"`)      |
| `-g COLSxROWS`        | Initial terminal size (default: 80x24)                          |
| `-P TEXT`             | Render TEXT to PNG (output path as positional arg)              |
| `-D PREFIX`           | COLR layer debug: save each layer as `PREFIX_layer00.png`, etc. |
| `-L` / `--list-fonts` | List available monospace fonts and exit                         |
| `--ft-hinting S`      | FreeType hinting: none/light/normal/mono (default: light)       |
| `--reflow`            | Enable text reflow on resize (unstable, libvterm bug)           |
| `--padding`           | Enable padding around terminal content                          |
| `--gtk4`              | Use GTK4/libadwaita platform backend                            |
| `--sdl3`              | Use SDL3 platform backend (overrides config file)               |
| `--demo TEXT`         | Display TEXT in terminal without spawning a shell (for testing) |

### Keyboard Shortcuts

| Shortcut            | Action                                               |
| ------------------- | ---------------------------------------------------- |
| `Ctrl+C`            | Copy selection to clipboard (sends SIGINT otherwise) |
| `Ctrl+Shift+C`      | Copy selection to clipboard                          |
| `Ctrl+Shift+V`      | Paste from clipboard                                 |
| `Shift+PageUp/Down` | Scroll through scrollback buffer                     |
| Right-click         | Copy selection if active, otherwise paste            |

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
platform = gtk4
reflow = true
padding = false
verbose = false
word_chars = abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/?#[]@!$&'()*+,;=%~
```

### Available Keys

All keys are optional. Only the `[terminal]` section is recognized.

| Key          | Values                            | Default        | Description                                 |
| ------------ | --------------------------------- | -------------- | ------------------------------------------- |
| `font`       | Fontconfig pattern                | `monospace`    | Font family and size (e.g. `monospace-16`)  |
| `geometry`   | `COLSxROWS`                       | `80x24`        | Initial terminal dimensions                 |
| `hinting`    | `none`, `light`, `normal`, `mono` | `light`        | FreeType hinting mode                       |
| `platform`   | `sdl3`, `gtk4`                    | `sdl3`         | Platform backend                            |
| `reflow`     | `true`/`false`                    | `false`        | Text reflow on resize                       |
| `padding`    | `true`/`false`                    | `false`        | Padding around terminal content             |
| `verbose`    | `true`/`false`                    | `false`        | Debug output                                |
| `word_chars` | Character string                  | `A-Za-z0-9_-/` | Characters treated as word for double-click |

Boolean values accept `true`/`false`, `yes`/`no`, or `1`/`0`. Lines starting with `#` or `;` are comments.

## Terminfo

bloom-terminal ships a custom terminfo entry (`bloom-terminal-256color`) based on `xterm-256color`. It is compiled and installed automatically by `./build.sh --install` via `tic`. The child shell's `TERMINFO_DIRS` is set to `~/.local/share/terminfo` so the entry is found without system-wide installation.

If you SSH to a remote host that lacks the entry, the remote shell will fall back to a generic terminal type. You can copy the compiled entry to the remote host:

```bash
infocmp bloom-terminal-256color | ssh remote-host 'tic -x -'
```

## Dependencies

- libvterm
- SDL3
- fontconfig
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz (>= 2.0)
- libpng

Optional:

- gtk4 + libadwaita-1 (for `--gtk4` platform backend)
- EGL + GBM + libdrm (for zero-copy DMA-BUF rendering in GTK4 backend)

### Fedora 41+

```bash
# Build tools
sudo dnf install gcc autoconf automake libtool pkgconf-pkg-config

# Required libraries
sudo dnf install libvterm-devel SDL3-devel fontconfig-devel freetype-devel harfbuzz-devel libpng-devel

# Optional: GTK4 backend
sudo dnf install gtk4-devel libadwaita-devel mesa-libEGL-devel mesa-libgbm-devel libdrm-devel

# Optional: compile_commands.json for editors
sudo dnf install bear
```

## Windows Cross-Compilation

bloom-terminal can be cross-compiled for Windows using Fedora's mingw64 toolchain. The Windows build uses ConPTY for terminal emulation.

### Prerequisites (Fedora)

```bash
sudo dnf install mingw64-gcc mingw64-SDL3 mingw64-freetype mingw64-harfbuzz mingw64-fontconfig mingw64-libpng
```

libvterm is downloaded and cross-compiled automatically (no mingw64 package exists).

### Building

```bash
./build.sh --mingw64
```

This produces `build-mingw64/src/.libs/bloom-terminal.exe` with all required DLLs copied alongside. The mingw64 build uses a separate directory so it doesn't conflict with the Linux `build/`.

### Testing with QEMU/KVM

For full interactive testing (ConPTY shell sessions), use a Windows VM with QEMU/KVM:

```bash
./build.sh --vm-setup       # Download ISOs, create disk images (one-time)
./build.sh --vm-install      # Boot VM from ISO to install Windows
./build.sh --vm-deploy       # Cross-compile and write files to VM transfer disk
./build.sh --vm              # Boot the VM (transfer disk appears as second drive)
```

The installer requires a virtio storage driver: **Load driver** → **Browse** → `F:` → `viostor/w11/amd64`.

### Windows PTY

The Windows build uses ConPTY (`CreatePseudoConsole`) instead of Unix PTYs. The implementation is in `src/pty_win32.c`. The GTK4 backend is not available on Windows; only the SDL3 backend is used.

## Testing

Unit tests are run via the Autotools test harness:

```bash
cd build && make check
```

Current test suites:

- **test_atlas** — Glyph texture atlas: insert/lookup, shelf packing, staging buffer contents, spatial and load-factor eviction, plus regression tests for hash table overflow, probe chain corruption, and post-eviction staging bugs
- **test_pty_pause** — PTY pause/resume during selection: platform wrapper delegation, pause on select, resume on clear/copy/resize, full select-copy cycles
- **test_unicode** — Unicode helpers: emoji range detection, ZWJ, skin tone modifiers, regional indicators, UTF-8 decoding (ASCII, multibyte, 4-byte emoji, invalid input, truncation)
- **test_conf** — Config parser: init defaults, font/geometry/hinting/boolean/word_chars/platform parsing, comments, unknown keys, section handling

All tests support `-v` for verbose output. Visual testing of rendering and terminal features is done manually using example scripts.

## Development

The project includes:

- `.clang-format` for code formatting
- `build.sh` for automated builds
- Example scripts demonstrating terminal features, including `examples/unicode/emoji.sh` which exercises COLR/emoji paths

### Code Formatting

Run `./build.sh --format` to format all source files. This requires:

- **clang-format** — C source and headers (`src/`)
- **shfmt** — shell scripts (`examples/`)
- **prettier** — markdown files

```bash
# Fedora 41+
sudo dnf install clang-tools-extra shfmt
npm install --prefix ~/.local prettier
```

## Author

Thomas Christensen

## License

MIT — see [COPYING](COPYING) for details.
