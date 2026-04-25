# bloom-vt — follow-ups

Living checklist for the `feature/bloom-vt` branch. Promote items to PRs as
they get worked on. Order is roughly priority, not strict dependency.

## Open items (resolve before flipping default)

- **Cluster truncation at the renderer boundary** — `src/term_bvt.c::convert_cell`
  copies bvt clusters into `dst->chars[TERM_MAX_CHARS_PER_CELL]` (= 6) to fit
  the existing `TerminalCell` struct. bvt's grid stores arbitrary-length
  clusters correctly, but anything > 6 codepoints (e.g. 7-cp ZWJ family
  👨‍👩‍👧‍👦, very long combining-mark sequences) loses the tail at the
  renderer boundary. See "Renderer migration" below.

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

## Soak test (after harness B lands)

Drive each through both backends, diff PNGs:

- `vim`, `nvim`, `emacs -nw`
- `htop`, `btop`, `lazygit`, `claude-code`
- `glow README.md`, `bat src/term.c`, `cat unicode-demo.txt`
- `tput`, `tic -L`, `infocmp`
- `chafa --format sixel image.png` (DCS sixel passthrough)
- vttest visual diff against libvterm

Acceptance: byte-identical for non-emoji, structurally-correct
bvt-only divergence accepted with a one-line note in CLAUDE.md.

Live interactive sweep (workstation only): mouse scroll + click + drag
selection in tmux, window resize reflow, altscreen swap via vim.

## Step 15 — Default flip + libvterm removal

Once soak passes:

1. Flip default in `src/main.c` and `src/png_mode.c` to
   `&terminal_backend_bvt`.
2. Delete `src/term_vt.c` and the libvterm `pkg-config` line in
   `configure.ac`.
3. Remove the `BLOOM_TERMINAL_VT` env-var dispatch.
4. Drop the `ext_grid` SGR rewriting (it lives in `term_vt.c`, dies with it).
5. Remove the libvterm stanza from `src/Makefile.am`.

## Step 16 — VS16 shift hack removal (blocked on step 15)

Cannot ship while libvterm is still wired up — the iterator's shift-vs-absorb
logic at `src/term.c:611-646` is the only thing widening libvterm's
width=1 VS16 cells without dropping the trailing space in naive output
(cat / glow / bat). Once `term_vt.c` is gone, all cells carry the correct
UAX-derived width.

- Simplify `TerminalRowIter` in `src/term.c:595-646` to a plain
  `vt += cell.width` walk; delete the peek-ahead branch.
- Delete `terminal_cell_presentation_width()` (`src/term.c:584-593`);
  replace its one caller in `src/rend_sdl3.c:2117` with `cell.width`.
- Collapse `terminal_vt_col_to_vis_col` / `terminal_vis_col_to_vt_col`
  (`src/term.c:648-676`) to identity; update the mouse handler at
  `src/main.c:326`.
- Update CLAUDE.md "Emoji Width Paradigm" section.

## Step 17 — Renderer migration (cell.chars → grapheme accessor)

Lifts the 6-codepoint-per-cell cap that bvt currently honors only for
parity with `TerminalCell.chars[TERM_MAX_CHARS_PER_CELL]`.

- Replace `chars[6]` on `TerminalCell` with a `uint32_t grapheme_id` (and
  keep `cp` for the single-codepoint fast path).
- Add `terminal_cell_get_grapheme(term, cell, out, cap)` that calls
  `bvt_cell_get_grapheme()` for the bvt path.
- Renderer's font-shaping path consumes the codepoint array via this
  accessor — `src/rend_sdl3.c` glyph lookup keys move from `cell.chars[]`
  to `(cell.cp, cell.grapheme_id)`.

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
