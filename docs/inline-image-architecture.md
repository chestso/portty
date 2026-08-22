# Inline Image Architecture: Unified Design

## Motivation

Four image/animation systems in coffer share the same fundamental problem:
receive RGBA pixel data, anchor it to a grid line, render it with alpha,
scroll it with text. They currently duplicate the same infrastructure.

| System | Transport | Alpha | Placements | ConPTY | Status |
|---|---|---|---|---|---|
| Sixel | DCS | Binary (0/255) | 1:1 | Needs DLL | Done |
| Lottie | APC (OSC 5555) | Full 8-bit | 1:N | OSC carrier | Done |
| iTerm2 | OSC 1337 | Full 8-bit (PNG) | 1:1 | Yes (OSC) | Planned |
| Kitty | APC (OSC 5555) | Full 8-bit | 1:N + z-index | OSC carrier | Planned |

### What's duplicated today

Sixel and Lottie each independently implement:
- Two-tier memory: dense record array + best-fit free-list buffer pool
- Budget eviction (oldest `abs_line` first)
- Scroll culling (placements past scrollback capacity)
- Clear by display rows (erase/cls)
- Clear all (RIS/altscreen)
- `abs_line` anchoring (scrolls with text)
- Version bumping (texture re-upload signal)
- Public query API with scratch array
- Per-placement layer (foreground/background)

The only differences are what they *shouldn't* share:
- Sixel: DCS byte-level decoder, color registers, sixel bands, decode canvas
- Lottie: JSON parser, ThorVG rasterizer, frame timing, fit/scale logic
- iTerm2: OSC 1337 parser, PNG decode, width/height scaling
- Kitty: APC parser, image IDs, z-index, animation frames, virtual placements

---

## What Already Exists: Four Parallel Implementations

### Sixel (`sixel.c`)

```
SxRec { id, version, layer, abs_line, col, w, h, rows_tall, rgba, cap }
CfrSixelState { recs[], spares[], live_bytes, scratch, canvas, palette, decode... }
SX_LIVE_MAX=128MB, SX_MAX_IMAGES=256, SX_SPARE_MAX=16, SX_RETAIN_MAX=32MB
```

1:1 image-to-placement. `abs_line`/`col`/`layer` live directly on `SxRec`.
Cursor advances below the image after placement. DCS decoder produces RGBA
with binary alpha (0 or 255).

### Lottie (`lottie.c`)

```
LtRec { id, version, rgba, rgba_cap, px_w, px_h, design_w/h, fit, scale,
        current_frame, playing, loop, dirty, tvg_anim, tvg_canvas,
        placements[], arena... }
CfrLottiePlacement { id, abs_line, row, col, rows, cols, layer, opacity_x256 }
CfrLottieState { recs[], spares[], live_bytes, scratch, pl_scratch, chunks... }
LT_LIVE_MAX=128MB, LT_MAX_ANIMS=64, LT_MAX_PLACEMENTS=32, LT_RETAIN_MAX=64MB
```

1:N image-to-placement. Each `LtRec` has an array of `CfrLottiePlacement`.
`abs_line`/`col`/`layer`/`opacity` live on placements. Cursor is NOT advanced
(Lottie uses an explicit `place` command). ThorVG rasterizes to RGBA with full
alpha.

### Key observation

Lottie already has the 1:N placement model that kitty needs. The placement
struct is almost exactly what kitty needs — it just lacks `z_index`,
`src_x/y/w/h` (source rectangle), `pix_offset_x/y`, and `placement_id`.

---

## Phase 0: Extract Shared Image Store

### New file: `coffer/src/image_store.c`, `coffer/src/image_store.h`

Extract the common storage, memory, and grid-maintenance code shared by
all four systems.

#### Image record: `CfrImg`

```c
typedef struct {
    uint64_t id;
    uint32_t version;
    uint8_t layer;    /* 0 = foreground, 1 = background */
    uint8_t source;   /* IMG_SRC_SIXEL, IMG_SRC_LOTTIE, IMG_SRC_ITERM, IMG_SRC_KITTY */
    long abs_line;    /* absolute line (scrolls with text) */
    int col;
    int w, h;         /* display pixel dimensions */
    int rows_tall;    /* cells tall (cached for cull/clear) */
    int cols_wide;    /* cells wide (cached) */
    uint8_t *rgba;
    size_t cap;
} CfrImg;
```

#### Placement record: `CfrPlacement`

For systems with 1:N image-to-placement (lottie, kitty). Sixel and iTerm2 use
1:1 (the placement is implicit in the image record).

```c
typedef struct {
    uint64_t id;
    uint32_t image_id;   /* index into CfrImg array */
    long abs_line;
    int col;
    int rows, cols;
    uint8_t layer;
    uint8_t opacity_x256;
    int z_index;          /* kitty only; 0 for others */
    int src_x, src_y;     /* source rect (kitty only; 0 for others) */
    int src_w, src_h;
    int pix_offset_x;     /* pixel offset in first cell (kitty only) */
    int pix_offset_y;
    int parent_img;       /* relative placement (kitty only; 0 = none) */
    int parent_place;
    int cell_off_x;
    int cell_off_y;
} CfrPlacement;
```

#### Store state: `CfrImgStore`

```c
typedef struct {
    /* Tier 1: image records */
    CfrImg *imgs;
    int img_count, img_cap;
    uint64_t next_img_id;

    /* Tier 1b: placement records (for 1:N systems) */
    CfrPlacement *places;
    int place_count, place_cap;
    uint64_t next_place_id;

    /* Tier 2: pixel-buffer free-list pool */
    ImgSpare spares[IMG_SPARE_MAX];
    int spare_count;
    size_t retain_bytes;

    /* Live budget tracking */
    size_t live_bytes;

    /* Query scratch arrays */
    CfrImage *img_scratch;
    int img_scratch_cap;
    CfrImagePlacement *place_scratch;
    int place_scratch_cap;
} CfrImgStore;
```

#### Buffer pool (shared by all systems)

```c
uint8_t *img_buf_alloc(CfrTerm *vt, CfrImgStore *st, size_t need, size_t *out_cap);
void img_buf_release(CfrTerm *vt, CfrImgStore *st, uint8_t *ptr, size_t cap);
```

These are moved verbatim from `sixel.c` (`sx_buf_alloc`/`sx_buf_release`) and
`lottie.c` (`lt_buf_alloc`/`lt_buf_release`), which are already identical
implementations. Both sixel and lottie call into these instead of their own
copies.

#### Store API

```c
/* Create/destroy */
CfrImgStore *cfr_img_store_new(CfrTerm *vt);
void cfr_img_store_free(CfrTerm *vt, CfrImgStore *st);

/* Add a 1:1 image (sixel, iTerm2). Creates an image with an implicit
 * single placement at the cursor. Handles buffer alloc, eviction,
 * cursor advance, and damage. */
int cfr_img_add(CfrTerm *vt, CfrImgStore *st,
                const uint8_t *rgba, int w, int h,
                uint8_t layer, uint8_t source);
/* Returns image index, or -1 on failure */

/* Add a placement to an existing image (lottie, kitty). */
int cfr_img_add_placement(CfrTerm *vt, CfrImgStore *st,
                          int img_idx, long abs_line, int col,
                          int rows, int cols, uint8_t layer,
                          uint8_t opacity);
/* Returns placement index, or -1 */

/* Find an image at a given anchor + layer (for animation/frame replacement). */
int cfr_img_find_at(CfrImgStore *st, long abs_line, int col, uint8_t layer);

/* Replace image pixel data (animation / frame update). */
void cfr_img_replace(CfrTerm *vt, CfrImgStore *st, int idx,
                     const uint8_t *rgba, int w, int h);

/* Eviction */
void img_evict_to_budget(CfrTerm *vt, CfrImgStore *st, size_t incoming);

/* Scroll/clear hooks */
void cfr_img_note_scroll(CfrTerm *vt, CfrImgStore *st, int lines);
void cfr_img_clear_display_rows(CfrTerm *vt, CfrImgStore *st, int top, int bot);
void cfr_img_clear_all(CfrTerm *vt, CfrImgStore *st);

/* Public query */
const CfrImage *cfr_img_get(CfrTerm *vt, CfrImgStore *st, int *out_count);
const CfrImagePlacement *cfr_img_get_placements(CfrTerm *vt, CfrImgStore *st,
                                                 int *out_count);
```

#### Cursor advancement (shared)

```c
/* Move the cursor below a placed image and scroll the grid as needed.
 * Called by cfr_img_add() for sixel/iTerm2. Lottie and kitty manage
 * their own cursor (or don't advance it). */
void img_advance_cursor(CfrTerm *vt, int rows_tall, int cols_wide);
```

Moved verbatim from `sx_advance_cursor()`. Sixel-specific modes (DECSDM
mode 80, mode 8452) are checked inside the function — they only affect
sixel, but the function is generic enough to be shared. Non-sixel callers
always get default behavior (cursor below).

#### Public structs

```c
typedef struct {
    uint64_t id;
    uint32_t version;
    uint8_t layer;
    uint8_t source;     /* IMG_SRC_SIXEL, etc. */
    int row;            /* unified: abs_line - img_abs_top */
    int col;
    int width_px;
    int height_px;
    const uint8_t *rgba;
} CfrImage;

typedef struct {
    uint64_t id;
    uint64_t image_id;
    int row;             /* unified */
    int col;
    int rows, cols;
    uint8_t layer;
    uint8_t opacity_x256;
    int z_index;
} CfrImagePlacement;

/* Backward-compat aliases */
typedef CfrImage CfrSixel;
typedef CfrImagePlacement CfrLottiePlacement;
```

### How each system uses the shared store

**Sixel** (`sixel.c`):
- DCS decoder stays in `sixel.c` (canvas, palette, bands, color registers)
- `cfr_sixel_finish()` calls `cfr_img_add(vt, store, rgba, w, h, 0, IMG_SRC_SIXEL)`
- Sixel-specific cursor modes checked inside `img_advance_cursor()`
- No placements (1:1 — the image IS the placement)
- `CfrSixelState` keeps only decode state; storage moves to `CfrImgStore`

**Lottie** (`lottie.c`):
- JSON parser, ThorVG rasterizer, frame timing stay in `lottie.c`
- After rasterization, calls `cfr_img_replace()` to update pixels
- Placements created via `cfr_img_add_placement()`
- `CfrLottieState` keeps only JSON/chunk state; storage moves to `CfrImgStore`
- `LtRec` loses `rgba`, `rgba_cap`, `placements`, `placement_count` — these
  now live in the shared `CfrImg`/`CfrPlacement` arrays
- `LtRec` keeps: `json_root`, `arena`, `tvg_*`, `design_w/h`, `px_w/h`,
  `fit`, `scale`, frame playback fields

**iTerm2** (`osc_1337.c`, new):
- OSC 1337 parser, base64 decode, PNG decode (stb_image)
- Calls `cfr_img_add(vt, store, rgba, w, h, 0, IMG_SRC_ITERM)`
- No placements (1:1)

**Kitty** (`graphics.c`, new):
- APC parser, image IDs, z-index, frames, virtual placements
- Calls `cfr_img_add()` for image data, `cfr_img_add_placement()` for
  placements with z_index
- Animation frames via `cfr_img_replace()`
- Uses the shared buffer pool for pixel data

### Single store or multiple stores?

**One shared `CfrImgStore`** for all systems. The `source` field on each
image record distinguishes them. Scroll/clear hooks iterate all records
regardless of source — an image scrolls the same whether it came from
sixel, iTerm2, or kitty.

Lottie's `LtRec` array (JSON metadata, ThorVG state, frame timing) stays
separate — it's parallel to the `CfrImg` array, linked by `id`. When lottie
rasterizes a new frame, it finds its `CfrImg` by `id` and calls
`cfr_img_replace()`.

### `coffer_internal.h` changes

```c
struct CfrTerm {
    ...
    CfrImgStore *images;       /* shared image store (replaces sixel+lottie stores) */
    struct CfrSixelState *sixel;  /* DCS decode state only (canvas, palette) */
    struct CfrLottieState *lottie; /* JSON/ThorVG state only */
    ...
};
```

### `print.c` / `modes.c` changes

Scroll/clear call sites become single calls:
```c
/* Was: cfr_sixel_note_scroll(vt, lines); cfr_lottie_note_scroll(vt, lines); */
cfr_img_note_scroll(vt, vt->images, lines);

/* Was: cfr_sixel_clear_display_rows(vt, top, bot);
 *      cfr_lottie_clear_display_rows(vt, top, bot); */
cfr_img_clear_display_rows(vt, vt->images, top, bot);

/* Was: cfr_sixel_clear_all(vt); cfr_lottie_clear_all(vt); */
cfr_img_clear_all(vt, vt->images);
```

### Portty `rend_sdl3.c` changes

- `sixel_cache` → `image_cache` (generic)
- `render_sixel_images()` → `render_images()` — handles all sources
- For z-index: `render_images(data, term, Z_NEGATIVE)` before text,
  `render_images(data, term, Z_NON_NEGATIVE)` after text
- `terminal_get_sixels()` → `terminal_get_images()` (alias kept)

### Migration strategy

1. **Pass 1:** Create `image_store.c` with generic functions. Sixel calls
   into it. Lottie calls into it. Build and test.
2. **Pass 2:** Rename `CfrSixel`→`CfrImage`, `CfrLottiePlacement`→
   `CfrImagePlacement`. Add `source` field. Add typedef aliases. Build
   and test.
3. **Pass 3:** Merge scroll/clear hooks in `print.c`/`modes.c`. Build
   and test.

---

## Phase 1: iTerm2 Inline Images (OSC 1337)

### Why iTerm2 first

- Smallest implementation: OSC handler + PNG decode + `cfr_img_add()`
- Works on Windows without ConPTY workarounds (OSC passes through)
- Immediate value: full alpha support via `chafa -f iterm2`
- Validates the shared store with a second 1:1 image source

### `coffer/src/osc_1337.c` (new)

Add `case 1337` to `cfr_osc_dispatch()` in `osc.c`.

Sub-commands:
- `File=` — parse params, base64 decode, decode image, call `cfr_img_add()`
- `MultipartFile=` / `FilePart=` / `FileEnd` — chunked transfer
- `Capabilities` — respond with `F` (inline file support)
- `ReportCellSize` — respond with cell dimensions

Width/height scaling (cells, pixels, percent) is computed before calling
`cfr_img_add()` — the store stores final display dimensions.

### What chafa emits

```
ESC [ ? 25 l
ESC ] 1337 ; File = inline = 1 ; width = 50 ; height = 24 ; preserveAspectRatio = 0 : <base64-png> BEL \r \n
ESC [ ? 25 h
```

Chafa encodes the image as PNG (with alpha) in a single `File=` sequence.

---

## Phase 2: Kitty Graphics (APC)

### APC router

`coffer/src/apc.c` — routes based on first byte:
- `G` → graphics dispatcher (`graphics.c`)
- else → Lottie dispatcher (`lottie.c`)

Updates to `parser.c` and `osc.c` (OSC 5555): call `cfr_apc_dispatch()`.

### Kitty store

Kitty uses the shared `CfrImgStore` for image data and buffer pool, but
manages its own placement records (z-index, source rectangles, virtual
placements, relative placements). The kitty-specific placement fields
(`z_index`, `src_x/y/w/h`, `pix_offset_x/y`, `parent_img/place`, etc.) live
in `CfrPlacement` which is already part of the shared store.

Kitty action handlers call:
- `cfr_img_add()` for transmit (`a=t`/`a=T`)
- `cfr_img_add_placement()` for place (`a=p`)
- `cfr_img_replace()` for frame updates (`a=f`)
- `cfr_img_find_at()` for animation replacement

Animation control (`a=a`), compose (`a=c`), virtual placements (`U=1`),
and relative placements (`P/Q/H/V`) are kitty-specific logic in
`graphics.c` that operates on the shared store's records.

### ConPTY carrier

Reuses OSC 5555 (inbound) and OSC 5556 (outbound), shared with Lottie.
The APC router distinguishes by `G` prefix.

---

## Phase 3: Image Decoding

### stb_image (shared by iTerm2 and kitty)

Vendor `stb_image.h` in `coffer/third_party/stb/`. Auto-fetched by
`configure.ac` if missing (same pattern as QOI).

### `coffer/src/image_decode.c` (new)

```c
uint8_t *cfr_image_decode(const uint8_t *data, size_t len,
                          int *width, int *height);
```

Decodes PNG/JPEG/BMP/GIF/TGA to RGBA. Used by iTerm2 (PNG from base64)
and kitty (`f=100` PNG format).

For kitty `f=32` (raw RGBA) and `f=24` (raw RGB), no image decode is
needed — just base64 decode and optional RGB→RGBA expansion.

---

## Phase 4: Portty Rendering (Unified)

### `rend_sdl3.c`

The existing sixel render path becomes the unified image render path:

| Old | New |
|---|---|
| `sixel_cache` | `image_cache` |
| `sixel_cache_reconcile` | `image_cache_reconcile` |
| `sixel_get_texture` | `image_get_texture` |
| `render_sixel_images` | `render_images` |
| `terminal_get_sixels` | `terminal_get_images` |

All sources render identically: RGBA texture with `SDL_BLENDMODE_BLEND`.
The `source` field is for diagnostics only.

For z-index (kitty), split into two render passes:
1. `render_images(data, term, /*negative_z=*/true)` — before text
2. `render_images(data, term, /*negative_z=*/false)` — after text

Sixel/iTerm2/lottie images have `z_index = 0` (non-negative).

Lottie rendering stays separate (`render_lottie_layer()`) because it has
its own texture cache and animation tick. Future unification could merge
lottie textures into the shared `image_cache`, but that's not required
for the initial implementation.

---

## Implementation Order

| Step | Phase | Description | Effort |
|---|---|---|---|
| 1 | 0 | Extract `image_store.c`: buffer pool, record lifecycle, eviction | Medium |
| 2 | 0 | Sixel calls into shared store (`cfr_img_add`) | Small |
| 3 | 0 | Lottie calls into shared store (`cfr_img_add_placement`, `cfr_img_replace`) | Medium |
| 4 | 0 | Merge scroll/clear hooks in `print.c`/`modes.c` | Small |
| 5 | 0 | Rename types: `CfrSixel`→`CfrImage`, add `source`, typedef aliases | Small |
| 6 | 0 | Build, test, verify sixel + lottie still work | — |
| 7 | 3 | Vendor stb_image, add `cfr_image_decode()` | Small |
| 8 | 1 | OSC 1337 handler: `File=`, `Capabilities`, `ReportCellSize` | Small |
| 9 | 1 | `handle_file()`: base64 decode, PNG decode, `cfr_img_add` | Small |
| 10 | 1 | Alpha test: `chafa -f iterm2` with transparent image | — |
| 11 | 1 | Multipart: `MultipartFile=`/`FilePart=`/`FileEnd` | Small |
| 12 | 1 | Width/height scaling (cells, pixels, percent) | Small |
| 13 | 2 | APC router (`apc.c`) | Tiny |
| 14 | 2 | Kitty graphics parser + transmit + place | Medium |
| 15 | 2 | Kitty query + delete | Small |
| 16 | 2 | Portty rendering: z-index split | Small |
| 17 | 2 | Chunked transfer, PNG, zlib | Medium |
| 18 | 2 | Animations, compose | Medium |
| 19 | 2 | Relative + virtual placements | Medium |
| 20 | — | Diagnostics update | Trivial |

Steps 1-6 are the refactor (no new features, but sixel + lottie share
code). Steps 7-10 deliver iTerm2 alpha images on Windows. Steps 13-16
deliver kitty graphics on POSIX.

---

## Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Single shared store | One `CfrImgStore` for all four systems | Scroll/clear/eviction/buffer-pool are identical; `source` field distinguishes |
| Lottie keeps separate metadata | `LtRec` array parallel to `CfrImg` | JSON/ThorVG/frame-timing is lottie-specific; linked by `id` |
| Sixel keeps decode state | `CfrSixelState` holds canvas/palette only | DCS decoder is sixel-specific; storage moves to shared store |
| Kitty uses shared placement records | `CfrPlacement` with z_index, src_rect | Already in shared store; avoids separate kitty placement store |
| `img_advance_cursor` shared | Sixel modes checked inside | Only sixel uses DECSDM/8452; others get default behavior |
| stb_image for decode | Vendored single-header | Supports PNG/JPEG/BMP/GIF; no build system changes |
| iTerm2 before kitty | Phase 1 before Phase 2 | Smallest effort, Windows-safe, validates shared store |
| `CfrSixel` as deprecated alias | typedef to `CfrImage` | Avoids breaking portty during migration |
| Lottie render stays separate for now | Own texture cache + tick | Can merge later; not blocking |
