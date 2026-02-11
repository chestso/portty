# TODO - bloom-terminal

---

## 1. SDL3 GPU API Migration

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
`apply_font_variations()` applies per-style overrides. Currently only the weight (wght)
axis is used in practice. Remaining work is to wire up additional axes at the
application level.

### Slant Axis (slnt)

**Range:** -15° to 0°
**Use Case:** Oblique/italic text without separate font file

```c
void set_italic_via_slant(FtFontData *ft_data, bool italic) {
    float slant = italic ? -12.0f : 0.0f;
    ft_set_axis_value(ft_data, "slnt", slant);
}
```

Terminal use: Italic escape sequence (`\033[3m`) could use slant instead of loading separate font.

### Italic Axis (ital)

**Range:** 0.0 (Roman) to 1.0 (Italic)
**Use Case:** True italic with alternate letterforms (different 'a', 'g', etc.), unlike slant which just skews glyphs.

```c
void set_true_italic(FtFontData *ft_data, bool italic) {
    float ital = italic ? 1.0f : 0.0f;
    ft_set_axis_value(ft_data, "ital", ital);
}
```

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

**User Config File (e.g., `~/.config/bloom-terminal/fontrc`):**

```ini
[font.axes]
weight = 400
width = 100
slant = 0
italic = 0
optical_size = auto

# Custom axes (if font supports them)
#ROND = 50
#SOFT = 0

[font.styles]
bold.weight = 700
italic.slant = -12
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

## 4. Subpixel Rendering and LCD Filtering

`FtFontData` has `subpixel_order` and `lcd_filter` fields (populated from `FontOptions`),
but `FT_LOAD_TARGET_LCD` and `FT_Library_SetLcdFilter()` are never called — rendering
always uses `FT_RENDER_MODE_NORMAL`.

### Goals

- RGB subpixel rendering for LCD screens
- FreeType LCD filtering support
- Configuration for RGB vs BGR subpixel order

### Technical Design

#### FreeType LCD Rendering

```c
void enable_lcd_rendering(FtFontData *ft_data, int subpixel_order) {
    FT_Library_SetLcdFilter(ft_library, FT_LCD_FILTER_DEFAULT);

    switch (subpixel_order) {
        case FC_RGBA_RGB:
        case FC_RGBA_BGR:
            ft_data->ft_load_flags = FT_LOAD_TARGET_LCD;
            break;
        case FC_RGBA_VRGB:
        case FC_RGBA_VBGR:
            ft_data->ft_load_flags = FT_LOAD_TARGET_LCD_V;
            break;
        default:
            ft_data->ft_load_flags = FT_LOAD_TARGET_NORMAL;
    }
}

GlyphBitmap *render_lcd_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                               uint8_t fg_r, uint8_t fg_g, uint8_t fg_b) {
    FT_Load_Glyph(ft_data->ft_face, glyph_index, ft_data->ft_load_flags);
    FT_Render_Glyph(ft_data->ft_face->glyph, FT_RENDER_MODE_LCD);

    FT_Bitmap *bitmap = &ft_data->ft_face->glyph->bitmap;

    GlyphBitmap *result = malloc(sizeof(GlyphBitmap));
    result->width = bitmap->width / 3;  // LCD mode is 3x wider
    result->height = bitmap->rows;
    result->pixels = malloc(result->width * result->height * 4);

    for (int y = 0; y < result->height; y++) {
        uint8_t *src_row = bitmap->buffer + y * bitmap->pitch;
        uint8_t *dst_row = result->pixels + y * result->width * 4;

        for (int x = 0; x < result->width; x++) {
            uint8_t r_coverage = src_row[x * 3 + 0];
            uint8_t g_coverage = src_row[x * 3 + 1];
            uint8_t b_coverage = src_row[x * 3 + 2];

            dst_row[x * 4 + 0] = (fg_r * r_coverage) / 255;
            dst_row[x * 4 + 1] = (fg_g * g_coverage) / 255;
            dst_row[x * 4 + 2] = (fg_b * b_coverage) / 255;
            dst_row[x * 4 + 3] = (r_coverage + g_coverage + b_coverage) / 3;
        }
    }

    return result;
}
```

#### Subpixel Order Detection

```c
typedef enum {
    SUBPIXEL_NONE,
    SUBPIXEL_RGB,
    SUBPIXEL_BGR,
    SUBPIXEL_VRGB,
    SUBPIXEL_VBGR
} SubpixelOrder;

SubpixelOrder detect_subpixel_order(FcPattern *pattern) {
    int rgba;
    if (FcPatternGetInteger(pattern, FC_RGBA, 0, &rgba) == FcResultMatch) {
        switch (rgba) {
            case FC_RGBA_RGB:  return SUBPIXEL_RGB;
            case FC_RGBA_BGR:  return SUBPIXEL_BGR;
            case FC_RGBA_VRGB: return SUBPIXEL_VRGB;
            case FC_RGBA_VBGR: return SUBPIXEL_VBGR;
            default:           return SUBPIXEL_NONE;
        }
    }
    return SUBPIXEL_NONE;
}
```

### Notes

- Subpixel rendering assumes static pixel grid (doesn't work well with scaling/rotation)
- Color fringing on non-neutral backgrounds
- Not ideal for OLED/PenTile displays
- Make optional (disabled by default)
- Expose as config option: `subpixel_rendering = auto|rgb|bgr|vrgb|vbgr|none`

---

## 5. Custom Terminal Emulation Library

### Problem

libvterm doesn't properly handle multi-cell Unicode combining:

- **ZWJ sequences** (👨‍👩‍👧): Split into separate cells (👨, ZWJ, 👩, ZWJ, 👧)
- **Flag emoji** (🇺🇸): Regional indicators in separate cells (🇺, 🇸)
- **Skin tone modifiers** (👋🏽): Base emoji and modifier in separate cells

This is a fundamental limitation of libvterm's cell-based model.

### Goals

- Maintain grapheme clusters as atomic units during terminal emulation
- Proper cursor movement over grapheme clusters
- Correct width calculation for complex emoji sequences
- Consistent behavior between live terminal and scrollback

### Alternatives to Evaluate

1. **Fork libvterm**: Modify cell storage to support grapheme clusters
   - Pros: Minimal architectural changes to bloom-terminal
   - Cons: Ongoing maintenance burden, may diverge from upstream

2. **Custom cell layer**: Keep libvterm for escape sequence parsing, replace its cell storage with a custom layer that preserves grapheme clusters
   - Pros: Minimal scope, reuses libvterm's mature escape sequence handling
   - Cons: Tight coupling to libvterm internals

3. **Custom implementation**: Build terminal emulation from scratch
   - Pros: Full control over cell model and grapheme handling
   - Cons: Massive undertaking, many edge cases

### How Other Terminals Solve This

All three major GPU-accelerated terminals (foot, kitty, alacritty) wrote their own VT
parsers from scratch rather than using libvterm. None of their implementations are
available as reusable C libraries.

#### kitty: Indexed Grapheme Storage

The core problem: a terminal grid stores one cell per column, but a single visible
character like 👨‍👩‍👧 (family emoji) is actually 5 codepoints joined by zero-width joiners.
A naive cell model either truncates this to one codepoint or scatters it across cells.

kitty solves this by splitting each cell into two structs. The first (`CPUCell`) holds
the text content — but instead of storing codepoints directly, it stores either a single
codepoint (the common case for ASCII/Latin) or an index into a separate shared table
called `TextCache`. The second struct (`GPUCell`) holds only rendering attributes (colors,
style flags, sprite index into the texture atlas).

`TextCache` is a reference-counted, deduplicated store for multi-codepoint sequences.
When kitty encounters a ZWJ emoji or combining character sequence, it inserts the full
codepoint sequence into the `TextCache` and stores just the index in the cell. Multiple
cells can reference the same entry (e.g., if the same emoji appears twice on screen),
and entries are freed when their reference count drops to zero.

This means:

- Simple ASCII characters cost no extra memory (codepoint stored inline in the cell).
- Complex grapheme clusters are stored once and referenced by index.
- The GPU-facing struct stays small and fixed-size (no variable-length data).

kitty also performs full UAX#29 grapheme cluster segmentation (the Unicode algorithm that
defines where one "user-perceived character" ends and the next begins). When processing
PTY input, it checks whether each incoming codepoint forms a grapheme boundary with the
previous cell. If not, the codepoint is appended to the previous cell's content rather
than advancing to a new cell.

#### foot: libutf8proc for Grapheme Segmentation

foot takes a different approach. Instead of building its own Unicode segmentation, it
uses **libutf8proc** — a small C library (originally from the Julia language project)
that implements Unicode algorithms including grapheme cluster boundary detection
(UAX#29), normalization, and character properties.

When foot receives terminal output, it uses libutf8proc's `utf8proc_grapheme_break()`
to decide whether consecutive codepoints belong to the same grapheme cluster. If they
do, they are kept together as a single unit for rendering.

For the actual rasterization, foot uses **fcft** (a standalone C font library by foot's
author) which has a `fcft_grapheme_rasterize()` function that takes an entire grapheme
cluster and renders it as one glyph via HarfBuzz shaping.

foot's grapheme clustering is an optional build-time feature. The biggest unsolved
problem is width: `wcswidth()` (the standard C function for string display width) gives
wrong answers for many grapheme clusters, so foot offers a config toggle between
`wcswidth` (stays in sync with the shell but adds extra spacing) and `double-width`
(looks better but risks cursor desync with the application).

#### alacritty: Minimal Approach

alacritty (Rust) stores each cell as a base `char` plus an optional `Vec<char>` for
zero-width combining characters. It does not do full UAX#29 grapheme segmentation —
combining marks are appended to the base character, but complex sequences like ZWJ
emoji may not be treated as atomic clusters. Its VT parser (`vte` crate) is a widely
used standalone Rust library but has no C API.

### Before Starting

1. Benchmark current emoji rendering to establish baseline
2. Create proof-of-concept with simplest alternative first
3. Consider hybrid approach: keep libvterm for escape sequences, custom layer for cell management
4. Evaluate libutf8proc for grapheme segmentation (small, C, permissive license)
