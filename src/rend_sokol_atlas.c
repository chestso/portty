#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rend_sokol_atlas.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

bool rend_sokol_atlas_init(RendSokolAtlas *atlas, bool linearize_color)
{
    memset(atlas, 0, sizeof(*atlas));
    atlas->linearize_color = linearize_color;

    uint8_t *staging = calloc(
        (size_t)REND_ATLAS_TEXTURE_SIZE * REND_ATLAS_TEXTURE_SIZE, 4);
    if (!staging)
        return false;

    rend_atlas_init(&atlas->packing, staging);

    atlas->texture = sg_make_image(&(sg_image_desc){
        .width = REND_ATLAS_TEXTURE_SIZE,
        .height = REND_ATLAS_TEXTURE_SIZE,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.dynamic_update = true,
        .label = "sokol-glyph-atlas",
    });
    if (atlas->texture.id == 0) {
        free(staging);
        return false;
    }

    atlas->sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .label = "sokol-glyph-atlas-sampler",
    });

    atlas->texture_view = sg_make_view(&(sg_view_desc){
        .texture.image = atlas->texture,
        .label = "sokol-glyph-atlas-view",
    });

    atlas->texture_created = true;

    vlog("Sokol atlas initialized: %dx%d RGBA\n",
         REND_ATLAS_TEXTURE_SIZE, REND_ATLAS_TEXTURE_SIZE);
    return true;
}

void rend_sokol_atlas_destroy(RendSokolAtlas *atlas)
{
    if (!atlas)
        return;
    if (atlas->texture_created) {
        sg_destroy_view(atlas->texture_view);
        sg_destroy_sampler(atlas->sampler);
        sg_destroy_image(atlas->texture);
        atlas->texture_created = false;
    }
    free(atlas->packing.staging);
    atlas->packing.staging = NULL;
    memset(atlas->packing.entries, 0, sizeof(atlas->packing.entries));
    atlas->packing.entry_count = 0;
}

void rend_sokol_atlas_begin_frame(RendSokolAtlas *atlas)
{
    rend_atlas_begin_frame(&atlas->packing);
}

RendSokolAtlasEntry *rend_sokol_atlas_lookup(RendSokolAtlas *atlas, void *font_data,
                                             int glyph_id, uint32_t color)
{
    return rend_atlas_lookup(&atlas->packing, font_data, glyph_id, color);
}

RendSokolAtlasEntry *rend_sokol_atlas_insert(RendSokolAtlas *atlas, void *font_data,
                                             int glyph_id, uint32_t color,
                                             GlyphBitmap *bmp, bool is_color)
{
    return rend_atlas_insert(&atlas->packing, font_data, glyph_id, color,
                             bmp, is_color, atlas->linearize_color);
}

RendSokolAtlasEntry *rend_sokol_atlas_insert_empty(RendSokolAtlas *atlas, void *font_data,
                                                   int glyph_id, uint32_t color)
{
    return rend_atlas_insert_empty(&atlas->packing, font_data, glyph_id, color);
}

void rend_sokol_atlas_flush(RendSokolAtlas *atlas)
{
    if (!atlas->packing.dirty)
        return;
    sg_update_image(atlas->texture, &(sg_image_data){
                                        .mip_levels[0] = {
                                            .ptr = atlas->packing.staging,
                                            .size = (size_t)REND_ATLAS_TEXTURE_SIZE *
                                                    REND_ATLAS_TEXTURE_SIZE * 4,
                                        },
                                    });
    atlas->packing.dirty = false;
}

void rend_sokol_atlas_log_stats(RendSokolAtlas *atlas)
{
    rend_atlas_log_stats(&atlas->packing);
}
