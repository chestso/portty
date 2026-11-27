/*
 * portty — Host-side Lottie software rasterizer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lottie_raster.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LOTTIE_RASTER_MAX_BYTES (128u * 1024u * 1024u)

struct LottieRaster
{
#ifdef HAVE_THORVG
    struct LottieInstance *instances;
    int count;
    int cap;
    size_t live_bytes;
#endif
};

bool lottie_raster_available(void)
{
#ifdef HAVE_THORVG
    return true;
#else
    return false;
#endif
}

#ifndef HAVE_THORVG

LottieRaster *lottie_raster_create(void)
{
    return NULL;
}

void lottie_raster_destroy(LottieRaster *raster)
{
    (void)raster;
}

void lottie_raster_sync(LottieRaster *raster, const CfrLottie *anims,
                        int count)
{
    (void)raster;
    (void)anims;
    (void)count;
}

bool lottie_raster_get(LottieRaster *raster, uint64_t handle,
                       LottieFrame *out)
{
    (void)raster;
    (void)handle;
    (void)out;
    return false;
}

#else

#include <thorvg_capi.h>

typedef struct LottieInstance
{
    uint64_t handle;
    uint32_t definition_rev;
    uint32_t raster_rev;
    int definition_seen;
    int raster_seen;
    int w;
    int h;
    int frame;
    int frame_count;
    int ever_drawn;
    int valid;
    uint8_t *rgba;
    size_t cap;
    Tvg_Animation anim;
    Tvg_Canvas canvas;
} LottieInstance;

static void instance_drop_scene(LottieInstance *inst)
{
    if (inst->canvas && inst->anim)
        tvg_canvas_remove(inst->canvas,
                          tvg_animation_get_picture(inst->anim));
    if (inst->anim)
        tvg_animation_del(inst->anim);
    if (inst->canvas)
        tvg_canvas_destroy(inst->canvas);
    inst->anim = NULL;
    inst->canvas = NULL;
    inst->ever_drawn = 0;
    inst->valid = 0;
}

static void instance_release(LottieRaster *raster, LottieInstance *inst)
{
    instance_drop_scene(inst);
    if (raster->live_bytes >= inst->cap)
        raster->live_bytes -= inst->cap;
    else
        raster->live_bytes = 0;
    free(inst->rgba);
    inst->rgba = NULL;
    inst->cap = 0;
}

static int instance_find(const LottieRaster *raster, uint64_t handle)
{
    for (int i = 0; i < raster->count; i++)
        if (raster->instances[i].handle == handle)
            return i;
    return -1;
}

static int instance_reserve(LottieRaster *raster, LottieInstance *inst,
                            int w, int h)
{
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / 4u ||
        (size_t)h > SIZE_MAX / ((size_t)w * 4u))
        return 0;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (need > LOTTIE_RASTER_MAX_BYTES)
        return 0;

    if (inst->cap < need) {
        size_t retained = inst->cap;
        if (LOTTIE_RASTER_MAX_BYTES - raster->live_bytes + retained < need)
            return 0;
        uint8_t *pixels = realloc(inst->rgba, need);
        if (!pixels)
            return 0;
        inst->rgba = pixels;
        raster->live_bytes -= retained;
        inst->cap = need;
        raster->live_bytes += need;
    }
    inst->w = w;
    inst->h = h;
    return 1;
}

static int instance_load(LottieRaster *raster, LottieInstance *inst,
                         const CfrLottie *anim)
{
    instance_drop_scene(inst);
    inst->valid = 0;
    if (!anim->json || anim->json_len == 0 ||
        anim->json_len > UINT32_MAX)
        return 0;
    if (!instance_reserve(raster, inst, anim->px_w, anim->px_h))
        return 0;

    inst->anim = tvg_animation_new();
    if (!inst->anim)
        return 0;
    Tvg_Paint picture = tvg_animation_get_picture(inst->anim);
    if (!picture)
        goto fail;
    if (tvg_picture_load_data(picture, (const char *)anim->json,
                              (uint32_t)anim->json_len, "lottie+json", NULL,
                              true) != TVG_RESULT_SUCCESS)
        goto fail;
    if (tvg_picture_set_size(picture, (float)anim->px_w,
                             (float)anim->px_h) != TVG_RESULT_SUCCESS)
        goto fail;

    inst->canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!inst->canvas)
        goto fail;
    if (tvg_swcanvas_set_target(inst->canvas, (uint32_t *)inst->rgba,
                                (uint32_t)inst->w, (uint32_t)inst->w,
                                (uint32_t)inst->h,
                                TVG_COLORSPACE_ABGR8888S) != TVG_RESULT_SUCCESS)
        goto fail;
    if (tvg_canvas_add(inst->canvas, picture) != TVG_RESULT_SUCCESS)
        goto fail;
    return 1;

fail:
    instance_drop_scene(inst);
    return 0;
}

static int instance_resize(LottieInstance *inst, const CfrLottie *anim)
{
    if (!inst->anim || !inst->canvas)
        return 0;
    Tvg_Paint picture = tvg_animation_get_picture(inst->anim);
    if (!picture)
        return 0;
    if (tvg_picture_set_size(picture, (float)anim->px_w,
                             (float)anim->px_h) != TVG_RESULT_SUCCESS)
        return 0;
    return tvg_swcanvas_set_target(inst->canvas, (uint32_t *)inst->rgba,
                                   (uint32_t)inst->w, (uint32_t)inst->w,
                                   (uint32_t)inst->h,
                                   TVG_COLORSPACE_ABGR8888S) == TVG_RESULT_SUCCESS;
}

static int instance_draw(LottieInstance *inst, int frame, int frame_count)
{
    if (!inst->anim || !inst->canvas || !inst->rgba)
        return 0;

    Tvg_Result sr = tvg_animation_set_frame(inst->anim, (float)frame);
    if (sr != TVG_RESULT_SUCCESS && sr != TVG_RESULT_INSUFFICIENT_CONDITION)
        return 0;

    /* ThorVG 1.0 on Windows can defer animation initialization until a draw.
     * Exercise another frame once, then select the requested frame. */
    if (sr == TVG_RESULT_INSUFFICIENT_CONDITION && !inst->ever_drawn &&
        frame_count > 1) {
        float warm = (float)frame + 1.0f;
        tvg_animation_set_frame(inst->anim, warm);
        if (tvg_canvas_draw(inst->canvas, true) != TVG_RESULT_SUCCESS ||
            tvg_canvas_sync(inst->canvas) != TVG_RESULT_SUCCESS)
            return 0;
        sr = tvg_animation_set_frame(inst->anim, (float)frame);
        if (sr != TVG_RESULT_SUCCESS &&
            sr != TVG_RESULT_INSUFFICIENT_CONDITION)
            return 0;
    }

    tvg_canvas_update(inst->canvas);
    if (tvg_canvas_draw(inst->canvas, true) != TVG_RESULT_SUCCESS)
        return 0;
    if (tvg_canvas_sync(inst->canvas) != TVG_RESULT_SUCCESS)
        return 0;
    inst->ever_drawn = 1;
    return 1;
}

LottieRaster *lottie_raster_create(void)
{
    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS)
        return NULL;
    LottieRaster *raster = calloc(1, sizeof(*raster));
    if (!raster)
        tvg_engine_term();
    return raster;
}

void lottie_raster_destroy(LottieRaster *raster)
{
    if (!raster)
        return;
    for (int i = 0; i < raster->count; i++)
        instance_release(raster, &raster->instances[i]);
    free(raster->instances);
    free(raster);
    tvg_engine_term();
}

void lottie_raster_sync(LottieRaster *raster, const CfrLottie *anims,
                        int count)
{
    if (!raster)
        return;

    for (int i = 0; i < raster->count;) {
        int live = 0;
        for (int j = 0; anims && j < count; j++) {
            if (anims[j].handle == raster->instances[i].handle) {
                live = 1;
                break;
            }
        }
        if (live) {
            i++;
            continue;
        }
        instance_release(raster, &raster->instances[i]);
        raster->instances[i] = raster->instances[--raster->count];
    }

    for (int i = 0; anims && i < count; i++) {
        const CfrLottie *anim = &anims[i];
        int idx = instance_find(raster, anim->handle);
        if (idx < 0) {
            if (raster->count >= raster->cap) {
                int ncap = raster->cap ? raster->cap * 2 : 8;
                LottieInstance *ni = realloc(raster->instances,
                                             (size_t)ncap * sizeof(*ni));
                if (!ni)
                    continue;
                raster->instances = ni;
                raster->cap = ncap;
            }
            idx = raster->count++;
            memset(&raster->instances[idx], 0, sizeof(raster->instances[idx]));
            raster->instances[idx].handle = anim->handle;
        }

        LottieInstance *inst = &raster->instances[idx];
        int needs_draw = 0;
        if (!inst->definition_seen ||
            inst->definition_rev != anim->definition_rev) {
            inst->definition_rev = anim->definition_rev;
            inst->definition_seen = 1;
            inst->valid = instance_load(raster, inst, anim);
            needs_draw = 1;
        } else if (inst->w != anim->px_w || inst->h != anim->px_h) {
            if (!instance_reserve(raster, inst, anim->px_w, anim->px_h) ||
                !instance_resize(inst, anim)) {
                inst->valid = 0;
                inst->raster_rev = anim->raster_rev;
                inst->raster_seen = 1;
                continue;
            }
            needs_draw = 1;
        }

        if (!needs_draw && inst->raster_seen &&
            inst->raster_rev == anim->raster_rev)
            continue;

        inst->raster_rev = anim->raster_rev;
        inst->raster_seen = 1;
        inst->frame = anim->current_frame;
        inst->frame_count = anim->frame_count;
        if (inst->anim && inst->canvas)
            inst->valid = instance_draw(inst, anim->current_frame,
                                        anim->frame_count);
    }
}

bool lottie_raster_get(LottieRaster *raster, uint64_t handle,
                       LottieFrame *out)
{
    if (!raster || !out)
        return false;
    int idx = instance_find(raster, handle);
    if (idx < 0)
        return false;
    LottieInstance *inst = &raster->instances[idx];
    if (!inst->valid || !inst->rgba)
        return false;
    out->handle = inst->handle;
    out->raster_rev = inst->raster_rev;
    out->w = inst->w;
    out->h = inst->h;
    out->rgba = inst->rgba;
    return true;
}

#endif /* HAVE_THORVG */
