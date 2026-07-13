#ifndef REND_SOKOL_ATLAS_H
#define REND_SOKOL_ATLAS_H

#include <sokol/sokol_gfx.h>
#include "font.h"
#include "rend_common.h"
#include <stdbool.h>
#include <stdint.h>

// Sokol-specific glyph atlas. Embeds the shared RendAtlas packing engine
// (rend_common.c) and adds an sg_image for GPU upload.

typedef struct
{
    RendAtlas packing;
    sg_image texture;
    sg_sampler sampler;
    sg_view texture_view;
    bool texture_created;
    bool linearize_color;
} RendSokolAtlas;

typedef RendAtlasEntry RendSokolAtlasEntry;
typedef RendAtlasRegion RendSokolAtlasRegion;

bool rend_sokol_atlas_init(RendSokolAtlas *atlas, bool linearize_color);
void rend_sokol_atlas_destroy(RendSokolAtlas *atlas);
void rend_sokol_atlas_begin_frame(RendSokolAtlas *atlas);
RendSokolAtlasEntry *rend_sokol_atlas_lookup(RendSokolAtlas *atlas, void *font_data,
                                             int glyph_id, uint32_t color);
RendSokolAtlasEntry *rend_sokol_atlas_insert(RendSokolAtlas *atlas, void *font_data,
                                             int glyph_id, uint32_t color,
                                             GlyphBitmap *bmp, bool is_color);
RendSokolAtlasEntry *rend_sokol_atlas_insert_empty(RendSokolAtlas *atlas, void *font_data,
                                                   int glyph_id, uint32_t color);
void rend_sokol_atlas_flush(RendSokolAtlas *atlas);
void rend_sokol_atlas_log_stats(RendSokolAtlas *atlas);

#endif // REND_SOKOL_ATLAS_H
