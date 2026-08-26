# portty

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/chestso/portty)

A terminal emulator with pluggable backends for terminal emulation, rendering, platform windowing, and fonts.

Currently ships with coffer (terminal), SDL3 (renderer/platform), FreeType/HarfBuzz (fonts). Builds natively on Windows (MSYS2/UCRT64: ConPTY, native font resolver, DWM styling) and macOS (Core Text font resolver).

## Features

**Terminal emulation: [coffer](https://github.com/chestso/coffer)** (external VT engine, consumed via pkg-config)

- UAX #11 + UAX #29 grapheme-cluster width, arbitrary-length clusters per cell, working reflow, page-based scrollback ring
- Sixel graphics — DCS sixel decoding, grid anchoring, RLE/RGB/HLS color, transparency, raster attributes; capability advertising (DA1 reports `4`, DECSET 80/1070/8452, XTSMGRAPHICS); animated in-place updates (DECSDM mode 80) with frame swapping via image id + version
- iTerm2 inline images — OSC 1337 (`File=`/multipart) parsing, PNG/JPEG/BMP/GIF/TGA decode, full 8-bit alpha, cursor-anchored placement
- Kitty graphics — APC `G` protocol (transmit/display/delete/query/frame/transparency), chunked transfer (`m=1/0`), zlib (`o=z`) payloads, virtual and relative placements, z-index layering. On Windows, ConPTY strips APC, so commands ride OSC 5555 and responses leave via OSC 5556
- Lottie animations — APC sequence parsing (`ESC _ … ST`) with eight commands (load, load-chunk, place, play, pause, stop, seek, delete); placement tracking with opacity and layer; ThorVG rasterization. On Windows, ConPTY strips APC, so commands ride OSC 5555 and responses leave via OSC 5556
- OSC-8 hyperlinks — parsing and tracking
- OSC 52 clipboard set — sequence parsing (read queries silently refused for security)
- OSC 10/11 color queries — responds to foreground/background color queries with the terminal's default colors
- XTWINOPS (CSI `t`) — reports text-area size in cells (mode 18), pixels (mode 14), and cell size (mode 16)
- Kitty keyboard protocol — push/pop/set/query plus Disambiguate and Report-all flags
- Working-directory tracking — OSC 7 (`file://` URI) and OSC 9;9 (ConEmu protocol)
- Window title — OSC 2 parsing
- Damage accumulation — flushed once per frame into a single dirty signal

**Rendering: portty** (SDL3 GPU backend)

- Damage-driven rendering — frame repainted only when terminal content, cursor, selection, or scrollback view changes; idle terminal does no rendering work
- Gamma-correct text rendering — antialiased glyph coverage composited in **linear light** via SDL's GPU renderer (Vulkan on Linux, Direct3D 12 on Windows, Metal on macOS), giving physically-correct weight like kitty. Tunable with `text_composition_strategy` config key (luminance-aware fragment shader on GPU renderer)
- Text shaping with HarfBuzz
- Font rasterization with FreeType
- Custom COLR v1 paint graph traversal (gradients, transforms, compositing)
- Bold, italic, and bold-italic font styles (variable font axes, platform-native resolution, synthetic fallback)
- Variable-font support (MM_Var) and axis control
- Dynamic font fallback (up to 8 runtime fallback fonts with codepoint cache; Fontconfig on Linux, Core Text on macOS, FreeType scan on Windows)
- Unicode and emoji support (COLR v1 color fonts)
- Emoji rendering with coverage-aware font routing — the color emoji font is used whenever it carries the glyph, regardless of VS15/VS16; VS15 (U+FE0E) does not force the text font. See "Emoji Rendering Paradigm" below.
- Unified inline image rendering — sixel, iTerm2, and kitty graphics share one image cache and compositing path (`render_images`), drawn in two z-index passes: negative-z kitty placements behind text, non-negative (sixel/iTerm2 + kitty `z>=0`) on top. Kitty placements support source rectangles and z-index layering
- Lottie rendering — RGBA frame fetch via coffer API (`cfr_get_lotties()`, `cfr_get_lottie_placements()`, `cfr_lottie_tick()`), foreground/background layer compositing
- Procedural box drawing and block element rendering (U+2500–U+257F), with float stroke thickness and proportional margins for seamless diagonal tiling
- Text selection with clipboard support (Ctrl+C or Ctrl+Shift+C to copy, right-click copy/paste). In the alternate screen buffer, left-click/drag selection is blocked when no mouse tracking protocol is active — the application owns the display and terminal-level selection can clobber the app's own clipboard operations (OSC 52) and paint visual artifacts over its UI. Hold Shift to override and select anyway. Right-click paste still works in altscreen. When a mouse tracking mode is active (e.g. an app sends `?1002h`), mouse events are forwarded to the application; Shift overrides the grab so you can select text even while the app owns the pointer.
- OSC 52 clipboard write — applications (tmux `set-clipboard`, neovim `clipboard=osc52`, lazygit, helix, etc.) can copy to system clipboard
- Soft-wrap aware word selection and copy
- Underline styles (single, double, curly, dotted, dashed) with SGR 58/59 color support
- OSC-8 hyperlinks — pointer cursor changes on hover; hover hint shows full URI. Ctrl+click opens via system handler. Scheme allow-list (http/https/ftp/ftps/mailto/file) refuses `javascript:`, `data:`, etc.
- Strikethrough rendering (span-based, DPI-aware)
- Reverse video attribute rendering
- Blinking text (SGR 5) is parsed but deliberately not rendered — widely considered an accessibility hazard and visual distraction in modern terminals
- Nerd Fonts v2 to v3 codepoint translation, with icons rendered inline alongside text
- Notification panel — a top strip for transient messages (e.g. disallowed-URL-scheme warnings on Ctrl+click), dismissible via close button
- Scrollback navigation with mouse wheel and Ctrl+Shift+PageUp/Down (page) / Ctrl+Shift+Up/Down (line)
- Selection drag autoscroll — extending a selection drag past the viewport edge scrolls the view and grows the selection at ~30 Hz
- HiDPI support (pixel density scaling for underlines and UI elements)
- Window title — sets platform window title from coffer-parsed OSC 2
- Custom terminfo entry (`TERM=portty-vty-256color`) with truecolor, cursor style, and bracketed paste
- Working-directory spawning — `Ctrl+Shift+N` spawns a new terminal in the shell's CWD (from OSC 7/OSC 9;9). On Windows, portty injects a `PROMPT_COMMAND` into bash/zsh to emit OSC 7 automatically (ConPTY children can't be inspected via `ReadProcessMemory`)
- Built-in diagnostics report (`Ctrl+Shift+F6`) — version/build, renderer, GPU + driver (permissively-licensed open-source drivers flagged green), font resolution, effective config, and session state, shown in an internal scrollable pager. The capabilities line lists all three inline image protocols (sixel, iTerm2, kitty graphics) alongside OSC 8, grapheme clusters, and reflow. It renders in-process (no external `$PAGER`), so its clickable OSC-8 "report issues" link works regardless of which pager you use
- Emacs integration — `data/portty.el` (installed to `$(datadir)/emacs/site-lisp/term`) sets up terminal initialization for Emacs, including automatic `xterm-mouse-mode` so the mouse works in `emacs -nw`
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

| Flag                        | Description                                                     |
| --------------------------- | --------------------------------------------------------------- |
| `-h`                        | Show help message                                               |
| `-v`                        | Verbose output (font resolution, COLR, atlas events)            |
| `-f PATTERN`                | Font via fontconfig pattern (e.g. `-f "Cascadia Code-14"`)      |
| `-g COLSxROWS`              | Initial terminal size (default: 80x24)                          |
| `--dpi-scale SCALE`         | Multiply detected DPI scale (default: 1.0)                      |
| `-L` / `--list-fonts`       | List available monospace fonts and exit                         |
| `-H S` / `--ft-hinting S`   | FreeType hinting: none/light/normal/mono (default: light)       |
| `-d TEXT` / `--demo TEXT`   | Display TEXT in terminal without spawning a shell (for testing) |
| `-V` / `--version`          | Print version and exit                                          |
| `-s N` / `--scrollback N`   | Scrollback history lines (default: 1000, 0 to disable)          |
| `-S FILE` / `--script FILE` | Run script FILE (see [Scripting](#scripting))                   |
| `-W` / `--ambiguous-wide`   | Render ambiguous-width chars as 2 cells                         |

### Keyboard Shortcuts

| Shortcut                 | Action                                                                       |
| ------------------------ | ---------------------------------------------------------------------------- |
| `Ctrl+C`                 | Copy selection to clipboard (sends SIGINT otherwise)                         |
| `Ctrl+Shift+C`           | Copy selection to clipboard                                                  |
| `Ctrl+Shift+V`           | Paste from clipboard                                                         |
| `Ctrl+Shift+PageUp/Down` | Scroll through scrollback buffer (one page)                                  |
| `Ctrl+Shift+Up/Down`     | Scroll one line through scrollback buffer                                    |
| `Shift+drag`             | Override: select text when app owns the pointer (mouse mode) or in altscreen |
| Right-click              | Copy selection if active, otherwise paste (works in altscreen too)           |
| `Ctrl+click` on link     | Open OSC-8 URL via the system handler                                        |
| `Ctrl+Shift+F6`          | Open the diagnostics report (built-in pager)                                 |
| `Ctrl+Shift+N`           | Spawn a new terminal window in the shell's CWD                               |

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

## Scripting

portty includes a built-in scripting system for automated debugging and regression testing. Scripts are plain-text files loaded via the `-S` / `--script` CLI flag. Each command executes one per frame inside the render loop, so you can drive the terminal, inspect its state, and capture screenshots at precise points.

```bash
portty --script debug.txt -- my-shell
```

### Script File Format

One command per line. Lines starting with `#` and blank lines are ignored. The `send`/`emit` commands support `\n`, `\r`, `\t`, `\e` (ESC), `\xNN`, and `\\\` escape sequences; surrounding double quotes are stripped.

### Commands

| Command                                                               | Description                                                                           |
| --------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `wait <seconds>`                                                      | Pause script execution for N seconds (monotonic clock)                                |
| `send <text>`                                                         | Write text to the PTY input (child's stdin). Supports `\n \r \t \e \xNN \\\` escapes. |
| `sendln <text>`                                                       | Same as `send`, but appends `\r\n` (as if the user pressed Return)                    |
| `emit <text>`                                                         | Emit text directly to the terminal emulator (not the child). Supports `\e`/`\xNN`.    |
| `raw <hex bytes>`                                                     | Write raw binary bytes to the PTY input (e.g. `raw 1b 5b 6d` = `ESC [ m`)             |
| `emit-raw <hex bytes>`                                                | Write raw binary bytes directly to the terminal emulator                              |
| `assert-contains <text>`                                              | Assert the terminal grid contains the given substring (prints PASS/FAIL)              |
| `assert-not-contains <text>`                                          | Assert the terminal grid does NOT contain the given substring                         |
| `screendump <path>`                                                   | Save the framebuffer to a PNG file (captured after render, before present)            |
| `dumprow <row>`                                                       | Print all cells in a terminal row                                                     |
| `dumpcells <row> <col_start> <col_end>`                               | Print cells in the given range with codepoint, width, attributes, and fg/bg colors    |
| `dump-sixel`                                                          | Print the current sixel image state (count and per-image info)                        |
| `mousemove <x> <y>`                                                   | Simulate a mouse move to physical pixel coordinates                                   |
| `resize <cols> <rows>`                                                | Resize terminal grid to given columns/rows                                            |
| `winsize <width> <height>`                                            | Set window pixel size                                                                 |
| `panel <id> <col> <row> <cols> <rows> "title" "body" [level] [flags]` | Show a panel at grid position with title and body                                     |
| `panel_hide <id>`                                                     | Hide panel by ID                                                                      |
| `assert-hover`                                                        | Assert an OSC-8 hyperlink is currently hovered (prints PASS/FAIL)                     |
| `assert-no-hover`                                                     | Assert no OSC-8 hyperlink is currently hovered (prints PASS/FAIL)                     |
| `record-start <dir> [fps]`                                            | Start frame recording to directory at target FPS (default 30), QOI format             |
| `record-stop`                                                         | Stop frame recording and close manifest                                               |
| `quit`                                                                | Request application quit                                                              |

### `send` vs `emit`

There are two separate input paths for scripts:

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

### Mouse and hover commands

`mousemove` simulates a mouse move to the given **physical** pixel coordinates. This is useful for testing hover states (such as OSC-8 hyperlink previews) without a real mouse or window server. Coordinates are in the same physical pixel space the app uses for cell math, so scale them by the content scale if testing on HiDPI displays.

Hover an OSC-8 hyperlink to capture the hover preview:

```
# Run with: portty -S hover_demo.script
wait 0.5
emit \e[2J\e[H
emit \e]8;;https://example.com/some-long-url-path\e\\Hover me\e]8;;\e\\\r\n
wait 0.5
# Hover over the "Hover me" link (adjust x/y to match your layout)
mousemove 200 40
wait 1.0
assert-hover
screendump /tmp/portty-hover.png
wait 5.0
quit
```

`assert-hover` and `assert-no-hover` check whether an OSC-8 hyperlink is currently hovered, printing PASS or FAIL. These are useful for regression testing hover state management.

### Frame Recording

`record-start` captures the framebuffer after each render at a target frame rate (default 30 FPS). Frames are written as QOI files (`frame_000001.qoi`, `frame_000002.qoi`, ...) — QOI encodes 15-33x faster than PNG with only ~20-40% larger files, enabling real-time capture at full frame rate on both backends.

A `frames.csv` manifest is written alongside the images with per-frame timestamps:

```csv
index,timestamp,filename
1,0.033,frame_000001.qoi
2,0.066,frame_000002.qoi
```

The timestamp is captured before each frame write, so gaps in timestamps indicate dropped frames. To detect drops:

```bash
awk -F, 'NR>1 {print $2}' frames.csv | \
  awk 'NR>1 {diff=$1-prev; if(diff>0.05) print "drop before frame "NR": "diff" sec gap"; prev=$1} NR==1{prev=$1}'
```

Example recording script:

```
record-start /tmp/frames 30
wait 5.0
record-stop
quit
```

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
font = Cascadia Code-14
geometry = 120x40
hinting = light
verbose = false
word_chars = abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/?#[]@!$&'()*+,;=%~
scrollback = 1000
shell = /bin/bash
text_composition_strategy = kitty
ambiguous_wide = false
```

### Available Keys

All keys are optional. Keys appear directly at the top level.

| Key                         | Values                                                | Default            | Description                                                                                                                                          |
| --------------------------- | ----------------------------------------------------- | ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `font`                      | Fontconfig pattern                                    | `monospace`        | Font family and size (e.g. `monospace-16`)                                                                                                           |
| `geometry`                  | `COLSxROWS`                                           | `80x24`            | Initial terminal dimensions                                                                                                                          |
| `hinting`                   | `none`, `light`, `normal`, `mono`                     | `light`            | FreeType hinting mode                                                                                                                                |
| `verbose`                   | `true`/`false`                                        | `false`            | Debug output                                                                                                                                         |
| `word_chars`                | Character string                                      | `A-Za-z0-9_-./~`   | Characters treated as word for double-click                                                                                                          |
| `scrollback`                | Non-negative integer                                  | `1000`             | Scrollback history lines (0 disables)                                                                                                                |
| `shell`                     | Shell path (optionally with args)                     | `$SHELL`/`COMSPEC` | Default shell when no `--` args given (e.g. `/bin/bash --norc`); falls back to `$SHELL` then `/bin/sh` on Unix, `$COMSPEC` then `cmd.exe` on Windows |
| `text_composition_strategy` | `kitty`, `neutral`/`correct`, or `<gamma> <contrast>` | `neutral`          | Glyph-weight curve on top of linear-light blending, luminance-aware on the GPU renderer (`kitty` = gamma 1.7 / contrast 30)                          |
| `ambiguous_wide`            | `true`/`false`                                        | `false`            | Render East Asian Ambiguous-width characters as 2 cells (opt-in; matches xterm `cjk` locale behavior)                                                |

Boolean values accept `true`/`false`, `yes`/`no`, or `1`/`0`. Lines starting with `#` or `;` are comments.

## Emoji Rendering Paradigm

portty uses [coffer](https://github.com/chestso/coffer) for terminal emulation and cell management, including UAX #11 + UAX #29 grapheme-cluster width computation. See coffer's README for details on width handling, continuation cells, and grapheme storage.

portty's renderer deliberately breaks the Unicode rule that VS15 (text presentation selector) forces text-only rendering. The guiding principle is **beauty over spec**: if the color emoji font has a glyph for a codepoint, portty prefers it — even when VS15 is present. VS15 only narrows the cell width to 1 column; it does not block access to the color emoji font. This is the same treatment already given to ambiguous-width symbols: they route to the color emoji font (if it carries the glyph) at 1-cell width. VS15 is just the explicit form of that.

**Coverage-aware emoji selection.** A codepoint routes to the color emoji font whenever the emoji font carries the glyph, regardless of VS15. Regional indicators are always emoji. When VS15 (U+FE0E) is present, the cell width is 1 column (as computed by coffer), but routing to the color emoji font still happens if the glyph is available. Plain text-default emoji codepoints (Dingbats, Misc Symbols) without VS16 stay on the text font when the emoji font lacks the glyph — they are not silently downgraded to a missing-emoji glyph or routed through the emoji path only to fall back after shaping fails.

**Symbol-class glyphs preserve font design.** Dingbats, Misc Symbols, Misc Technical, Geometric Shapes, Supplemental Arrows-B, and Misc Symbols & Arrows rendered through a text font keep their natural design width and are centered horizontally in the cell (FreeType's left bearing is intentionally discarded because mono fonts often calibrate it to an oversized advance — e.g. Noto Sans Mono ✶ has advance 1.2×em with the ink centered in that wider advance). They sit on the typographic baseline vertically. No downscale is applied: portty never resamples non-emoji glyphs, which would visibly destroy crispness. Horizontal overhang into neighbor cells is allowed and handled cleanly by the two-pass row draw (backgrounds first, then glyphs).

**Nerd Fonts render inline as icon-class text glyphs.** NF codepoints live in the BMP Private Use Area and Supplementary PUA (U+E000–U+F8FF, U+F0000–U+FFFFD, U+100000–U+10FFFD). NF glyphs are sized like text — the font's outline data renders them at native cap-height, the same as a letter `A`. They take the plain text rendering path: no downscale, no min-fit clamp, FreeType's `bitmap_left` honored on the typographic baseline, horizontal overhang into neighbor cells absorbed by the two-pass row draw. Nerd Fonts v2 codepoints in U+F900–U+FAFF are translated to their v3 SPUA equivalents before shaping. Note the cell-width difference: coffer classifies the v2 range U+F900–U+FAFF as double-width (CJK Compatibility Ideographs), so a v2 codepoint occupies 2 cells in the grid, while the v3 SPUA range is ambiguous-width and occupies 1 cell by default (2 only with `ambiguous_wide`). The translation happens in the renderer after coffer has already assigned the cell width, so a v2 codepoint keeps its 2-cell slot even though the translated v3 glyph renders at 1-cell metrics. The icons are color-modulated by the foreground SGR color (NF glyphs are plain outlines, not COLR color emoji), unlike pre-modulated emoji.

**Renderer policy lives in `rend_common`.** The three decisions above (emoji coverage routing, downscale gating, NF v2→v3, symbol-class horizontal centering) are factored into three shared helpers — `rend_resolve_cell_style`, `rend_plan_glyph`, `rend_apply_glyph_layout`, plus `rend_downscale_bitmap`. The renderer (`rend_sdl3.c`) uses these and diverges from them only in atlas insertion and final placement. The invariant these helpers enforce: only the color-emoji pipeline ever resamples; everything else renders at FreeType's native metrics so crispness is preserved.

## Terminfo

portty ships a single terminfo entry under three aliases — `portty-vty-256color`, `portty-256color`, and `portty`. The default `TERM` is `portty-vty-256color`; the alternate names exist for users who prefer to set them.

The capability definitions live in coffer (`data/coffer.ti`, installed as `coffer-vty-256color`). portty's `data/portty.ti` is a two-line shim that `use=`s `coffer-vty-256color` and only contributes the portty aliases. coffer is the VT engine that emits and parses these sequences, so capabilities are defined once there and inherited by portty — no duplicate entry to drift out of sync. The monorepo build installs coffer into the same `$(datadir)/terminfo` before portty, so `tic` resolves the `use=` at install time. (A standalone portty build already has coffer installed as its pkg-config dependency, so resolution works there too.)

`setaf`/`setab` are inherited unchanged from `xterm-256color` (via `coffer.ti`'s own `use=xterm-256color`), so the entry's capability strings stay within the restricted operator subset that Haskell `vty-unix`'s terminfo parser accepts. Truecolor is signalled via the `Tc` flag (which emacs, tmux, vte, alacritty, kitty, ghostty, and most modern TUIs honor) and via `COLORTERM=truecolor` for apps that read the env var directly. Extension caps added on top of `xterm-256color`: `Tc` (truecolor), `hs`/`tsl`/`fsl`/`dsl` (status line / window title), `Smulx` (extended underline styles), `Setulc` (underline color), `Sync` (synchronized output, DEC mode 2026).

The `RGB` flag is deliberately **not** advertised: its ncurses contract is "feed packed 24-bit ints to `setaf` and it'll DTRT," which would require a custom `setaf` outside the vty-unix parser subset. Truecolor consumers are expected to use `Tc` or `COLORTERM`.

On Linux, the entry is compiled and installed automatically by `make install` via `tic`. On macOS, run `sh install-terminfo.sh` to compile and install the entry natively. The child shell's `TERMINFO_DIRS` includes both `~/.local/share/terminfo` and `~/.terminfo` so user-installed entries are found without system-wide installation.

If you SSH to a remote host that lacks the entry, the remote shell will fall back to a generic terminal type. You can copy the compiled entry to the remote host:

```bash
infocmp portty-vty-256color | ssh remote-host 'tic -x -'
```

## GNU Emacs

portty ships `data/portty.el`, installed by `make install` to `$(datadir)/emacs/site-lisp/term/portty.el` (typically `~/.local/share/emacs/site-lisp/term/portty.el`). This directory is in Emacs's default `load-path`.

### Automatic loading

Emacs loads terminal-specific initialization files at startup via `tty-run-terminal-initialization`. It searches for `term/<TERM>.el` in `load-path`, stripping hyphenated suffixes until it finds a match. With `TERM=portty-vty-256color`:

1. Looks for `term/portty-vty-256color.el` — not found
2. Strips `-vty-256color`, looks for `term/portty.el` — **found**

So `portty.el` loads automatically. No user configuration or `init.el` changes are needed. When loaded, it:

1. Loads Emacs's built-in `term/xterm.el` — reuses the entire xterm terminal initialization (keymaps, 256-color support, bracketed paste, focus tracking)
2. Explicitly enables `modifyOtherKeys` and `setSelection` via `xterm-extra-capabilities` — this avoids xterm version detection (which would not trigger for portty's `TERM`) and directly activates both features
3. Enables `xterm-mouse-mode` so the mouse works in `emacs -nw`

### Mouse support in `emacs -nw`

`portty.el` turns on `xterm-mouse-mode` during terminal initialization, so Emacs sends `DECSET 1000` (`?1000h`), `DECSET 1003` (`?1003h`), and the SGR mouse extension `DECSET 1006` (`?1006h`) on startup. portty parses those modes via coffer and forwards mouse events (including motion) to Emacs. No user configuration is required.

This is needed because Emacs does not enable xterm mouse tracking just because `TERM` is xterm-compatible. Emacs 31+ auto-enables `xterm-mouse-mode` only for a hardcoded allowlist of terminal names after querying XTVERSION (`Konsole`, `VTE`, `WezTerm`, `iTerm2`, `kitty`, `foot`), and portty isn't in that list. Emacs 30 has no XTVERSION-based auto-enable path at all. The terminal init file is the supported hook that works across both, and it runs for any Emacs version that loads `term/portty.el`.

On Emacs 31+, `portty.el` honors an explicit opt-out: if `xterm-mouse-mode` has already been called (for example `(xterm-mouse-mode -1)` in your init file), the auto-enable is skipped. On Emacs 30 that variable does not exist, so the mode is enabled unconditionally. Emacs 30 users who want mouse events to stay with the terminal instead of Emacs can opt out with:

```elisp
(add-hook 'tty-setup-hook (lambda () (xterm-mouse-mode -1)))
```

### Verifying it loaded

To confirm the terminal init ran, check that `xterm-extra-capabilities` is bound (it's defined by `term/xterm.el`, which `portty.el` loads):

```elisp
(boundp 'xterm-extra-capabilities)  ; => t
```

### Doom Emacs

Doom's `early-init.el` defers `tty-run-terminal-initialization` to `window-setup-hook` (a performance optimization to avoid blocking startup). This means `portty.el` and its CSI-u keymap entries are not installed until after Emacs finishes starting. Key presses during startup will not be decoded. Once `window-setup-hook` fires, everything works the same as vanilla Emacs.

### Ctrl + non-letter keys (e.g. `Ctrl+Shift+Alt+%`)

The legacy terminal encoding has no way to represent Ctrl combined with keys that lack a traditional ASCII control code (letters `A`–`Z`, `a`–`z`, `@`–`_`, space, `?`). For keys like `%`, `1`, `,`, `.`, `;`, etc., there is simply no corresponding C0 control character. Historically, many terminals (including xterm and VTE) silently swallowed these combinations — the keypress produced nothing, and the Ctrl modifier was lost.

portty (via coffer) follows kitty's convention: for Ctrl + keys with no traditional mapping, it emits a **CSI-u** escape sequence (`ESC [ <codepoint> ; <modifiers> u`) even in legacy mode (i.e. when the kitty keyboard protocol is not explicitly active). This preserves the full modifier state so applications can distinguish e.g. `Ctrl+%` from `%`.

Emacs's `term/xterm.el` already installs `input-decode-map` entries for CSI-u sequences covering a range of non-letter keys (`,`, `.`, `/`, `;`, `=`, `0`–`9`, `!`–`+`, `:`, `<`, `>`, `?`, `\`, tab, return) across multiple modifier combinations (Shift, Ctrl, Alt, and their combinations). Because `portty.el` loads `term/xterm.el`, these bindings are active by default — **no additional packages or configuration are needed** for Emacs to decode CSI-u sequences for these keys.

For example, `Ctrl+Shift+Alt+%` is emitted as `ESC[37;8u` (37 = codepoint for `%`, 8 = 1 + shift(1) + alt(2) + ctrl(4)), which Emacs decodes to `C-M-S-%`.

### Kitty keyboard protocol

When an application explicitly enables the kitty keyboard protocol (Disambiguate or Report-all flags), portty routes all Ctrl and Alt + key combinations through CSI-u, including those with traditional control codes. This is the unambiguous path — `Ctrl+A` becomes `ESC[97;5u` instead of `0x01`, so the application can recover the Shift modifier and distinguish `Ctrl+A` from a literal `0x01` byte.

## Dependencies

All platforms:

- coffer (VT engine, consumed via `pkg-config coffer >= 0.2.3`; source at https://github.com/chestso/coffer)
- SDL3
- freetype2 (>= 2.13 for COLR v1 APIs)
- harfbuzz (>= 2.0)
- libpng

The SDL3 backend uses SDL's GPU renderer for gamma-correct linear-light blending, so a working GPU backend is required **at runtime**: Vulkan on Linux, Direct3D 12 on Windows, Metal on macOS.

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

The default 16-color ANSI palette is [Dracula](https://draculatheme.com) (per the [official spec](https://draculatheme.com/spec)), provided by coffer. The default terminal background is pitch-black (`#000000`), separate from the palette's AnsiBlack.

## License

MIT — see [COPYING](COPYING) for details.
