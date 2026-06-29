# TODO - portty

---

## 1. SDL3 GPU API Migration

### Current state (not this migration)

We render through SDL's **high-level** `SDL_Renderer`, created with the
`"gpu"` driver name (`platform_sdl3.c:757`). On Linux that backend resolves
to Vulkan — this is the "we went all Vulkan" change. It bought us
**linear-light glyph blending** (the gpu/vulkan renderer honors SRGB_LINEAR
float render targets; `rend_sdl3.c:950-956`) and, on the GTK4 path, a
hand-written Vulkan device for zero-copy DMA-BUF export (`gtk4_vulkan.c`).

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

Font axis settings would extend the existing `bloom.conf` config file (parsed by
`bloom_conf.h/c`). Example additions to the `[terminal]` section:

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
