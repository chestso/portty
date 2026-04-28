# bloom-terminal

A terminal emulator with pluggable backends for terminal emulation, rendering, platform windowing, and fonts.

Currently ships with bloom-vt (terminal), SDL3 (renderer/platform), FreeType/HarfBuzz (fonts), and an optional GTK4/libadwaita platform backend for native GNOME integration. Cross-compiles for Windows (ConPTY, native font resolver, DWM styling) and macOS (Core Text font resolver).

## Features

- Full terminal emulation using bloom-vt — external VT engine (consumed via pkg-config) with UAX #11 + UAX #29 grapheme-cluster width, arbitrary-length clusters per cell, working reflow, and a page-based scrollback ring
- Rendering with SDL3
- Text shaping with HarfBuzz
- Font rasterization with FreeType
- Custom COLR v1 paint graph traversal (gradients, transforms, compositing)
- Bold, italic, and bold-italic font styles (variable font axes, platform-native resolution, synthetic fallback)
- Variable-font support (MM_Var) and axis control
- Dynamic font fallback (up to 8 runtime fallback fonts with codepoint cache; Fontconfig on Linux, Core Text on macOS, FreeType scan on Windows)
- Support for Unicode characters and emoji (COLR v1 color fonts)
- Emoji width paradigm: color glyphs preferred regardless of VS16; ambiguous-width symbols default to 1 cell; VS16 (U+FE0F) forces 2 cells. Widths are computed at insertion time (UAX #11 + #29) and stored on the cell, so the renderer walks rows in plain column order.
- Sixel graphics protocol support
- Procedural box drawing and block element rendering (U+2500–U+257F)
- Text selection with clipboard support (Ctrl+C or Ctrl+Shift+C to copy, right-click copy/paste)
- Soft-wrap aware word selection and copy
- Underline styles (single, double, curly, dotted, dashed) with SGR 58/59 color support
- OSC-8 hyperlinks — dotted Charm-purple underline at rest, solid on hover with pointer cursor; Ctrl+click opens via the system handler. Scheme allow-list (http/https/ftp/ftps/mailto) refuses `javascript:`, `data:`, `file://`, etc.
- Strikethrough rendering (span-based, DPI-aware)
- Reverse video attribute rendering
- Nerd Fonts v2 to v3 codepoint translation
- Scrollback buffer with mouse wheel and Shift+PageUp/Down
- HiDPI support (pixel density scaling for underlines and UI elements)
- Window title via OSC 2
- Custom terminfo entry (`TERM=bloom-terminal-vty-256color`) with truecolor, cursor style, and bracketed paste (pasted text is distinguished from typed input so shells don't execute it prematurely)
- Kitty keyboard protocol (push/pop/set/query plus the Disambiguate and Report-all flags) — modern TUIs like Claude Code can tell Shift+Enter apart from plain Enter, and Ctrl+letter combos no longer collide with their literal control bytes
- Optional GTK4/libadwaita backend (`--gtk4`) for native client-side decorations on GNOME/Wayland

## Architecture

bloom-terminal uses a modular backend abstraction design:

- **Platform Backend**: Handles windowing, input events, clipboard, and the main event loop
  - Default: SDL3 (`platform_backend_sdl3`) — uses libdecor for Wayland decorations
  - Optional: GTK4/libadwaita (`platform_backend_gtk4`) — built as a dlopen plugin, provides native CSD with AdwHeaderBar. Uses zero-copy DMA-BUF rendering (GBM → EGL → `glBlitFramebuffer` → `GdkDmabufTexture`) when EGL/GBM are available, with `SDL_RenderReadPixels` fallback.

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: bloom-vt (`terminal_backend_bvt`) — external VT engine consumed via `pkg-config bloom-vt`, bridged through `term_bvt.c` (parser, page-based grid, scrollback ring, reflow, charsets). DEC ANSI parser (Williams state machine), UAX #11 + #29 cluster widths, page-arena style/grapheme interning, scrollback page ring

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
  - Linux: Fontconfig (`font_resolve_backend_fc`)
  - macOS: Core Text (`font_resolve_backend_ct`) with `CTFontCreateForString` codepoint fallback
  - Windows: Native registry-based resolver (`font_resolve_backend_w32`) with FreeType codepoint fallback

Each backend defines a standard interface (`PlatformBackend`, `TerminalBackend`, `RendererBackend`, `FontBackend`, `FontResolveBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic.

### GTK4 Plugin

The GTK4 backend is compiled as a separate shared library (`bloom-terminal-gtk4.so`) and loaded via `dlopen` only when `--gtk4` is passed. This avoids symbol conflicts between GTK4 and libdecor's GTK3 plugin, which both export identically-named symbols (`gtk_init`, `gtk_widget_get_type`, etc.).

The conflict chain: SDL3 on Wayland → dlopen's `libdecor-0.so` → dlopen's `libdecor-gtk.so` → loads `libgtk-3.so`. If GTK4 were linked directly into the binary, the dynamic linker would resolve libdecor's GTK3 calls to GTK4 symbols, causing crashes. GNOME/Mutter does not implement `xdg-decoration`, so libdecor cannot be disabled without losing window decorations on GNOME.

This workaround will become unnecessary when libdecor ships its out-of-process GTK4 plugin ([libdecor MR !176](https://gitlab.freedesktop.org/libdecor/libdecor/-/merge_requests/176)), which runs GTK4 in a separate child process via Wayland IPC. At that point, GTK4 can be linked directly into the main binary.

```
build/src/bloom-terminal                          # Main binary (no GTK4 symbols)
build/src/.libs/bloom-terminal-gtk4.so            # Plugin (dev build)

$PREFIX/bin/bloom-terminal                        # Installed binary
$PREFIX/lib/bloom-terminal/bloom-terminal-gtk4.so # Installed plugin
```

## Building

The project uses GNU Autotools. From a fresh checkout:

```bash
./autogen.sh
mkdir build && cd build
../configure --prefix=$HOME/.local --enable-debug
make -j$(nproc)
make check
make install
```

If GTK4 and libadwaita are available, the plugin is built automatically. Pass `--disable-gtk4` to `configure` to skip it. Pass plain `CFLAGS` (e.g. `CFLAGS="-O3 -DNDEBUG"`) for a release build instead of `--enable-debug`.

### Make Targets

- `make` — build everything
- `make check` — run the test suite
- `make install` — install to `$prefix` (default `$HOME/.local`); compiles terminfo via `tic`, installs the GTK4 plugin to `$prefix/lib/bloom-terminal/`
- `make format` — clang-format on `src/` and `tests/`, shfmt on `examples/` and `scripts/`, prettier on Markdown
- `make bear` — produce `compile_commands.json` for clangd

### Helper Scripts

| Script                      | Purpose                                                         |
| --------------------------- | --------------------------------------------------------------- |
| `scripts/build-mingw64.sh`  | Cross-compile for Windows using Fedora's mingw64 toolchain      |
| `scripts/build-osxcross.sh` | Cross-compile for macOS using osxcross                          |
| `scripts/profile.sh`        | Build with `-pg`, run a benchmark, write `profile-report.txt`   |
| `scripts/vm-w32.sh CMD`     | Windows VM lifecycle (`setup`/`install`/`run`/`deploy`)         |
| `scripts/vm-mac.sh CMD`     | macOS VM lifecycle (`setup`/`install`/`run`/`deploy`)           |
| `scripts/ref-png.sh T OUT`  | Generate reference PNG of TEXT using hb-view                    |
| `scripts/ref-layers.sh T P` | Export each COLR v1 paint layer as `<P>_layer00.png` etc.       |
| `scripts/setup-osxcross.sh` | One-time osxcross toolchain setup (used by `build-osxcross.sh`) |

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

| Flag                      | Description                                                     |
| ------------------------- | --------------------------------------------------------------- |
| `-h`                      | Show help message                                               |
| `-v`                      | Verbose output (font resolution, COLR, atlas events)            |
| `-f PATTERN`              | Font via fontconfig pattern (e.g. `-f "Cascadia Code-14"`)      |
| `-g COLSxROWS`            | Initial terminal size (default: 80x24)                          |
| `-P TEXT`                 | Render TEXT to PNG (output path as positional arg)              |
| `-D PREFIX`               | COLR layer debug: save each layer as `PREFIX_layer00.png`, etc. |
| `-L` / `--list-fonts`     | List available monospace fonts and exit                         |
| `-H S` / `--ft-hinting S` | FreeType hinting: none/light/normal/mono (default: light)       |
| `-G` / `--gtk4`           | Use GTK4/libadwaita platform backend                            |
| `-S` / `--sdl3`           | Use SDL3 platform backend (overrides config file)               |
| `-d TEXT` / `--demo TEXT` | Display TEXT in terminal without spawning a shell (for testing) |
| `-s N` / `--scrollback N` | Scrollback history lines (default: 1000, 0 to disable)          |

### Keyboard Shortcuts

| Shortcut             | Action                                               |
| -------------------- | ---------------------------------------------------- |
| `Ctrl+C`             | Copy selection to clipboard (sends SIGINT otherwise) |
| `Ctrl+Shift+C`       | Copy selection to clipboard                          |
| `Ctrl+Shift+V`       | Paste from clipboard                                 |
| `Shift+PageUp/Down`  | Scroll through scrollback buffer                     |
| Right-click          | Copy selection if active, otherwise paste            |
| `Ctrl+click` on link | Open OSC-8 URL via the system handler                |

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
verbose = false
word_chars = abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/?#[]@!$&'()*+,;=%~
scrollback = 1000
```

### Available Keys

All keys are optional. Only the `[terminal]` section is recognized.

| Key          | Values                            | Default        | Description                                 |
| ------------ | --------------------------------- | -------------- | ------------------------------------------- |
| `font`       | Fontconfig pattern                | `monospace`    | Font family and size (e.g. `monospace-16`)  |
| `geometry`   | `COLSxROWS`                       | `80x24`        | Initial terminal dimensions                 |
| `hinting`    | `none`, `light`, `normal`, `mono` | `light`        | FreeType hinting mode                       |
| `platform`   | `sdl3`, `gtk4`                    | `sdl3`         | Platform backend                            |
| `verbose`    | `true`/`false`                    | `false`        | Debug output                                |
| `word_chars` | Character string                  | `A-Za-z0-9_-/` | Characters treated as word for double-click |
| `scrollback` | Non-negative integer              | `1000`         | Scrollback history lines (0 disables)       |

Boolean values accept `true`/`false`, `yes`/`no`, or `1`/`0`. Lines starting with `#` or `;` are comments.

## Emoji Width Paradigm

bloom-terminal enforces three rules for how emoji and symbols are rendered:

1. **Color preferred.** When a color glyph is available in the primary or fallback font, it is used. This decision is independent of VS16 (the emoji presentation selector) — the emoji font is chosen based on whether the base codepoint is in an emoji range or whether a fallback font supplies a color raster.
2. **Ambiguous width = 1 cell.** Ambiguous-width symbols (e.g. ⚠ U+26A0, ☀ U+2600) default to 1 cell regardless of whether they render with the color emoji font. They stay 1 cell wide unless followed by VS16.
3. **VS16 forces 2 cells.** When U+FE0F follows an emoji-presentation base codepoint, the cell width is 2 — e.g. `⚠` is 1 cell but `⚠️` is 2 cells.

bloom-vt computes UAX #11 + UAX #29 cluster widths at insertion time and stores them on the cell, so VS16 emoji come through with `cell.width == 2` and the cell immediately to its right is a continuation cell with `cell.width == 0`. The renderer walks rows in plain column order via `TerminalRowIter` and increments by `cell.width` — no peek-ahead, no shift-vs-absorb decision, no separate "visual" column space. Mouse, cursor, and selection coordinates all share the same single column space.

Multi-codepoint clusters (ZWJ family chains, flag sequences, long combining-mark runs) are stored in a per-page grapheme arena and accessed via `terminal_cell_get_grapheme()` — there is no per-cell codepoint cap, so 7-codepoint sequences like 👨‍👩‍👧‍👦 round-trip through the renderer without truncation.

## Terminfo

bloom-terminal ships a single terminfo entry (based on `xterm-256color`) under three aliases — `bloom-terminal-vty-256color`, `bloom-terminal-256color`, and `bloom-terminal`. The default `TERM` is `bloom-terminal-vty-256color`; the alternate names exist for users who prefer to set them.

`setaf`/`setab` are inherited unchanged from `xterm-256color`, so the entry's capability strings stay within the restricted operator subset that Haskell `vty-unix`'s terminfo parser accepts. Truecolor is signalled via the `Tc` flag (which emacs, tmux, vte, alacritty, kitty, ghostty, and most modern TUIs honor) and via `COLORTERM=truecolor` for apps that read the env var directly. Extension caps added on top of `xterm-256color`: `Smulx` (extended underline styles), `Setulc` (underline color), `Ss`/`Se` (cursor shape), `Ms` (OSC 52 set-clipboard), `BE`/`BD` (bracketed paste), `PS`/`PE` (paste delimiters), `hs`/`tsl`/`fsl`/`dsl` (status line / window title), `sitm`/`ritm` (italic), `smxx`/`rmxx` (strikethrough).

The `RGB` flag is deliberately **not** advertised: its ncurses contract is "feed packed 24-bit ints to `setaf` and it'll DTRT," which would require a custom `setaf` outside the vty-unix parser subset. Truecolor consumers are expected to use `Tc` or `COLORTERM`.

On Linux, the entry is compiled and installed automatically by `make install` via `tic`. On macOS (QEMU VM), run `sh /Volumes/NO\ NAME/install-terminfo.sh` to compile and install natively. The child shell's `TERMINFO_DIRS` includes both `~/.local/share/terminfo` and `~/.terminfo` so user-installed entries are found without system-wide installation.

If you SSH to a remote host that lacks the entry, the remote shell will fall back to a generic terminal type. You can copy the compiled entry to the remote host:

```bash
infocmp bloom-terminal-vty-256color | ssh remote-host 'tic -x -'
```

## Dependencies

All platforms:

- bloom-vt (VT engine, consumed via `pkg-config bloom-vt`; source at `/home/thomasc/git/bloom-vt`)
- SDL3
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz (>= 2.0)
- libpng

Linux only:

- fontconfig (font discovery)

macOS only:

- Core Text + Core Foundation (system frameworks, always available)

Optional (Linux):

- gtk4 + libadwaita-1 (for `--gtk4` platform backend)
- EGL + GBM + libdrm (for zero-copy DMA-BUF rendering in GTK4 backend)

### Fedora 41+

```bash
# Build tools
sudo dnf install gcc autoconf automake libtool pkgconf-pkg-config

# Required libraries
sudo dnf install SDL3-devel fontconfig-devel freetype-devel harfbuzz-devel libpng-devel

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

### Building

```bash
./scripts/build-mingw64.sh
```

This produces `build-mingw64/src/.libs/bloom-terminal.exe` with all required DLLs copied alongside. The mingw64 build uses a separate directory so it doesn't conflict with the Linux `build/`.

### Testing with QEMU/KVM

For full interactive testing (ConPTY shell sessions), use a Windows VM with QEMU/KVM:

```bash
./scripts/vm-w32.sh setup    # Download ISOs, create disk images (one-time)
./scripts/vm-w32.sh install  # Boot VM from ISO to install Windows
./scripts/vm-w32.sh deploy   # Cross-compile and write files to VM transfer disk
./scripts/vm-w32.sh run      # Boot the VM (transfer disk appears as second drive)
```

The installer requires a virtio storage driver: **Load driver** → **Browse** → `F:` → `viostor/w11/amd64`.

### Windows Details

- **PTY**: ConPTY (`CreatePseudoConsole`) instead of Unix PTYs (`src/pty_w32.c`)
- **Font resolver**: Native Windows registry-based font discovery (`src/font_resolve_w32.c`) replaces Fontconfig. Default fallback chain: Cascadia Mono → Consolas → Courier New.
- **DWM styling**: Dark title bar, Mica backdrop, custom caption color, rounded corners on Windows 11 (degrades gracefully on older versions)
- **Platform**: SDL3 only (GTK4 backend is not available on Windows)

## macOS Cross-Compilation

bloom-terminal can be cross-compiled for macOS using [osxcross](https://github.com/tpoechtrager/osxcross). The macOS build uses native Core Text font resolution — no Homebrew or external dependencies needed at runtime.

### Prerequisites

1. Set up the osxcross toolchain (downloads ~700 MB SDK from Apple, requires free Apple ID):

```bash
# Download "Command Line Tools for Xcode" .dmg from https://developer.apple.com/download/all/
./scripts/setup-osxcross.sh ~/Downloads/Command_Line_Tools_for_Xcode_*.dmg
```

osxcross is installed into `./osxcross/` (gitignored). All dependencies (zlib, libpng, FreeType, HarfBuzz, SDL3) are cross-compiled automatically on first build. No sudo required.

### Building

```bash
./scripts/build-osxcross.sh
```

This produces `build-osxcross/src/bloom-terminal` (Mach-O 64-bit x86_64). The osxcross build uses a separate directory so it doesn't conflict with the Linux `build/` or Windows `build-mingw64/`.

### Testing with QEMU/KVM

For interactive testing, use a macOS VM with QEMU/KVM via [OSX-KVM](https://github.com/kholia/OSX-KVM):

```bash
./scripts/vm-mac.sh setup    # Download recovery image, create disk images (one-time)
./scripts/vm-mac.sh install  # Boot from recovery to install macOS
./scripts/vm-mac.sh deploy   # Cross-compile and write binary to VM transfer disk
./scripts/vm-mac.sh run      # Boot the VM (transfer disk appears as USB drive)
```

The installer boots into OpenCore — select **"macOS Base System"**, then use **Disk Utility** to erase the SATA disk (APFS, GUID) before installing.

The transfer disk mounts as a USB drive in Finder. On first use, install the terminfo entry (one-time):

```bash
sh /Volumes/NO\ NAME/install-terminfo.sh
```

Then run bloom-terminal from the USB drive or copy it locally.

### macOS Details

- **PTY**: Standard BSD `forkpty()` from `<util.h>` — same POSIX implementation as Linux (`src/pty.c`)
- **Font resolver**: Native Core Text (`src/font_resolve_ct.c`). Default fallback chain: SF Mono → Menlo → Monaco → Courier. Emoji via Apple Color Emoji.
- **Platform**: SDL3 only (GTK4 backend is not available on macOS)

## Testing

```bash
cd build && make check
```

## Development

The project includes:

- `.clang-format` for code formatting
- `autogen.sh` + autotools (`configure.ac`, `Makefile.am`) for the build
- Example scripts demonstrating terminal features, including `examples/unicode/emoji.sh` which exercises COLR/emoji paths

### Code Formatting

Run `make format` (from `build/`) to format all source files. This requires:

- **clang-format** — C source and headers (`src/`, `tests/`)
- **shfmt** — shell scripts (`examples/`, `scripts/`)
- **prettier** — Markdown files

```bash
# Fedora 41+
sudo dnf install clang-tools-extra shfmt
npm install --prefix ~/.local prettier
```

## Author

Thomas Christensen

## License

MIT — see [COPYING](COPYING) for details.
