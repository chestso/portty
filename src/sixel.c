#include "sixel.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

#define SIXEL_MAX_COLORS  256
#define SIXEL_INIT_WIDTH  512
#define SIXEL_INIT_HEIGHT 128
#define SIXEL_BAND_HEIGHT 6

typedef struct
{
    uint8_t r, g, b;
} SixelColor;

// Default VGA 16-color palette
// clang-format off
static const SixelColor default_palette[16] = {
    {   0,   0,   0 }, {   0,   0, 170 }, {   0, 170,   0 }, {   0, 170, 170 },
    { 170,   0,   0 }, { 170,   0, 170 }, { 170, 170,   0 }, { 170, 170, 170 },
    {  85,  85,  85 }, {  85,  85, 255 }, {  85, 255,  85 }, {  85, 255, 255 },
    { 255,  85,  85 }, { 255,  85, 255 }, { 255, 255,  85 }, { 255, 255, 255 },
};
// clang-format on

typedef enum
{
    SIXEL_STATE_NORMAL,
    SIXEL_STATE_COLOR,  // after '#', reading color params
    SIXEL_STATE_REPEAT, // after '!', reading repeat count
    SIXEL_STATE_RASTER, // after '"', reading raster attributes
} SixelState;

struct SixelParser
{
    // Pixel buffer
    uint8_t *pixels;
    int buf_width;
    int buf_height;

    // Current draw position
    int x;
    int y; // top of current six-pixel band

    // Max extent actually drawn
    int max_x;
    int max_y;

    // Color palette
    SixelColor palette[SIXEL_MAX_COLORS];
    int current_color;

    // Parser state
    SixelState state;
    int params[5];
    int param_count;
    int accumulator;
    bool has_accumulator;

    // Background mode from DCS P2 (0/2 = fill bg, 1 = transparent)
    int bg_mode;
};

// Ensure pixel buffer is large enough for (x, y+5)
static void ensure_capacity(SixelParser *p, int need_x, int need_y)
{
    int new_w = p->buf_width;
    int new_h = p->buf_height;
    bool changed = false;

    while (new_w <= need_x) {
        new_w *= 2;
        changed = true;
    }
    while (new_h <= need_y) {
        new_h += SIXEL_INIT_HEIGHT;
        changed = true;
    }

    if (!changed)
        return;

    uint8_t *new_pixels = calloc((size_t)new_w * new_h, 4);
    if (!new_pixels)
        return;

    // Copy existing rows
    for (int row = 0; row < p->buf_height && row < new_h; row++) {
        int copy_w = p->buf_width < new_w ? p->buf_width : new_w;
        memcpy(new_pixels + (size_t)row * new_w * 4, p->pixels + (size_t)row * p->buf_width * 4,
               (size_t)copy_w * 4);
    }

    free(p->pixels);
    p->pixels = new_pixels;
    p->buf_width = new_w;
    p->buf_height = new_h;
}

// Draw a single sixel character (6 vertical pixels encoded in one byte)
static void draw_sixel(SixelParser *p, uint8_t sixel)
{
    ensure_capacity(p, p->x, p->y + SIXEL_BAND_HEIGHT - 1);

    SixelColor *col = &p->palette[p->current_color % SIXEL_MAX_COLORS];

    for (int bit = 0; bit < SIXEL_BAND_HEIGHT; bit++) {
        if (sixel & (1 << bit)) {
            int py = p->y + bit;
            if (py < p->buf_height && p->x < p->buf_width) {
                size_t offset = ((size_t)py * p->buf_width + p->x) * 4;
                p->pixels[offset + 0] = col->r;
                p->pixels[offset + 1] = col->g;
                p->pixels[offset + 2] = col->b;
                p->pixels[offset + 3] = 255;
            }
        }
    }

    if (p->x > p->max_x)
        p->max_x = p->x;
    int band_bottom = p->y + SIXEL_BAND_HEIGHT - 1;
    if (band_bottom > p->max_y)
        p->max_y = band_bottom;

    p->x++;
}

// Convert HLS to RGB (sixel uses 0-360 hue, 0-100 luminance/saturation)
static SixelColor hls_to_rgb(int h, int l, int s)
{
    SixelColor result = { 0, 0, 0 };
    if (s == 0) {
        uint8_t v = (uint8_t)(l * 255 / 100);
        result.r = result.g = result.b = v;
        return result;
    }

    double hue = h / 360.0;
    double lum = l / 100.0;
    double sat = s / 100.0;

    double m2 = (lum <= 0.5) ? lum * (1.0 + sat) : lum + sat - lum * sat;
    double m1 = 2.0 * lum - m2;

    double tr = hue + 1.0 / 3.0;
    double tg = hue;
    double tb = hue - 1.0 / 3.0;

    if (tr < 0)
        tr += 1.0;
    if (tr > 1)
        tr -= 1.0;
    if (tg < 0)
        tg += 1.0;
    if (tg > 1)
        tg -= 1.0;
    if (tb < 0)
        tb += 1.0;
    if (tb > 1)
        tb -= 1.0;

    double rgb[3] = { tr, tg, tb };
    uint8_t *out[3] = { &result.r, &result.g, &result.b };

    for (int i = 0; i < 3; i++) {
        double val;
        if (rgb[i] < 1.0 / 6.0)
            val = m1 + (m2 - m1) * 6.0 * rgb[i];
        else if (rgb[i] < 0.5)
            val = m2;
        else if (rgb[i] < 2.0 / 3.0)
            val = m1 + (m2 - m1) * (2.0 / 3.0 - rgb[i]) * 6.0;
        else
            val = m1;
        *out[i] = (uint8_t)(val * 255.0 + 0.5);
    }

    return result;
}

static void finish_color_command(SixelParser *p)
{
    if (p->param_count == 0)
        return;

    int color_index = p->params[0] % SIXEL_MAX_COLORS;

    if (p->param_count == 1) {
        // Just select color
        p->current_color = color_index;
    } else if (p->param_count >= 5) {
        int pu = p->params[1]; // color coordinate system
        int p1 = p->params[2];
        int p2 = p->params[3];
        int p3 = p->params[4];

        if (pu == 1) {
            // HLS
            p->palette[color_index] = hls_to_rgb(p1, p2, p3);
        } else if (pu == 2) {
            // RGB (0-100 range)
            p->palette[color_index].r = (uint8_t)(p1 * 255 / 100);
            p->palette[color_index].g = (uint8_t)(p2 * 255 / 100);
            p->palette[color_index].b = (uint8_t)(p3 * 255 / 100);
        }
        p->current_color = color_index;
    }
}

static void finish_raster_command(SixelParser *p)
{
    // "Pan;Pad;Ph;Pv — we only care about Ph and Pv (image dimensions)
    // for pre-allocation, but they're optional hints
    if (p->param_count >= 4 && p->params[2] > 0 && p->params[3] > 0) {
        int hint_w = p->params[2];
        int hint_h = p->params[3];
        // Pre-allocate if hints are reasonable
        if (hint_w > 0 && hint_w < 10000 && hint_h > 0 && hint_h < 10000) {
            ensure_capacity(p, hint_w - 1, hint_h - 1);
        }
    }
}

SixelParser *sixel_parser_create(void)
{
    SixelParser *p = calloc(1, sizeof(SixelParser));
    if (!p)
        return NULL;

    // Initialize default palette
    for (int i = 0; i < 16 && i < SIXEL_MAX_COLORS; i++)
        p->palette[i] = default_palette[i];

    return p;
}

void sixel_parser_destroy(SixelParser *parser)
{
    if (!parser)
        return;
    free(parser->pixels);
    free(parser);
}

void sixel_parser_begin(SixelParser *parser)
{
    if (!parser)
        return;

    free(parser->pixels);

    parser->pixels = calloc((size_t)SIXEL_INIT_WIDTH * SIXEL_INIT_HEIGHT, 4);
    parser->buf_width = SIXEL_INIT_WIDTH;
    parser->buf_height = SIXEL_INIT_HEIGHT;
    parser->x = 0;
    parser->y = 0;
    parser->max_x = 0;
    parser->max_y = 0;
    parser->current_color = 0;
    parser->state = SIXEL_STATE_NORMAL;
    parser->param_count = 0;
    parser->accumulator = 0;
    parser->has_accumulator = false;
    parser->bg_mode = 0;

    // Re-init default palette
    for (int i = 0; i < 16 && i < SIXEL_MAX_COLORS; i++)
        parser->palette[i] = default_palette[i];
}

void sixel_parser_feed(SixelParser *parser, const char *data, size_t len)
{
    if (!parser || !data)
        return;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        switch (parser->state) {
        case SIXEL_STATE_COLOR:
            if (ch >= '0' && ch <= '9') {
                parser->accumulator = parser->accumulator * 10 + (ch - '0');
                parser->has_accumulator = true;
            } else if (ch == ';') {
                if (parser->param_count < 5) {
                    parser->params[parser->param_count++] = parser->accumulator;
                }
                parser->accumulator = 0;
                parser->has_accumulator = false;
            } else {
                // End of color command
                if (parser->has_accumulator && parser->param_count < 5) {
                    parser->params[parser->param_count++] = parser->accumulator;
                }
                finish_color_command(parser);
                parser->state = SIXEL_STATE_NORMAL;
                parser->accumulator = 0;
                parser->has_accumulator = false;
                // Re-process this character in normal state
                i--;
            }
            break;

        case SIXEL_STATE_REPEAT:
            if (ch >= '0' && ch <= '9') {
                parser->accumulator = parser->accumulator * 10 + (ch - '0');
                parser->has_accumulator = true;
            } else {
                // Next char should be the sixel to repeat
                int count = parser->has_accumulator ? parser->accumulator : 1;
                parser->state = SIXEL_STATE_NORMAL;
                parser->accumulator = 0;
                parser->has_accumulator = false;

                if (ch >= '?' && ch <= '~') {
                    uint8_t sixel = ch - '?';
                    for (int r = 0; r < count; r++)
                        draw_sixel(parser, sixel);
                }
                // Don't re-process; the char was the repeated sixel
            }
            break;

        case SIXEL_STATE_RASTER:
            if (ch >= '0' && ch <= '9') {
                parser->accumulator = parser->accumulator * 10 + (ch - '0');
                parser->has_accumulator = true;
            } else if (ch == ';') {
                if (parser->param_count < 5) {
                    parser->params[parser->param_count++] = parser->accumulator;
                }
                parser->accumulator = 0;
                parser->has_accumulator = false;
            } else {
                if (parser->has_accumulator && parser->param_count < 5) {
                    parser->params[parser->param_count++] = parser->accumulator;
                }
                finish_raster_command(parser);
                parser->state = SIXEL_STATE_NORMAL;
                parser->accumulator = 0;
                parser->has_accumulator = false;
                i--;
            }
            break;

        case SIXEL_STATE_NORMAL:
            if (ch >= '?' && ch <= '~') {
                // Sixel data character
                draw_sixel(parser, ch - '?');
            } else if (ch == '#') {
                // Color introducer
                parser->state = SIXEL_STATE_COLOR;
                parser->param_count = 0;
                parser->accumulator = 0;
                parser->has_accumulator = false;
            } else if (ch == '!') {
                // Repeat introducer
                parser->state = SIXEL_STATE_REPEAT;
                parser->accumulator = 0;
                parser->has_accumulator = false;
            } else if (ch == '"') {
                // Raster attributes
                parser->state = SIXEL_STATE_RASTER;
                parser->param_count = 0;
                parser->accumulator = 0;
                parser->has_accumulator = false;
            } else if (ch == '$') {
                // Graphics carriage return
                parser->x = 0;
            } else if (ch == '-') {
                // Graphics new line
                parser->x = 0;
                parser->y += SIXEL_BAND_HEIGHT;
            }
            // Ignore other characters (SUB, DEL, etc.)
            break;
        }
    }
}

SixelImage *sixel_parser_finish(SixelParser *parser)
{
    if (!parser || !parser->pixels)
        return NULL;

    int width = parser->max_x + 1;
    int height = parser->max_y + 1;

    if (width <= 0 || height <= 0)
        return NULL;

    SixelImage *img = calloc(1, sizeof(SixelImage));
    if (!img)
        return NULL;

    img->pixels = malloc((size_t)width * height * 4);
    if (!img->pixels) {
        free(img);
        return NULL;
    }

    // Copy the relevant portion from the parser buffer
    for (int row = 0; row < height; row++) {
        memcpy(img->pixels + (size_t)row * width * 4,
               parser->pixels + (size_t)row * parser->buf_width * 4, (size_t)width * 4);
    }

    img->width = width;
    img->height = height;
    img->valid = true;

    vlog("Sixel image finished: %dx%d pixels\n", width, height);

    return img;
}

void sixel_image_free(SixelImage *img)
{
    if (!img)
        return;
    free(img->pixels);
    free(img);
}
