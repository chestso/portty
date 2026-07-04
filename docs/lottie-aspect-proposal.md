# Proposal: Aspect-Correct Lottie Scaling with Cell Report

## Problem

The pipeline distorts aspect because:

1. **plotty** converts design px to cell grid using hardcoded `DEFAULT_CELL_W_PX=10`, `DEFAULT_CELL_H_PX=20` (actual cell: 9x22)
2. **coffer** does `tvg_picture_set_size(px_w, px_h)` — a non-uniform stretch from design space to the cell box

## Design Principle

Separate rasterization size (aspect-correct, controlled by pixel and/or cell constraints) from placement cells (engine-computed from rasterization divided by cell px).

The client controls how big via `max_width`/`max_height` (pixels) and/or `max_cols`/`max_rows` (cells), and how to fit via the `fit` field. The engine converts cell constraints to pixels internally, computes the aspect-correct size, derives which cells are used, and reports them back.

Two distinct use cases:

1. **Fit** — scale animation to fill a cell area (keeping aspect), center within it. May scale up or down.
2. **Center** — no scaling, place at design size centered in a cell area. May overflow (cropped) or be much smaller than the area.

## Protocol Changes

### `load` — new size constraint and fit fields, `rows`/`cols` removed from placement

```json
{
  "cmd": "load",
  "id": 1,
  "lottie": { ... },
  "max_cols": 20,
  "max_rows": 10,
  "fit": "contain",
  "placement": { "row": 5, "col": 10, "center": true },
  "layer": "foreground",
  "opacity": 1.0,
  "play": { "speed": 1.0, "loop": true, "autostart": true }
}
```

| Field              | Type   | Default     | Description                                                                  |
| ------------------ | ------ | ----------- | ---------------------------------------------------------------------------- |
| `max_width`        | int    | 0 (unset)   | Max rasterization width in px. 0 = no px width constraint.                   |
| `max_height`       | int    | 0 (unset)   | Max rasterization height in px. 0 = no px height constraint.                 |
| `max_cols`         | int    | 0 (unset)   | Max placement width in cells. Converted to px: `max_cols * cell_w_px`.       |
| `max_rows`         | int    | 0 (unset)   | Max placement height in cells. Converted to px: `max_rows * cell_h_px`.      |
| `fit`              | string | `"contain"` | `"contain"` = scale to fit constraints, `"none"` = use explicit `scale`.     |
| `scale`            | float  | 1.0         | Uniform scale factor. Only used when `fit: "none"`. Ignored for `"contain"`. |
| `placement.row`    | int    | cursor row  | Top-left of the available area.                                              |
| `placement.col`    | int    | cursor col  | Top-left of the available area.                                              |
| `placement.center` | bool   | false       | Center the placement within the area defined by `row`/`col` + constraints.   |
| `placement.rows`   | —      | **removed** | Engine computes: `ceil(raster_h / cell_h_px)`.                               |
| `placement.cols`   | —      | **removed** | Engine computes: `ceil(raster_w / cell_w_px)`.                               |

**Fit modes:**

| `fit`       | Sizing                                              | Centering                  | Overflow    |
| ----------- | --------------------------------------------------- | -------------------------- | ----------- |
| `"contain"` | Scale to fit within constraints (largest that fits) | Centered if `center: true` | No overflow |
| `"none"`    | Scale by explicit `scale` field (default 1.0)       | Centered if `center: true` | May crop    |

**Size computation** (engine-side, always aspect-correct):

```
# Convert cell constraints to pixels, take tightest
px_max_w = max_width  > 0 ? max_width  : infinity
px_max_h = max_height > 0 ? max_height : infinity
if max_cols  > 0: px_max_w = min(px_max_w, max_cols  * cell_w_px)
if max_rows  > 0: px_max_h = min(px_max_h, max_rows  * cell_h_px)

if fit == "contain":
    scale_w = px_max_w < infinity ? px_max_w / design_w : infinity
    scale_h = px_max_h < infinity ? px_max_h / design_h : infinity
    scale = min(scale_w, scale_h)
else:  # fit == "none"
    scale = explicit_scale  # default 1.0, user may set 2.0, 0.5, etc.

px_w = round(design_w * scale)
px_h = round(design_h * scale)
```

**Centering** (if `center: true`):

```
area_rows = max_rows > 0 ? max_rows : (term_rows - row)
area_cols = max_cols > 0 ? max_cols : (term_cols - col)
cells_rows = ceil(px_h / cell_h_px)
cells_cols = ceil(px_w / cell_w_px)
actual_row = row + (area_rows - cells_rows) / 2
actual_col = col + (area_cols - cells_cols) / 2
```

With `fit: "none"`, `cells_rows`/`cells_cols` may exceed `area_rows`/`area_cols` — the placement overflows and is clipped by the renderer. With `fit: "contain"`, the placement always fits within the area.

**Common cases:**

| Client intent                             | Send                                                                             |
| ----------------------------------------- | -------------------------------------------------------------------------------- |
| Fit in 20x10 cell area                    | `max_cols: 20, max_rows: 10`                                                     |
| Fit in full terminal, centered            | `max_cols: 80, max_rows: 24, placement: {center: true}`                          |
| Design size, centered in full terminal    | `fit: "none", placement: {center: true}`                                         |
| Design size, centered in 20x10 area       | `fit: "none", max_cols: 20, max_rows: 10, placement: {center: true}`             |
| 2x design size, centered in full terminal | `fit: "none", scale: 2.0, placement: {center: true}`                             |
| 0.5x design size, centered in 20x10 area  | `fit: "none", scale: 0.5, max_cols: 20, max_rows: 10, placement: {center: true}` |
| Fit in 300x200px                          | `max_width: 300, max_height: 200`                                                |
| Design size at position                   | omit all                                                                         |

### `place` — new size constraint and fit fields, `rows`/`cols` removed from placement

```json
{
  "cmd": "place",
  "id": 1,
  "max_cols": 40,
  "max_rows": 20,
  "fit": "none",
  "placement": { "row": 0, "col": 0, "center": true },
  "layer": "background",
  "opacity": 0.9
}
```

- `max_width`/`max_height`/`max_cols`/`max_rows`/`fit`/`scale` optional — if any present and different from current, triggers seamless re-rasterization + buffer realloc + cell recompute.
- If omitted, keeps current rasterization size, fit mode, and scale.
- `placement.center` — re-centers within the area without re-rasterizing (position-only change).
- `placement.rows`/`placement.cols` **removed** — always engine-computed.
- This handles the chunked-upload path: chunks upload JSON, then `place` with size constraints, `fit`, and `scale` sets the size.

### Report (new — engine to client)

After every `load` or `place`, the engine emits a report via the output callback. The report uses the same wire format as commands — APC with base64-encoded JSON on POSIX, OSC 5556 with base64-encoded JSON on Windows (mirroring the OSC 5555 command carrier):

| Direction       | POSIX                       | Windows                          |
| --------------- | --------------------------- | -------------------------------- |
| Client → engine | `ESC _ <base64-json> ESC \` | `ESC ] 5555 ; <base64-json> BEL` |
| Engine → client | `ESC _ <base64-json> ESC \` | `ESC ] 5556 ; <base64-json> BEL` |

The report payload is a JSON object:

```json
{
  "type": "report",
  "id": 1,
  "row": 5,
  "col": 10,
  "rows": 4,
  "cols": 8,
  "raster_w": 375,
  "raster_h": 375,
  "cell_w_px": 9,
  "cell_h_px": 22
}
```

This gives the client:

- **Which cells are used** (`row`, `col`, `rows`, `cols`)
- **Rasterization px** (`raster_w`, `raster_h`) — for aspect verification
- **Cell px** (`cell_w_px`, `cell_h_px`) — so the client can compute `max_width`/`max_height` for the next placement without a separate query

Benefits of APC+JSON over semicolon-delimited OSC:

- Same parser as commands (base64-decode + JSON key lookup)
- Extensible — add fields without format changes
- No OSC numeric namespace collision risk
- Semantically correct (APC = Application Program Command)
- Silently discarded by other terminals (xterm ignores unknown APC)

## coffer Implementation (`lottie.c`)

### `lt_cmd_load()` — replace design-to-cell conversion

```c
/* Parse size constraints (default 0 = no constraint) */
int max_width = 0, max_height = 0;
int max_cols = 0, max_rows = 0;
val = lt_json_find_key(json, json_len, "max_width", &vlen);
if (val)
    max_width = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_height", &vlen);
if (val)
    max_height = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_cols", &vlen);
if (val)
    max_cols = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_rows", &vlen);
if (val)
    max_rows = (int)lt_json_int(val, vlen);

/* Parse fit mode (default "contain") */
bool fit_none = false;
val = lt_json_find_key(json, json_len, "fit", &vlen);
if (val && vlen >= 6 && memcmp(val, "\"none\"", 6) == 0)
    fit_none = true;

/* Parse explicit scale (default 1.0, only used when fit: "none") */
double explicit_scale = 1.0;
val = lt_json_find_key(json, json_len, "scale", &vlen);
if (val)
    explicit_scale = lt_json_double(val, vlen);
if (explicit_scale <= 0.0)
    explicit_scale = 1.0;

/* Parse center flag (default false) */
bool center = false;
if (placement) {
    val = lt_json_find_key(placement, vlen, "center", &pvlen);
    if (val)
        center = lt_json_bool(val, pvlen);
}

/* Convert cell constraints to pixels, take tightest */
double px_max_w = -1.0;  /* -1 = infinity (no constraint) */
double px_max_h = -1.0;
if (max_width > 0)
    px_max_w = (double)max_width;
if (max_cols > 0) {
    double cw = (double)max_cols * vt->cell_w_px;
    if (px_max_w < 0 || cw < px_max_w)
        px_max_w = cw;
}
if (max_height > 0)
    px_max_h = (double)max_height;
if (max_rows > 0) {
    double ch = (double)max_rows * vt->cell_h_px;
    if (px_max_h < 0 || ch < px_max_h)
        px_max_h = ch;
}

/* Compute aspect-correct rasterization size */
double scale = 1.0;
if (!fit_none) {
    if (px_max_w > 0 && design_w > 0)
        scale = px_max_w / design_w;
    if (px_max_h > 0 && design_h > 0) {
        double sh = px_max_h / design_h;
        if (sh < scale)
            scale = sh;
    }
} else {
    scale = explicit_scale;
}

int px_w = (int)(design_w * scale + 0.5);
int px_h = (int)(design_h * scale + 0.5);
if (px_w < 1) px_w = 1;
if (px_h < 1) px_h = 1;

/* Placement cells derived from rasterization size / cell px */
int pcols = (px_w + vt->cell_w_px - 1) / vt->cell_w_px;
int prows = (px_h + vt->cell_h_px - 1) / vt->cell_h_px;
if (pcols < 1) pcols = 1;
if (prows < 1) prows = 1;

/* Center within the available area if requested */
if (center) {
    int area_cols = max_cols > 0 ? max_cols : (vt->cols - pcol);
    int area_rows = max_rows > 0 ? max_rows : (vt->rows - prow);
    prow += (area_rows - prows) / 2;
    pcol += (area_cols - pcols) / 2;
    if (prow < 0) prow = 0;
    if (pcol < 0) pcol = 0;
}
```

- `px_w`/`px_h` now mean **rasterization** dimensions (not cell box). Buffer = `px_w * px_h * 4`.
- `tvg_picture_set_size(picture, px_w, px_h)` is now a **uniform** scale (both derived from same `scale`).
- Client-specified `prows`/`pcols` are no longer read or used.

### `lt_cmd_place()` — parse `max_width`/`max_height`, seamless re-rasterize if changed

The rescale preserves the current frame and playback state. ThorVG's `tvg_picture_set_size()` updates the scale transform without reloading JSON or rebuilding the scene graph, so the user sees the same frame at the new size and playback continues from there.

```c
/* Parse size constraints (default: keep current values) */
int new_max_w = rec->max_width;
int new_max_h = rec->max_height;
int new_max_cols = rec->max_cols;
int new_max_rows = rec->max_rows;
bool new_fit_none = rec->fit_none;
double new_scale = rec->explicit_scale;
val = lt_json_find_key(json, json_len, "max_width", &vlen);
if (val)
    new_max_w = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_height", &vlen);
if (val)
    new_max_h = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_cols", &vlen);
if (val)
    new_max_cols = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "max_rows", &vlen);
if (val)
    new_max_rows = (int)lt_json_int(val, vlen);
val = lt_json_find_key(json, json_len, "fit", &vlen);
if (val && vlen >= 6 && memcmp(val, "\"none\"", 6) == 0)
    new_fit_none = true;
else if (val && vlen >= 9 && memcmp(val, "\"contain\"", 9) == 0)
    new_fit_none = false;
val = lt_json_find_key(json, json_len, "scale", &vlen);
if (val)
    new_scale = lt_json_double(val, vlen);
if (new_scale <= 0.0)
    new_scale = 1.0;

/* Parse center flag */
bool center = false;
if (placement) {
    val = lt_json_find_key(placement, vlen, "center", &pvlen);
    if (val)
        center = lt_json_bool(val, pvlen);
}

/* Recompute rasterization size if constraints or fit changed */
bool size_changed = (new_max_w != rec->max_width ||
                     new_max_h != rec->max_height ||
                     new_max_cols != rec->max_cols ||
                     new_max_rows != rec->max_rows ||
                     new_fit_none != rec->fit_none ||
                     new_scale != rec->explicit_scale);
if (size_changed) {
    rec->max_width = new_max_w;
    rec->max_height = new_max_h;
    rec->max_cols = new_max_cols;
    rec->max_rows = new_max_rows;
    rec->fit_none = new_fit_none;
    rec->explicit_scale = new_scale;

    /* Convert cell constraints to pixels, take tightest */
    double px_max_w = -1.0;
    double px_max_h = -1.0;
    if (new_max_w > 0)
        px_max_w = (double)new_max_w;
    if (new_max_cols > 0) {
        double cw = (double)new_max_cols * vt->cell_w_px;
        if (px_max_w < 0 || cw < px_max_w)
            px_max_w = cw;
    }
    if (new_max_h > 0)
        px_max_h = (double)new_max_h;
    if (new_max_rows > 0) {
        double ch = (double)new_max_rows * vt->cell_h_px;
        if (px_max_h < 0 || ch < px_max_h)
            px_max_h = ch;
    }

    double scale = 1.0;
    if (!new_fit_none) {
        if (px_max_w > 0 && rec->design_w > 0)
            scale = px_max_w / rec->design_w;
        if (px_max_h > 0 && rec->design_h > 0) {
            double sh = px_max_h / rec->design_h;
            if (sh < scale)
                scale = sh;
        }
    } else {
        scale = new_scale;
    }

    int new_px_w = (int)(rec->design_w * scale + 0.5);
    int new_px_h = (int)(rec->design_h * scale + 0.5);
    if (new_px_w < 1) new_px_w = 1;
    if (new_px_h < 1) new_px_h = 1;

    /* Realloc RGBA buffer if size changed */
    size_t need = (size_t)new_px_w * (size_t)new_px_h * 4;
    if (need != rec->rgba_cap) {
        st->live_bytes -= rec->rgba_cap;
        lt_buf_release(vt, st, rec->rgba, rec->rgba_cap);
        rec->rgba = lt_buf_alloc(vt, st, need);
        rec->rgba_cap = need;
        st->live_bytes += need;
    }

    rec->px_w = new_px_w;
    rec->px_h = new_px_h;

    /* Update ThorVG picture size (no JSON reload, no scene rebuild) */
    Tvg_Paint pic = tvg_animation_get_picture(rec->tvg_anim);
    tvg_picture_set_size(pic, (float)new_px_w, (float)new_px_h);

    /* Recreate SW canvas with new target buffer */
    if (rec->tvg_canvas) {
        tvg_canvas_remove(rec->tvg_canvas, tvg_animation_get_picture(rec->tvg_anim));
        tvg_canvas_destroy(rec->tvg_canvas);
    }
    rec->tvg_canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    tvg_swcanvas_set_target(rec->tvg_canvas, (uint32_t *)rec->rgba,
                            (uint32_t)new_px_w, (uint32_t)new_px_w,
                            (uint32_t)new_px_h, TVG_COLORSPACE_ARGB8888);
    tvg_canvas_add(rec->tvg_canvas, pic);

    /* Re-rasterize current frame (not frame 0) — seamless rescale */
    lt_rasterize(rec);  /* uses rec->current_frame */

    rec->version++;
}
/* recompute placement rows/cols from current px_w/px_h */
int pcols = (rec->px_w + vt->cell_w_px - 1) / vt->cell_w_px;
int prows = (rec->px_h + vt->cell_h_px - 1) / vt->cell_h_px;

/* Center within the available area if requested */
if (center) {
    int area_cols = new_max_cols > 0 ? new_max_cols : (vt->cols - pcol);
    int area_rows = new_max_rows > 0 ? new_max_rows : (vt->rows - prow);
    prow += (area_rows - prows) / 2;
    pcol += (area_cols - pcols) / 2;
    if (prow < 0) prow = 0;
    if (pcol < 0) pcol = 0;
}
```

**What is preserved across rescale:**

- `current_frame` — no jump to frame 0
- `playing` / `speed` / `loop` — playback state untouched
- `last_tick_us` — frame timing continues seamlessly
- `tvg_anim` — ThorVG animation object reused (no JSON re-parse, no scene graph rebuild)

**What changes:**

- `px_w` / `px_h` — new rasterization dimensions
- `rgba` buffer — reallocated to new size
- `tvg_canvas` — recreated with new target buffer
- `placements[].rows` / `placements[].cols` — recomputed from new px dimensions

### Report emission — new helper called after load/place

```c
static void lt_emit_report(CfrTerm *vt, uint64_t id,
                           int row, int col, int rows, int cols,
                           int raster_w, int raster_h)
{
    /* Build JSON report */
    char json[160];
    int jn = snprintf(json, sizeof(json),
        "{\"type\":\"report\",\"id\":%llu,\"row\":%d,\"col\":%d,"
        "\"rows\":%d,\"cols\":%d,\"raster_w\":%d,\"raster_h\":%d,"
        "\"cell_w_px\":%d,\"cell_h_px\":%d}",
        (unsigned long long)id, row, col, rows, cols,
        raster_w, raster_h, vt->cell_w_px, vt->cell_h_px);
    if (jn <= 0)
        return;

    /* Base64-encode */
    char b64[256];
    size_t b64_len = lt_base64_encode(
        (const uint8_t *)json, (size_t)jn, b64, sizeof(b64));
    if (b64_len == 0)
        return;

    /* Emit APC on POSIX, OSC 5556 on Windows */
#ifdef _WIN32
    char seq[280];
    int n = snprintf(seq, sizeof(seq), "\x1b]5556;%.*s\x07",
                     (int)b64_len, b64);
#else
    char seq[280];
    int n = snprintf(seq, sizeof(seq), "\x1b_%.*s\x1b\\",
                     (int)b64_len, b64);
#endif
    if (n > 0)
        cfr_emit_bytes(vt, (const uint8_t *)seq, (size_t)n);
}
```

Note: `lt_base64_encode()` is a small helper added alongside the existing `lt_base64_decode()`. The `#ifdef _WIN32` gate matches the command-side pattern where `cfr_lottie_apc_dispatch()` is called from both APC and OSC 5555 paths.

### `LtRec` struct — add size constraint fields

```c
int max_width;   /* pixel constraint, 0 = no limit */
int max_height;  /* pixel constraint, 0 = no limit */
int max_cols;    /* cell constraint, 0 = no limit */
int max_rows;    /* cell constraint, 0 = no limit */
bool fit_none;      /* true = no auto-fit (fit:"none"), false = contain */
double explicit_scale; /* user-specified scale, only used when fit_none (default 1.0) */
```

### `lt_cmd_load_chunk()` — unchanged

The synthetic load JSON stays as-is (no placement, no size constraints, no fit, no scale). The client sends `place` with `max_cols`/`max_rows`/`max_width`/`max_height`/`fit`/`scale`/`center` after chunks complete.

## portty Renderer (`rend_sdl3.c`)

### `render_lottie_layer()` — center the canvas texture within the cell box

```c
int box_w = pl->cols * data->cell_width;
int box_h = pl->rows * data->cell_height;
/* Center canvas within cell box (transparent padding for contain,
 * clip for fit:none overflow) */
int off_x = (box_w - anim->canvas_w) / 2;
int off_y = (box_h - anim->canvas_h) / 2;
SDL_FRect dst = {
    (float)(px + off_x), (float)(py + off_y),
    (float)anim->canvas_w, (float)anim->canvas_h
};
SDL_RenderTexture(data->renderer, tex, NULL, &dst);
```

With `fit: "contain"`, the texture is `canvas_w x canvas_h` (rasterization size) and the cell box is `cols * cell_w x rows * cell_h` (slightly larger due to ceiling division). The centering offset is at most 1 cell pixel — transparent padding.

With `fit: "none"`, the texture may be larger than the cell box (design size exceeds the area). The centering offset is negative, so `dst.x`/`dst.y` are before `px`/`py` — the texture is clipped by the renderer to the viewport. No explicit scissor needed since SDL clips to the render target by default.

No `SDL_RenderTextureScaled` or aspect logic needed — the texture is already aspect-correct in both modes.

## plotty Changes (`protocol.py`, `tui.py`)

### `protocol.py`

- Remove `DEFAULT_CELL_W_PX` / `DEFAULT_CELL_H_PX`
- `apc_load()` / `apc_place()` gain `max_width: int = 0`, `max_height: int = 0`, `max_cols: int = 0`, `max_rows: int = 0`, `fit: str = "contain"`, `scale: float = 1.0`, `center: bool = False` parameters, emitted in JSON
- `compute_placement()` removed — no longer converts design px to cells
- Add `parse_report()` to base64-decode and parse the APC/OSC 5556 report JSON
- Add `read_report()` that reads from `/dev/tty` (needs `O_RDWR`) with a short timeout

### `tui.py` — `_recompute_placement()` becomes

1. Load with `max_cols=avail_cols, max_rows=avail_rows, placement: {center: true}` at row/col of the available area — fits and centers in one round-trip
2. Read APC report — get final `rows`, `cols`, `raster_w`, `raster_h` for layout
3. If the user toggles the help panel or resizes, send `place` with updated `max_cols`/`max_rows` — seamless rescale
4. If the user wants original size centered: send `place` with `fit: "none"` — no re-rasterization (if already at design size), just repositions
5. If the user wants 2x size centered: send `place` with `fit: "none", scale: 2.0` — seamless re-rasterize at 2x, repositioned

## Test Updates

### coffer `test_cfr_lottie.c`

- `test_load_basic`: `canvas_w`/`canvas_h` now = design size (40x24), not cell box
- `test_load_multiple`: `l2->canvas_w` = 40 (design 40, no constraint), `l2->canvas_h` = 40 (not 42)
- Remove any tests that pass client-specified `rows`/`cols` and expect them to be used for rasterization
- Add test: `max_width=80` with 40x40 design -> canvas = 80x80, cells = ceil(80/10) x ceil(80/6)
- Add test: `max_width=60, max_height=30` with 40x40 design -> scale = min(1.5, 0.75) = 0.75 -> canvas = 30x30
- Add test: `max_cols=8, max_rows=4` with 10x6 cells -> px constraints = 80x24 -> same as max_width=80, max_height=24
- Add test: `max_cols=4, max_width=50` -> tightest = min(40, 50) = 40px width
- Add test: `fit: "none"` with 40x40 design, no constraints -> canvas = 40x40 (design size, scale 1.0)
- Add test: `fit: "none", scale: 2.0` with 40x40 design -> canvas = 80x80
- Add test: `fit: "none", scale: 0.5` with 40x40 design -> canvas = 20x20
- Add test: `fit: "none"` with `max_cols=4, max_rows=4` and 100x100 design -> canvas = 100x100, placement cells = 10x17 (overflows 4x4 area)
- Add test: `center: true` with `max_cols=20, max_rows=10` and 40x24 cells -> placement row/col centered within 20x10 area
- Add test: `center: true` with no constraints -> placement centered within full terminal
- Add test: APC report is emitted with correct fields (parse base64+JSON, verify all fields)

### portty `test_lottie.c`

- `test_load_basic`: recalculate expected `canvas_w`/`canvas_h` (design size, not cell box)
- Add test: `max_width`/`max_height` produces aspect-correct canvas
- Add test: `fit: "none"` produces design-size canvas
- Add test: `fit: "none", scale: 2.0` produces 2x canvas
- Add test: `center: true` centers placement within area

## Spec Doc (`lottie-spec.md`)

Update section 2.1 (load), section 2.3 (place) with `max_width`/`max_height`/`max_cols`/`max_rows`/`fit`/`scale`/`center` fields. Add section 2.8 (report — APC base64-JSON on POSIX, OSC 5556 base64-JSON on Windows). Update section 2.2 (placement coordinates) to note rows/cols are engine-computed and centering is supported. Update section 4.1 (`CfrLottie`) to note `canvas_w`/`canvas_h` = rasterization dimensions (aspect-correct, derived from constraints, fit mode, and scale).

## Migration / Compatibility

All size constraint fields default to 0 (design size), `fit` defaults to `"contain"`, `scale` defaults to 1.0, `center` defaults to false, and `placement.rows`/`cols` are silently ignored if present. This means existing clients that send rows/cols will get different behavior (engine computes cells instead of using client values), but the aspect will be correct. No version negotiation needed.

## Files Changed

| Repo   | File                      | Changes                                                                                                                                        |
| ------ | ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| coffer | `src/lottie.c`            | `max_width`/`max_height`/`max_cols`/`max_rows`/`fit`/`scale`/`center` parsing, aspect-correct `px_w`/`px_h`, `place` re-rasterize, report emit |
| coffer | `include/coffer/coffer.h` | Comment updates on `canvas_w`/`canvas_h`                                                                                                       |
| coffer | `tests/test_cfr_lottie.c` | Update expectations, add scale + report tests                                                                                                  |
| coffer | `docs/lottie-spec.md`     | Document `max_width`/`max_height`/`max_cols`/`max_rows`/`fit`/`scale`/`center`, report, updated semantics                                      |
| portty | `src/rend_sdl3.c`         | Center texture in cell box                                                                                                                     |
| portty | `tests/test_lottie.c`     | Update canvas expectations                                                                                                                     |
| plotty | `src/plotty/protocol.py`  | `max_cols`/`max_rows`/`max_width`/`max_height`/`fit`/`scale`/`center` params, remove hardcoded cell px, report parsing                         |
| plotty | `src/plotty/tui.py`       | Cell-constraint + fit/center placement, report reading                                                                                         |
