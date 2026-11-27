# Inline Image Architecture: Status and Follow-ups

> **Status: implemented end-to-end.** This document supersedes the original
> design doc. It records what shipped, how each system now uses the shared
> store, and the cleanup work that remains.

## Summary

Four image/animation systems in coffer now share one storage, memory, and
grid-maintenance layer (`image_store.c`). Each keeps only its own transport
and decoding logic.

| System | Transport               | Alpha            | Placements    | ConPTY                                 | Status |
| ------ | ----------------------- | ---------------- | ------------- | -------------------------------------- | ------ |
| Sixel  | DCS                     | Binary (0/255)   | 1:1           | Bundled OpenConsole (DCS)              | Done   |
| Lottie | APC (OSC 5555 fallback) | Full 8-bit       | 1:N           | Bundled OpenConsole; OSC 5555 fallback | Done   |
| iTerm2 | OSC 1337                | Full 8-bit (PNG) | 1:1           | Yes (OSC)                              | Done   |
| Kitty  | APC (OSC 5555 fallback) | Full 8-bit       | 1:N + z-index | Bundled OpenConsole; OSC 5555 fallback | Done   |

## What shipped

### Shared image store (`coffer/src/image_store.c` + `.h`)

The common infrastructure, extracted from sixel and lottie:

- **`CfrImg`** — one stored graphics object: `id`, `handle`, `version`,
  `layer`, `source` (`IMG_SRC_*`), `abs_line`, `col`, `w`/`h`,
  `rows_tall`/`cols_wide`, `rgba`, `cap`, and protocol-private
  `source_state`. Lottie objects have no engine pixel buffer.
- **`CfrPlacement`** — one placement for the 1:N model (lottie, kitty):
  `id`, `image_id`, `abs_line`, `col`, `rows`/`cols`, `layer`,
  `opacity_x256`, `z_index`, `src_x/y/w/h`, `pix_offset_x/y`,
  `cell_off_x/y`, `parent_img`/`parent_place`.
- **`CfrImgStore`** — tier-1 image records + tier-1b placement records +
  tier-2 best-fit free-list buffer pool + live-byte budget + query scratch
  arrays.

Key operations:

- `cfr_img_add` — 1:1 image at the cursor (sixel, iTerm2).
- `cfr_img_put_named` — id-keyed raster image (kitty transmit).
- `cfr_img_add_object` — id-keyed vector object with protocol-owned state
  (Lottie); the host rasterizes its JSON.
- `cfr_img_add_placement` / `cfr_img_get_placements` /
  `cfr_img_get_placements_for` — 1:N placement management and query.
- `cfr_img_replace` / `cfr_img_find_at` / `cfr_img_find_by_id` /
  `cfr_img_remove`.
- `cfr_img_note_scroll` / `cfr_img_clear_display_rows` / `cfr_img_clear_all`
  — placement-aware grid maintenance. A 1:N image is culled/cleared by its
  placements' `abs_line`, not the record's (which is `0` for named images).

`img_advance_cursor` is shared; sixel's DECSDM/8452 modes are checked inside
it, and it lands the cursor below the image's cell footprint, so the text
that follows a 1:1 image (sixel, iTerm2) is never under its pixels. A kitty
placement has its own landing (`k_advance_cursor`): the cursor moves right
past the image and stays on its last row, kitty's rule.

### Per-system integration

**Sixel** — `sixel.c` keeps the DCS decoder (canvas, palette, bands, color
registers) and calls `cfr_img_add(..., IMG_SRC_SIXEL)` on finish. No
placements; the image record _is_ the placement.

**Lottie** — coffer's `lottie.c` keeps transport, JSON metadata, frame timing,
fit/scale logic and placement lifetime. The store record owns the
protocol-private `LtRec` through `source_state` and destroys it with the
object. `cfr_get_lotties()` exposes the JSON, device-pixel target size, current
frame, stable handle and separate definition/raster revisions. Portty's
`lottie_raster.c` owns ThorVG scenes and current-frame RGBA buffers; no pixels
are stored in coffer. Scroll, erase and screen switches need no parallel-record
reconciliation.

**iTerm2** — `osc_1337.c` parses `File=`/multipart, base64-decodes, decodes the
PNG via stb_image, then `cfr_img_add(..., IMG_SRC_ITERM)`.

**Kitty** — `apc.c` routes `G` → `graphics.c`, else → lottie. `graphics.c`
handles `a=t/T`, `a=p`, `a=q`, `a=d`, `a=f`, `a=a`, `a=c`, chunked transfer
(`m=1/0`), zlib (`o=z` via `cfr_zlib_decompress`), cursor advance (`c=/r=`),
virtual placements (`U=1`), and relative placements (`P=/Q=` with `x=/y=`
offsets). Responses leave via OSC 5556 on Windows (the system conhost strips APC; the bundled OpenConsole passes it).

### Decoding

`image_decode.c` provides:

- `cfr_image_decode` — PNG/JPEG/BMP/GIF/TGA → RGBA (stb_image).
- `cfr_zlib_decompress` — RFC 1950 inflate (stb_image's public-domain zlib),
  used by kitty `o=z` payloads.

### Portty rendering (`rend_sdl3.c`)

- `sixel_cache` → `image_cache`; `render_sixel_images` → `render_images`.
- `render_images(data, term, negative_z)` runs in two passes: negative-z
  kitty placements behind text, non-negative raster images on top.
- Kitty placements render via `terminal_get_image_placements` with source
  rects. Lottie uses the same generic placement array, matched by
  `image_handle`.
- `render_lottie_layer` is deliberately separate from bitmap minification:
  a Lottie size change re-rasterizes the vector scene at the exact device-pixel
  size instead of resampling a cached frame. Its texture cache keys on
  `(handle, raster_rev)`, and its linear-light upload staging is reused.

#### Minification filtering

The SDL 2D renderer samples with one fixed 2x2 tap, whatever the scale
ratio, and its texture API exposes no mip levels. Left to the sampler, any
image shown much smaller than its raster aliases: fine stripes break into
moiré, thin strokes and small text drop out. That hits every path
that shrinks an image — a kitty placement (`c=`/`r=`), an iTerm2
`width=`/`height=` hint, or an image whose box got smaller with the window.

`image_get_texture` therefore takes the bulk of the shrink on the CPU first.
`rend_pick_shrink_factors` chooses integer factors `kx`/`ky` (rounded
`source / destination`, and only when the box is at most half the crop);
`rend_downscale_rgba` does an exact box average in premultiplied,
linear-light space (transparent pixels contribute no colour; averaging sRGB
bytes instead would darken the result relative to the GPU's linear
sampling). The residual the sampler sees is inside [0.8, 1.25], where its
2x2 tap is an adequate filter. Shrinks below 2x and all magnifications stay
with the sampler.

A reduced texture holds only its crop, so the draw uses the whole texture
and ignores the requested source rect; a native texture holds the whole
buffer, so the draw keeps the crop. Variants are cached per image, keyed on
the crop and factors (`ImageCacheEntry`), version-refreshed in place for
animations, and capped per image (`IMAGE_VARIANTS_MAX`) so resizes cannot
grow the cache without bound. `tests/test-image-downscale.sh` guards the
behaviour end to end through an iTerm2 pixel-sized placement.

### Tests

Headless TDD throughout:

- coffer: `test_cfr_image_store` (22), `test_cfr_lottie` (34),
  `test_cfr_graphics`, `test_cfr_osc_1337`, plus sixel.
- portty: `test_kitty`, `test_osc1337`, `test_lottie`, `test_lottie_raster`
  and the sixel suites. The raster test runs without SDL or a GPU. Full `make check` is green in both repos.

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

### `CfrPlacement` field cleanup

Several placement fields are kitty-only but live on the shared struct with the
"0 for others" convention (`src_x/y/w/h`, `pix_offset_x/y`, `cell_off_x/y`,
`parent_*`). Acceptable, but a follow-up could document which fields each
source actually populates, or move the kitty-only fields into a sidecar
struct to keep the shared record lean.

### Naming audit for `sixel_abs_top`

`CfrTerm` still names the image scroll baseline `sixel_abs_top`, even though
lottie, iTerm2, and kitty all anchor to it now. Rename to a protocol-neutral
name (e.g. `img_abs_top` / `image_abs_top`) for consistency.

### Diagnostics

`diag.c` already lists "iTerm2 images" and "kitty graphics" as capabilities;
confirm it doesn't still say "planned" anywhere and that the ConPTY DCS note
is accurate with the bundled `conpty.dll`/`OpenConsole.exe`.
