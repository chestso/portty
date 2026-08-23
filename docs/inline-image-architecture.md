# Inline Image Architecture: Status and Follow-ups

> **Status: implemented end-to-end.** This document supersedes the original
> design doc. It records what shipped, how each system now uses the shared
> store, and the cleanup work that remains.

## Summary

Four image/animation systems in coffer now share one storage, memory, and
grid-maintenance layer (`image_store.c`). Each keeps only its own transport
and decoding logic.

| System | Transport      | Alpha            | Placements    | ConPTY            | Status |
| ------ | -------------- | ---------------- | ------------- | ----------------- | ------ |
| Sixel  | DCS            | Binary (0/255)   | 1:1           | Needs bundled DLL | Done   |
| Lottie | APC (OSC 5555) | Full 8-bit       | 1:N           | OSC carrier       | Done   |
| iTerm2 | OSC 1337       | Full 8-bit (PNG) | 1:1           | Yes (OSC)         | Done   |
| Kitty  | APC (OSC 5555) | Full 8-bit       | 1:N + z-index | OSC carrier       | Done   |

## What shipped

### Shared image store (`coffer/src/image_store.c` + `.h`)

The common infrastructure, extracted from sixel and lottie:

- **`CfrImg`** — one stored image: `id`, `version`, `layer`, `source`
  (`IMG_SRC_*`), `abs_line`, `col`, `w`/`h`, `rows_tall`/`cols_wide`,
  `rgba`, `cap`.
- **`CfrPlacement`** — one placement for the 1:N model (lottie, kitty):
  `id`, `image_id`, `abs_line`, `col`, `rows`/`cols`, `layer`,
  `opacity_x256`, `z_index`, `src_x/y/w/h`, `pix_offset_x/y`,
  `cell_off_x/y`, `parent_img`/`parent_place`.
- **`CfrImgStore`** — tier-1 image records + tier-1b placement records +
  tier-2 best-fit free-list buffer pool + live-byte budget + query scratch
  arrays.

Key operations:

- `cfr_img_add` — 1:1 image at the cursor (sixel, iTerm2).
- `cfr_img_add_named` — id-keyed image (lottie, kitty transmit).
- `cfr_img_blank_named` / `cfr_img_mark_dirty` — raster-in-place helpers for
  lottie (ThorVG writes directly into the store's buffer).
- `cfr_img_add_placement` / `cfr_img_get_placements` /
  `cfr_img_get_placements_for` — 1:N placement management and query.
- `cfr_img_replace` / `cfr_img_find_at` / `cfr_img_find_by_id` /
  `cfr_img_remove`.
- `cfr_img_note_scroll` / `cfr_img_clear_display_rows` / `cfr_img_clear_all`
  — placement-aware grid maintenance. A 1:N image is culled/cleared by its
  placements' `abs_line`, not the record's (which is `0` for named images).

`img_advance_cursor` is shared; sixel's DECSDM/8452 modes are checked inside
it, and non-sixel callers get default cursor-below behavior.

### Per-system integration

**Sixel** — `sixel.c` keeps the DCS decoder (canvas, palette, bands, color
registers) and calls `cfr_img_add(..., IMG_SRC_SIXEL)` on finish. No
placements; the image record _is_ the placement.

**Lottie** — `lottie.c` keeps the JSON parser, ThorVG rasterizer, frame
timing, and fit/scale logic. Pixel buffers and placements now live in the
shared store:

- `LtRec` drops `rgba`, `rgba_cap`, `placements`, `placement_count`; it keeps
  `json`/arena, `tvg_*`, design/px dims, and playback fields.
- `cfr_img_blank_named`+`cfr_img_mark_dirty` back the raster target;
  `cfr_img_add_placement` creates placements.
- `cfr_lottie_note_scroll`/`cfr_lottie_clear_display_rows` now delegate to
  `cfr_img_*` on the shared store, then reconcile the parallel `LtRec` array.

**iTerm2** — `osc_1337.c` parses `File=`/multipart, base64-decodes, decodes the
PNG via stb_image, then `cfr_img_add(..., IMG_SRC_ITERM)`.

**Kitty** — `apc.c` routes `G` → `graphics.c`, else → lottie. `graphics.c`
handles `a=t/T`, `a=p`, `a=q`, `a=d`, `a=f`, `a=a`, `a=c`, chunked transfer
(`m=1/0`), zlib (`o=z` via `cfr_zlib_decompress`), cursor advance (`c=/r=`),
virtual placements (`U=1`), and relative placements (`P=/Q=` with `x=/y=`
offsets). Responses leave via OSC 5556 on Windows (ConPTY strips APC).

### Decoding

`image_decode.c` provides:

- `cfr_image_decode` — PNG/JPEG/BMP/GIF/TGA → RGBA (stb_image).
- `cfr_zlib_decompress` — RFC 1950 inflate (stb_image's public-domain zlib),
  used by kitty `o=z` payloads.

### Portty rendering (`rend_sdl3.c`)

- `sixel_cache` → `image_cache`; `render_sixel_images` → `render_images`.
- `render_images(data, term, negative_z)` runs in two passes: negative-z
  placements behind text, non-negative (sixel/iTerm2/lottie default `z=0`)
  on top.
- Kitty placements render via `terminal_get_image_placements` with source
  rects.
- Lottie still uses its own texture cache + animation tick
  (`render_lottie_layer`).

### Tests

Headless TDD throughout:

- coffer: `test_cfr_image_store` (22), `test_cfr_lottie` (34),
  `test_cfr_graphics` (21), `test_cfr_osc_1337` (9), plus sixel.
- portty: `test_kitty` (kitty bridge), `test_osc1337` (iTerm2 bridge), and
  the existing sixel/lottie suites. Full `make check` is green in both repos.

## Follow-ups

Items below are cleanup/refinement, not functional gaps.

### Remove backward-compat aliases and legacy names

The original migration kept several aliases to avoid breaking portty. Most are
now folded into canonical names; the remainder are tagged below as still open.

**Done:**

- **`CfrSixel` → `CfrImage`.** `CfrImage` is now the sole struct name; the
  duplicated `typedef` in `coffer.h`/`image_store.h` and the `CfrSixel` alias
  are removed.
- **`cfr_get_sixels` → `cfr_get_images`.** Renamed across coffer and portty.
- **`terminal_get_sixels` / `get_sixels` → `terminal_get_images` /
  `get_images`.** Renamed across portty's `TerminalBackend`.
- **`cfr_sixel_note_scroll`/`cfr_sixel_clear_display_rows`/`cfr_sixel_clear_all`**
  thin wrappers removed; `print.c`/`modes.c`/`term.c` now call `cfr_img_*`
  directly.

**Still open:**

- **`CfrLottiePlacement` / `cfr_get_lottie_placements`** remain a parallel to
  `CfrImagePlacement` / `cfr_get_image_placements_for`. Decide whether lottie
  should reuse the generic placement query and drop its lottie-specific
  surface (or make `CfrLottiePlacement` a true prefix of
  `CfrImagePlacement`).
- **Lottie's `cfr_lottie_note_scroll`/`cfr_lottie_clear_display_rows`**
  reconcile the parallel `LtRec` array and still re-invoke `cfr_img_*` from
  `print.c`; see the reconciliation follow-up below.

Rationale: one canonical name per concept. The `source` field already
distinguishes protocols; the struct/function name doesn't need to.

### `CfrPlacement` field cleanup

Several placement fields are kitty-only but live on the shared struct with the
"0 for others" convention (`src_x/y/w/h`, `pix_offset_x/y`, `cell_off_x/y`,
`parent_*`). Acceptable, but a follow-up could document which fields each
source actually populates, or move the kitty-only fields into a sidecar
struct to keep the shared record lean.

### Lottie texture cache unification

Lottie still renders via `render_lottie_layer` with its own texture cache and
animation tick, separate from the unified `image_cache`. The original design
notes this is optional. If merged, lottie frames render through
`render_images` and the `lottie_cache` disappears.

### Scroll/clear reconciliation duplication

`cfr_lottie_note_scroll`/`cfr_lottie_clear_display_rows` reconcile the
parallel `LtRec` array after `cfr_img_*` culls the shared store. This
two-collection dance is the last structural artifact of the migration. A
cleaner model would give the store an owning callback or let lottie rebuild
its index from the store on demand, eliminating the reconcile pass.

### Naming audit for `sixel_abs_top`

`CfrTerm` still names the image scroll baseline `sixel_abs_top`, even though
lottie, iTerm2, and kitty all anchor to it now. Rename to a protocol-neutral
name (e.g. `img_abs_top` / `image_abs_top`) for consistency.

### Diagnostics

`diag.c` already lists "iTerm2 images" and "kitty graphics" as capabilities;
confirm it doesn't still say "planned" anywhere and that the ConPTY DCS note
is accurate with the bundled `conpty.dll`/`OpenConsole.exe`.
