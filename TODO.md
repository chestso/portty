# TODO - portty

---

## 1. SDL3 GPU API Migration

### Current state (not this migration)

We render through SDL's **high-level** `SDL_Renderer`, created with the
`"gpu"` driver name (`platform_sdl3.c:757`). On Linux that backend resolves
to Vulkan — this is the "we went all Vulkan" change. It bought us
**linear-light glyph blending** (the gpu/vulkan renderer honors SRGB_LINEAR
float render targets; `rend_sdl3.c:950-956`).

But the glyph draw loop is still one `SDL_RenderTexture` per cell plus
`SDL_RenderFillRect` per background/underline/cursor/selection
(`rend_sdl3.c` `render_cell`). SDL batches these into vertex buffers
internally, but we issue per-cell API calls and don't own the pipeline or
shaders. The DMA-BUF Vulkan code only handles buffer export/handoff — it
does not draw glyphs.

So "Vulkan" here is the **render backend**, not this item. This migration is
about programming the **low-level `SDL_GPU` API directly** (`SDL_CreateGPUDevice`,
our own pipeline + SPIR-V shaders, `SDL_DrawGPUPrimitivesInstanced`).

### What this would add

- **Instanced rendering**: one `GlyphInstance` array for the whole grid,
  drawn in a single instanced call instead of N per-cell `SDL_RenderTexture`
  calls — collapses CPU draw-submission from O(cells) to ~O(1) per frame.
- Our own fragment shader (could fold the gamma/coverage curve and color in
  directly rather than leaning on SDL's render pipeline).
- More direct control over CPU↔GPU sync.

The payoff is **CPU draw-call overhead**, not GPU work or correctness — and
it's already blunted by the damage-driven, VSync-off, event-driven design
(we only redraw on change, and SDL already batches). Worth doing only if
profiling shows draw submission is a bottleneck (very large grids, sustained
full-screen scroll). Keep as a profile-gated optimization, not a default.

### Goals

- Migrate to SDL3 GPU API for lower-level control
- Reduce CPU-GPU synchronization overhead
- Enable instanced rendering

### Technical Design

#### 1.1 Instanced Rendering

Draw all glyphs in one GPU call using instancing.

```c
typedef struct GlyphInstance {
    float x, y;                    // Screen position
    float u0, v0, u1, v1;          // Atlas UV coordinates
    float width, height;           // Glyph dimensions
    uint8_t r, g, b, a;           // Color
} GlyphInstance;

// Build instance array for entire terminal screen
void build_glyph_instances(Terminal *term, GlyphInstance *instances, int *count) {
    int idx = 0;
    for (int row = 0; row < term->rows; row++) {
        for (int col = 0; col < term->cols; col++) {
            TermCell *cell = get_cell(term, row, col);

            // Lookup glyph in atlas
            AtlasCell *atlas_cell = atlas_lookup(atlas, cell->codepoint);

            instances[idx].x = col * cell_width;
            instances[idx].y = row * cell_height;
            instances[idx].u0 = atlas_cell->u0;
            instances[idx].v0 = atlas_cell->v0;
            instances[idx].u1 = atlas_cell->u1;
            instances[idx].v1 = atlas_cell->v1;
            instances[idx].width = atlas_cell->width;
            instances[idx].height = atlas_cell->height;
            instances[idx].r = cell->fg_r;
            instances[idx].g = cell->fg_g;
            instances[idx].b = cell->fg_b;
            instances[idx].a = 255;

            idx++;
        }
    }
    *count = idx;
}

// Single draw call for all glyphs
void render_terminal_gpu(SDL_GPUDevice *device, GlyphInstance *instances, int count) {
    // Upload instance data to GPU buffer
    SDL_GPUBuffer *instance_buffer = upload_instances(device, instances, count);

    // Bind atlas texture
    SDL_BindGPUFragmentSamplers(render_pass, 0, &atlas_binding, 1);

    // Draw instanced (one call for entire screen!)
    SDL_DrawGPUPrimitivesInstanced(render_pass, 6, count, 0, 0);
}
```

#### 1.2 Graphics Pipeline Setup

**Vertex Shader:**

```glsl
#version 450

layout(location = 0) in vec2 in_pos;        // Quad vertex (0,0) to (1,1)
layout(location = 1) in vec2 in_uv;         // Quad UV

// Per-instance attributes
layout(location = 2) in vec2 instance_pos;
layout(location = 3) in vec4 instance_uv;   // u0, v0, u1, v1
layout(location = 4) in vec2 instance_size;
layout(location = 5) in vec4 instance_color;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

layout(push_constant) uniform PushConstants {
    mat4 projection;                        // Orthographic projection
} pc;

void main() {
    // Calculate screen position
    vec2 screen_pos = instance_pos + in_pos * instance_size;
    gl_Position = pc.projection * vec4(screen_pos, 0.0, 1.0);

    // Calculate atlas UV
    frag_uv = mix(instance_uv.xy, instance_uv.zw, in_uv);
    frag_color = instance_color;
}
```

**Fragment Shader:**

```glsl
#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D atlas_texture;

void main() {
    float alpha = texture(atlas_texture, frag_uv).a;
    out_color = vec4(frag_color.rgb, alpha * frag_color.a);
}
```

### Implementation Plan

1. **Phase 1: GPU Device Setup** — Initialize SDL_GPUDevice, create swap chain, set up render targets
2. **Phase 2: Pipeline Creation** — Compile shaders (SPIR-V), create graphics pipeline, set up vertex/instance buffers
3. **Phase 3: Integration** — Replace SDL_Renderer calls, benchmark vs current implementation

---

## 2. Variable Font Axes

The variation axis infrastructure is fully implemented: `cache_mm_var()` caches axis
metadata, `ft_set_variation_axis()` / `ft_set_variation_axes()` set values, and
`apply_font_variations()` applies per-style overrides. The `wght` axis is used for bold,
and `ital`/`slnt` axes are used for italic rendering. Remaining work is to wire up
additional axes at the application level.

### Optical Size (opsz)

**Range:** 6pt to 144pt
**Use Case:** Optimize rendering for different font sizes. Small terminals get more open spacing, large terminals get finer details.

```c
void set_optical_size(FtFontData *ft_data, float font_size_pt) {
    ft_set_axis_value(ft_data, "opsz", font_size_pt);
}
```

### Grade (GRAD)

**Range:** -200 to 200
**Use Case:** Adjust weight without changing metrics (for accessibility). User preference for "high contrast mode" without changing cell sizes.

```c
void adjust_for_readability(FtFontData *ft_data, int grade) {
    ft_set_axis_value(ft_data, "GRAD", (float)grade);
}
```

### Custom Axes

Support for font-specific axes (e.g., "ROND" for roundness, "SOFT" for softness).

```c
bool ft_set_custom_axis(FtFontData *ft_data, uint32_t tag, float value) {
    for (int i = 0; i < ft_data->num_axes; i++) {
        if (ft_data->axes[i].tag == tag) {
            if (value < ft_data->axes[i].min_value ||
                value > ft_data->axes[i].max_value) {
                return false;
            }

            ft_data->axes[i].current_value = value;

            FT_Fixed *coords = build_coordinate_array(ft_data);
            FT_Set_Var_Design_Coordinates(ft_data->ft_face, ft_data->num_axes, coords);
            hb_ft_font_changed(ft_data->hb_font);
            free(coords);

            return true;
        }
    }
    return false;
}
```

### Configuration Interface

Font axis settings would extend the existing `portty.conf` config file (parsed by
`portty_conf.h/c`). Example additions to the `[terminal]` section:

```ini
[terminal]
font = Cascadia Code-14
optical_size = auto
grade = 0

# Per-style overrides
bold_weight = 700
italic_slant = -12
```

---

## 3. Bidirectional Text (BiDi) Support

### Goals

- Full Unicode BiDi Algorithm (UAX #9)
- Mixed LTR/RTL text on same line
- Proper cursor movement in RTL text

### Technical Design

#### BiDi Resolution with FriBidi

```c
#include <fribidi.h>

typedef struct BiDiRun {
    int start;
    int length;
    hb_direction_t dir;
    uint8_t level;
} BiDiRun;

BiDiRun *analyze_bidi(uint32_t *codepoints, int count, int *num_runs) {
    FriBidiCharType *types = malloc(count * sizeof(FriBidiCharType));
    FriBidiLevel *levels = malloc(count * sizeof(FriBidiLevel));

    fribidi_get_bidi_types(codepoints, count, types);

    FriBidiLevel base_level = FRIBIDI_TYPE_LTR;
    fribidi_get_par_embedding_levels(types, count, &base_level, levels);

    BiDiRun *runs = malloc(count * sizeof(BiDiRun));
    int run_count = 0;

    int run_start = 0;
    FriBidiLevel current_level = levels[0];

    for (int i = 1; i <= count; i++) {
        if (i == count || levels[i] != current_level) {
            runs[run_count].start = run_start;
            runs[run_count].length = i - run_start;
            runs[run_count].level = current_level;
            runs[run_count].dir = (current_level % 2) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
            run_count++;

            if (i < count) {
                run_start = i;
                current_level = levels[i];
            }
        }
    }

    free(types);
    free(levels);

    *num_runs = run_count;
    return runs;
}
```

#### Rendering BiDi Text

```c
void render_bidi_line(Renderer *rend, uint32_t *codepoints, int count,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    int num_runs;
    BiDiRun *runs = analyze_bidi(codepoints, count, &num_runs);

    for (int i = 0; i < num_runs; i++) {
        BiDiRun *run = &runs[i];

        ShapedGlyphRun *shaped = shape_text(
            ft_data,
            &codepoints[run->start],
            run->length,
            run->dir
        );

        renderer_draw_shaped_text(rend, shaped, ft_data, x, y, r, g, b);
        x += shaped->total_advance;

        free_shaped_run(shaped);
    }

    free(runs);
}
```

#### Cursor Movement in BiDi

```c
int cursor_move_visual(BiDiContext *ctx, int current_pos, int direction) {
    int *visual_to_logical = malloc(ctx->length * sizeof(int));
    fribidi_reorder_line(ctx->levels, ctx->length, 0, ctx->base_dir,
                         NULL, NULL, NULL, visual_to_logical);

    int visual_pos = -1;
    for (int i = 0; i < ctx->length; i++) {
        if (visual_to_logical[i] == current_pos) {
            visual_pos = i;
            break;
        }
    }

    int new_visual_pos = visual_pos + direction;
    if (new_visual_pos < 0 || new_visual_pos >= ctx->length) {
        free(visual_to_logical);
        return current_pos;
    }

    int new_logical_pos = visual_to_logical[new_visual_pos];
    free(visual_to_logical);

    return new_logical_pos;
}
```

### External Dependencies

- **GNU FriBidi:** BiDi algorithm implementation
- **HarfBuzz:** Already supports RTL shaping
- **FreeType:** No changes needed

### Implementation Plan

1. Add FriBidi dependency to build system
2. Implement BiDi analysis function
3. Modify renderer to handle multiple runs per line
4. Add cursor movement logic
5. Test with Arabic/Hebrew test files

---

## 4. VS15 Blank-Glyph Bug

### Problem

When VS15 (U+FE0E, text presentation selector) follows an emoji-default codepoint,
the cell width is correctly set to 1 by coffer (`width.c:572`: VS15 → 1 cell), but
the glyph is blank — nothing is rendered.

### Root Cause

`rend_sdl3.c:1809`: `if (!has_vs15 && ...)` completely blocks emoji font routing when
VS15 is present. The code then falls through to `FONT_STYLE_NORMAL`, but the
monospace/text font typically has no glyph for emoji-default codepoints (e.g. ⚡ U+26A1,
⚽ U+26BD). The shaped rendering path and single-glyph fallback both fail to find a
glyph, and fontconfig fallback may not cover these codepoints either. Result: blank cell.

### Fix

After VS15 routes to text font and the text font lacks the glyph, fall back to the
emoji font with text semantics (1-cell width, monochrome if possible). VS15 should
control presentation style, not make the glyph unreachable.

---

## 5. Sokol Backend: 1-Pixel Gap in Diagonal Box-Drawing Characters

### Problem

Diagonal box-drawing characters (╱ U+2571, ╲ U+2572, ╳ U+2573) show a 1-pixel
gap at row seams in the Sokol backend. The same characters render seamlessly in
the SDL3 backend. Block elements (█ ▀ ▄) and regular box-drawing characters
(─ │ ┌ ┐ └ ┘) render seamlessly in both backends.

### Context

The Sokol backend now has procedural box-drawing support (intercepting
`rend_boxdraw_is_supported()` before the font fallback path) and two-pass
rendering (backgrounds first, then glyphs on top), bringing it to feature parity
with SDL3 for box-drawing. The remaining 1-pixel gap is a rasterization issue,
not a draw-order issue — it persists even when drawing ONLY glyph quads with no
background quads at all.

### Root Cause (Unresolved)

The bitmap pixel at the seam position has non-zero alpha (191), the UV mapping
appears correct, and the quad geometry covers the pixel — but the rendered output
is black. This is a subtle OpenGL rasterization/sampling difference between SDL3's
`SDL_RenderTexture` and Sokol's raw quad rendering. Likely candidates:

- OpenGL pixel ownership test / viewport / scissor setup differences
- Edge rasterization convention at the bottom of glyph quads
- Texture wrap mode (currently NEAREST, no explicit clamp)

### Investigation Notes

- Confirmed bitmap alpha at seam = 191 (non-zero)
- Confirmed quad geometry covers the seam pixel
- Confirmed UV mapping is correct
- Gap exists without bg quads (not a draw-order issue)
- SDL3 renders the same bitmap seamlessly
- Changes to `rend_boxdraw.c` (padding, line extensions, solid endpoints) were
  tried and reverted — they either didn't help or made things worse, and they
  affect shared code used by SDL3

### Affected Files

- `src/backend_sokol.c` — glyph vertex setup, quad geometry, UV mapping,
  three-pass draw calls
- `src/rend_boxdraw.c` — shared procedural box-drawing bitmap generation
  (unchanged; modifications here affect SDL3 too)

---

## 6. Unimplemented Terminfo Capabilities

These capabilities are inherited from `xterm-256color` via `use=xterm-256color`
but are not yet fully implemented. coffer now has explicit case handlers (no
more silent `default: break` fallthrough) with logging for unimplemented
sequences. portty renders none of these yet.

### Stateful (parsed and stored in coffer, renderer noop)

These follow the blink pattern: `CFR_ATTR_BLINK` is parsed and stored in
coffer, portty maps it to `TerminalCellAttr.blink`, but neither renderer reads
it.

| Capability                   | Sequence    | coffer status                                               | portty renderer                                               |
| ---------------------------- | ----------- | ----------------------------------------------------------- | ------------------------------------------------------------- |
| `dim` (SGR 2)                | `\E[2m`     | `CFR_ATTR_DIM` bit set, cleared by SGR 22                   | Noop (not read by either backend)                             |
| `invis` (SGR 8)              | `\E[8m`     | `CFR_ATTR_INVIS` bit set, cleared by SGR 28                 | Noop (not read by either backend)                             |
| `blink` (SGR 5)              | `\E[5m`     | `CFR_ATTR_BLINK` bit set, cleared by SGR 25                 | Noop (not read by either backend)                             |
| `flash` (DECSCNM, ?5)        | `\E[?5h`    | `CFR_MODE_REVERSE_VIDEO` tracked, `set_mode` callback fired | Noop (screen-level reverse not handled; per-cell SGR 7 works) |
| `smm`/`rmm` (meta, ?1034)    | `\E[?1034h` | `CFR_MODE_META` tracked, logged once                        | Noop                                                          |
| `smglr`/`mgc` (margins, ?69) | `\E[?69h`   | `CFR_MODE_LEFT_RIGHT_MARGINS` tracked, logged once          | Noop                                                          |

### Full implementation design for left/right margins (?69)

DECSET ?69 enables left/right margin support via DECSLRM (`CSI Pl;Pr s`).
Full implementation requires:

- **New fields in CfrTerm:** `int margin_left`, `margin_right` (default 0 and
  `cols - 1`).
- **DECSLRM dispatch** (`CSI Pl;Pr s` with `?69`): Parse and validate left/right
  margins. Only act when `CFR_MODE_LEFT_RIGHT_MARGINS` is on.
- **Print path:** Autowrap checks against `margin_right` instead of `cols - 1`.
- **Cursor movement:** Clamp cursor column to `[margin_left, margin_right]`.
- **Erase operations:** `EL` (erase in line) and `ED` (erase in display)
  respect left/right margins when origin mode + margins are on.
- **Scroll operations:** `IL`/`DL`/`SU`/`SD` respect all four margins.
- ** DECSTBM interaction:** Top/bottom margins (`scroll_top`/`scroll_bottom`)
  already work; left/right would need similar clamping in the same code paths.
- ** DECSET ?69l (reset):** Restore margins to full width.
- **Origin mode (DECOM):** When both DECOM and ?69 are on, cursor coordinates
  are relative to the top-left of the margin region.

### Noop + log (no state to track)

| Capability                | Sequence    | coffer status                        |
| ------------------------- | ----------- | ------------------------------------ |
| `mc0`/`mc4`/`mc5` (CSI i) | `\E[i` etc. | Logged once per CfrTerm, then silent |
| `meml` (ESC l)            | `\El`       | Logged once per CfrTerm, then silent |
| `memu` (ESC m)            | `\Em`       | Logged once per CfrTerm, then silent |
| `initc` (OSC 4)           | `\E]4;...`  | Logged once per CfrTerm, then silent |
| `oc` (OSC 104)            | `\E]104`    | Logged once per CfrTerm, then silent |

### Fully implemented in coffer

| Capability         | Sequence    | coffer status                                        |
| ------------------ | ----------- | ---------------------------------------------------- |
| `rep` (REP, CSI b) | `\E[%p1%db` | Fully implemented: repeats last printed char N times |

### Backend-specific rendering gaps

Some attributes are parsed and stored by coffer but only rendered by one
backend. These are not terminfo concerns (both backends handle the same VT
input), but are important to track for visual parity.

**Rendered by SDL3 only (not Sokol):**

| Attribute                                                   | SDL3                  | Sokol           |
| ----------------------------------------------------------- | --------------------- | --------------- |
| underline (5 styles: single, double, curly, dotted, dashed) | `rend_sdl3.c:124-234` | Not implemented |
| strikethrough                                               | `rend_sdl3.c:240`     | Not implemented |
| `ul_color` (underline color)                                | `rend_sdl3.c:1051`    | Not implemented |

**Rendered differently between backends:**

| Attribute       | SDL3                                    | Sokol                                                    |
| --------------- | --------------------------------------- | -------------------------------------------------------- |
| reverse (SGR 7) | Pre-swaps fg/bg in `term_cfr.c:511-516` | Reads `attrs.reverse` directly at `backend_sokol.c:1694` |

**Not rendered by either backend:**

| Attribute                    | Stored in `TerminalCellAttr` | SDL3        | Sokol       |
| ---------------------------- | ---------------------------- | ----------- | ----------- |
| blink (SGR 5)                | `blink:1`                    | Not read    | Not read    |
| dim (SGR 2)                  | `dim:1`                      | Not read    | Not read    |
| invis (SGR 8)                | `invis:1`                    | Not read    | Not read    |
| font (SGR 10-19)             | `font:4`                     | Not read    | Not read    |
| dwl (DECDWL)                 | `dwl:1`                      | Not read    | Not read    |
| dhl (DECDHL)                 | `dhl:2`                      | Not read    | Not read    |
| DECSCNM (screen reverse, ?5) | N/A (mode, not cell attr)    | Not handled | Not handled |

Note: cursor blink (DECSET ?12) is separate from text blink (SGR 5) and is
fully implemented in both backends via timer-based cursor visibility toggling.

---

## 7. Popular Unsupported Capabilities

These capabilities are not in the terminfo entry, not inherited, and not
implemented, but are commonly expected by modern TUIs.

### OSC 11 (set default foreground color)

Not in terminfo, not implemented. Some TUIs query this to detect background
color for contrast detection. coffer's OSC dispatcher forwards unknown OSCs to
the generic `callbacks.osc` hook, but portty does not handle OSC 11. Full
implementation would require parsing `OSC 11 ; ?` queries (responding with the
current default foreground RGB) and `OSC 11 ; rgb:R/G/B` sets.

### OSC 12 (set default background color)

Not in terminfo, not implemented. Same use as OSC 11 for background color
detection. Would follow the same parse/respond/set pattern.

### OSC 112 (reset cursor color)

Not in terminfo, not implemented. Resets cursor color to default (same as
`Cr=\E]112\007` in xterm-256color, which resets cursor color specifically, as
opposed to OSC 12 which sets it).

### SGR 53 (overline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Would need a
new `CFR_ATTR_OVERLINE` and renderer support (draw a line above the cell).
Note: terminfo has no standard cap for overline; it is a SGR-only feature.

### SGR 21 (double underline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Could map to
underline style 2 (double) which coffer already supports via SGR 4:2.
Alternatively, add `case 21:` that sets `pen.underline = CFR_UL_DOUBLE`.

### XTVERSION (CSI > q)

Not in terminfo, not implemented. TUIs use this to query the terminal's name
and version for feature detection. The `RV` cap in xterm-256color sends
`ESC[>c` but coffer does not respond to `CSI > q` (XTVERSION query). DA2
(`CSI > c`) IS implemented and responds with `ESC[>1;0;0c`.

### E3 (clear scrollback)

Inherited from xterm-256color as `\E[3J`. coffer handles `CSI 3 J` but routes
it to the same code path as `CSI 2 J` (clear visible grid only). The
scrollback buffer is NOT purged. `cfr_scrollback_clear()` exists but is never
called from the ED path. Fix: call `cfr_scrollback_clear(vt)` in the `case 3:`
branch of `cfr_erase_in_display()`.

### DECSCA (Select Character Protection, CSI " q)

Not in terminfo, not implemented. Used for protected characters that survive
erase operations. Would require a per-cell protection attribute and
modification of all erase operations to check it.

### SGR font selection (10-19)

Parsed and stored in `attrs.font` but no renderer reads it. Full
implementation would require loading and selecting alternate font faces via
SGR 10 (primary), 11-19 (alternative fonts).
