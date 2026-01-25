# Future Enhancements - bloom-term

This document outlines potential future enhancements for the bloom-term terminal emulator, building on the FreeType + HarfBuzz + SDL3 architecture.

---

## 1. SDL3 GPU API Migration

**Priority:** Medium
**Complexity:** High
**Performance Gain:** Moderate-to-High (10-30% for large glyph counts)
**Status:** ❌ **NOT STARTED**

### Current State

- Using SDL_Renderer with SDL_Texture for glyph rendering
- Texture atlas already implemented (see section 4)
- Simple, well-tested, portable approach

### Enhancement Goals

- Migrate to SDL3 GPU API for lower-level control
- Reduce CPU-GPU synchronization overhead
- ✅ ~~Enable texture atlas~~ (already implemented - see section 4)
- ❌ Enable instanced rendering

### Technical Design

#### 1.1 Texture Atlas Generation

**Status:** ✅ **ALREADY IMPLEMENTED** (see `src/rend_sdl3_atlas.{c,h}`)

The two-page atlas with shelf packing is already in place. The design below shows the original concept for reference:

```c
typedef struct GlyphAtlas {
    SDL_GPUTexture *texture;        // 2048x2048 RGBA texture
    int cell_width;                 // Individual glyph cell size
    int cell_height;
    int glyphs_per_row;            // e.g., 2048/16 = 128
    int glyphs_per_col;

    struct {
        uint32_t codepoint;
        bool occupied;
        float u0, v0, u1, v1;      // UV coordinates in atlas
    } *cells;                       // Array of cell metadata

    int next_free_cell;            // LRU eviction pointer
} GlyphAtlas;
```

**Atlas Packing Algorithm:**

```c
bool atlas_add_glyph(GlyphAtlas *atlas, uint32_t codepoint, GlyphBitmap *bitmap) {
    // Find free cell (or evict least recently used)
    int cell_index = atlas_find_or_evict_cell(atlas);

    int row = cell_index / atlas->glyphs_per_row;
    int col = cell_index % atlas->glyphs_per_row;

    // Calculate pixel offset in atlas
    int x_offset = col * atlas->cell_width;
    int y_offset = row * atlas->cell_height;

    // Upload glyph bitmap to atlas region
    SDL_GPUTextureRegion region = {
        .texture = atlas->texture,
        .x = x_offset,
        .y = y_offset,
        .w = bitmap->width,
        .h = bitmap->height,
        .start_layer = 0,
        .num_layers = 1
    };

    // Upload via transfer buffer
    SDL_GPUTransferBuffer *transfer = create_transfer_buffer(device, bitmap);
    SDL_UploadToGPUTexture(cmd_buffer, &transfer_source, &region, false);

    // Store UV coordinates
    atlas->cells[cell_index].u0 = (float)x_offset / 2048.0f;
    atlas->cells[cell_index].v0 = (float)y_offset / 2048.0f;
    atlas->cells[cell_index].u1 = (float)(x_offset + bitmap->width) / 2048.0f;
    atlas->cells[cell_index].v1 = (float)(y_offset + bitmap->height) / 2048.0f;
    atlas->cells[cell_index].codepoint = codepoint;
    atlas->cells[cell_index].occupied = true;

    return true;
}
```

#### 1.2 Instanced Rendering

**Concept:** Draw all glyphs in one GPU call using instancing.

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

#### 1.3 Graphics Pipeline Setup

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

1. **Phase 1: GPU Device Setup**
   - Initialize SDL_GPUDevice
   - Create swap chain
   - Set up render targets

2. **Phase 2: Atlas Management**
   - Implement texture atlas
   - Add LRU eviction policy
   - Test with ASCII subset (128 glyphs)

3. **Phase 3: Pipeline Creation**
   - Compile shaders (SPIR-V)
   - Create graphics pipeline
   - Set up vertex/instance buffers

4. **Phase 4: Integration**
   - Replace SDL_Renderer calls
   - Benchmark performance
   - A/B test vs current implementation

### Expected Performance

- **Current:** ~10,000 draw calls for 80x24 terminal = ~240ms/frame @ 60fps budget
- **GPU API:** 1 draw call = ~2-5ms/frame
- **Overall:** 10-30% performance improvement + lower CPU usage

---

## 2. Advanced Variable Font Axes

**Priority:** Low
**Complexity:** Medium
**User Benefit:** Enhanced typography and accessibility
**Status:** ⚠️ **PARTIAL** (basic variable font support exists)

### Current State

✅ **Already Implemented:**

- Variable font (MM_Var) support
- Weight (wght) axis: Dynamic adjustment for bold style
- Axis caching and coordinate array management
- FreeType/HarfBuzz synchronization via `hb_ft_font_changed()`

❌ **Not Yet Implemented:**

- Width (wdth) axis
- Other axes: slnt, ital, opsz, GRAD
- User configuration for custom axes

### Additional Axes to Support

#### 2.1 Slant Axis (slnt)

**Range:** -15° to 0°  
**Use Case:** Oblique/italic text without separate font file

```c
// Enable italic style via slant axis
void set_italic_via_slant(FtFontData *ft_data, bool italic) {
    float slant = italic ? -12.0f : 0.0f;  // -12° slant
    ft_set_axis_value(ft_data, "slnt", slant);
}
```

**Terminal Use:** Italic escape sequence (`\033[3m`) could use slant instead of loading separate font.

#### 2.2 Italic Axis (ital)

**Range:** 0.0 (Roman) to 1.0 (Italic)  
**Use Case:** True italic with alternate letterforms

```c
// Use cursive italic forms
void set_true_italic(FtFontData *ft_data, bool italic) {
    float ital = italic ? 1.0f : 0.0f;
    ft_set_axis_value(ft_data, "ital", ital);
}
```

**Difference from Slant:** Italic axis provides true cursive forms (different 'a', 'g', etc.), while slant just skews glyphs.

#### 2.3 Optical Size (opsz)

**Range:** 6pt to 144pt  
**Use Case:** Optimize rendering for different font sizes

```c
// Automatically adjust optical size based on font size
void set_optical_size(FtFontData *ft_data, float font_size_pt) {
    ft_set_axis_value(ft_data, "opsz", font_size_pt);
}
```

**Terminal Use:** Small terminals (10pt) get more open spacing, large terminals (20pt) get finer details.

#### 2.4 Grade (GRAD)

**Range:** -200 to 200  
**Use Case:** Adjust weight without changing metrics (for accessibility)

```c
// Increase boldness without affecting layout
void adjust_for_readability(FtFontData *ft_data, int grade) {
    ft_set_axis_value(ft_data, "GRAD", (float)grade);
}
```

**Terminal Use:** User preference for "high contrast mode" without changing cell sizes.

#### 2.5 Custom Axes

Support for font-specific axes (e.g., "ROND" for roundness, "SOFT" for softness).

```c
// Generic axis setter
bool ft_set_custom_axis(FtFontData *ft_data, uint32_t tag, float value) {
    // Find axis in ft_data->axes array
    for (int i = 0; i < ft_data->num_axes; i++) {
        if (ft_data->axes[i].tag == tag) {
            if (value < ft_data->axes[i].min_value ||
                value > ft_data->axes[i].max_value) {
                return false;  // Out of range
            }

            ft_data->axes[i].current_value = value;

            // Rebuild coordinate array and apply
            FT_Fixed *coords = build_coordinate_array(ft_data);
            FT_Set_Var_Design_Coordinates(ft_data->ft_face, ft_data->num_axes, coords);
            hb_ft_font_changed(ft_data->hb_font);
            free(coords);

            return true;
        }
    }
    return false;  // Axis not found
}
```

### Configuration Interface

**User Config File (e.g., `~/.config/bloom-term/fontrc`):**

```ini
[font.axes]
# Standard axes
weight = 400
width = 100
slant = 0
italic = 0
optical_size = auto  # Auto-detect from font size

# Custom axes (if font supports them)
#ROND = 50
#SOFT = 0

[font.styles]
# Override axes for specific styles
bold.weight = 700
italic.slant = -12
```

### Implementation Effort

- **API:** Already designed in main proposal
- **Config Parsing:** ~2 days
- **UI/Testing:** ~3 days
- **Total:** ~1 week

---

## 3. Bidirectional Text (BiDi) Support

**Priority:** Low (unless targeting RTL languages)
**Complexity:** High
**User Benefit:** Proper Arabic, Hebrew, Persian rendering
**Status:** ❌ **NOT STARTED**

### Current State

- LTR (left-to-right) only
- No Unicode BiDi algorithm
- No FriBidi dependency

### Enhancement Goals

- Full Unicode BiDi Algorithm (UAX #9)
- Mixed LTR/RTL text on same line
- Proper cursor movement in RTL text

### Technical Design

#### 3.1 BiDi Resolution with HarfBuzz

**HarfBuzz Direction Support:**

```c
typedef enum {
    HB_DIRECTION_LTR,   // Left-to-right (English, etc.)
    HB_DIRECTION_RTL,   // Right-to-left (Arabic, Hebrew)
    HB_DIRECTION_TTB,   // Top-to-bottom (Mongolian)
    HB_DIRECTION_BTT    // Bottom-to-top (rare)
} hb_direction_t;
```

**BiDi Analysis:**

```c
#include <fribidi.h>  // GNU FriBidi library for BiDi algorithm

typedef struct BiDiRun {
    int start;              // Start index in codepoint array
    int length;             // Length of run
    hb_direction_t dir;     // Direction of this run
    uint8_t level;          // BiDi embedding level
} BiDiRun;

BiDiRun *analyze_bidi(uint32_t *codepoints, int count, int *num_runs) {
    // Allocate FriBidi types array
    FriBidiCharType *types = malloc(count * sizeof(FriBidiCharType));
    FriBidiLevel *levels = malloc(count * sizeof(FriBidiLevel));

    // Get character types (L, R, AL, EN, etc.)
    fribidi_get_bidi_types(codepoints, count, types);

    // Resolve BiDi levels
    FriBidiLevel base_level = FRIBIDI_TYPE_LTR;  // Or RTL for RTL paragraphs
    fribidi_get_par_embedding_levels(types, count, &base_level, levels);

    // Split into runs of consistent direction
    BiDiRun *runs = malloc(count * sizeof(BiDiRun));  // Max possible runs
    int run_count = 0;

    int run_start = 0;
    FriBidiLevel current_level = levels[0];

    for (int i = 1; i <= count; i++) {
        if (i == count || levels[i] != current_level) {
            // End of run
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

#### 3.2 Rendering BiDi Text

**Shape Each Run Separately:**

```c
void render_bidi_line(Renderer *rend, uint32_t *codepoints, int count,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    // Analyze BiDi structure
    int num_runs;
    BiDiRun *runs = analyze_bidi(codepoints, count, &num_runs);

    // Process runs in visual order (may be different from logical order)
    for (int i = 0; i < num_runs; i++) {
        BiDiRun *run = &runs[i];

        // Shape this run with its direction
        ShapedGlyphRun *shaped = shape_text(
            ft_data,
            &codepoints[run->start],
            run->length,
            run->dir  // HB_DIRECTION_RTL or HB_DIRECTION_LTR
        );

        // Render shaped run
        renderer_draw_shaped_text(rend, shaped, ft_data, x, y, r, g, b);

        // Advance x position by total run width
        x += shaped->total_advance;

        free_shaped_run(shaped);
    }

    free(runs);
}
```

#### 3.3 Cursor Movement in BiDi

**Logical vs Visual Movement:**

```c
// Move cursor logically (in text buffer order)
int cursor_move_logical(BiDiContext *ctx, int current_pos, int delta) {
    int new_pos = current_pos + delta;
    if (new_pos < 0 || new_pos >= ctx->length) {
        return current_pos;  // Out of bounds
    }
    return new_pos;
}

// Move cursor visually (left/right on screen)
int cursor_move_visual(BiDiContext *ctx, int current_pos, int direction) {
    // Use FriBidi to convert logical position to visual
    int *visual_to_logical = malloc(ctx->length * sizeof(int));
    fribidi_reorder_line(ctx->levels, ctx->length, 0, ctx->base_dir,
                         NULL, NULL, NULL, visual_to_logical);

    // Find current visual position
    int visual_pos = -1;
    for (int i = 0; i < ctx->length; i++) {
        if (visual_to_logical[i] == current_pos) {
            visual_pos = i;
            break;
        }
    }

    // Move visually
    int new_visual_pos = visual_pos + direction;
    if (new_visual_pos < 0 || new_visual_pos >= ctx->length) {
        free(visual_to_logical);
        return current_pos;
    }

    // Convert back to logical
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

### Estimated Effort

- **Core BiDi:** 1-2 weeks
- **Cursor Logic:** 1 week
- **Testing:** 1 week
- **Total:** 3-4 weeks

---

## 4. Advanced Glyph Caching

**Priority:** Medium
**Complexity:** Medium
**Performance Gain:** High (50-80% for repeated glyphs)
**Status:** ✅ **IMPLEMENTED** (as of current version)

### Current State

✅ **Two-page texture atlas implemented** (`src/rend_sdl3_atlas.{c,h}`):

- Page 0: Small glyphs (≤48px)
- Page 1: Large glyphs (>48px)
- 2048×2048 RGBA textures
- Shelf-based packing algorithm
- FNV-1a hash-based lookup (O(1) with 4096 hash table entries)
- LRU eviction when pages fill
- Comprehensive vlog diagnostics

The implementation described below was **already completed**. Remaining work:

### Enhancement Goals (REMAINING)

- ❌ Cache shaped runs (e.g., "fi" ligature) - not yet implemented
- ❌ Shaped run cache - not yet implemented

### Technical Design

#### 4.1 Glyph Cache with LRU Eviction

```c
#define GLYPH_CACHE_SIZE 2048  // Cache up to 2048 unique glyphs

typedef struct CachedGlyph {
    uint32_t codepoint;
    FontStyle style;
    uint8_t r, g, b;           // Foreground color

    SDL_Texture *texture;      // Rendered texture
    int width, height;
    int x_offset, y_offset;
    int advance;

    uint64_t last_used;        // Timestamp for LRU
    bool occupied;
} CachedGlyph;

typedef struct GlyphCache {
    CachedGlyph *entries;
    int capacity;
    int count;
    uint64_t frame_counter;    // Increment each frame for LRU

    // Hash table for O(1) lookup
    int *hash_table;           // Maps hash -> cache index
    int hash_table_size;
} GlyphCache;

// Hash function
uint32_t hash_glyph(uint32_t codepoint, FontStyle style, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t hash = codepoint;
    hash = hash * 31 + style;
    hash = hash * 31 + r;
    hash = hash * 31 + g;
    hash = hash * 31 + b;
    return hash;
}

// Lookup in cache
CachedGlyph *cache_lookup(GlyphCache *cache, uint32_t codepoint, FontStyle style,
                          uint8_t r, uint8_t g, uint8_t b) {
    uint32_t hash = hash_glyph(codepoint, style, r, g, b);
    int idx = cache->hash_table[hash % cache->hash_table_size];

    if (idx >= 0 && cache->entries[idx].occupied) {
        CachedGlyph *glyph = &cache->entries[idx];
        if (glyph->codepoint == codepoint &&
            glyph->style == style &&
            glyph->r == r && glyph->g == g && glyph->b == b) {

            // Update LRU timestamp
            glyph->last_used = cache->frame_counter;
            return glyph;
        }
    }

    return NULL;  // Cache miss
}

// Insert into cache (with LRU eviction)
void cache_insert(GlyphCache *cache, uint32_t codepoint, FontStyle style,
                  uint8_t r, uint8_t g, uint8_t b, SDL_Texture *texture,
                  int width, int height, int x_offset, int y_offset, int advance) {
    uint32_t hash = hash_glyph(codepoint, style, r, g, b);
    int slot;

    if (cache->count < cache->capacity) {
        // Find empty slot
        slot = cache->count++;
    } else {
        // Evict LRU entry
        slot = 0;
        uint64_t oldest = cache->entries[0].last_used;
        for (int i = 1; i < cache->capacity; i++) {
            if (cache->entries[i].last_used < oldest) {
                oldest = cache->entries[i].last_used;
                slot = i;
            }
        }

        // Free old texture
        SDL_DestroyTexture(cache->entries[slot].texture);
    }

    // Insert new entry
    CachedGlyph *entry = &cache->entries[slot];
    entry->codepoint = codepoint;
    entry->style = style;
    entry->r = r;
    entry->g = g;
    entry->b = b;
    entry->texture = texture;
    entry->width = width;
    entry->height = height;
    entry->x_offset = x_offset;
    entry->y_offset = y_offset;
    entry->advance = advance;
    entry->last_used = cache->frame_counter;
    entry->occupied = true;

    // Update hash table
    cache->hash_table[hash % cache->hash_table_size] = slot;
}
```

#### 4.2 Shaped Run Cache

**For ligatures and complex sequences:**

```c
typedef struct CachedShapedRun {
    uint32_t *codepoints;      // Key: sequence of codepoints
    int codepoint_count;
    FontStyle style;

    ShapedGlyphRun *shaped;    // Cached shaped result
    uint64_t last_used;
    bool occupied;
} CachedShapedRun;

#define SHAPED_CACHE_SIZE 256

typedef struct ShapedRunCache {
    CachedShapedRun entries[SHAPED_CACHE_SIZE];
    uint64_t frame_counter;
} ShapedRunCache;

// Lookup shaped run
ShapedGlyphRun *shaped_cache_lookup(ShapedRunCache *cache,
                                     uint32_t *codepoints, int count,
                                     FontStyle style) {
    for (int i = 0; i < SHAPED_CACHE_SIZE; i++) {
        CachedShapedRun *entry = &cache->entries[i];

        if (entry->occupied && entry->codepoint_count == count &&
            entry->style == style) {

            // Compare codepoint sequences
            bool match = true;
            for (int j = 0; j < count; j++) {
                if (entry->codepoints[j] != codepoints[j]) {
                    match = false;
                    break;
                }
            }

            if (match) {
                entry->last_used = cache->frame_counter;
                return entry->shaped;
            }
        }
    }

    return NULL;  // Cache miss
}
```

#### 4.3 Cache Statistics and Tuning

```c
typedef struct CacheStats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;

    double hit_rate;
} CacheStats;

void cache_print_stats(GlyphCache *cache, CacheStats *stats) {
    stats->hit_rate = (double)stats->hits / (double)stats->lookups;

    printf("Glyph Cache Statistics:\n");
    printf("  Capacity: %d\n", cache->capacity);
    printf("  Occupied: %d (%.1f%%)\n", cache->count,
           100.0 * cache->count / cache->capacity);
    printf("  Lookups: %lu\n", stats->lookups);
    printf("  Hits: %lu (%.2f%%)\n", stats->hits, 100.0 * stats->hit_rate);
    printf("  Misses: %lu\n", stats->misses);
    printf("  Evictions: %lu\n", stats->evictions);
}
```

### Cache Tuning Parameters

1. **Cache Size:** Balance memory vs hit rate
   - 512 entries: ~10MB (typical glyph = 20KB texture)
   - 2048 entries: ~40MB
   - 8192 entries: ~160MB (for emoji-heavy terminals)

2. **Eviction Policy:**
   - **LRU:** Best for general use
   - **LFU** (Least Frequently Used): Better for code editors
   - **FIFO:** Simplest, worst hit rate

3. **Color Keying:**
   - Cache per-color (current design): More entries but accurate colors
   - Cache grayscale, colorize on GPU: Fewer entries but shader complexity

### Implementation Plan

1. Implement basic LRU cache (1 day)
2. Add hash table for O(1) lookup (1 day)
3. Integrate with renderer (1 day)
4. Add shaped run cache (2 days)
5. Benchmark and tune (2 days)
6. **Total:** ~1 week

### Expected Performance

- **ASCII-heavy terminals:** 80-90% hit rate (26 lowercase + 26 uppercase + digits + symbols)
- **Unicode terminals:** 60-70% hit rate (more unique glyphs)
- **Ligature-heavy code:** 50-60% hit rate (many unique sequences)

---

## 5. Subpixel Rendering and LCD Filtering

**Priority:** Low
**Complexity:** Medium
**User Benefit:** Sharper text on LCD screens
**Status:** ⚠️ **PARTIAL** (foundation exists but not active)

### Current State

⚠️ **Partially Enabled:**

- `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` is defined in `src/font_ft.c:1`
- Basic grayscale anti-aliasing active
- No FT_LOAD_TARGET_LCD flag usage
- No LCD filter configuration

### Enhancement Goals (REMAINING)

- RGB subpixel rendering for LCD screens
- FreeType LCD filtering support
- Configuration for RGB vs BGR subpixel order

### Technical Design

#### 5.1 FreeType LCD Rendering

```c
// Enable LCD filtering
void enable_lcd_rendering(FtFontData *ft_data, int subpixel_order) {
    // Set LCD filter (reduces color fringing)
    FT_Library_SetLcdFilter(ft_library, FT_LCD_FILTER_DEFAULT);
    // Or: FT_LCD_FILTER_LIGHT, FT_LCD_FILTER_LEGACY

    // Update load flags
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

// Render with LCD subpixel
GlyphBitmap *render_lcd_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                               uint8_t fg_r, uint8_t fg_g, uint8_t fg_b) {
    FT_Load_Glyph(ft_data->ft_face, glyph_index, ft_data->ft_load_flags);
    FT_Render_Glyph(ft_data->ft_face->glyph, FT_RENDER_MODE_LCD);

    FT_Bitmap *bitmap = &ft_data->ft_face->glyph->bitmap;
    // bitmap->pixel_mode == FT_PIXEL_MODE_LCD (3 bytes per pixel: R, G, B)

    GlyphBitmap *result = malloc(sizeof(GlyphBitmap));
    result->width = bitmap->width / 3;  // LCD mode is 3x wider
    result->height = bitmap->rows;
    result->pixels = malloc(result->width * result->height * 4);

    // Convert LCD bitmap to RGBA
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

#### 5.2 Subpixel Order Configuration

```c
typedef enum {
    SUBPIXEL_NONE,    // No subpixel rendering
    SUBPIXEL_RGB,     // R-G-B horizontal
    SUBPIXEL_BGR,     // B-G-R horizontal
    SUBPIXEL_VRGB,    // R-G-B vertical
    SUBPIXEL_VBGR     // B-G-R vertical
} SubpixelOrder;

// Auto-detect from fontconfig
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

### Challenges

- Subpixel rendering assumes static pixel grid (doesn't work well with scaling/rotation)
- Color fringing on non-neutral backgrounds
- Not ideal for OLED/PenTile displays

### Recommendation

- Make optional (disabled by default)
- Expose as config option: `subpixel_rendering = auto|rgb|bgr|vrgb|vbgr|none`
- Provide toggle for users to test

---

## 6. Font Fallback Chain

**Priority:** Medium
**Complexity:** Medium
**User Benefit:** Better Unicode coverage
**Status:** ❌ **NOT STARTED**

### Current State

- Single font per style (normal, bold, emoji) via `font_resolver.c`
- No fallback chain mechanism
- Missing glyphs render as □ (tofu)

### Enhancement Goals

- Multiple fallback fonts
- Automatic fallback selection per codepoint
- Coverage cache to avoid repeated lookups

### Technical Design

```c
#define MAX_FALLBACK_FONTS 8

typedef struct FontFallbackChain {
    FtFontData *fonts[MAX_FALLBACK_FONTS];
    int count;

    // Coverage cache: maps codepoint -> font index
    struct {
        uint32_t codepoint;
        int font_index;  // Which font has this glyph
    } *coverage_cache;
    int coverage_cache_size;
} FontFallbackChain;

// Find which font in chain has this glyph
int find_font_for_codepoint(FontFallbackChain *chain, uint32_t codepoint) {
    // Check cache first
    // ... cache lookup code ...

    // Check each font
    for (int i = 0; i < chain->count; i++) {
        FT_UInt glyph_index = FT_Get_Char_Index(chain->fonts[i]->ft_face, codepoint);
        if (glyph_index != 0) {
            // Cache result
            // ... cache insert code ...
            return i;
        }
    }

    return -1;  // No font has this glyph
}

// Render with fallback
GlyphBitmap *render_with_fallback(FontFallbackChain *chain, uint32_t codepoint,
                                   uint8_t r, uint8_t g, uint8_t b) {
    int font_idx = find_font_for_codepoint(chain, codepoint);
    if (font_idx >= 0) {
        return rasterize_glyph(chain->fonts[font_idx],
                               FT_Get_Char_Index(chain->fonts[font_idx]->ft_face, codepoint),
                               r, g, b);
    }

    // Render missing glyph symbol
    return render_missing_glyph(r, g, b);
}
```

**Default Fallback Chain Example:**

```
1. JetBrains Mono (primary, good ASCII/Latin coverage)
2. DejaVu Sans Mono (extended Latin, Greek, Cyrillic)
3. Noto Sans Mono CJK (Chinese, Japanese, Korean)
4. Noto Sans Mono Arabic (Arabic script)
5. Noto Color Emoji (emoji)
6. Symbola (Unicode symbols)
```

---

## Implementation Status

| Enhancement        | Status         | Notes                                                                       |
| ------------------ | -------------- | --------------------------------------------------------------------------- |
| SDL3 GPU API       | ❌ Not Started | Still using SDL_Renderer                                                    |
| Variable Font Axes | ⚠️ Partial     | Only 'wght' (weight) axis implemented; no slnt, ital, opsz, GRAD support    |
| BiDi Support       | ❌ Not Started | No FriBidi integration, LTR only                                            |
| Glyph Caching      | ✅ **DONE**    | Two-page texture atlas with shelf packing, FNV-1a hash, LRU eviction        |
| Subpixel Rendering | ⚠️ Partial     | FT_CONFIG_OPTION_SUBPIXEL_RENDERING defined but FT_LOAD_TARGET_LCD not used |
| Font Fallback      | ❌ Not Started | Single font per type (normal/bold/emoji), no fallback chain                 |

## Summary Table

| Enhancement        | Priority | Complexity | Effort    | Performance Gain        | Status         |
| ------------------ | -------- | ---------- | --------- | ----------------------- | -------------- |
| SDL3 GPU API       | Medium   | High       | 3-4 weeks | 10-30% rendering        | ❌ Not Started |
| Variable Font Axes | Low      | Medium     | 1 week    | None (UX improvement)   | ⚠️ Partial     |
| BiDi Support       | Low      | High       | 3-4 weeks | None (RTL languages)    | ❌ Not Started |
| Glyph Caching      | Medium   | Medium     | 1 week    | 50-80% rendering        | ✅ **DONE**    |
| Subpixel Rendering | Low      | Medium     | 1 week    | None (visual quality)   | ⚠️ Partial     |
| Font Fallback      | Medium   | Medium     | 1-2 weeks | None (Unicode coverage) | ❌ Not Started |

**Recommended Order:**

1. ~~**Glyph Caching**~~ - ✅ **DONE** (two-page atlas with shelf packing)
2. **Font Fallback** - Better Unicode support
3. **SDL3 GPU API** - Performance for large terminals
4. **Variable Font Axes** - Enhanced typography (complete remaining axes)
5. **Subpixel Rendering** - Visual polish (enable FT_LOAD_TARGET_LCD)
6. **BiDi Support** - Only if RTL language support needed

---

**Document Version:** 1.0  
**Date:** 2026-01-22  
**Status:** Planning Document
