# portty

A terminal emulator with pluggable backends for terminal emulation, rendering, platform windowing, and fonts.

Currently ships with coffer (terminal), SDL3 or Sokol (renderer/platform), FreeType/HarfBuzz (fonts). Builds natively on Windows (MSYS2/UCRT64: ConPTY, native font resolver, DWM styling) and macOS (Core Text font resolver).

## Features

- Full terminal emulation using coffer — external VT engine (consumed via pkg-config) with UAX #11 + UAX #29 grapheme-cluster width, arbitrary-length clusters per cell, working reflow, and a page-based scrollback ring
- Rendering with SDL3 GPU or Sokol (OpenGL/Metal/D3D11/WebGPU)
- Damage-driven rendering — coffer's accumulated damage is flushed once per frame into a single dirty signal, so the frame is repainted only when terminal content, the cursor, selection, or the scrollback view actually changes; an idle terminal does no rendering work
- Gamma-correct text rendering — antialiased glyph coverage is composited in **linear light** via SDL's GPU renderer (Vulkan on Linux, Direct3D 12 on Windows, Metal on macOS), so text gets its physically-correct, heavier/softer weight like kitty rather than the thin look of sRGB-space blending. Tunable with the kitty-style `text_composition_strategy` config key, which on the GPU renderer runs as a luminance-aware fragment shader — thickening dark-on-light text (reverse video) without bolding normal light-on-dark text.
- Text shaping with HarfBuzz
- Font rasterization with FreeType
- Custom COLR v1 paint graph traversal (gradients, transforms, compositing)
- Bold, italic, and bold-italic font styles (variable font axes, platform-native resolution, synthetic fallback)
- Variable-font support (MM_Var) and axis control
- Dynamic font fallback (up to 8 runtime fallback fonts with codepoint cache; Fontconfig on Linux, Core Text on macOS, FreeType scan on Windows)
- Support for Unicode characters and emoji (COLR v1 color fonts)
- Emoji width paradigm: coverage-aware routing (color emoji font used when VS16 forces emoji presentation, the codepoint is a regional indicator, or the emoji font actually carries the glyph; VS15 forces text); ambiguous-width symbols default to 1 cell; VS16 (U+FE0F) forces 2 cells; symbol-class glyphs from a text font keep their natural design width and sit on the typographic baseline. Widths are computed at insertion time (UAX #11 + #29) and stored on the cell, so the renderer walks rows in plain column order.
- Sixel graphics — DCS sixel images are decoded and stored inside coffer and anchored to the grid, so they scroll, enter scrollback, and clear with the text they sit on. Spec coverage includes RLE, RGB and DEC HLS color (correct blue-origin hue), transparency, and raster attributes. Capability is advertised so sixel-aware tools (`img2sixel`, `lsix`, `chafa`) actually emit graphics: the DA1 reply reports `4`, plus DECSET 80/1070/8452 and XTSMGRAPHICS. Animated/in-place updates (DECSDM mode 80) swap frames in place — the renderer re-uploads a cached texture by the engine's image id + version, and the engine recycles pixel buffers from a pool so streaming same-size frames doesn't churn the heap
- Lottie animations — APC sequences (`ESC _ … ST`) with base64-encoded JSON payloads load, place, and control Lottie animations on the grid. Eight commands (load, load-chunk, place, play, pause, stop, seek, delete) manage animation state and placement tracking. Placements carry per-instance opacity and layer (foreground or background). Animations scroll with the text, enter scrollback, and are cleared with the rows they sit on — the same ownership model as sixel. ThorVG rasterizes each frame; the host fetches RGBA pixels via the coffer API (`cfr_get_lotties()`, `cfr_get_lottie_placements()`, `cfr_lottie_tick()`) and composites them as foreground and background layers. A Python TUI player ([plotty](contrib/plotty)) provides interactive playback with keyboard controls for pause, seek, speed, opacity, and layer toggling. On Windows, ConPTY strips APC sequences (`ESC _`) — the same limitation that prevents the kitty image protocol from working (Windows Terminal issue #8389). `PSEUDOCONSOLE_PASSTHROUGH_MODE` (flag 0x8) is enabled with fallback, but on some builds it is accepted yet unknown sequences are still dropped. As a workaround, plotty on Windows carries the Lottie payload inside **OSC 5555** (`ESC ] 5555 ; <base64> BEL`), which ConPTY does pass through; coffer routes OSC 5555 to the same APC dispatch, so the payload is processed identically regardless of carrier
- Procedural box drawing and block element rendering (U+2500–U+257F)
- Text selection with clipboard support (Ctrl+C or Ctrl+Shift+C to copy, right-click copy/paste). In the alternate screen buffer, left-click/drag selection is blocked when no mouse tracking protocol is active — the application owns the display and terminal-level selection can clobber the app's own clipboard operations (OSC 52) and paint visual artifacts over its UI. Hold Shift to override and select anyway. Right-click paste still works in altscreen. When a mouse tracking mode is active (e.g. an app sends `?1002h`), mouse events are forwarded to the application; Shift overrides the grab so you can select text even while the app owns the pointer.
- OSC 52 clipboard set — applications (tmux `set-clipboard`, neovim `clipboard=osc52`, lazygit, helix, etc.) can copy text to the system clipboard via escape sequence. Read queries (`OSC 52 ; c ; ?`) are silently refused so any program running in the terminal — including processes on the remote end of an SSH session — can't ask the terminal to hand it the contents of your clipboard (passwords, tokens, etc.).
- Soft-wrap aware word selection and copy
- Underline styles (single, double, curly, dotted, dashed) with SGR 58/59 color support
- OSC-8 hyperlinks — dotted Charm-purple underline at rest, solid on hover with pointer cursor; a hover hint shows the full URI. Ctrl+click opens via the system handler. Scheme allow-list (http/https/ftp/ftps/mailto/file) refuses `javascript:`, `data:`, etc.
- Strikethrough rendering (span-based, DPI-aware)
- Reverse video attribute rendering
- Nerd Fonts v2 to v3 codepoint translation
- Notification panel — a top strip for transient messages (e.g. disallowed-URL-scheme warnings on Ctrl+click), dismissible via close button. The `notification_transparency` config key makes it translucent instead of opaque
- Scrollback buffer with mouse wheel and Shift+PageUp/Down
- Selection drag autoscroll — extending a selection drag past the viewport edge scrolls the view and grows the selection at ~30 Hz
- HiDPI support (pixel density scaling for underlines and UI elements)
- Window title via OSC 2
- Custom terminfo entry (`TERM=portty-vty-256color`) with truecolor, cursor style, and bracketed paste (pasted text is distinguished from typed input so shells don't execute it prematurely)
- Kitty keyboard protocol (push/pop/set/query plus the Disambiguate and Report-all flags) — modern TUIs like Claude Code can tell Shift+Enter apart from plain Enter, and Ctrl+letter combos no longer collide with their literal control bytes
- Working-directory tracking via OSC 7 (`file://` URI) and OSC 9;9 (ConEmu protocol) — shells emit these on `cd` so `Ctrl+Shift+N` can spawn a new terminal in the same directory. On Windows, portty injects a `PROMPT_COMMAND` into bash/zsh to emit OSC 7 automatically (ConPTY children can't be inspected via `ReadProcessMemory`)
- Built-in diagnostics report (`Ctrl+Shift+F6`) — version/build, renderer, GPU + driver (permissively-licensed open-source drivers flagged green), font resolution, effective config, and session state, shown in an internal scrollable pager. It renders in-process (no external `$PAGER`), so its clickable OSC-8 "report issues" link works regardless of which pager you use
- Emacs integration — `data/portty.el` (installed to `$(datadir)/emacs/site-lisp/term`) sets up terminal initialization for Emacs
- Desktop integration — freedesktop.org `.desktop` entry, hicolor scalable + symbolic icons, and a Windows Start Menu shortcut (installed/uninstalled automatically by `make install`/`make uninstall`)

## Known Issues

### Wayland: mouse release not detected after dragging out of and back into the window

On Wayland, when the pointer crosses the window border during a selection drag, the compositor sends `wl_pointer.leave` and SDL synthesizes a `BUTTON_UP` event — even though the physical button is still held. portty detects this border artifact and keeps the drag alive so the selection continues to update when the pointer re-enters the window.

However, after the border crossing, SDL has lost all button state (`SDL_GetMouseState` and `SDL_GetGlobalMouseState` both return 0). When the user physically releases the button inside the window, no `BUTTON_UP` event is delivered because SDL already thinks the button is up. The selection drag stays active and continues to follow the mouse until a **click** (starts a new selection, clearing the old one) or a **copy** (right-click or `Ctrl+C`) resets it.

This is a fundamental Wayland protocol limitation ([SDL issue #14980](https://github.com/libsdl-org/SDL/issues/14980), closed as "not our bug"), not a portty bug. The compositor only sends pointer events for surfaces the application owns, and once `wl_pointer.leave` fires, the button state is irrecoverably lost.

**Workaround**: Click anywhere to cancel the stuck selection, then click and drag to start a new one. Right-click copies the current selection and clears it.

## Architecture

portty uses a modular backend abstraction design:

- **PorttyBackend**: Unified platform + rendering backend — handles windowing, input events, clipboard, PTY lifecycle, the main event loop, and graphics output. Selected at configure time via `--with-backend=sdl3|sokol` (default: `sdl3`).
  - **SDL3** (`backend_sdl3.c`) — uses SDL3 for both windowing and GPU rendering. Draws the frame into an `RGBA64_FLOAT` / `SRGB_LINEAR` target via SDL's GPU renderer (Vulkan/D3D12/Metal), so glyph coverage is blended in linear light and re-encoded to sRGB on present. Uses libdecor for Wayland decorations. The SDL3 renderer (`rend_sdl3.c`) is called directly — no separate renderer vtable.
    - Texture atlas with shelf packing and FNV-1a hash-based lookup; LRU eviction when the atlas fills
    - Color-glyph (emoji) texels are sRGB→linear decoded as they enter the atlas: SDL linearizes draw/vertex colors on this path but not sampled texels, so without the decode the present-time re-encode would double-encode color emoji and wash them out. White text-coverage texels are gamma-invariant, so text is unaffected
  - **Sokol** (`backend_sokol.c`) — uses sokol_app for windowing and sokol_gfx for rendering (OpenGL Core on Linux, Metal on macOS, D3D11 on Windows, WebGPU on web). Renders inline within the backend; uses shared atlas packing from `rend_common.c` and a Sokol-specific atlas adapter (`rend_sokol_atlas.c`)

- **Terminal Backend**: Handles terminal emulation and screen state
  - Current implementation: coffer (`terminal_backend_cfr`) — external VT engine consumed via `pkg-config coffer`, bridged through `term_cfr.c` (parser, page-based grid, scrollback ring, reflow, charsets). DEC ANSI parser (Williams state machine), UAX #11 + #29 cluster widths, page-arena style/grapheme interning, scrollback page ring

- **Font Backend**: Handles font loading, shaping, and glyph rasterization
  - Current implementation: FreeType/HarfBuzz (`font_backend_ft`)
  - Custom COLR v1 paint tree traversal implemented in `src/colr.c`
  - FreeType provides COLR v1 APIs for accessing paint data; recursive evaluation, affine transforms, and Porter-Duff compositing are implemented manually
  - Supports solid fills, linear/radial/sweep gradients, transforms, glyph masking, and basic composite modes
  - Some paint semantics (extend modes, all composite operators, transform edge-cases) are best-effort

- **Font Resolver Backend**: Handles font discovery and selection
  - Linux: Fontconfig (`font_resolve_backend_fc`)
  - macOS: Core Text (`font_resolve_backend_ct`) with `CTFontCreateForString` codepoint fallback
  - Windows: Native resolver (`font_resolve_backend_w32`) — GDI enumeration + DirectWrite path resolution for UWP/Store fonts + FreeType codepoint fallback

Each backend defines a standard interface (`PorttyBackend`, `TerminalBackend`, `FontBackend`, `FontResolveBackend`) with `*_init()`/`*_destroy()` lifecycle functions, allowing implementations to be swapped without changing the core application logic. Shared app logic lives in `portty_app.c`; shared rendering helpers (atlas packing, sRGB LUT, font loading) live in `rend_common.c`.

## Building

The project uses GNU Autotools. From a fresh checkout:

```bash
./autogen.sh
mkdir build && cd build
../configure --prefix=$HOME/.local
make -j$(nproc)
make check
make install
```

Use `--enable-release` for an optimized build or `--enable-debug` for unsanitized debug.

To build the Sokol backend instead of SDL3:

```bash
./autogen.sh
mkdir build-sokol && cd build-sokol
../configure --with-backend=sokol
make -j$(nproc)
make check
```

### Make Targets

- `make` — build everything
- `make check` — run the test suite
- `make install` — install to `$prefix` (default `$HOME/.local`); compiles terminfo via `tic`; on Windows, also creates a Start Menu shortcut
- `make format` — clang-format on `src/` and `tests/`, shfmt on `scripts/`, black on `contrib/`, prettier on Markdown
- `make bear` — produce `compile_commands.json` for clangd
- `make regen-shaders` — recompile the GPU glyph-coverage fragment shader (requires glslangValidator)
- `make regen-icon` — regenerate the embedded window icon PNG header from the source SVG (requires rsvg-convert)
- `make dist-portable` — Windows-only: stage exe + DLLs + terminfo into a self-contained ZIP

### Helper Scripts

| Script                           | Purpose                                                                          |
| -------------------------------- | -------------------------------------------------------------------------------- |
| `scripts/profile.sh`             | Build with `-pg`, run a benchmark, write `profile-report.txt`                    |
| `scripts/ref-png.sh T OUT`       | Generate reference PNG of TEXT using hb-view                                     |
| `scripts/ref-layers.sh T P`      | Export each COLR v1 paint layer as `<P>_layer00.png` etc.                        |
| `scripts/colr_layers.py`         | Export individual COLR v1 paint layers as PNG (Python fonttools + blackrenderer) |
| `scripts/pty_record.py`          | Record PTY traffic from a child process (debug feature-detect probes)            |
| `scripts/build-ucrt64.sh`        | Build natively on Windows (MSYS2 UCRT64)                                         |
| `scripts/make-dist-zip.sh`       | Assemble a portable Windows ZIP (called by `make dist-portable`)                 |
| `scripts/install-shortcut.ps1`   | Install a Start Menu shortcut for Portty on Windows (called by `make install`)   |
| `scripts/uninstall-shortcut.ps1` | Uninstall the Start Menu shortcut (called by `make uninstall`)                   |

## Usage

```bash
# Run the terminal emulator
build/src/portty

# Run with verbose output (useful to debug font/COLR/emoji handling)
build/src/portty -v

# Run a specific command instead of the default shell
build/src/portty -- htop

# Display text without spawning a shell (for testing)
build/src/portty --demo "Hello, world!"

# Render text to a PNG file (SDL3 backend only)
build/src/portty -P "😀" output.png

# Render a command's output to PNG (SDL3 backend only)
build/src/portty -P "" --exec ls --wait 500 output.png
```

### CLI Flags

| Flag                        | Description                                                                              |
| --------------------------- | ---------------------------------------------------------------------------------------- |
| `-h`                        | Show help message                                                                        |
| `-v`                        | Verbose output (font resolution, COLR, atlas events)                                     |
| `-f PATTERN`                | Font via fontconfig pattern (e.g. `-f "Cascadia Code-14"`)                               |
| `-g COLSxROWS`              | Initial terminal size (default: 80x24)                                                   |
| `-P TEXT`                   | Render TEXT to PNG (output path as positional arg; SDL3 only)                            |
| `-D PREFIX`                 | COLR layer debug: save each layer as `PREFIX_layer00.png`, etc. (SDL3 only)              |
| `-L` / `--list-fonts`       | List available monospace fonts and exit (Sokol only)                                     |
| `-H S` / `--ft-hinting S`   | FreeType hinting: none/light/normal/mono (default: light; Sokol only)                    |
| `-d TEXT` / `--demo TEXT`   | Display TEXT in terminal without spawning a shell (for testing; Sokol only)              |
| `-V` / `--version`          | Print version and exit                                                                   |
| `--exec CMD`                | With `-P`, spawn CMD on a PTY and render its output to PNG (SDL3 only)                   |
| `--wait MS`                 | With `-P --exec`, milliseconds to drain the PTY before capture (default: 200; SDL3 only) |
| `-s N` / `--scrollback N`   | Scrollback history lines (default: 1000, 0 to disable)                                   |
| `-S FILE` / `--script FILE` | Run debug script FILE (see [Debug Scripting](#debug-scripting))                          |

### Keyboard Shortcuts

| Shortcut             | Action                                                                       |
| -------------------- | ---------------------------------------------------------------------------- |
| `Ctrl+C`             | Copy selection to clipboard (sends SIGINT otherwise)                         |
| `Ctrl+Shift+C`       | Copy selection to clipboard                                                  |
| `Ctrl+Shift+V`       | Paste from clipboard                                                         |
| `Shift+PageUp/Down`  | Scroll through scrollback buffer                                             |
| `Shift+drag`         | Override: select text when app owns the pointer (mouse mode) or in altscreen |
| Right-click          | Copy selection if active, otherwise paste (works in altscreen too)           |
| `Ctrl+click` on link | Open OSC-8 URL via the system handler                                        |
| `Ctrl+Shift+F6`      | Open the diagnostics report (built-in pager)                                 |
| `Ctrl+Shift+N`       | Spawn a new terminal window in the shell's CWD                               |

### Diagnostics Report

`Ctrl+Shift+F6` opens a built-in diagnostics report — version/build, renderer + GPU/driver, font resolution, effective config, and session state — in an internal scrollable pager (not an external `$PAGER`), so its clickable OSC-8 "report issues" link works regardless of your pager. Permissively-licensed open-source GPU drivers (Mesa) are shown green.

While the pager is open:

| Key / action                     | Effect                    |
| -------------------------------- | ------------------------- |
| `q` / `Esc`                      | Close                     |
| `↑`/`↓`, `j`/`k`                 | Scroll one line           |
| `PageUp`/`PageDown`, `Space`/`b` | Scroll one page           |
| `g`/`Home`, `G`/`End`            | Jump to top / bottom      |
| Mouse wheel                      | Scroll                    |
| Drag, double-click, triple-click | Select text / word / line |
| `Ctrl+C` or `Ctrl+Shift+C`       | Copy selection            |
| `Ctrl+click` on a link           | Open URL                  |

## Debug Scripting

portty includes a built-in scripting system for automated debugging and regression testing. Scripts are plain-text files loaded via the `-S` / `--script` CLI flag. Each command executes one per frame inside the render loop, so you can drive the terminal, inspect its state, and capture screenshots at precise points.

```bash
portty --script debug.txt -- my-shell
```

### Script File Format

One command per line. Lines starting with `#` and blank lines are ignored. The `send` command supports `\n`, `\r`, `\t`, `\e` (ESC), and `\\` escape sequences; surrounding double quotes are stripped.

### Commands

| Command                                 | Description                                                                        |
| --------------------------------------- | ---------------------------------------------------------------------------------- |
| `wait <seconds>`                        | Pause script execution for N seconds (monotonic clock)                             |
| `send <text>`                           | Write text to the PTY (supports `\n \r \t \e \\` escapes)                          |
| `raw <hex bytes>`                       | Write raw binary bytes to the PTY (e.g. `raw 1b 5b 6d` = `ESC [ m`)                |
| `assert-contains <text>`                | Assert the terminal grid contains the given substring (prints PASS/FAIL)           |
| `assert-not-contains <text>`            | Assert the terminal grid does NOT contain the given substring                      |
| `screendump <path>`                     | Save the framebuffer to a PNG file (captured after render, before present)         |
| `dumprow <row>`                         | Print all cells in a terminal row                                                  |
| `dumpcells <row> <col_start> <col_end>` | Print cells in the given range with codepoint, width, attributes, and fg/bg colors |
| `dumpverts <row> <col_start> <col_end>` | Dump GPU vertex data for glyphs (Sokol backend only)                               |
| `verifybuf <row> <col_start> <col_end>` | Verify GPU vertex buffer contents (Sokol backend only, deferred to post-present)   |
| `quit`                                  | Request application quit                                                           |

### Example Script

```
# Type a command and verify the output
send echo hello\n
wait 0.5
assert-contains hello
screendump /tmp/portty-screenshot.png
quit
```

### Backend Support

The Sokol backend supports all commands. The SDL3 backend supports all commands except `dumpverts` and `verifybuf` (GPU-specific), which print "not supported by this backend" and skip.

## Configuration

portty can be configured with an INI-style config file called `portty.conf`. CLI flags always take precedence over config file values.

### File Locations

The first file found is used:

1. `./portty.conf` (project-level, current working directory)
2. `$XDG_CONFIG_HOME/portty/portty.conf` (defaults to `~/.config/portty/portty.conf`; on Windows, checked before APPDATA if set)
3. `%APPDATA%\portty\portty.conf` (Windows only)

### Example

```ini
# portty.conf
[terminal]
font = Cascadia Code-14
geometry = 120x40
hinting = light
verbose = false
word_chars = abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/?#[]@!$&'()*+,;=%~
scrollback = 1000
shell = /bin/bash
text_composition_strategy = kitty
```

### Available Keys

All keys are optional. Only the `[terminal]` section is recognized.

| Key                         | Values                                                | Default            | Description                                                                                                                                          |
| --------------------------- | ----------------------------------------------------- | ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `font`                      | Fontconfig pattern                                    | `monospace`        | Font family and size (e.g. `monospace-16`)                                                                                                           |
| `geometry`                  | `COLSxROWS`                                           | `80x24`            | Initial terminal dimensions                                                                                                                          |
| `hinting`                   | `none`, `light`, `normal`, `mono`                     | `light`            | FreeType hinting mode                                                                                                                                |
| `verbose`                   | `true`/`false`                                        | `false`            | Debug output                                                                                                                                         |
| `word_chars`                | Character string                                      | `A-Za-z0-9_-/`     | Characters treated as word for double-click                                                                                                          |
| `scrollback`                | Non-negative integer                                  | `1000`             | Scrollback history lines (0 disables)                                                                                                                |
| `shell`                     | Shell path (optionally with args)                     | `$SHELL`/`COMSPEC` | Default shell when no `--` args given (e.g. `/bin/bash --norc`); falls back to `$SHELL` then `/bin/sh` on Unix, `$COMSPEC` then `cmd.exe` on Windows |
| `text_composition_strategy` | `kitty`, `neutral`/`correct`, or `<gamma> <contrast>` | `neutral`          | Glyph-weight curve on top of linear-light blending, luminance-aware on the GPU renderer (`kitty` = gamma 1.7 / contrast 30)                          |
| `notification_transparency` | `true`/`false`                                        | `false`            | Draw the top notification panel translucent instead of opaque                                                                                        |
| `platform`                  | `sdl3`                                                | _(unset)_          | Select the platform backend (only `sdl3` is accepted; use `--with-backend` at configure time for backend selection)                                  |

Boolean values accept `true`/`false`, `yes`/`no`, or `1`/`0`. Lines starting with `#` or `;` are comments.

## Emoji Width Paradigm

portty enforces four rules for how emoji and symbols are rendered:

1. **Coverage-aware emoji selection.** A codepoint routes to the color emoji font when (a) VS16 (U+FE0F) forces emoji presentation, (b) it is a regional indicator (always emoji), or (c) the codepoint has default emoji presentation (Unicode `Emoji_Presentation`, tracked to Emoji 17.0) and the emoji font carries the glyph. VS15 (U+FE0E) forces text presentation in all cases. Plain text-default emoji codepoints (Dingbats, Misc Symbols) without VS16 stay on the text font when the emoji font lacks the glyph — they are not silently downgraded to a missing-emoji glyph or routed through the emoji path only to fall back after shaping fails.
2. **Ambiguous width = 1 cell.** Ambiguous-width symbols (e.g. ⚠ U+26A0, ☀ U+2600) default to 1 cell. They stay 1 cell wide unless followed by VS16.
3. **VS16 forces 2 cells.** When U+FE0F follows an emoji-presentation base codepoint, the cell width is 2 — e.g. `⚠` is 1 cell but `⚠️` is 2 cells.
4. **Symbol-class glyphs preserve font design.** Dingbats, Misc Symbols, Misc Technical, Geometric Shapes, Supplemental Arrows-B, and Misc Symbols & Arrows rendered through a text font keep their natural design width, sit on the typographic baseline vertically, and are centered horizontally in the cell (FreeType's left bearing is intentionally discarded because mono fonts often calibrate it to an oversized advance — e.g. Noto Sans Mono ✶ has advance 1.2×em with the ink centered in that wider advance). Only vertical overflow triggers a downscale; horizontal overhang into neighbor cells is allowed and handled cleanly by the two-pass row draw (backgrounds first, then glyphs).

coffer computes UAX #11 + UAX #29 cluster widths at insertion time and stores them on the cell, so VS16 emoji come through with `cell.width == 2` and the cell immediately to its right is a continuation cell with `cell.width == 0`. The renderer walks rows in plain column order via `TerminalRowIter` and increments by `cell.width` — no peek-ahead, no shift-vs-absorb decision, no separate "visual" column space. Mouse, cursor, and selection coordinates all share the same single column space.

Multi-codepoint clusters (ZWJ family chains, flag sequences, long combining-mark runs) are stored in a per-page grapheme arena and accessed via `terminal_cell_get_grapheme()` — there is no per-cell codepoint cap, so 7-codepoint sequences like 👨‍👩‍👧‍👦 round-trip through the renderer without truncation.

## Terminfo

portty ships a single terminfo entry (based on `xterm-256color`) under three aliases — `portty-vty-256color`, `portty-256color`, and `portty`. The default `TERM` is `portty-vty-256color`; the alternate names exist for users who prefer to set them.

`setaf`/`setab` are inherited unchanged from `xterm-256color`, so the entry's capability strings stay within the restricted operator subset that Haskell `vty-unix`'s terminfo parser accepts. Truecolor is signalled via the `Tc` flag (which emacs, tmux, vte, alacritty, kitty, ghostty, and most modern TUIs honor) and via `COLORTERM=truecolor` for apps that read the env var directly. Extension caps added on top of `xterm-256color`: `Smulx` (extended underline styles), `Setulc` (underline color), `Ss`/`Se` (cursor shape), `Ms` (OSC 52 set-clipboard), `BE`/`BD` (bracketed paste), `PS`/`PE` (paste delimiters), `hs`/`tsl`/`fsl`/`dsl` (status line / window title), `sitm`/`ritm` (italic), `smxx`/`rmxx` (strikethrough).

The `RGB` flag is deliberately **not** advertised: its ncurses contract is "feed packed 24-bit ints to `setaf` and it'll DTRT," which would require a custom `setaf` outside the vty-unix parser subset. Truecolor consumers are expected to use `Tc` or `COLORTERM`.

On Linux, the entry is compiled and installed automatically by `make install` via `tic`. On macOS (QEMU VM), run `sh /Volumes/NO\ NAME/install-terminfo.sh` to compile and install natively. The child shell's `TERMINFO_DIRS` includes both `~/.local/share/terminfo` and `~/.terminfo` so user-installed entries are found without system-wide installation.

If you SSH to a remote host that lacks the entry, the remote shell will fall back to a generic terminal type. You can copy the compiled entry to the remote host:

```bash
infocmp portty-vty-256color | ssh remote-host 'tic -x -'
```

## Dependencies

All platforms:

- coffer (VT engine, consumed via `pkg-config coffer`; source at https://github.com/chestso/coffer)
- SDL3 (when using the SDL3 backend)
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz (>= 2.0)
- libpng

The SDL3 backend uses SDL's GPU renderer for gamma-correct linear-light blending, so a working GPU backend is required **at runtime**: Vulkan on Linux, Direct3D 12 on Windows, Metal on macOS.

The Sokol backend uses sokol_gfx instead of SDL3 for rendering (OpenGL Core on Linux, Metal on macOS, D3D11 on Windows), so no Vulkan/D3D12 runtime is needed.

Linux only:

- fontconfig (font discovery)
- a Vulkan runtime (loader + ICD, e.g. `mesa-vulkan-drivers`) for the GPU renderer

macOS only:

- Core Text + Core Foundation (system frameworks, always available; SDL3 also links GameController, CoreHaptics, CoreMotion, and CoreMedia)

### Fedora 41+

```bash
# Build tools
sudo dnf install gcc autoconf automake libtool pkgconf-pkg-config

# Required libraries
sudo dnf install SDL3-devel fontconfig-devel freetype-devel harfbuzz-devel libpng-devel

# Runtime: a Vulkan driver for the GPU renderer (most systems have it)
sudo dnf install mesa-vulkan-drivers vulkan-loader

# Optional: compile_commands.json for editors
sudo dnf install bear
```

## Windows Native Build

portty builds natively on Windows using MSYS2 with the UCRT64 environment. The Windows build uses ConPTY for terminal emulation.

### Prerequisites

Install [MSYS2](https://www.msys2.org/), then in a UCRT64 shell (`msys2_shell.cmd -ucrt64`):

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 \
      mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-harfbuzz \
      mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-autotools

# Optional: only needed to regenerate the Windows .ico (see [Regenerating the Windows icon](#regenerating-the-windows-icon))
pacman -S mingw-w64-ucrt-x86_64-imagemagick mingw-w64-ucrt-x86_64-librsvg
```

coffer is not in the official pacman repo — build and install it separately (`make install` in the coffer repo, or ensure `PKG_CONFIG_PATH` points at its build output).

### Building

The easiest way is the helper script, which can be run from any shell (git-bash, cmd, PowerShell, or an MSYS2 shell). It re-execs into a real MSYS2 UCRT64 shell, applies the `sh` workaround (see below), and runs the full build. It assumes the [prerequisites](#prerequisites) are already installed:

```bash
./scripts/build-ucrt64.sh            # autoreconf + configure + make + check
./scripts/build-ucrt64.sh --install  # build, then make install
./scripts/build-ucrt64.sh --enable-release  # extra args forwarded to configure
```

Or build manually from a UCRT64 shell:

```bash
./autogen.sh   # or: aclocal && autoheader && automake && autoconf
mkdir build && cd build
../configure
make -j$(nproc)
make check
```

The default build mode skips ASan/UBSan (not available on MinGW) and uses unsanitized debug flags automatically.

### Regenerating the Windows icon

The committed `data/icons/portty.ico` is a multi-resolution icon (256/128/64/48/32/16) generated from the source SVG via a 2-pass process: `rsvg-convert` renders each size individually, then `icotool` combines them. To regenerate after changing the SVG:

```bash
SVG=data/icons/hicolor/scalable/apps/portty.svg
OUTDIR=data/icons
for size in 16 32 48 64 128 256; do
    rsvg-convert -w $size -h $size -f png "$SVG" -o "$OUTDIR/portty-$size.png"
done
icotool -c -o "$OUTDIR/portty.ico" "$OUTDIR"/portty-{16,32,48,64,128,256}.png
rm "$OUTDIR"/portty-{16,32,48,64,128,256}.png
```

Requires `rsvg-convert` (librsvg) and `icotool` (icoutils).

To regenerate the embedded window icon (used by `SDL_SetWindowIcon`):

```bash
make -C build/src regen-icon    # needs rsvg-convert
```

### `autoreconf` fails on scoop-installed MSYS2 (sh workaround)

On some MSYS2 installs (notably scoop-managed), `/usr/bin/sh` and `/usr/bin/bash` are the same binary. When bash is invoked as `sh` — as `libtoolize` does via `#!/usr/bin/env sh` — its POSIX-mode `test -d` builtin intermittently fails to see `/ucrt64` paths (a mount-table visibility race), aborting `autoreconf` with:

```
libtoolize: error: $pkgauxdir is not a directory: '/ucrt64/share/libtool/build-aux'
```

`build-ucrt64.sh` applies the workaround automatically. To do it manually, shadow `sh` with a bash copy earlier on `PATH` before running `autoreconf`:

```bash
FIXSH=$(mktemp -d /tmp/portty-fixsh.XXXXXX)
cp /usr/bin/bash "$FIXSH/sh"
export PATH="$FIXSH:$PATH"
./autogen.sh          # now succeeds
rm -rf "$FIXSH"
```

### Windows Details

- **PTY**: ConPTY (`CreatePseudoConsole`) instead of Unix PTYs (`src/pty_w32.c`)
- **Font resolver**: Native Windows font discovery (`src/font_resolve_w32.c`) replaces Fontconfig. Uses `EnumFontFamiliesExW` (GDI) for accurate family/style/pitch enumeration, dual registry scan (HKLM + HKCU) for file path resolution of traditional installed fonts, and DirectWrite (`IDWriteLocalFontFileLoader::GetFilePathFromKey`) as a fallback for UWP/Store fonts (e.g. Cascadia Mono) that GDI can enumerate but the registry cannot resolve. The system default console font is read from `HKCU\Console\FaceName` (resolving the `__DefaultTTFont__` sentinel). Default monospace fallback chain: Cascadia Mono → Cascadia Code → Consolas → Courier New.
- **DWM styling**: Dark title bar, Mica backdrop, custom caption color, rounded corners on Windows 11 (degrades gracefully on older versions)
- **ConPTY passthrough**: `PSEUDOCONSOLE_PASSTHROUGH_MODE` (flag 0x8, Windows 11 22H2+) is enabled by default with fallback to standard mode on older builds. When effective, it relays the raw VT stream from the child process, preserving unknown sequences (e.g. APC for Lottie). On some builds the flag is accepted but does not actually pass unknown sequences through; the OSC 5555 workaround handles this transparently
- **Platform**: SDL3

## Testing

```bash
cd build && make check
```

| Test                      | What it covers                                                        |
| ------------------------- | --------------------------------------------------------------------- |
| `test_atlas`              | Texture atlas: insert/lookup, shelf packing, eviction                 |
| `test_unicode`            | Emoji detection, ZWJ, skin tones, UTF-8 decoding                      |
| `test_conf`               | Config parser: fonts, geometry, hinting, booleans                     |
| `test_term_cfr`           | Terminal backend bridging to coffer                                   |
| `test_osc52`              | OSC 52 clipboard set sequences                                        |
| `test_clipboard_deferred` | Wayland clipboard deferred-free invariant (use-after-free regression) |
| `test_altscreen_mouse`    | Altscreen/mouse-mode state and dispatch logic                         |
| `test_sixel`              | Sixel image end-to-end through host bridge                            |
| `test_lottie`             | Lottie animation bridge to coffer                                     |
| `test_paste_normalize`    | Clipboard line-ending normalization (CRLF→LF fix for Windows paste)   |
| `test_diag`               | Diagnostics report generation                                         |
| `test_rend_common`        | Shared rendering helpers (atlas packing, sRGB LUT)                    |
| `test_portty_app`         | Shared app logic and state management                                 |
| `test_timer`              | Timer wheel and callback scheduling                                   |
| `test_pty_pause`          | PTY pause/resume during selection (SDL3 + POSIX only)                 |
| `test_debug_script`       | Debug script parser (all command types, edge cases, escapes)          |
| `test_path_compat`        | MSYS2/Unix → Windows path conversion                                  |
| `test_boxdraw`            | Procedural box-drawing alignment                                      |
| `test_cursor_render`      | Cursor-only render regression                                         |
| `test_scroll`             | Mouse-wheel scroll logic: sub-tick accumulation                       |

The test count differs by backend: 20 tests on SDL3 (POSIX), 19 on Sokol/Windows — `test_pty_pause` is SDL3+POSIX only.

Run individual tests with `-v` for verbose output, e.g. `./build/tests/test_atlas -v`.

## plotty — Lottie Player

[plotty](contrib/plotty) is a Python TUI application that plays Lottie animations in portty using the APC protocol. It provides interactive playback with keyboard controls for pause, seek, speed, opacity, and layer toggling.

### Install

```bash
pip install contrib/plotty
```

Requires Python ≥ 3.10 and [Textual](https://textual.textualize.io/) (installed automatically as a dependency).

### Usage

```bash
# Play an animation (loops by default)
plotty animation.json

# Play once
plotty -L animation.json

# Start at 2× speed
plotty -s 2 animation.json

# Run inside portty
portty -- plotty animation.json
```

### Controls

| Key     | Action              |
| ------- | ------------------- |
| `space` | Pause / resume      |
| `←`/`→` | Seek -5 / +5 frames |
| `+`/`-` | Speed up / down     |
| `r`     | Restart             |
| `L`     | Toggle loop         |
| `b`     | Toggle bg/fg layer  |
| `[`/`]` | Opacity -/+ 10%     |
| `?`     | Toggle help panel   |
| `q`     | Quit                |

## Development

The project includes:

- `.clang-format` for code formatting
- `autogen.sh` + autotools (`configure.ac`, `Makefile.am`) for the build
- Example files under `examples/` — plain `cat`-able demos (colors, attributes, unicode, emoji-width)

### Code Formatting

Run `make format` (from `build/`) to format all source files. This requires:

- **clang-format** — C source and headers (`src/`, `tests/`)
- **black** — Python files (`contrib/`)
- **shfmt** — shell scripts (`scripts/`)
- **prettier** — Markdown files

```bash
# Fedora 41+
sudo dnf install clang-tools-extra black shfmt
npm install --prefix ~/.local prettier
```

## Author

Thomas Christensen

## Acknowledgments

A tip of the hat to [Charm](https://charm.land): portty's default 16-color palette is their [CharmTone](https://github.com/charmbracelet/x/tree/main/exp/charmtone) scheme (via coffer), the cream foreground (`#fffdf5`) is their own body text, and the OSC-8 hyperlinks wear Charm purple. Thanks for keeping the terminal beautiful. 🌸

## License

MIT — see [COPYING](COPYING) for details.
