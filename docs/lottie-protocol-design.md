# Lottie Animation Protocol for bloom-vt / bloom-terminal

## Implementation Status

| Component                             | Status   | Notes                                                                                                      |
| ------------------------------------- | -------- | ---------------------------------------------------------------------------------------------------------- |
| APC parser dispatch                   | **Done** | `BVT_STATE_APC_STRING` in bloom-vt parser, routes to `bvt_lottie_apc_dispatch()`                           |
| Base64 + JSON command parsing         | **Done** | `lottie.c`: `lt_json_find_key()`, `lt_base64_decode()`, all 8 command handlers                             |
| Animation state management            | **Done** | `LtRec` dense array, `LtChunkAccum` for chunked uploads                                                    |
| Buffer pool (spare)                   | **Done** | `LtSpare[16]`, best-fit free-list, 64 MiB retain cap                                                       |
| Placement management                  | **Done** | `BvtLottiePlacement` per-animation, auto-assigned stable id                                                |
| Frame advancement (`bvt_lottie_tick`) | **Done** | Tick-based timing, loop/clamp, version bump                                                                |
| Public API (`bvt_get_lotties` etc.)   | **Done** | Separate animation + placement snapshot queries                                                            |
| Scroll/clear/cull                     | **Done** | `bvt_lottie_note_scroll`, `bvt_lottie_clear_display_rows`, `bvt_lottie_clear_all`                          |
| ThorVG rasterization                  | **Done** | `lt_rasterize()` with ThorVG C API; sRGB→linear pre-linearization; `--disable-thorvg` fallback zeroes RGBA |
| Host bridge (term.h/term_bvt.c)       | **Done** | `get_lotties`, `get_lottie_placements`, `lottie_tick` vtable entries                                       |
| Host renderer (rend_sdl3.c)           | **Done** | `lottie_cache[]` (64 entries), `lottie_cache_reconcile`, `render_lottie_layer()`                           |
| Engine-side tests (test_bvt_lottie)   | **Done** | 22 test cases covering load/place/delete/play/pause/stop/seek/tick/chunk/scroll/clear/resize/opacity       |
| Host-side tests (test_lottie)         | **Done** | 14 test cases covering load/play/pause/stop/seek/delete/place/tick/clear/background                        |

---

## Overview

This document specifies a terminal escape sequence protocol for playing Lottie
(vector) animations in bloom-terminal, with the VT engine (bloom-vt) owning the
protocol parsing, animation state, **rasterization, and pixel data** — the same
ownership model as sixel. The host renderer (bloom-terminal) owns only the GPU
texture cache and compositing.

### Design Goals

1. **Non-destructive foreground** — animations are supplemental; terminal cells
   (text, colors, SGR attributes) remain fully interactive underneath.
2. **Background or foreground** — animations can render _behind_ text (as cell
   background replacement) or _in front of_ text (as foreground), with per-pixel
   alpha transparency.
3. **Cell-anchored** — animations are attached to a rectangular cell region and
   scroll with text, exactly like sixel images.
4. **High FPS** — the architecture must support 60 FPS with multiple concurrent
   animations without per-frame CPU rasterization overhead.
5. **Low memory fragmentation** — a pre-allocated arena/slab strategy avoids
   per-frame malloc/free churn.
6. **Terminal protocol conventions** — follows the existing bloom-vt OSC/DCS
   dispatch patterns; unrecognised sequences are safely ignored by other
   terminals.

### Ownership Model (mirrors sixel)

| Resource                 | Owner                       | Analogy to sixel               |
| ------------------------ | --------------------------- | ------------------------------ |
| Raw Lottie JSON body     | bloom-vt                    | N/A (sixel has no parse tree)  |
| ThorVG painter/surface   | bloom-vt                    | Sixel decoder state            |
| RGBA pixel buffer        | bloom-vt (`BvtLottie.rgba`) | `BvtSixel.rgba` (engine-owned) |
| GPU texture cache        | bloom-terminal              | Sixel texture cache            |
| Compositing / draw calls | bloom-terminal              | `render_sixel_images()`        |

bloom-vt rasterizes each frame into a pixel buffer and exposes it via
`bvt_get_lotties()` — identical to how sixel exposes `BvtSixel.rgba`. The host
uploads pixels to GPU textures and composites. This means:

- bloom-terminal has **zero Lottie-specific state** beyond the GPU texture cache.
- Adding a Lottie-capable host only requires: query `bvt_get_lotties()`, manage
  textures, draw. No ThorVG dependency in the host.
- The engine controls the full lifecycle: load → parse → rasterize → advance →
  evict. The host never drives rasterization.

---

## 1. Protocol Design (bloom-vt side)

### 1.1 Escape Sequence Format

Lottie animations use **APC** (Application Program Command, `ESC _`), per
ECMA-48 §7.2.1: "APC is the opening delimiter of a command string for an
application program."

The payload is a JSON object, base64-encoded to avoid escaping issues.

```
APC <base64-json> ST
```

In 7-bit representation:

```
ESC _ <base64-json> ESC \
```

Where `<base64-json>` decodes to a JSON command object (see §1.2).

**Rationale for APC over OSC/DCS:**

- **ECMA-48 semantics**: APC is defined as "command string for an application
  program" — a graphics protocol is exactly that. OSC is for operating system
  commands (window title, clipboard, palette); DCS is for device control
  (sixel streaming). A Lottie animation protocol is an application-level
  command, making APC the semantically correct channel.
- **Kitty precedent**: Kitty's graphics protocol uses APC (`ESC _ G ... ST`),
  establishing APC as the de facto channel for terminal graphics protocols.
- **Safe in other terminals**: xterm explicitly states "xterm implements no
  APC functions; Pt is ignored." All well-behaved terminals silently discard
  unknown APC strings, making it safe to emit from any application.
- **No namespace collision**: OSC codes are a flat numeric namespace with no
  registry — any chosen number could conflict with a future registered code.
  APC has no such collision risk since the entire string is application-defined.
- **DCS is reserved for sixel**: DCS supports structured parameters before
  the data string, but is already used for sixel's streaming decoder. Lottie
  JSON is an atomic document, not a streaming byte-by-byte format.

**Rationale for base64:**

- Lottie JSON contains `"`, `\`, and potentially characters that could
  conflict with the ST terminator. Base64 avoids all delimiter collisions.
- The base64 overhead (~33%) is acceptable because Lottie JSON is typically
  small (1–50 KB) and is transmitted once, not per-frame.

### 1.2 Command Vocabulary

Each APC payload is a JSON object with a `"cmd"` field:

#### `load` — Upload a Lottie animation

```json
{
  "cmd": "load",
  "id": 1,
  "lottie": { ... },       // Full Lottie JSON (inline)
  "placement": {
    "row": 5,
    "col": 10,
    "rows": 4,
    "cols": 8
  },
  "layer": "background",   // "background" | "foreground" (default: "foreground")
  "opacity": 0.85,         // 0.0–1.0 global opacity (default: 1.0)
  "play": {
    "speed": 1.0,          // playback rate multiplier (default: 1.0)
    "loop": true,          // loop after reaching end (default: true)
    "autostart": true      // start playing immediately (default: true)
  }
}
```

- `id`: positive integer (1–4294967295), client-assigned. Used for all
  subsequent references. Sending `load` with an existing `id` replaces the
  animation in-place (no flicker — uses version bump, like sixel animation).
- `lottie`: the complete Lottie JSON body. For large animations, use
  `load-chunk` (§1.2.5).
- `placement.row/col`: anchor cell (current cursor position if omitted).
- `placement.rows/cols`: cell rectangle the animation occupies. If omitted,
  computed from the Lottie canvas size (`w`/`h`) and the terminal's cell
  pixel dimensions.
- `layer`: `"background"` renders _behind_ cell text; `"foreground"` renders
  _in front of_ cell text. Default: `"foreground"`.
- `opacity`: global alpha multiplier applied at compositing time.

#### `place` — Place a previously loaded animation at a new cell region

```json
{
  "cmd": "place",
  "id": 1,
  "placement": {
    "row": 0,
    "col": 0,
    "rows": 4,
    "cols": 8
  },
  "layer": "background",
  "opacity": 0.9
}
```

- An animation can have **multiple placements** (like Kitty's placement model).
- Each placement is identified by `(id, placement_index)` — the engine
  auto-assigns `placement_index` sequentially.
- A placement can change `layer` and `opacity` independently of the source
  animation.

#### `play` / `pause` / `stop` — Control playback

```json
{ "cmd": "play",  "id": 1, "speed": 2.0, "loop": false }
{ "cmd": "pause", "id": 1 }
{ "cmd": "stop",  "id": 1 }   // resets to frame 0
```

- `speed`: playback rate multiplier (default: 1.0).
- `loop`: whether to loop (default: true).
- `pause` freezes at the current frame.
- `stop` resets to frame 0 and pauses.

#### `seek` — Jump to a specific frame

```json
{ "cmd": "seek", "id": 1, "frame": 15 }
```

- `frame`: 0-based frame index. Clamped to `[ip, op)` range of the animation.

#### `delete` — Remove an animation

```json
{ "cmd": "delete", "id": 1 }
```

- Removes the animation and all its placements. The pixel buffer is released
  to the spare pool. The host's GPU texture is freed on the next frame's
  `lottie_cache_reconcile` pass.

#### `load-chunk` — Upload a large animation in chunks

For Lottie files exceeding ~4 KB of base64, chunk the upload:

```json
{ "cmd": "load-chunk", "id": 1, "seq": 0, "total": 3, "data": "<base64-part-0>" }
{ "cmd": "load-chunk", "id": 1, "seq": 1, "total": 3, "data": "<base64-part-1>" }
{ "cmd": "load-chunk", "id": 1, "seq": 2, "total": 3, "data": "<base64-part-2>" }
```

- `seq`: 0-based chunk index.
- `total`: total number of chunks.
- The engine concatenates all `data` fields (after base64-decoding each)
  before parsing the Lottie JSON.
- A `load-chunk` with `seq == total - 1` triggers parsing, rasterization of
  frame 0, and placement (behaves like `load` at that point).
- If a new `load-chunk` with `seq == 0` arrives while a previous chunked
  upload is in progress, the previous upload is discarded.

### 1.3 Engine-Side State Machine

```
                         ┌──────────────────────┐
  APC ──────────────────►│ bvt_lottie_apc_      │
                         │ dispatch()           │
                         └──────────┬───────────┘
                                    │
                           base64-decode
                           JSON parse "cmd"
                                    │
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
        ┌──────────┐        ┌──────────────┐       ┌───────────┐
        │  load /  │        │  play/pause/ │       │  delete / │
        │  load-   │        │  stop/seek / │       │  place    │
        │  chunk   │        │  place       │       │           │
        └────┬─────┘        └──────┬───────┘       └─────┬─────┘
             │                     │                     │
             ▼                     ▼                     ▼
  ┌───────────────────┐   ┌──────────────────┐   ┌─────────────────┐
  │ Parse Lottie JSON │   │ Update playback  │   │ Remove record + │
  │ Init ThorVG       │   │ state in LtRec   │   │ destroy painter │
  │ Rasterize frame 0 │   │ Re-rasterize if  │   │ release rgba buf│
  │ Un-premultiply +  │   │ frame changed    │   │ release arena   │
  │ BGRA→RGBA +       │   │ version++        │   │ version++       │
  │ sRGB→linear       │   └──────────────────┘   └─────────────────┘
  │ Store in LtRec    │
  └───────────────────┘
```

**New internal structures in bloom-vt:**

```c
/* Lottie animation record — one per loaded animation.
 * Directly analogous to SxRec: owns pixel buffer, has id + version. */
typedef struct
{
    uint64_t id;             /* client-assigned id (stable cache key) */
    uint32_t version;        /* bumps on any state change */

    /* Raw Lottie JSON body — stored for ThorVG's tvg_picture_load_data().
     * ThorVG parses internally; no separate parse tree is needed. */
    void     *json_root;     /* unused (retained for struct compat) */

    /* Rasterized pixel buffer (engine-owned, like SxRec.rgba) */
    uint8_t  *rgba;          /* RGBA32 pixel data for current frame */
    size_t    rgba_cap;      /* allocated bytes */

    /* Design space from Lottie JSON (w, h fields) — abstract coordinate system */
    int       design_w;      /* Lottie animation width  */
    int       design_h;      /* Lottie animation height */

    /* Rasterization pixel dimensions (placement cells × cell pixel size) */
    int       px_w;          /* pixel width of rgba buffer */
    int       px_h;          /* pixel height of rgba buffer */

    /* Playback state */
    int       current_frame; /* 0-based, within [ip, op) */
    int       frame_ip;      /* Lottie in-point */
    int       frame_op;      /* Lottie out-point */
    double    frame_fr;      /* Lottie framerate */
    double    speed;         /* playback rate multiplier */
    bool      playing;       /* actively advancing frames? */
    bool      loop;          /* loop at end? */
    bool      dirty;         /* frame changed, rgba needs re-upload */

    /* Timing */
    uint64_t  last_tick_us;  /* last frame advance timestamp */

    /* Placements — cell regions where this animation is visible */
    BvtLottiePlacement *placements;
    int         placement_count;
    int         placement_cap;

    /* Arena for raw Lottie JSON body — passed to tvg_picture_load_data().
     * ThorVG parses JSON internally; no separate parse tree/arena is needed. */
    uint8_t  *arena_base;
    size_t     arena_offset;
    size_t     arena_cap;
} LtRec;

/* Chunked upload accumulator */
typedef struct
{
    uint64_t id;
    uint8_t *buf;
    size_t   buf_len;
    size_t   buf_cap;
    int      chunks_received;
    int      chunks_total;
} LtChunkAccum;

/* Global Lottie state (analogous to BvtSixelState) */
struct BvtLottieState
{
    LtRec         *recs;
    int            rec_count;
    int            rec_cap;
    uint64_t       next_placement_id;
    size_t         live_bytes;    /* sum of all LtRec.rgba_cap */

    LtChunkAccum  *chunks;       /* in-progress chunked uploads */
    int            chunk_count;
    int            chunk_cap;

    /* Spare buffer pool (like SxSpare) for rgba pixel buffers */
    LtSpare        spares[LT_SPARE_MAX]; /* 16 entries */
    int            spare_count;
    size_t         retain_bytes;

    /* Scratch buffer for bvt_get_lotties() snapshots */
    uint8_t       *scratch;
    size_t         scratch_cap;

    /* Scratch buffer for bvt_get_lottie_placements() snapshots */
    BvtLottiePlacement *pl_scratch;
    int            pl_scratch_cap;
};
```

### 1.4 Integration into bloom-vt APC dispatch

The parser already has a dedicated `BVT_STATE_APC_STRING` state that
accumulates bytes until ST/BEL terminates the string, then calls
`bvt_lottie_apc_dispatch()`:

```c
/* In parser.c — ESC _ routes to APC_STRING state.
 * On termination, dispatch internally to lottie. */
case BVT_STATE_APC_STRING:
    /* ST/BEL received → dispatch the accumulated APC payload */
    bvt_lottie_apc_dispatch(vt, p->apc_buf, p->apc_len);
    break;
```

The lottie protocol uses APC exclusively — there is no OSC 837 path.

The engine parses the JSON, manages animation state, **rasterizes frames**
(via ThorVG when available; RGBA buffer is zeroed when built with
`--disable-thorvg`), and exposes RGBA pixel data. The host queries the
current state each frame (§2.1).

### 1.5 Public API Additions (bloom_vt.h)

```c
/* A placement of a Lottie animation on the terminal grid. Anchored by
 * absolute line so the animation scrolls with text. `layer` is 0 for
 * foreground (drawn over text), 1 for background (drawn behind text).
 * `opacity_x256` is the per-placement opacity scaled to 0–255.
 * `id` is auto-assigned by the engine (stable, unique per placement). */
typedef struct
{
    uint64_t id;           /* auto-assigned stable placement id */
    long     abs_line;       /* absolute line index (engine-internal) */
    int      row;            /* display-relative row (abs_line - abs_top) */
    int      col;
    int      rows;           /* cell height */
    int      cols;           /* cell width */
    uint8_t  layer;          /* 0 = foreground, 1 = background */
    uint8_t  opacity_x256;   /* 0–255 */
} BvtLottiePlacement;

/* A Lottie animation snapshot, returned by bvt_get_lotties(). Engine-owned.
 * `rgba` is the rasterized RGBA32 pixel data for the current frame,
 * valid until the next bvt_input_write()/bvt_get_lotties() call.
 * Placements are queried separately via bvt_get_lottie_placements(). */
typedef struct
{
    uint64_t id;
    uint32_t version;
    int      canvas_w;       /* rasterization pixel width  (placement cols × cell_w_px) */
    int      canvas_h;       /* rasterization pixel height (placement rows × cell_h_px) */
    const uint8_t *rgba;     /* engine-owned RGBA32, valid until next mutation */
    int      current_frame;  /* current playback position */
    int      frame_count;    /* frame_op - frame_ip */
    bool     playing;        /* is the animation advancing? */
    double   speed;          /* playback rate multiplier */
    bool     loop;           /* loop at end? */
    int      placement_count;
} BvtLottie;

/* Query current state of all active Lottie animations.
 * Returned pointer valid until next bvt_input_write / bvt_get_lotties.
 * Each entry's .rgba is the rasterized pixels for the current frame.
 * Returns a contiguous array of BvtLottie — placements are queried
 * separately via bvt_get_lottie_placements(). */
const BvtLottie *bvt_get_lotties(BvtTerm *vt, int *count);

/* Query placements for a specific animation.
 * Returns a snapshot array with .row pre-computed for the renderer.
 * Valid until the next bvt_get_lottie_placements() call. */
const BvtLottiePlacement *bvt_get_lottie_placements(BvtTerm *vt, uint64_t id,
                                                     int *count);

/* Advance all playing animations to the frame appropriate for the given
 * timestamp, and re-rasterize any whose frame changed.
 * Call once per frame before bvt_get_lotties().
 * Returns true if any animation advanced (rgba content changed). */
bool bvt_lottie_tick(BvtTerm *vt, uint64_t now_us);

/* Notify Lottie subsystem of scroll (cull scrolled-off placements). */
void bvt_lottie_note_scroll(BvtTerm *vt, int lines);

/* Notify Lottie subsystem of display clear. */
void bvt_lottie_clear_display_rows(BvtTerm *vt, int top, int bot);

/* Free all Lottie state (painters, surfaces, arenas, pixel buffers).
 * Internal-only (declared in bloom_vt_internal.h, not the public header). */
void bvt_lottie_state_free(BvtTerm *vt);
```

**Comparison with sixel API:**

| sixel                            | lottie                                      | Notes                                   |
| -------------------------------- | ------------------------------------------- | --------------------------------------- |
| `bvt_get_sixels(vt, &count)`     | `bvt_get_lotties(vt, &count)`               | Same pull model, contiguous arrays      |
| `BvtSixel.id` / `.version`       | `BvtLottie.id` / `.version`                 | Same cache-key pattern                  |
| `BvtSixel.rgba`                  | `BvtLottie.rgba`                            | Engine-owned, valid until next mutation |
| `BvtSixel.row/col/layer`         | `BvtLottiePlacement.row/col/layer`          | Separate placement query API            |
| N/A (no tick)                    | `bvt_lottie_tick(vt, now_us)`               | New: frame advancement                  |
| N/A                              | `bvt_get_lottie_placements(vt, id, &count)` | Separate placement query with .row      |
| `bvt_sixel_note_scroll()`        | `bvt_lottie_note_scroll()`                  | Same cull logic                         |
| `bvt_sixel_clear_display_rows()` | `bvt_lottie_clear_display_rows()`           | Same clear logic                        |

### 1.6 Frame Advancement and Rasterization Model

`bvt_lottie_tick(vt, now_us)` is called by the host once per frame. It
advances playing animations and **re-rasterizes in-place** when the frame
changes:

```
For each LtRec where playing == true:
    elapsed = now_us - last_tick_us
    frame_delta = elapsed × speed × fr / 1_000_000
    new_frame = current_frame + floor(frame_delta)
    if new_frame >= op:
        if loop:
            new_frame = ip + (new_frame - ip) % (op - ip)
        else:
            new_frame = op - 1
            playing = false
    if new_frame != current_frame:
        current_frame = new_frame
        lt_rasterize(rec)            // re-rasterize into rec->rgba
        rec->dirty = true
        rec->version++
    last_tick_us += frame_delta × 1_000_000 / (speed × fr)
```

**`lt_rasterize(rec)` — in-engine rasterization:**

> **Note:** ThorVG is now linked (optional, auto-detected via pkg-config `thorvg-1`;
> pass `--disable-thorvg` to bloom-vt's `configure` to build without). The actual
> implementation follows the pseudocode below. RGBA pixels are pre-linearized
> (sRGB→linear) after rasterization so they composite correctly in the host's
> linear-light render pipeline. Without ThorVG, the RGBA buffer is zeroed.

Current implementation with ThorVG:

```c
static void lt_rasterize(LtRec *r) {
    // Set ThorVG animation frame
    tvg_animation_set_frame(r->tvg_anim, (float)r->current_frame);

    // Rasterize into our RGBA buffer via ThorVG SW canvas
    tvg_canvas_update(r->tvg_canvas);
    tvg_canvas_draw(r->tvg_canvas, true);
    tvg_canvas_sync(r->tvg_canvas);

    // ThorVG outputs premultiplied BGRA — convert to non-premultiplied
    // linear-light RGBA for correct compositing in the host renderer.
    // 1. Un-premultiply (divide RGB by alpha × 255)
    // 2. Swap R↔B (BGRA → RGBA byte order)
    // 3. Linearize each RGB channel (sRGB → linear)
    lt_linearize_rgba(r->rgba, r->px_w, r->px_h);
    r->dirty = true;
}
```

**Why engine-side timing and rasterization?**

- The engine owns the full lifecycle — identical to sixel's internal decoder.
- Multiple placements of the same animation share one rasterization (one
  `LtRec` → many `LtPlacement`). The host never re-rasterizes per-placement.
- The host has zero Lottie knowledge — it just sees `BvtLottie.rgba` pixels,
  exactly like it sees `BvtSixel.rgba` pixels.

---

## 2. Renderer Design (bloom-terminal side)

### 2.1 Data Flow Per Frame

```
Frame start:
  1. bvt_lottie_tick(vt, now_us)          // advance + re-rasterize
  2. bvt_damage_flush(vt)                 // damage callback as usual
  3. bvt_get_lotties(vt, &lottie_count)   // pull RGBA pixel snapshots
  4. bvt_get_sixels(vt, &sixel_count)     // pull sixel state (unchanged)

Rendering (inside draw_scene_linear):
  A. render_visible_cells()               // per-row: cell bg (skip under bg-lottie) + glyphs + decorations
  B. render_lottie_layer(data, term, 1)    // background-layer animations (alpha-blended over cells)
  C. render_lottie_layer(data, term, 0)    // foreground-layer animations (over everything)
  D. render_sixel_images()                // sixel images (unchanged)

  Linear→sRGB encode-out blit
```

**Two-pass Lottie rendering** solves the "background vs foreground" split:

- Background Lottie (layer=1) renders **after** all cell content (backgrounds,
  glyphs, decorations) with alpha blending. For cells covered by a
  background-layer placement, the cell's solid background is skipped during
  pass A (`cell_under_bg_lottie` check), so the animation replaces the cell
  background. Text on top is then alpha-blended with the animation underneath.
- Foreground Lottie (layer=0) renders **after** background Lottie, so the
  animation appears in front of everything including text.

### 2.2 Texture Cache (identical pattern to sixel)

The host manages a GPU texture cache keyed by `id`, versioned by `version`,
and updated when pixels change. **This is the exact same pattern as
`sixel_cache`** — the host never rasterizes, only uploads and composites.

```c
struct {
    SDL_Texture *texture;
    uint64_t     id;           /* matches BvtLottie.id */
    uint32_t     version;      /* matches BvtLottie.version */
    int          w, h;         /* pixel dimensions */
} lottie_cache[LOTTIE_CACHE_MAX];  /* 64 entries */
int lottie_cache_count;
```

**Cache operations** (mirror `sixel_cache_reconcile`, `sixel_get_texture`):

| Operation                     | Sixel equivalent          | Lottie behavior                             |
| ----------------------------- | ------------------------- | ------------------------------------------- |
| Reconcile                     | `sixel_cache_reconcile()` | Evict textures whose `id` is no longer live |
| Hit by id, same version       | Return cached texture     | Return cached texture                       |
| Hit by id, version changed    | `SDL_UpdateTexture()`     | `SDL_UpdateTexture()` from `BvtLottie.rgba` |
| Hit by id, dimensions changed | Destroy + recreate        | Destroy + recreate                          |
| Miss                          | Create + upload           | Create + upload from `BvtLottie.rgba`       |

**Per-frame cost analysis:**

| Scenario           | Engine work               | Host work                                     |
| ------------------ | ------------------------- | --------------------------------------------- |
| Paused animation   | None                      | 1 `SDL_RenderTexture` per placement           |
| Playing, new frame | ThorVG rasterize + memcpy | 1 `SDL_UpdateTexture` + 1 `SDL_RenderTexture` |
| Deleted animation  | None                      | `lottie_cache_reconcile` evicts texture       |

A **paused** animation costs exactly the same as a sixel image — just a
texture blit. A **playing** animation at 30 FPS incurs one ThorVG
rasterization (in bloom-vt) + one GPU texture upload (in bloom-terminal) per
frame.

### 2.3 Rendering Integration

A single parameterized function handles both layers:

```c
// Render Lottie animations for a given layer.
// layer=1: background (alpha-blended after cells, cell bg skipped underneath)
// layer=0: foreground (drawn over everything)
static void render_lottie_layer(RendererSdl3Data *data, TerminalBackend *term, int layer);
```

**Placement rendering** (mirrors `render_sixel_images`):

```c
const BvtLottie *lotties = terminal_get_lotties(term, &lottie_count);
lottie_cache_reconcile(data, lotties, lottie_count);

for (int i = 0; i < lottie_count; i++) {
    const BvtLottie *l = &lotties[i];
    SDL_Texture *tex = lottie_get_texture(data, l);  // cache lookup / upload
    if (!tex) continue;

    int pl_count = 0;
    const BvtLottiePlacement *places =
        terminal_get_lottie_placements(term, l->id, &pl_count);

    for (int j = 0; j < pl_count; j++) {
        if (places[j].layer != target_layer) continue;

        int screen_row = places[j].abs_line - abs_top + scroll_offset;
        int px = places[j].col * cell_width;
        int py = screen_row * cell_height;
        int dst_w = places[j].cols * cell_width;
        int dst_h = places[j].rows * cell_height;

        // Frustum cull
        if (py + dst_h <= 0 || py >= height) continue;
        if (px + dst_w <= 0 || px >= width)  continue;

        // Per-placement opacity
        SDL_SetTextureAlphaModFloat(tex, places[j].opacity_x256 / 255.0f);

        SDL_FRect dst = { px, py, dst_w, dst_h };
        SDL_RenderTexture(renderer, tex, NULL, &dst);
    }
}
```

### 2.4 Linear-Light Considerations

Lottie animations with semi-transparent pixels must composite correctly in
the linear-light pipeline. The implementation uses **Option A** (pre-linearize
at rasterization time).

**Actual `lt_linearize_rgba` pipeline** (called after ThorVG rasterization):

1. **Un-premultiply**: ThorVG outputs premultiplied alpha (BGRA byte order on
   little-endian). Each RGB channel is divided by alpha and scaled to 255 to
   reverse premultiplication.
2. **Swap R↔B**: ThorVG's BGRA byte order is converted to the RGBA byte order
   expected by the host renderer.
3. **sRGB→linear**: Each RGB channel is linearized using the sRGB transfer
   function (`s/12.92` for low values, `((s+0.055)/1.055)^2.4` otherwise).
   Alpha is left unchanged (non-premultiplied).

The host uploads already-linearized RGBA pixels; the GPU texture is in linear
space. Alpha compositing in the linear render target is then correct. For
background animations with text on top, this is critical for correct alpha
blending at edges.

**Alternative (not used): GPU-side linearization.** Upload sRGB pixels, but
create the texture with `SDL_COLORSPACE_SRGB_LINEAR` so SDL3 linearizes on
sample. This would require SDL3 ≥ 3.2.0 with correct colorspace texture
support and adds host-side complexity with no benefit over pre-linearization.

### 2.5 TerminalBackend Interface Additions

```c
/* In term.h — new function pointers in TerminalBackend */
struct TerminalBackend {
    // ... existing pointers ...

    const BvtLottie *(*get_lotties)(TerminalBackend *term, int *count);
    const BvtLottiePlacement *(*get_lottie_placements)(TerminalBackend *term, uint64_t id, int *count);
    bool (*lottie_tick)(TerminalBackend *term, uint64_t now_us);
};

/* In term.h — new thin wrappers */
const BvtLottie *terminal_get_lotties(TerminalBackend *term, int *count);
const BvtLottiePlacement *terminal_get_lottie_placements(TerminalBackend *term, uint64_t id, int *count);
bool terminal_lottie_tick(TerminalBackend *term, uint64_t now_us);

/* In term_bvt.c — bridge implementations */
static const BvtLottie *bvt_back_get_lotties(TerminalBackend *term, int *count) {
    BvtBackendData *d = term->backend_data;
    return bvt_get_lotties(d->vt, count);
}
static const BvtLottiePlacement *bvt_back_get_lottie_placements(TerminalBackend *term, uint64_t id, int *count) {
    BvtBackendData *d = term->backend_data;
    return bvt_get_lottie_placements(d->vt, id, count);
}
static bool bvt_back_lottie_tick(TerminalBackend *term, uint64_t now_us) {
    BvtBackendData *d = term->backend_data;
    return bvt_lottie_tick(d->vt, now_us);
}
```

---

## 3. Memory Strategy

### 3.1 Problem Statement

Lottie animations have three distinct memory pressure points:

1. **Raw Lottie JSON** — the Lottie JSON body, potentially large (50+ KB
   per animation), stored in the per-animation arena and passed to ThorVG's
   `tvg_picture_load_data()`. This is _static_ for the animation's lifetime.
   ThorVG parses internally; no separate parse tree is maintained.
2. **ThorVG painter + surface** — internal rasterization state, opaque to us.
   One per animation, alive for the animation's lifetime.
3. **RGBA pixel buffer** — canvas, size = `w × h × 4`. Owned by bloom-vt
   (`LtRec.rgba`), reused across frames (no per-frame allocation).
4. **GPU texture** — owned by the host renderer, one per animation. Managed by
   the existing cache reconciliation pattern.

The primary fragmentation risk is repeated alloc/free of JSON trees and pixel
buffers as animations are loaded and deleted.

### 3.2 Arena for Raw Lottie JSON Body

The current implementation stores the raw Lottie JSON body in a `malloc`'d
buffer (`LtRec.arena_base`) and passes it to ThorVG's
`tvg_picture_load_data()`. ThorVG parses JSON internally; no separate
parse tree or arena allocator is needed. The buffer is freed on animation
deletion.

The full arena allocator described below was originally planned for a
custom JSON parser. It is **no longer needed** since ThorVG handles parsing
internally, but the design is retained for reference.

**Design: Per-animation arena with slab-allocated nodes.**

Instead of using `malloc()` for each JSON node during parsing, the engine uses
a **bump arena** per animation. When an animation is deleted, the entire arena
is reset in one operation — no individual frees, zero fragmentation.

```c
typedef struct {
    uint8_t *base;          /* mmap'd or malloc'd block */
    size_t   offset;        /* bump pointer */
    size_t   capacity;      /* total size */
} LtArena;

/* Allocate from arena — O(1), no fragmentation, no per-node metadata */
static void *arena_alloc(LtArena *a, size_t size, size_t align) {
    size_t aligned = (a->offset + align - 1) & ~(align - 1);
    if (aligned + size > a->capacity) return NULL;  /* out of space */
    void *ptr = a->base + aligned;
    a->offset = aligned + size;
    return ptr;
}

/* Reset entire arena — O(1), all nodes freed at once */
static void arena_reset(LtArena *a) {
    a->offset = 0;
}

/* Destroy arena — one free() for entire parse tree */
static void arena_destroy(LtArena *a) {
    free(a->base);
    *a = (LtArena){0};
}
```

**Arena sizing strategy:**

- On `load`, estimate arena size as `base64_len × 1.5` (JSON expansion ratio).
- If parsing exceeds the arena, double the arena and re-parse (rare).
- Typical Lottie: 5–50 KB JSON → 7.5–75 KB arena. Negligible.

**Why not a global arena?**

- Per-animation arenas allow O(1) deletion of a single animation without
  disturbing others.
- A global arena would require compacting or accepting waste from deleted
  animations.

### 3.3 Spare Buffer Pool for RGBA Pixel Buffers (Engine Side)

Following the exact pattern of bloom-vt's `SxSpare` free-list pool. The pool
lives in `BvtLottieState` (engine-owned), not in the host:

```c
typedef struct {
    uint8_t *buf;
    size_t   cap;
} LtSpare;

#define LT_SPARE_MAX     16
#define LT_RETAIN_MAX    (64 * 1024 * 1024)  /* 64 MiB */

/* lt_buf_alloc: find best-fit spare, or fresh bvt_alloc */
/* lt_buf_release: retain into pool if under budget, else bvt_dealloc */
```

**Why 64 MiB retain limit?** Lottie animations have larger canvases than sixel
(sixel is typically small inline images; Lottie may fill 4×8 cells at HiDPI =
~300×600 pixels = 720 KB per buffer). 64 MiB retains ~90 frames worth of
buffers — sufficient for typical concurrent animation counts while bounding
memory.

**Animation replacement reuses the buffer in-place**, exactly like sixel:

- If `rec->rgba_cap >= need`: reuse `rec->rgba` (no alloc).
- If `rec->rgba_cap < need`: release old buffer (→ spare pool or free),
  allocate new (possibly from spare pool).

### 3.4 Memory Budgets

| Resource                     | Budget        | Rationale                                           |
| ---------------------------- | ------------- | --------------------------------------------------- |
| Max concurrent animations    | 64            | `LT_MAX_ANIMS` — covers all reasonable terminal UIs |
| Max placements per animation | 32            | `LT_MAX_PLACEMENTS` — rare to need more             |
| Live pixel data (engine)     | 128 MiB       | `LT_LIVE_MAX` — matches sixel budget                |
| Spare buffer pool (engine)   | 64 MiB        | `LT_RETAIN_MAX` — retains buffers for reuse         |
| Per-animation JSON arena     | No hard limit | Soft: reject if arena > 2 MiB                       |
| GPU texture cache (host)     | 64 entries    | `LOTIE_CACHE_MAX` — 64 animations × 1 texture       |

**Budget enforcement:**

- When a new animation would exceed `LT_MAX_ANIMS`, evict the animation with
  the oldest `abs_line` (same LRU-by-scroll-position as sixel).
- When `LT_LIVE_MAX` would be exceeded, evict oldest animations until budget
  is clear.

### 3.5 Frame-Advance Memory Cost: Zero

The critical design property: **advancing a frame produces no allocations.**

- The raw Lottie JSON body is immutable after `load`.
- ThorVG rasterizes in-place into the surface, then `memcpy` into the
  pre-allocated `rec->rgba` buffer.
- The GPU texture is updated in-place via `SDL_UpdateTexture()`.
- The `version++` bump signals the host to re-upload pixels, but no new
  memory is allocated in the common case.

The only allocation that occurs during playback is if the **canvas size
changes** (which cannot happen for a given Lottie file — `w`/`h` are fixed),
or if a new animation is loaded while the spare pool is empty.

### 3.6 Summary: Allocation Points

| Event                           | Allocations                                                     | Deallocations                                                                             |
| ------------------------------- | --------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `load`                          | 1 arena, 1 RGBA buffer, 1 ThorVG painter+surface, 1 GPU texture | 0                                                                                         |
| `delete`                        | 0                                                               | arena free, RGBA buffer → spare pool or free, ThorVG painter destroy, GPU texture destroy |
| Frame advance                   | 0                                                               | 0                                                                                         |
| Replace (`load` with same `id`) | 0 (arena reset + reuse, RGBA buffer reuse, painter reuse)       | 0                                                                                         |
| Scroll-off cull                 | 0                                                               | arena free, RGBA buffer → spare, painter destroy, GPU texture destroy                     |

**Per-animation lifetime allocations: 4** (arena buffer, RGBA buffer, ThorVG
painter+surface, GPU texture).
**Per-frame allocations during playback: 0.**

### 3.7 Comparison with Sixel Memory Model

| Aspect                          | Sixel                     | Lottie                      |
| ------------------------------- | ------------------------- | --------------------------- |
| Pixel data owner                | `SxRec.rgba` (engine)     | `LtRec.rgba` (engine)       |
| Pixel buffer pool               | `SxSpare[16]` (engine)    | `LtSpare[16]` (engine)      |
| Budget enforcement              | `SX_LIVE_MAX` 128 MiB     | `LT_LIVE_MAX` 128 MiB       |
| Decode/rasterize state          | Sixel decoder (internal)  | ThorVG painter (internal)   |
| Version bump on change          | `SxRec.version++`         | `LtRec.version++`           |
| Buffer reuse on replace         | `r->cap >= need` check    | `r->rgba_cap >= need` check |
| GPU texture cache               | `sixel_cache[256]` (host) | `lottie_cache[64]` (host)   |
| Texture update on change        | `SDL_UpdateTexture`       | `SDL_UpdateTexture`         |
| Per-frame alloc during playback | 0                         | 0                           |

---

## 4. Interaction with Existing Subsystems

### 4.1 Cell Ownership

Lottie animations **do not replace or modify cells**. The cells beneath an
animation retain their text, colors, and SGR attributes. The animation is a
supplemental visual layer.

**User experience:**

- **Background layer**: the animation is alpha-blended over all cell content
  (backgrounds + text + decorations). For cells covered by a background-layer
  placement, the cell's solid background color is skipped during
  `render_visible_cells` (via `cell_under_bg_lottie`), so the animation
  replaces the cell background. Text drawn on top then composites correctly
  against the animation.
- **Foreground layer**: animation renders over all cell content with alpha
  blending. Text is fully visible through transparent regions of the animation.

**Actual render pipeline order** (per frame in `draw_scene_linear`):

1. `render_visible_cells()` — per-row: cell backgrounds (skipped under bg-lottie),
   glyphs, cursors, selection, underlines, strikethroughs
2. `render_lottie_layer(data, term, 1)` — background-layer animations (alpha-blended)
3. `render_lottie_layer(data, term, 0)` — foreground-layer animations
4. `render_sixel_images()` — sixel images

### 4.2 Scroll Behavior

Lottie placements scroll identically to sixel images:

- Anchored by `abs_line` in bloom-vt's absolute coordinate space.
- `bvt_lottie_note_scroll()` culls placements that scrolled past scrollback.
- The host renderer maps `abs_line` → display row using the same formula as
  sixel: `screen_row = abs_line - abs_top + scroll_offset`.

### 4.3 Clear Behavior

- `ED` (erase display): `bvt_lottie_clear_display_rows()` removes foreground
  placements in the cleared row range. Background placements survive (text
  erase should not remove background decorations).
- Terminal reset: `bvt_lottie_clear_all()` removes all animations (internal-only,
  called from `bvt_free()`; not exposed in the public `bloom_vt.h` header).

### 4.4 Selection and Cursor

- Selection highlights render **on top of** background Lottie but **below**
  foreground Lottie. This matches the existing z-order: selection is part of
  the cell rendering pipeline, which sits between the two Lottie passes.
- The cursor renders in its current position within the cell pipeline. A
  foreground Lottie may obscure the cursor — this is acceptable and matches
  the existing sixel foreground behavior.

### 4.5 Resize

On terminal resize:

- bloom-vt's cell pixel dimensions (`cell_w_px`/`cell_h_px`) are updated
  via `bvt_set_cell_pixels()` with new cell dimensions.
- **Existing placements are not re-rasterized.** The pixel dimensions
  (`px_w`/`px_h`) of existing animations remain based on the cell size at
  the time of placement. Only new placements use the updated cell pixel
  dimensions.
- `design_w`/`design_h` (from Lottie JSON) remain unchanged for all
  animations.
- **Future improvement**: re-rasterize existing placements when cell pixel
  dimensions change, so animations scale correctly after resize.

### 4.6 Alternate Screen

Switching to alternate screen (mode 1049) should clear all Lottie placements
in the alternate screen, matching sixel behavior.

---

## 5. Complete Protocol Examples

### 5.1 Simple Foreground Spinner

```
ESC _ eyJjbWQiOiJsb2FkIiwiaWQiOjEsImxvdHRpZSI6eyJ2IjoiNS42LjAiLCJmciI6MzAsImlwIjowLCJvcCI6OTAsInciOjQwLCJoIjo0MCwibGF5ZXJzIjpbXX0sInBsYWNlbWVudCI6eyJyb3ciOjUsImNvbCI6MTAsInJvd3MiOjIsImNvbHMiOjJ9LCJsYXllciI6ImZvcmVncm91bmQifQ== ESC \
```

### 5.2 Background Animation with Opacity

```json
{
  "cmd": "load",
  "id": 2,
  "lottie": { ... },
  "placement": { "row": 0, "col": 0, "rows": 24, "cols": 80 },
  "layer": "background",
  "opacity": 0.3
}
```

### 5.3 Pause and Seek

```
APC eyJjbWQiOiJwYXVzZSIsImlkIjoxfQ== ST
APC eyJjbWQiOiJzZWVrIiwiaWQiOjEsImZyYW1lIjoxNX0= ST
```

### 5.4 Multiple Placements

```json
{ "cmd": "load",  "id": 3, "lottie": { ... } }
{ "cmd": "place", "id": 3, "placement": { "row": 0, "col": 0, "rows": 2, "cols": 2 }, "layer": "foreground" }
{ "cmd": "place", "id": 3, "placement": { "row": 0, "col": 78, "rows": 2, "cols": 2 }, "layer": "foreground" }
```

### 5.5 Cleanup

```json
{ "cmd": "delete", "id": 1 }
{ "cmd": "delete", "id": 2 }
{ "cmd": "delete", "id": 3 }
```

---

## 6. Implementation Plan

### Phase 1: bloom-vt — Protocol, State, and Rasterization ✅

1. ✅ Add ThorVG dependency to bloom-vt's build system (`--disable-thorvg`, auto-detect via pkg-config).
2. ✅ Add `BvtLottieState` to `BvtTerm` internal struct (lazy, `vt->lottie`).
3. ✅ Add APC dispatch in `parser.c` → `bvt_lottie_apc_dispatch()`.
4. ✅ Implement `lottie.c`:
   - ✅ `bvt_lottie_apc_dispatch()` — base64 decode, JSON parse, route by `cmd`.
   - ✅ `lt_cmd_load()` — parse Lottie JSON fields, create `LtRec`, init ThorVG, rasterize first frame.
   - ✅ `lt_cmd_place()` — add placement to animation.
   - ✅ `lt_cmd_play/pause/stop/seek()` — playback state changes.
   - ✅ `lt_cmd_delete()` — release rgba buffer, remove record.
   - ✅ `lt_cmd_load_chunk()` — chunked upload with accumulator.
   - ✅ `bvt_lottie_tick()` — advance playing animations, mark dirty.
   - ✅ `lt_rasterize()` — ThorVG integration with un-premultiply + BGRA→RGBA swap + sRGB→linear pre-linearization.
   - ✅ `bvt_lottie_note_scroll()` / `bvt_lottie_clear_display_rows()`.
   - ✅ `bvt_get_lotties()` / `bvt_get_lottie_placements()` — snapshot queries.
5. ✅ Integrate spare buffer pool (mirrors `SxSpare`).
6. ✅ Add `bvt_lottie_state_free()` to `bvt_free()`.
7. ✅ Arena: raw JSON storage passed to `tvg_picture_load_data`; full arena allocator not needed.
8. ✅ Engine-side unit tests — `test_bvt_lottie` with 22 test cases.

### Phase 2: bloom-terminal — Texture Cache and Compositing ✅ (done)

1. ✅ Add `get_lotties`, `get_lottie_placements`, `lottie_tick` to
   `TerminalBackend` vtable + bridge in `term_bvt.c`.
2. ✅ Add `lottie_cache[]` and management functions in `rend_sdl3.c` (mirror
   `sixel_cache`).
3. ✅ Implement `render_lottie_layer()` (handles both foreground and background).
4. ✅ Integrate into `draw_scene_linear()` pipeline.
5. ✅ End-to-end test via `test_lottie.c` (14 test cases).

### Phase 3: Polish ✅

1. ✅ Skip cell bg draw under background-layer placements (overdraw optimization — `cell_under_bg_lottie` in `rend_sdl3.c`).
2. ✅ Resize handling — design space (from JSON `w`/`h`) is separate from rasterization pixel dimensions (placement cells × cell pixel size). Existing placements are **not** re-rasterized on cell-pixel change; only new placements use the updated dimensions. See §4.5.
3. ✅ Budget enforcement and eviction (`lt_evict_to_budget`, mirrors sixel).
4. ⏳ Performance profiling — ensure 60 FPS with 10+ concurrent animations.
5. ✅ ~~Configuration option~~ — removed; `--disable-thorvg` at build time is sufficient (mirrors sixel which has no runtime toggle).
6. ✅ Pre-linearize RGBA in `lt_rasterize()` for correct linear-light compositing.
7. ✅ ThorVG integration — actual pixel rasterization via C API (`tvg_animation_*`, `tvg_swcanvas_*`).
8. ✅ Full arena allocator — no longer needed; ThorVG parses JSON internally via `tvg_picture_load_data()`.
9. ✅ `BvtConfig` refactor — `bvt_new()` now takes `const BvtConfig *cfg` instead of positional parameters. Cell pixel dimensions (`cell_w_px`/`cell_h_px`) are required at creation; `scrollback`, `reflow`, and `ambiguous_wide` are configurable via the struct. Removed zero-fallback paths in sixel/CSI code.

---

## 7. Alternatives Considered

### APC (chosen) vs OSC

Using APC (`ESC _ ... ST`) is the semantically correct choice per ECMA-48.
See §1.1 for the full rationale. The original design used OSC 837 because
bloom-vt's parser already routed unrecognised OSC codes to a callback while
APC was swallowed. Adding APC dispatch to the parser is a trivial change
(same accumulator pattern as OSC), and doing so aligns with ECMA-48 semantics
and Kitty's graphics protocol precedent.

### DCS instead of OSC/APC

DCS supports structured parameters before the data string and is already used
for sixel (streaming). Lottie JSON is an atomic document, not a streaming
format, and doesn't benefit from DCS parameter structure. DCS is also already
reserved for sixel's internal decoder.

### Direct pixel streaming (like sixel)

Instead of sending Lottie JSON and rasterizing in the engine, the client could
pre-rasterize frames and send them as pixel data (like sixel animation).
**Rejected because:**

- Lottie JSON is typically 1–50 KB; pre-rasterized RGBA frames at 4×8 cells
  HiDPI are ~300×600×4 = 720 KB _per frame_. A 30-frame animation would be
  ~21 MB of pixel data vs ~50 KB of JSON.
- The JSON approach enables resolution-independent rendering (crisp at any
  DPI or zoom level).
- Frame composition (Lottie's `c`/`r` partial frames) would need to be done
  client-side, duplicating effort.

### Host-side rasterization (original design)

The original design placed rasterization in bloom-terminal with ThorVG linked
only to the host. **Revised to engine-side** because:

- It creates an inconsistency with sixel: the host must manage ThorVG state
  (painter, surface, pixel buffer) tightly coupled to the animation lifecycle
  that bloom-vt controls.
- A Lottie-capable host requires ThorVG as a dependency. With engine-side
  rasterization, any host that can consume `BvtLottie.rgba` (like it consumes
  `BvtSixel.rgba`) works with zero Lottie-specific code beyond texture cache.
- bloom-vt already has an internal sixel decoder — ThorVG is the analogous
  internal Lottie decoder.
- The engine can correctly manage canvas resize, frame timing, and pixel
  buffer reuse without cross-process coordination.

### Host-side JSON parsing (no Lottie awareness in bloom-vt)

The engine could fire an `osc` callback with the raw payload and let the host
handle everything. **Rejected because:**

- Animation state (playback, timing, loop) is grid-coupled — it must scroll
  with text, be culled on scroll-off, and cleared on ED. These are engine
  responsibilities (same rationale as sixel being internal to bloom-vt).
- Cell-anchoring and placement management are naturally part of the VT engine's
  coordinate system.
- The host should not need to understand absolute line coordinates or
  scrollback integration.

### GPU-side vector rendering

ThorVG supports a SwRaster backend only (CPU). GPU vector rendering (Vello,
Pathfinder) would eliminate the CPU rasterization step but introduces
significant complexity and GPU-specific code. This is a viable future
enhancement — the `version`/`rgba` cache invalidation model works identically
whether the pixels come from CPU or GPU rasterization.

---

## 8. Open Questions

1. **JSON parser choice**: ✅ Resolved — ThorVG parses JSON internally via
   `tvg_picture_load_data()`. The engine does not maintain its own parse tree;
   the arena stores raw JSON bytes for ThorVG consumption. A lightweight JSON
   parser (`lt_json_find_key`) is used only for extracting top-level command
   fields (`cmd`, `id`, `lottie`, `placement`, etc.) from the base64-decoded
   payload.

2. **Text layers**: ⏳ Skipped — Lottie text layers with embedded font data
   are not a priority for terminal UI animations (spinners, icons, progress
   indicators). ThorVG handles them internally but they remain untested.
   May revisit if a concrete use case arises.

3. **Precomp caching**: Lottie precompositions are rendered multiple times
   if referenced by multiple layers. ThorVG handles this internally, but
   if we ever move to a custom renderer, precomp caching would be needed.

4. **HiDPI scaling**: ✅ Resolved — the rasterization uses `px_w`/`px_h` (placement cells × cell_pixels) rather than the Lottie JSON's design-space `w`/`h`. ThorVG scales from design space to pixel dimensions via `tvg_picture_set_size()`.
   This naturally accounts for DPI since the cell pixel dimensions are known at
   creation time via `BvtConfig.cell_w_px`/`cell_h_px` (and updated on resize
   via `bvt_set_cell_pixels()`). **Caveat**: existing placements are not
   re-rasterized when cell pixels change; only new placements pick up the
   updated dimensions.

5. **Concurrent rasterization**: For many animations, rasterization could be
   parallelized across threads. ThorVG is not thread-safe per-painter, but
   multiple painters can run concurrently. This is a Phase 3 optimization.

6. **ThorVG dependency scope**: ✅ Resolved — optional via `--disable-thorvg`
   at build time (auto-detected via pkg-config `thorvg-1`). APC sequences are
   still accepted without ThorVG; the RGBA buffer is zeroed.
