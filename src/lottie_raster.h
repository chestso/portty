/*
 * portty — Host-side Lottie software rasterizer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */
#ifndef PORTTY_LOTTIE_RASTER_H
#define PORTTY_LOTTIE_RASTER_H

#include <coffer/coffer.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct LottieRaster LottieRaster;

typedef struct
{
    uint64_t handle;
    uint32_t raster_rev;
    int w;
    int h;
    /* Straight-alpha, sRGB-encoded RGBA8. Borrowed from the rasterizer and
     * valid until the next lottie_raster_sync() or lottie_raster_destroy(). */
    const uint8_t *rgba;
} LottieFrame;

bool lottie_raster_available(void);
LottieRaster *lottie_raster_create(void);
void lottie_raster_destroy(LottieRaster *raster);

/* Reconcile the host's scenes with the engine snapshot. A definition revision
 * rebuilds the vector scene; a raster revision redraws the current frame.
 * Unchanged revisions do no ThorVG work. */
void lottie_raster_sync(LottieRaster *raster, const CfrLottie *anims,
                        int count);

bool lottie_raster_get(LottieRaster *raster, uint64_t handle,
                       LottieFrame *out);

#endif /* PORTTY_LOTTIE_RASTER_H */
