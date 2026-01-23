/* Custom COLR v1 paint tree traversal code.
 *
 * FreeType exposes COLR v1 paint data via FT_Get_Color_Glyph_Paint and
 * related APIs, but does not render COLR v1 paint graphs automatically
 * through FT_LOAD_COLOR (which only handles COLR v0 layers and bitmap
 * color fonts like sbix/CBDT). This file implements the recursive paint
 * tree evaluation with affine transforms and Porter-Duff compositing.
 */

#include <ft2build.h>
#include FT_FREETYPE_H
#include "common.h"
#include "font.h"
#include "font_ft_internal.h"
#include <freetype2/freetype/ftcolor.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper: convert 16.16 fixed to double
static double ft_fixed_to_double(FT_Fixed v)
{
    return (double)v / 65536.0;
}

// Helper: convert 26.6 fixed (FT_Pos) to double
static double ft_pos_to_double(FT_Pos v)
{
    return (double)v / 64.0;
}

// Simple 2x3 affine matrix for transforms (row-major)
typedef struct
{
    double xx, xy, dx;
    double yx, yy, dy;
} Affine;

static void affine_identity(Affine *a)
{
    a->xx = 1.0;
    a->xy = 0.0;
    a->dx = 0.0;
    a->yx = 0.0;
    a->yy = 1.0;
    a->dy = 0.0;
}

static void affine_from_FT_Affine23(Affine *out, const FT_Affine23 *in)
{
    if (!out || !in)
        return;
    out->xx = ft_fixed_to_double(in->xx);
    out->xy = ft_fixed_to_double(in->xy);
    out->dx = ft_fixed_to_double(in->dx);
    out->yx = ft_fixed_to_double(in->yx);
    out->yy = ft_fixed_to_double(in->yy);
    out->dy = ft_fixed_to_double(in->dy);
}

static void affine_mul(Affine *out, const Affine *a, const Affine *b)
{
    // out = a * b
    Affine r;
    r.xx = a->xx * b->xx + a->xy * b->yx;
    r.xy = a->xx * b->xy + a->xy * b->yy;
    r.dx = a->xx * b->dx + a->xy * b->dy + a->dx;

    r.yx = a->yx * b->xx + a->yy * b->yx;
    r.yy = a->yx * b->xy + a->yy * b->yy;
    r.dy = a->yx * b->dx + a->yy * b->dy + a->dy;

    *out = r;
}

static void affine_apply(const Affine *a, double x, double y, double *rx, double *ry)
{
    if (!a) {
        *rx = x;
        *ry = y;
        return;
    }
    *rx = x * a->xx + y * a->xy + a->dx;
    *ry = x * a->yx + y * a->yy + a->dy;
}

// Affine helpers for Translate/Scale/Rotate/Skew with center handling
static void affine_from_translate(Affine *out, double dx, double dy)
{
    affine_identity(out);
    out->dx = dx;
    out->dy = dy;
}

static void affine_from_scale_center(Affine *out, double sx, double sy, double cx, double cy)
{
    out->xx = sx;
    out->xy = 0.0;
    out->dx = cx - sx * cx;
    out->yx = 0.0;
    out->yy = sy;
    out->dy = cy - sy * cy;
}

static void affine_from_rotate_center(Affine *out, double angle_rad, double cx, double cy)
{
    double c = cos(angle_rad);
    double s = sin(angle_rad);
    out->xx = c;
    out->xy = -s;
    out->dx = cx - (c * cx - s * cy);
    out->yx = s;
    out->yy = c;
    out->dy = cy - (s * cx + c * cy);
}

static void affine_from_skew_center(Affine *out, double x_skew_rad, double y_skew_rad, double cx, double cy)
{
    double tx = tan(x_skew_rad);
    double ty = tan(y_skew_rad);
    out->xx = 1.0;
    out->xy = tx;
    out->dx = cx - (1.0 * cx + tx * cy);
    out->yx = ty;
    out->yy = 1.0;
    out->dy = cy - (ty * cx + 1.0 * cy);
}

// Linear interpolation between two colors (RGBA 0..255)
static void lerp_color(uint8_t *out, uint8_t a_r, uint8_t a_g, uint8_t a_b, uint8_t a_a,
                       uint8_t b_r, uint8_t b_g, uint8_t b_b, uint8_t b_a, double t)
{
    if (t <= 0.0) {
        out[0] = a_r;
        out[1] = a_g;
        out[2] = a_b;
        out[3] = a_a;
        return;
    }
    if (t >= 1.0) {
        out[0] = b_r;
        out[1] = b_g;
        out[2] = b_b;
        out[3] = b_a;
        return;
    }
    out[0] = (uint8_t)round((1.0 - t) * a_r + t * b_r);
    out[1] = (uint8_t)round((1.0 - t) * a_g + t * b_g);
    out[2] = (uint8_t)round((1.0 - t) * a_b + t * b_b);
    out[3] = (uint8_t)round((1.0 - t) * a_a + t * b_a);
}

// Evaluate colorline stops into an array of stops (small dynamic array)
typedef struct
{
    double offset;
    uint8_t r, g, b, a;
} Stop;

static Stop *eval_colorline(FtFontData *ft_data, FT_ColorLine *cline, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, int *out_count)
{
    if (!cline || !out_count)
        return NULL;
    FT_ColorStopIterator it = cline->color_stop_iterator;
    FT_ColorStop stop;
    Stop *stops = NULL;
    int count = 0;
    while (FT_Get_Colorline_Stops(ft_data->ft_face, &stop, &it)) {
        Stop *n = realloc(stops, sizeof(Stop) * (count + 1));
        if (!n)
            break;
        stops = n;
        uint8_t r, g, b, a;
        resolve_colorindex(ft_data, stop.color, fg_r, fg_g, fg_b, &r, &g, &b, &a);
        stops[count].offset = ft_fixed_to_double(stop.stop_offset); // 16.16 -> double
        stops[count].r = r;
        stops[count].g = g;
        stops[count].b = b;
        stops[count].a = a;
        count++;
    }
    *out_count = count;
    return stops;
}

// Paint a linear gradient into an RGBA buffer covering bbox (in pixels).
// p0,p1,p2 are in FT 16.16 font units and will be transformed and scaled to pixel space.
static void paint_linear_gradient(FtFontData *ft_data, FT_PaintLinearGradient *lg,
                                  Affine *matrix, bool matrix_maps_font_units, uint8_t *buf, int w, int h,
                                  int dst_x_off, int dst_y_off, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !lg || !buf)
        return;

    double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
    // Convert p0,p1 from 16.16 font-units to device space using scale_factor
    double p0x = ft_fixed_to_double(lg->p0.x) * scale_factor;
    double p0y = ft_fixed_to_double(lg->p0.y) * scale_factor;
    double p1x = ft_fixed_to_double(lg->p1.x) * scale_factor;
    double p1y = ft_fixed_to_double(lg->p1.y) * scale_factor;
    // p2 optional used to rotate; for now ignore p2 (basic linear gradient)

    // Apply affine transform if present
    double tp0x, tp0y, tp1x, tp1y;
    affine_apply(matrix, p0x, p0y, &tp0x, &tp0y);
    affine_apply(matrix, p1x, p1y, &tp1x, &tp1y);

    // Direction vector
    double dx = tp1x - tp0x;
    double dy = tp1y - tp0y;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-8)
        len2 = 1e-8;

    // Evaluate color stops
    int stop_count = 0;
    Stop *stops = eval_colorline(ft_data, &lg->colorline, fg_r, fg_g, fg_b, &stop_count);
    if (!stops || stop_count == 0) {
        if (stops)
            free(stops);
        return;
    }

    // For each pixel in bbox, compute projection t along gradient (normalized by distance between p0 and p1)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // pixel center coordinates in destination space
            double px = (double)(x + dst_x_off);
            double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
            // vector from p0 to pixel
            double vx = px - tp0x;
            double vy = py - tp0y;
            double t = (vx * dx + vy * dy) / len2; // can be <0 or >1

            // Handle extend modes: only PAD for now
            if (t < 0.0) {
                t = 0.0;
            }
            if (t > 1.0) {
                t = 1.0;
            }

            // Find stops surrounding t
            int idx = 0;
            while (idx < stop_count && stops[idx].offset < t)
                idx++;
            uint8_t outc[4] = { 0, 0, 0, 0 };
            if (idx == 0) {
                outc[0] = stops[0].r;
                outc[1] = stops[0].g;
                outc[2] = stops[0].b;
                outc[3] = stops[0].a;
            } else if (idx >= stop_count) {
                outc[0] = stops[stop_count - 1].r;
                outc[1] = stops[stop_count - 1].g;
                outc[2] = stops[stop_count - 1].b;
                outc[3] = stops[stop_count - 1].a;
            } else {
                double t0 = stops[idx - 1].offset;
                double t1 = stops[idx].offset;
                double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                           stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
            }

            uint8_t *pixel = buf + (y * w + x) * 4;
            pixel[0] = outc[0];
            pixel[1] = outc[1];
            pixel[2] = outc[2];
            pixel[3] = outc[3];
        }
    }

    free(stops);
}

// Recursive paint evaluator for FT_COLR v1 paints. This function paints into `buf` (RGBA) of size w*h
// with an origin offset (dst_x_off,dst_y_off) applied to gradient coordinate space. The `opaque` argument
// references the paint table to evaluate.
static bool paint_colr_paint_recursive(FtFontData *ft_data, FT_OpaquePaint opaque, Affine *matrix,
                                       bool matrix_maps_font_units,
                                       uint8_t *buf, int w, int h, int dst_x_off, int dst_y_off,
                                       uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !buf)
        return false;

    FT_COLR_Paint paint;
    if (!FT_Get_Paint(ft_data->ft_face, opaque, &paint))
        return false;

    switch (paint.format) {
    case FT_COLR_PAINTFORMAT_SOLID:
    {
        uint8_t r, g, b, a;
        resolve_colorindex(ft_data, paint.u.solid.color, fg_r, fg_g, fg_b, &r, &g, &b, &a);
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = r;
            buf[i * 4 + 1] = g;
            buf[i * 4 + 2] = b;
            buf[i * 4 + 3] = a;
        }
        return true;
    }
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    {
        paint_linear_gradient(ft_data, &paint.u.linear_gradient, matrix, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
        return true;
    }
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
    {
        // Radial gradient implementation
        FT_PaintRadialGradient *rg = &paint.u.radial_gradient;
        // Convert centers and radii from 16.16 to pixel space and apply affine
        double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
        double c0x = ft_fixed_to_double(rg->c0.x) * scale_factor;
        double c0y = ft_fixed_to_double(rg->c0.y) * scale_factor;
        double c1x = ft_fixed_to_double(rg->c1.x) * scale_factor;
        double c1y = ft_fixed_to_double(rg->c1.y) * scale_factor;
        double r0 = ft_fixed_to_double(rg->r0) * scale_factor;
        double r1 = ft_fixed_to_double(rg->r1) * scale_factor;

        double tc0x, tc0y, tc1x, tc1y;
        affine_apply(matrix, c0x, c0y, &tc0x, &tc0y);
        affine_apply(matrix, c1x, c1y, &tc1x, &tc1y);

        int stop_count = 0;
        Stop *stops = eval_colorline(ft_data, &rg->colorline, fg_r, fg_g, fg_b, &stop_count);
        if (!stops || stop_count == 0) {
            if (stops)
                free(stops);
            return false;
        }

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double px = (double)(x + dst_x_off);
                double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
                // distance to c0
                double d = hypot(px - tc0x, py - tc0y);
                double denom = (r1 - r0);
                double t = denom == 0.0 ? 0.0 : (d - r0) / denom;
                if (t < 0.0) {
                    t = 0.0;
                }
                if (t > 1.0) {
                    t = 1.0;
                }

                int idx = 0;
                while (idx < stop_count && stops[idx].offset < t)
                    idx++;
                uint8_t outc[4];
                if (idx == 0) {
                    outc[0] = stops[0].r;
                    outc[1] = stops[0].g;
                    outc[2] = stops[0].b;
                    outc[3] = stops[0].a;
                } else if (idx >= stop_count) {
                    outc[0] = stops[stop_count - 1].r;
                    outc[1] = stops[stop_count - 1].g;
                    outc[2] = stops[stop_count - 1].b;
                    outc[3] = stops[stop_count - 1].a;
                } else {
                    double t0 = stops[idx - 1].offset;
                    double t1 = stops[idx].offset;
                    double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                    lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                               stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
                }
                uint8_t *pixel = buf + (y * w + x) * 4;
                pixel[0] = outc[0];
                pixel[1] = outc[1];
                pixel[2] = outc[2];
                pixel[3] = outc[3];
            }
        }
        free(stops);
        return true;
    }
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
    {
        // Sweep gradient implementation
        FT_PaintSweepGradient *sg = &paint.u.sweep_gradient;
        double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
        double cx = ft_fixed_to_double(sg->center.x) * scale_factor;
        double cy = ft_fixed_to_double(sg->center.y) * scale_factor;
        double tcx, tcy;
        affine_apply(matrix, cx, cy, &tcx, &tcy);

        double start_deg = ft_fixed_to_double(sg->start_angle) * 180.0;
        double end_deg = ft_fixed_to_double(sg->end_angle) * 180.0;

        int stop_count = 0;
        Stop *stops = eval_colorline(ft_data, &sg->colorline, fg_r, fg_g, fg_b, &stop_count);
        if (!stops || stop_count == 0) {
            if (stops)
                free(stops);
            return false;
        }

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double px = (double)(x + dst_x_off);
                double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
                double vx = px - tcx;
                double vy = py - tcy;
                // compute angle in degrees measured counter-clockwise from positive y axis
                double ang_rad = atan2(vx, vy); // swap to get from positive y
                double ang_deg = ang_rad * (180.0 / M_PI);
                if (ang_deg < 0)
                    ang_deg += 360.0;

                // Normalize between start_deg and end_deg
                double total = end_deg - start_deg;
                if (total < 0)
                    total += 360.0;
                double rel = ang_deg - start_deg;
                if (rel < 0)
                    rel += 360.0;
                double t = total == 0.0 ? 0.0 : rel / total;
                if (t < 0.0) {
                    t = 0.0;
                }
                if (t > 1.0) {
                    t = 1.0;
                }

                int idx = 0;
                while (idx < stop_count && stops[idx].offset < t)
                    idx++;
                uint8_t outc[4];
                if (idx == 0) {
                    outc[0] = stops[0].r;
                    outc[1] = stops[0].g;
                    outc[2] = stops[0].b;
                    outc[3] = stops[0].a;
                } else if (idx >= stop_count) {
                    outc[0] = stops[stop_count - 1].r;
                    outc[1] = stops[stop_count - 1].g;
                    outc[2] = stops[stop_count - 1].b;
                    outc[3] = stops[stop_count - 1].a;
                } else {
                    double t0 = stops[idx - 1].offset;
                    double t1 = stops[idx].offset;
                    double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                    lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                               stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
                }
                uint8_t *pixel = buf + (y * w + x) * 4;
                pixel[0] = outc[0];
                pixel[1] = outc[1];
                pixel[2] = outc[2];
                pixel[3] = outc[3];
            }
        }
        free(stops);
        return true;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE:
    {
        FT_PaintComposite *pc = &paint.u.composite;
        uint8_t *tmp_back = calloc((size_t)w * (size_t)h, 4);
        uint8_t *tmp_src = calloc((size_t)w * (size_t)h, 4);
        if (!tmp_back || !tmp_src) {
            if (tmp_back)
                free(tmp_back);
            if (tmp_src)
                free(tmp_src);
            return false;
        }

        // Evaluate backdrop and source paints into temporary buffers
        paint_colr_paint_recursive(ft_data, pc->backdrop_paint, matrix, matrix_maps_font_units, tmp_back, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
        paint_colr_paint_recursive(ft_data, pc->source_paint, matrix, matrix_maps_font_units, tmp_src, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);

        switch (pc->composite_mode) {
        case FT_COLR_COMPOSITE_SRC_OVER:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_PLUS:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                int r = s[0] + b[0];
                int g = s[1] + b[1];
                int bl = s[2] + b[2];
                int a = s[3] + b[3];
                dst[0] = (uint8_t)(r > 255 ? 255 : r);
                dst[1] = (uint8_t)(g > 255 ? 255 : g);
                dst[2] = (uint8_t)(bl > 255 ? 255 : bl);
                dst[3] = (uint8_t)(a > 255 ? 255 : a);
            }
            break;
        }
        case FT_COLR_COMPOSITE_MULTIPLY:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * (sr * br) + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * (sg * bg) + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * (sb * bb) + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SCREEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // Screen: B(Cs,Cb) = Cs + Cb - Cs*Cb
                float Br = sr + br - sr * br;
                float Bg = sg + bg - sg * bg;
                float Bb = sb + bb - sb * bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_OVERLAY:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // Overlay: B(Cs,Cb) = HardLight(Cb,Cs)
                float Br = br < 0.5f ? 2.0f * sr * br : 1.0f - 2.0f * (1.0f - sr) * (1.0f - br);
                float Bg = bg < 0.5f ? 2.0f * sg * bg : 1.0f - 2.0f * (1.0f - sg) * (1.0f - bg);
                float Bb = bb < 0.5f ? 2.0f * sb * bb : 1.0f - 2.0f * (1.0f - sb) * (1.0f - bb);
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DARKEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float Br = sr < br ? sr : br;
                float Bg = sg < bg ? sg : bg;
                float Bb = sb < bb ? sb : bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_LIGHTEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float Br = sr > br ? sr : br;
                float Bg = sg > bg ? sg : bg;
                float Bb = sb > bb ? sb : bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SOFT_LIGHT:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // SoftLight: if Cs <= 0.5: B = Cb - (1-2*Cs)*Cb*(1-Cb)
                //            else: B = Cb + (2*Cs-1)*(D(Cb)-Cb)
                //   where D(Cb) = sqrt(Cb) if Cb > 0.25
                //                  ((16*Cb-12)*Cb+4)*Cb otherwise
                float Dr, Dg, Db;
                Dr = br > 0.25f ? sqrtf(br) : ((16.0f * br - 12.0f) * br + 4.0f) * br;
                Dg = bg > 0.25f ? sqrtf(bg) : ((16.0f * bg - 12.0f) * bg + 4.0f) * bg;
                Db = bb > 0.25f ? sqrtf(bb) : ((16.0f * bb - 12.0f) * bb + 4.0f) * bb;
                float Br = sr <= 0.5f ? br - (1.0f - 2.0f * sr) * br * (1.0f - br)
                                      : br + (2.0f * sr - 1.0f) * (Dr - br);
                float Bg = sg <= 0.5f ? bg - (1.0f - 2.0f * sg) * bg * (1.0f - bg)
                                      : bg + (2.0f * sg - 1.0f) * (Dg - bg);
                float Bb = sb <= 0.5f ? bb - (1.0f - 2.0f * sb) * bb * (1.0f - bb)
                                      : bb + (2.0f * sb - 1.0f) * (Db - bb);
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_CLEAR:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = back[3];
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_OVER:
        {
            // DEST_OVER: Fs = (1-backA), Fd = 1
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = (1.0f - ba) * sa + ba;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_IN:
        {
            // SRC_IN: outA = srcA * backA; outRGB = srcRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * ba;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_IN:
        {
            // DEST_IN: outA = backA * srcA; outRGB = backRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba * sa;
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_OUT:
        {
            // SRC_OUT: outA = srcA * (1-backA); outRGB = srcRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * (1.0f - ba);
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_OUT:
        {
            // DEST_OUT: outA = backA * (1-srcA); outRGB = backRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba * (1.0f - sa);
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_ATOP:
        {
            // SRC_ATOP: Fs = backA, Fd = (1-srcA); outA = backA
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * ba + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa * ba + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa * ba + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_ATOP:
        {
            // DEST_ATOP: Fs = (1-backA), Fd = srcA; outA = srcA
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba * sa) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba * sa) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba * sa) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_XOR:
        {
            // XOR: Fs = (1-backA), Fd = (1-srcA)
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * (1.0f - ba) + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        default:
        {
            // Fallback for unimplemented composite modes: use SRC_OVER
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f;
                float ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float rr = (sr * sa + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        }
        free(tmp_back);
        free(tmp_src);
        return true;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE:
    {
        FT_PaintTranslate *pt = &paint.u.translate;
        double dx = ft_fixed_to_double(pt->dx) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double dy = ft_fixed_to_double(pt->dy) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_translate(&local, dx, dy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pt->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_SCALE:
    {
        FT_PaintScale *ps = &paint.u.scale;
        double sx = ft_fixed_to_double(ps->scale_x);
        double sy = ft_fixed_to_double(ps->scale_y);
        double cx = ft_fixed_to_double(ps->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(ps->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_scale_center(&local, sx, sy, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, ps->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_ROTATE:
    {
        FT_PaintRotate *pr = &paint.u.rotate;
        double angle_deg = ft_fixed_to_double(pr->angle) * 180.0;
        double angle_rad = angle_deg * (M_PI / 180.0);
        double cx = ft_fixed_to_double(pr->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(pr->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_rotate_center(&local, angle_rad, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pr->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_SKEW:
    {
        FT_PaintSkew *psk = &paint.u.skew;
        double xsk = ft_fixed_to_double(psk->x_skew_angle) * M_PI; // approximate
        double ysk = ft_fixed_to_double(psk->y_skew_angle) * M_PI; // approximate
        double cx = ft_fixed_to_double(psk->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(psk->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_skew_center(&local, xsk, ysk, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, psk->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_COLR_GLYPH:
    {
        FT_PaintColrGlyph *pcg = &paint.u.colr_glyph;
        FT_OpaquePaint nested = { NULL, 0 };
        if (!FT_Get_Color_Glyph_Paint(ft_data->ft_face, pcg->glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &nested))
            return false;
        return paint_colr_paint_recursive(ft_data, nested, matrix, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_COLR_LAYERS:
    {
        FT_PaintColrLayers *pcl = &paint.u.colr_layers;
        FT_LayerIterator it = pcl->layer_iterator; // copy iterator
        FT_OpaquePaint layer_opaque = { NULL, 0 };
        while (FT_Get_Paint_Layers(ft_data->ft_face, &it, &layer_opaque)) {
            uint8_t *tmp = calloc((size_t)w * (size_t)h, 4);
            if (!tmp)
                return false;
            paint_colr_paint_recursive(ft_data, layer_opaque, matrix, matrix_maps_font_units, tmp, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
            // composite tmp over buf (src-over)
            for (int i = 0; i < w * h; i++) {
                uint8_t *s = &tmp[i * 4];
                uint8_t *d = &buf[i * 4];
                float sa = s[3] / 255.0f;
                float da = d[3] / 255.0f;
                float outa = sa + da * (1.0f - sa);
                if (outa <= 0.0f) {
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float dr = d[0] / 255.0f, dg = d[1] / 255.0f, db = d[2] / 255.0f;
                float rr = (sr * sa + dr * da * (1.0f - sa)) / outa;
                float rg = (sg * sa + dg * da * (1.0f - sa)) / outa;
                float rb = (sb * sa + db * da * (1.0f - sa)) / outa;
                d[0] = (uint8_t)round(rr * 255.0f);
                d[1] = (uint8_t)round(rg * 255.0f);
                d[2] = (uint8_t)round(rb * 255.0f);
                d[3] = (uint8_t)round(outa * 255.0f);
            }
            free(tmp);
        }
        return true;
    }
    case FT_COLR_PAINTFORMAT_GLYPH:
    {
        FT_PaintGlyph *pg = &paint.u.glyph;
        FT_Face face = ft_data->ft_face;

        // Apply accumulated transform (relative to root scale) via FT_Set_Transform
        // so that the glyph outline is rendered at the correct transformed position.
        // Only apply when there's a non-trivial transform (translation or non-identity matrix).
        double div = matrix_maps_font_units ? ft_data->scale : 1.0;
        bool need_transform = (fabs(matrix->dx) > 1e-6 || fabs(matrix->dy) > 1e-6 ||
                               fabs(matrix->xx / div - 1.0) > 1e-6 || fabs(matrix->xy / div) > 1e-6 ||
                               fabs(matrix->yx / div) > 1e-6 || fabs(matrix->yy / div - 1.0) > 1e-6);
        if (need_transform) {
            FT_Matrix ft_matrix;
            ft_matrix.xx = (FT_Fixed)((matrix->xx / div) * 0x10000);
            ft_matrix.xy = (FT_Fixed)((matrix->xy / div) * 0x10000);
            ft_matrix.yx = (FT_Fixed)((matrix->yx / div) * 0x10000);
            ft_matrix.yy = (FT_Fixed)((matrix->yy / div) * 0x10000);
            FT_Vector ft_delta;
            ft_delta.x = (FT_Pos)round(matrix->dx * 64.0);
            ft_delta.y = (FT_Pos)round(matrix->dy * 64.0);
            FT_Set_Transform(face, &ft_matrix, &ft_delta);
        }

        int mw = 0, mh = 0, left = 0, top = 0;
        unsigned char *mask = rasterize_glyph_mask(ft_data, (FT_UInt)pg->glyphID, &mw, &mh, &left, &top);
        if (need_transform)
            FT_Set_Transform(face, NULL, NULL);
        if (!mask)
            return false;

        uint8_t *tmp = calloc((size_t)mw * (size_t)mh, 4);
        if (!tmp) {
            free(mask);
            return false;
        }

        FT_OpaquePaint child = pg->paint;
        paint_colr_paint_recursive(ft_data, child, matrix, matrix_maps_font_units, tmp, mw, mh, left, top, fg_r, fg_g, fg_b);

        for (int y = 0; y < mh; y++) {
            for (int x = 0; x < mw; x++) {
                uint8_t mask_a = mask[y * mw + x];
                if (mask_a == 0)
                    continue;
                int dst_x = left + x - dst_x_off;
                int dst_y = dst_y_off - top + y; // Y-flip: convert from FreeType Y-up to pixel Y-down
                if (dst_x < 0 || dst_x >= w || dst_y < 0 || dst_y >= h)
                    continue;

                uint8_t *dst = &buf[(dst_y * w + dst_x) * 4];
                uint8_t *src = &tmp[(y * mw + x) * 4];

                float sa = (mask_a / 255.0f) * (src[3] / 255.0f);
                float da = dst[3] / 255.0f;
                float outa = sa + da * (1.0f - sa);
                if (outa <= 0.0f)
                    continue;
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float dr = dst[0] / 255.0f, dg = dst[1] / 255.0f, db = dst[2] / 255.0f;
                float rr = (sr * sa + dr * da * (1.0f - sa)) / outa;
                float rg = (sg * sa + dg * da * (1.0f - sa)) / outa;
                float rb = (sb * sa + db * da * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
        }

        free(mask);
        free(tmp);
        return true;
    }
    case FT_COLR_PAINTFORMAT_TRANSFORM:
    {
        FT_PaintTransform *pt = &paint.u.transform;
        Affine local;
        affine_from_FT_Affine23(&local, &pt->affine);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pt->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    default:
        // Unsupported paint types fall back to solid transparent
        vlog("Unsupported FT_COLR paint format: %d\n", paint.format);
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = 0;
            buf[i * 4 + 1] = 0;
            buf[i * 4 + 2] = 0;
            buf[i * 4 + 3] = 0;
        }
        return false;
    }
}

// Render COLR v1 paint for a glyph id into an RGBA bitmap using FT_Get_Color_Glyph_Paint
GlyphBitmap *render_colr_paint_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;

    FT_OpaquePaint root = { NULL, 0 };
    bool have_root_transform = false;
    int got = FT_Get_Color_Glyph_Paint(ft_data->ft_face, glyph_index, FT_COLOR_INCLUDE_ROOT_TRANSFORM, &root);
    if (got) {
        have_root_transform = true;
    } else {
        vlog("FT_Get_Color_Glyph_Paint with INCLUDE_ROOT_TRANSFORM failed for glyph %u, trying NO_ROOT_TRANSFORM\n", glyph_index);
        if (!FT_Get_Color_Glyph_Paint(ft_data->ft_face, glyph_index, FT_COLOR_NO_ROOT_TRANSFORM, &root)) {
            vlog("FT_Get_Color_Glyph_Paint failed for glyph %u (both INCLUDE_ROOT_TRANSFORM and NO_ROOT_TRANSFORM)\n", glyph_index);
            return NULL;
        }
    }

    // Get root paint
    FT_COLR_Paint root_paint;
    if (!FT_Get_Paint(ft_data->ft_face, root, &root_paint)) {
        vlog("FT_Get_Paint failed for glyph %u\n", glyph_index);
        return NULL;
    }

    // Determine bounding box for glyph using FT_Get_Color_Glyph_ClipBox if available
    FT_ClipBox clip;
    int have_clip = FT_Get_Color_Glyph_ClipBox(ft_data->ft_face, glyph_index, &clip);
    int out_w = 0, out_h = 0, xoff = 0, yoff = 0;
    if (have_clip) {
        // clip coordinates are in 26.6 fixed; convert to pixels
        double blx = ft_pos_to_double(clip.bottom_left.x);
        double bly = ft_pos_to_double(clip.bottom_left.y);
        double trx = ft_pos_to_double(clip.top_right.x);
        double try_ = ft_pos_to_double(clip.top_right.y);
        vlog("COLR clipbox raw 26.6: bl=(%ld,%ld) tr=(%ld,%ld)\n",
             (long)clip.bottom_left.x, (long)clip.bottom_left.y,
             (long)clip.top_right.x, (long)clip.top_right.y);
        vlog("COLR clipbox pixels: bl=(%.2f,%.2f) tr=(%.2f,%.2f)\n", blx, bly, trx, try_);
        xoff = (int)floor(blx);
        yoff = (int)ceil(try_);
        out_w = (int)ceil(trx - blx);
        out_h = (int)ceil(try_ - bly);
        vlog("COLR clipbox result: xoff=%d yoff=%d w=%d h=%d\n", xoff, yoff, out_w, out_h);
        if (out_w <= 0 || out_h <= 0)
            return NULL;
    } else {
        // Fallback to rasterizing a simple monochrome glyph to determine bbox
        GlyphBitmap *gb = rasterize_glyph_index(ft_data, glyph_index, 255, 255, 255);
        if (!gb)
            return NULL;
        out_w = gb->width;
        out_h = gb->height;
        xoff = gb->x_offset;
        yoff = gb->y_offset;
        if (gb->pixels)
            free(gb->pixels);
        free(gb);
        if (out_w <= 0 || out_h <= 0)
            return NULL;
    }

    GlyphBitmap *out = malloc(sizeof(GlyphBitmap));
    if (!out)
        return NULL;
    out->width = out_w;
    out->height = out_h;
    out->x_offset = xoff;
    out->y_offset = yoff;
    out->advance = 0;
    out->glyph_id = glyph_index;
    out->pixels = calloc(out_w * out_h, 4);
    if (!out->pixels) {
        free(out);
        return NULL;
    }

    // Evaluate root paint into out->pixels
    // Decide whether paint coordinates are in font units or pixels
    // When FT_COLOR_INCLUDE_ROOT_TRANSFORM is used, FreeType returns all paint
    // coordinates in font units (unscaled). Otherwise, they're in pixels.
    bool matrix_maps_font_units = have_root_transform;
    Affine identity;
    affine_identity(&identity);
    paint_colr_paint_recursive(ft_data, root, &identity, matrix_maps_font_units, out->pixels, out_w, out_h, xoff, yoff, fg_r, fg_g, fg_b);

    return out;
}
