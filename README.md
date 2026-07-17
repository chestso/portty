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
- Emoji width paradigm: coverage-aware routing — the color emoji font is used whenever it carries the glyph, regardless of VS15/VS16; VS15 (U+FE0E) only narrows the cell to 1 column (it does not force the text font); VS16 (U+FE0F) forces 2 cells; ambiguous-width symbols default to 1 cell; symbol-class glyphs from a text font keep their natural design width and sit on the typographic baseline. Widths are computed at insertion time (UAX #11 + #29) and stored on the cell, so the renderer walks rows in plain column order.
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

See [BUGS.md](BUGS.md).

## Building and Hacking

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, architecture notes, and how to run the tests.

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

```

### CLI Flags

| Flag                        | Description                                                                 |
| --------------------------- | --------------------------------------------------------------------------- |
| `-h`                        | Show help message                                                           |
| `-v`                        | Verbose output (font resolution, COLR, atlas events)                        |
| `-f PATTERN`                | Font via fontconfig pattern (e.g. `-f "Cascadia Code-14"`)                  |
| `-g COLSxROWS`              | Initial terminal size (default: 80x24)                                      |
| `-D PREFIX`                 | COLR layer debug: save each layer as `PREFIX_layer00.png`, etc. (SDL3 only) |
| `-L` / `--list-fonts`       | List available monospace fonts and exit                                     |
| `-H S` / `--ft-hinting S`   | FreeType hinting: none/light/normal/mono (default: light)                   |
| `-d TEXT` / `--demo TEXT`   | Display TEXT in terminal without spawning a shell (for testing)             |
| `-V` / `--version`          | Print version and exit                                                      |
| `-s N` / `--scrollback N`   | Scrollback history lines (default: 1000, 0 to disable)                      |
| `-S FILE` / `--script FILE` | Run debug script FILE (see [Debug Scripting](#debug-scripting))             |

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

One command per line. Lines starting with `#` and blank lines are ignored. The `send`/`emit` commands support `\n`, `\r`, `\t`, `\e` (ESC), `\xNN`, and `\\\` escape sequences; surrounding double quotes are stripped.

### Commands

| Command                                 | Description                                                                           |
| --------------------------------------- | ------------------------------------------------------------------------------------- |
| `wait <seconds>`                        | Pause script execution for N seconds (monotonic clock)                                |
| `send <text>`                           | Write text to the PTY input (child's stdin). Supports `\n \r \t \e \xNN \\\` escapes. |
| `sendln <text>`                         | Same as `send`, but appends `\r\n` (as if the user pressed Return)                    |
| `emit <text>`                           | Emit text directly to the terminal emulator (not the child). Supports `\e`/`\xNN`.    |
| `raw <hex bytes>`                       | Write raw binary bytes to the PTY input (e.g. `raw 1b 5b 6d` = `ESC [ m`)             |
| `emit-raw <hex bytes>`                  | Write raw binary bytes directly to the terminal emulator                              |
| `assert-contains <text>`                | Assert the terminal grid contains the given substring (prints PASS/FAIL)              |
| `assert-not-contains <text>`            | Assert the terminal grid does NOT contain the given substring                         |
| `screendump <path>`                     | Save the framebuffer to a PNG file (captured after render, before present)            |
| `dumprow <row>`                         | Print all cells in a terminal row                                                     |
| `dumpcells <row> <col_start> <col_end>` | Print cells in the given range with codepoint, width, attributes, and fg/bg colors    |
| `dumpverts <row> <col_start> <col_end>` | Dump GPU vertex data for glyphs (Sokol backend only)                                  |
| `verifybuf <row> <col_start> <col_end>` | Verify GPU vertex buffer contents (Sokol backend only, deferred to post-present)      |
| `mousemove <x> <y>`                     | Simulate a mouse move to logical pixel coordinates (Sokol backend only)               |
| `notify ["title"] ["body"]`             | Show a transient notification panel (Sokol backend only)                              |
| `quit`                                  | Request application quit                                                              |

### `send` vs `emit`

There are two separate input paths for debug scripts:

- **`send` / `sendln` / `raw`** write to the **PTY input** (the child process's stdin). Use these when you want to simulate a user typing into the shell. The shell must output escape sequences on stdout before coffer sees them.
- **`emit` / `emit-raw`** write directly to **coffer's terminal emulator**, bypassing the child. Use these when you want to feed VT sequences straight to the terminal without involving the shell.

For `send`, append `\r` or use `sendln` when you want the shell to execute the line:

```
# Simulate the user typing "echo hello" and pressing Return
sendln echo hello
wait 0.5
assert-contains hello
```

`send` with `printf` is the right way to make the shell emit terminal escape sequences:

```
# Correct: printf outputs the ESC bytes on stdout, coffer parses them
send printf '\\033[4mSingle\\033[0m'\r
wait 0.5
assert-contains Single
```

`emit` is useful when you want to bypass the shell entirely. Escape sequences are expanded by the script parser and fed straight to the VT engine:

```
emit \e[2J\e[H
emit \e[4mSingle\e[0m \e[4:2mDouble\e[0m\r\n
wait 0.5
assert-contains Single
assert-contains Double
```

Key points:

- Use `send`/`sendln` to simulate user input to the shell; use `emit` to drive the terminal directly.
- Inside `printf` arguments for `send`, use `\033` (not `\e`).
- `emit` supports `\e` and `\xNN` for arbitrary bytes; `send` does too, but those bytes go to the child stdin.
- `raw` / `emit-raw` are useful for binary input (e.g. `raw 03` for Ctrl-C, `raw 1a` for Ctrl-Z).

### Example Scripts

Drive the terminal directly with `emit`:

```
# Run with: portty -S emit_demo.script
wait 0.5
emit \e[2J\e[H
emit \e[4mSingle underline\e[0m\r\n
emit \e[4:2mDouble underline\e[0m\r\n
emit \e[9mStrikethrough\e[0m\r\n
wait 0.5
assert-contains Single
assert-contains Strikethrough
screendump /tmp/portty-emit.png
wait 5.0
quit
```

Simulate a user typing commands with `send`/`sendln`:

```
# Run with: portty -S send_demo.script
wait 0.5
sendln clear
wait 0.5
send printf '\\033[4mSingle\\033[0m\r\n'\r
wait 0.5
send printf '\\033[9mStrikethrough\\033[0m\r\n'\r
wait 0.5
assert-contains Single
assert-contains Strikethrough
screendump /tmp/portty-send.png
wait 5.0
quit
```

### Mouse and notification commands (Sokol backend only)

`mousemove` simulates a mouse move to the given **logical** pixel coordinates. This is useful for testing hover states (such as OSC-8 hyperlink previews) without a real mouse or window server. Coordinates are in the same logical pixel space the app uses for cell math, so they are unaffected by HiDPI content scaling.

`notify` shows a transient top notification panel. With one argument it sets the title and leaves the body empty; with `"title" "body"` it sets both. The title and body must each be wrapped in double quotes and separated by a space.

Show a notification and then hover an OSC-8 hyperlink to capture the hover preview:

```
# Run with: portty -S hover_demo.script
wait 0.5
emit \e[2J\e[H
emit \e]8;;https://example.com/some-long-url-path\e\\Hover me\e]8;;\e\\\r\n
wait 0.5
notify "Demo" "Notification body text"
wait 0.5
# Hover over the "Hover me" link (adjust x/y to match your layout)
mousemove 200 40
wait 1.0
screendump /tmp/portty-hover.png
wait 5.0
quit
```

### Backend Support

The Sokol backend supports all commands. The SDL3 backend supports all commands except `dumpverts`, `verifybuf`, `mousemove`, and `notify` (Sokol-specific), which print "not supported by this backend" and skip.

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

portty deliberately breaks the Unicode rule that VS15 (text presentation selector) forces text-only rendering. The guiding principle is **beauty over spec**: if the color emoji font has a glyph for a codepoint, portty prefers it — even when VS15 is present. VS15 only narrows the cell width to 1 column; it does not block access to the color emoji font. This is the same treatment already given to ambiguous-width symbols: they route to the color emoji font (if it carries the glyph) at 1-cell width. VS15 is just the explicit form of that.

portty enforces four rules for how emoji and symbols are rendered:

1. **Coverage-aware emoji selection.** A codepoint routes to the color emoji font whenever the emoji font carries the glyph, regardless of VS15. VS16 (U+FE0F) additionally forces emoji presentation and 2-cell width; regional indicators are always emoji. When VS15 (U+FE0E) is present, the cell width is 1 column, but routing to the color emoji font still happens if the glyph is available. When both VS15 and VS16 are present, VS15 takes precedence for width (1 cell) but does not suppress the color emoji font if it carries the glyph. Plain text-default emoji codepoints (Dingbats, Misc Symbols) without VS16 stay on the text font when the emoji font lacks the glyph — they are not silently downgraded to a missing-emoji glyph or routed through the emoji path only to fall back after shaping fails.
2. **Ambiguous width = 1 cell.** Ambiguous-width symbols (e.g. ⚠ U+26A0, ☀ U+2600) default to 1 cell. They stay 1 cell wide unless followed by VS16. Like all 1-cell symbols, they still route to the color emoji font when it carries the glyph — the 1-cell width means the glyph is rendered at 1-cell pixel budget, not that it is excluded from the emoji font.
3. **VS16 forces 2 cells.** When U+FE0F follows an emoji-presentation base codepoint, the cell width is 2 — e.g. `⚠` is 1 cell but `⚠️` is 2 cells.
4. **Symbol-class glyphs preserve font design.** Dingbats, Misc Symbols, Misc Technical, Geometric Shapes, Supplemental Arrows-B, and Misc Symbols & Arrows rendered through a text font keep their natural design width, sit on the typographic baseline vertically, and are centered horizontally in the cell (FreeType's left bearing is intentionally discarded because mono fonts often calibrate it to an oversized advance — e.g. Noto Sans Mono ✶ has advance 1.2×em with the ink centered in that wider advance). Only vertical overflow triggers a downscale; horizontal overhang into neighbor cells is allowed and handled cleanly by the two-pass row draw (backgrounds first, then glyphs).

coffer computes UAX #11 + UAX #29 cluster widths at insertion time and stores them on the cell, so VS16 emoji come through with `cell.width == 2` and the cell immediately to its right is a continuation cell with `cell.width == 0`. The renderer walks rows in plain column order via `TerminalRowIter` and increments by `cell.width` — no peek-ahead, no shift-vs-absorb decision, no separate "visual" column space. Mouse, cursor, and selection coordinates all share the same single column space.

Multi-codepoint clusters (ZWJ family chains, flag sequences, long combining-mark runs) are stored in a per-page grapheme arena and accessed via `terminal_cell_get_grapheme()` — there is no per-cell codepoint cap, so 7-codepoint sequences like 👨‍👩‍👧‍👦 round-trip through the renderer without truncation.

## Terminfo

portty ships a single terminfo entry (based on `xterm-256color`) under three aliases — `portty-vty-256color`, `portty-256color`, and `portty`. The default `TERM` is `portty-vty-256color`; the alternate names exist for users who prefer to set them.

`setaf`/`setab` are inherited unchanged from `xterm-256color`, so the entry's capability strings stay within the restricted operator subset that Haskell `vty-unix`'s terminfo parser accepts. Truecolor is signalled via the `Tc` flag (which emacs, tmux, vte, alacritty, kitty, ghostty, and most modern TUIs honor) and via `COLORTERM=truecolor` for apps that read the env var directly. Extension caps added on top of `xterm-256color`: `Tc` (truecolor), `hs`/`tsl`/`fsl`/`dsl` (status line / window title), `Smulx` (extended underline styles), `Setulc` (underline color), `Sync` (synchronized output, DEC mode 2026).

The `RGB` flag is deliberately **not** advertised: its ncurses contract is "feed packed 24-bit ints to `setaf` and it'll DTRT," which would require a custom `setaf` outside the vty-unix parser subset. Truecolor consumers are expected to use `Tc` or `COLORTERM`.

On Linux, the entry is compiled and installed automatically by `make install` via `tic`. On macOS, run `sh install-terminfo.sh` to compile and install the entry natively. The child shell's `TERMINFO_DIRS` includes both `~/.local/share/terminfo` and `~/.terminfo` so user-installed entries are found without system-wide installation.

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

## Author

Thomas Christensen

## Acknowledgments

A tip of the hat to [Charm](https://charm.land): portty's default 16-color palette is their [CharmTone](https://github.com/charmbracelet/x/tree/main/exp/charmtone) scheme (via coffer), the cream foreground (`#fffdf5`) is their own body text, and the OSC-8 hyperlinks wear Charm purple. Thanks for keeping the terminal beautiful. 🌸

## License

MIT — see [COPYING](COPYING) for details.
