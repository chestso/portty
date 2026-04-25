# bloom-vt — follow-ups

Living checklist for the `feature/bloom-vt` branch. Promote items to PRs as
they get worked on. Order is roughly priority, not strict dependency.

## Headless interactive testing infrastructure

- **A. `tests/test_bvt_pty.c`** — engine-only PTY harness. Spawns a child
  shell on a real PTY using `pty_create()` / `pty_read()` / `pty_write()` /
  `pty_destroy()` from `src/pty.c`, pipes raw output into `bvt_input_write()`,
  asserts on `bvt_get_cell` / `bvt_get_cursor` / `bvt_get_scrollback_lines` /
  `bvt_get_title`. No SDL, no FreeType, no atlas. Wire under the existing
  `if !HOST_WINDOWS` block in `tests/Makefile.am` next to `test_pty_pause`.
  Goes into `make check`. Test scenarios:
  - `echo hello` — basic stdout
  - `printf '\033[31mred\033[0m'` — SGR
  - `tput clear` / `tput cup 5 10` — cursor moves via terminfo
  - `printf <ZWJ family bytes>` — arbitrary-length clusters
  - `echo 你好` — CJK
  - `tput smcup; tput rmcup` — altscreen swap
  - `for i in {1..50}; do echo line$i; done` — scrollback
  - bracketed paste begin/end via `printf '\033[?2004h'`

- **B. `bloom-terminal -P --exec CMD [--wait MS]`** — visual A/B harness.
  Extend `src/png_mode.c`: when `--exec` is set, fork+exec CMD via
  `pty_create()`, drain into the chosen backend until child exits or
  `--wait MS` (default 200) elapses, then render to PNG and exit. Geometry
  from `-g` (default 80x24). Lets us A/B `glow README.md`, `vim`, `htop`,
  vttest snapshots etc. against libvterm with byte-identical comparison.

A goes in first (gating, in CI). B is opt-in for sweeps.

## Soak test status (PNG mode A/B via `-P --exec`)

**Byte-identical to libvterm** (acceptance: pass without further work):
- echo, ls, uname, env probes, multi-line for loops
- SGR fg 30-37, tput setaf/setab, tput cup, ED/EL (`\033[2J\033[H`)
- bold, italic, curly underline (`4:3`), dashed underline (`4:5`)
- truecolor `38;2;R;G;B`
- box-drawing characters `┌─┬─┐`
- DEC special graphics line drawing (`\033(0lqk\nx x\nmqj\033(B`)
- CJK ideographs (你好世界) — width=2 cells with width=0 continuation
- VS16 ⚠️
- 100-line scrollback push
- 200-char line wrap
- CR overprint (`AAAA\rB`)
- 8-step tabs
- Scrolling region (DECSTBM `\033[2;5r`), IL/DL, ECH
- glow markdown render of `#` / `##` / fenced code
- vim --version, bat plain, tmux ls, htop -d 5 (1s timeout), btop -p 0
  (1s timeout), chafa --format=symbols, ls --color, ps aux, date, uname

**Accepted divergences** (bvt is correct; libvterm path is non-standard
and goes away in step 15):
- 256-color cube — bvt: xterm-standard `0/95/135/175/215/255`;
  libvterm: naive `0/51/102/153/204/255`.
- Plain `\033[4m` — bvt: single underline (per ECMA-48);
  libvterm path at `term_vt.c:307`: deliberately maps to dotted.
- 7-cp ZWJ family (👨‍👩‍👧‍👦) — bvt stores the full cluster;
  libvterm caps at 6 codepoints.
- Single RI flag (🇩🇰), skin-tone modifiers (👋🏽) — bvt honors UAX
  cluster width; libvterm reports per-codepoint widths.

**Open** (low-priority edge cases, may resolve once libvterm is gone):
- `printf 'A\033[s\nB\033[uC'` (DECSC/DECRC across newline) — small
  PNG diff (18 bytes). Likely a difference in how cursor row tracks
  through LF when a save was taken before LF.
- `printf 'A\tB\tC\033[3g\rX\tY'` (CSI 3 g + tab after clear) — bvt
  vs libvterm differ on the post-clear `\t`. ECMA-48: with no tabs,
  HT advances by one. Verify bvt's HT handling matches xterm.

**Non-deterministic** (PNG cmp not meaningful — use harness A
assertions instead):
- htop / btop / live monitors

## Manual interactive sweep (workstation with display only)

Run the binary directly with `BLOOM_TERMINAL_VT=bloomvt`:

- `vim`, `nvim`, `emacs -nw`
- `htop`, `btop`, `lazygit`, `claude-code`
- `bat src/term.c`, `cat unicode-demo.txt`
- `chafa --format sixel image.png` (DCS sixel passthrough)
- mouse scroll + click + drag selection in tmux
- window resize reflow
- altscreen swap via vim

## Step 15 — Default flip + libvterm removal ✅ done (d5e62d8 + aae22d7)

bloom-vt is the default and the only backend. `src/term_vt.c` (1372
LOC), the libvterm `pkg-config` check, the `BLOOM_TERMINAL_VT`
env-var dispatch, the `ext_grid` SGR rewriting, the mingw64 +
osxcross libvterm cross-compile blocks in `build.sh`, and the
"--reflow UNSTABLE" warning are gone.

## Step 16 — VS16 shift hack removal ✅ done (7225bd7)

`TerminalRowIter` is a plain `vt_col += cell.width` walk;
`terminal_cell_presentation_width` is deleted;
`terminal_vt_col_to_vis_col` / `terminal_vis_col_to_vt_col` are
identity wrappers retained for source compatibility. CLAUDE.md
"Emoji Width Paradigm" rewritten.

## Step 17 — Renderer migration (cell.chars → grapheme accessor) ✅ done

The 6-codepoint-per-cell cap is gone. `TerminalCell` now carries
`(uint32_t cp, uint32_t grapheme_id)` instead of `chars[6]`; the full
sequence is fetched by the new `terminal_cell_get_grapheme(term,
unified_row, col, out, cap)` accessor (vtable hook on `TerminalBackend`).
The bvt backend implements it via `bvt_cell_get_grapheme()`. Updated
call-sites: `src/rend_sdl3.c` (glyph lookup + PNG trim scan),
`src/term.c` (selection char_class + clipboard text extraction). Coverage
in `tests/test_term_bvt.c::test_long_cluster_survives_accessor`
exercising the 7-cp ZWJ family 👨‍👩‍👧‍👦.

## Step 18 — Extract to `/home/thomasc/git/bloom-vt`

Once everything above is stable, lift `src/bloom_vt/` into its own repo:

- Autotools layout matching the other `bloom-*` projects.
- `bloom-vt.pc` for pkg-config consumers.
- README, license (BSD/MIT — match libvterm's spirit), top-level
  `build.sh`.
- Update `bloom-terminal`'s `configure.ac` to `pkg-config bloom-vt`.

## Out of scope for v1 (defer indefinitely)

- OSC 8 hyperlink rendering (parse + store id only; renderer support
  later).
- Kitty graphics protocol.
- Synchronized output (mode 2026) — easy to add once parser is solid.
- Image-cell underlay protocol — sixel scrolling is already handled by
  the existing sixel layer.
- Right-to-left text shaping — handled at HarfBuzz, not VT.

## Resolved during soak

- ~~DEC special graphics (line drawing) was unimplemented — `\033(0`
  designation was silently swallowed and `lqk\nx x\nmqj` rendered as
  literal ASCII instead of `┌─┐ │ │ └─┘`~~ — added charset slot
  tracking on BvtTerm, ESC ( ) * + dispatch, SO / SI shifts, and the
  standard VT100 0x5F..0x7E translation table at print time. Coverage
  in `test_bvt_parser.c::test_dec_graphics_g0` and `::test_dec_graphics_si_so`.
- ~~Wrap-aware selection broke at the visible/scrollback boundary, and
  resize never reflowed because reflow was off by default~~ — backend
  adapter now translates wrapline semantics across the boundary and
  flips reflow on by default for bvt. Tests in `test_term_bvt.c`. See
  cae5f69.
- ~~Ctrl+letter key combos didn't work in bloom-vt — Ctrl+C produced raw
  `c` instead of 0x03~~ — `bvt_send_text` now applies the standard
  Ctrl-byte transformation (Ctrl+@ → 0x00, Ctrl+A..Z → 0x01..0x1A, etc.)
  before forwarding. Fixed in 2de4465.
- ~~cf wiped the screen on launch — the brick (Haskell vty) inline TUI
  drew at row 0 instead of preserving prompts above~~ — bvt was missing
  DECOM (origin mode 6) entirely, and DECSTBM accepted the degenerate
  `CSI 1;1 r` brick emits during setup. Both fixed; see 04f4854.
  Repro lives in `tests/test_bvt_pty.c::test_cf_brick_inline_preserves_history`
  and unit coverage in `tests/test_bvt_parser.c::test_decom_cup` /
  `::test_decstbm_invalid_rejected` / `::test_cf_byte_replay`.

## Resolved during scaffolding (kept here for context)

- ~~`get_cell` / `get_dimensions` / `get_scrollback_cell` returned `0/1`
  instead of `-1/0`~~ — fixed in `src/term_bvt.c`; renderer was treating
  every cell as missing and PNGs came out 1 cell wide.
- ~~PNG mode hardcoded the libvterm backend~~ — `src/png_mode.c` now
  honors `BLOOM_TERMINAL_VT` so A/B comparison works.
- ~~Reverse video (`\033[7m … \033[27m`) PNG diverged from libvterm~~ —
  `term_bvt.c::convert_cell` now pre-swaps fg/bg and clears
  `bg.is_default`, matching the libvterm backend. Byte-identical PNG.
- ~~`test_bvt_parser` link error against `bvt_palette_lookup`~~ — added
  `palette.c` to `tests/Makefile.am`.
- ~~Reflow shrink put real content into scrollback because trailing
  empty rows produced empty logical lines~~ — `reflow.c` now tracks
  `cursor_line` and trims trailing empty logical lines that don't host
  the cursor.
- ~~`test_utf8_replacement` failed: bare 0x80 treated as C1 control~~ —
  removed C1 anywhere transition in the parser (xterm UTF-8 mode).
- ~~`test_style_intern_dedup` failed: pen color_flags was 0 initially,
  then set to defaults after first SGR reset~~ — initialize pen with
  `BVT_COLOR_DEFAULT_FG | _BG | _UL` in `bvt_new`.
- ~~256-color SGR (`\033[48;5;220m`) PNG diverged from libvterm~~ —
  *accepted divergence*. libvterm uses a naive 51-step ramp
  `(0, 51, 102, 153, 204, 255)` and produces `#FFCC00` for color 220.
  bvt uses the xterm-standard ramp `(0, 95, 135, 175, 215, 255)` and
  produces `#FFD700`, matching xterm / iTerm / foot / Alacritty. Document
  in CLAUDE.md when default flips.
- ~~Plain `\033[4m` underline diverged from libvterm~~ — *accepted
  divergence*. bloom-terminal's libvterm wrapper at `src/term_vt.c:307`
  deliberately rewrites plain SGR 4 to dotted (`ext_ul_style = 4`).
  Standard ECMA-48 SGR 4 = single underline. bvt follows the standard
  and renders `\033[4m` as single underline, matching vim, less, man,
  etc. The divergence vanishes once libvterm is removed (step 15).
