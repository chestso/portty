#include "rend_common.h"
#include "common.h"
#include "unicode.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

float rend_srgb_to_linear(uint8_t v)
{
    double c = v / 255.0;
    double lin = (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
    return (float)lin;
}

uint8_t rend_linear_to_srgb(float lin)
{
    double l = lin;
    double s = (l <= 0.0031308) ? (l * 12.92) : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
    int v = (int)(s * 255.0 + 0.5);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// =============================================================================
// Glyph downscaling — GPU-agnostic
// =============================================================================
//
// Scales a rasterized glyph bitmap to fit within max_w × max_h using
// area-averaging. When height_only_fit is true, only vertical overflow
// triggers a downscale (for symbol-class glyphs whose natural advance is
// wider than the cell — horizontal overhang is allowed and handled by the
// two-pass row draw). Otherwise both dimensions are checked and the
// bitmap is scaled by the smaller ratio to preserve aspect.
GlyphBitmap *rend_downscale_bitmap(GlyphBitmap *src, int max_w, int max_h,
                                   bool height_only_fit)
{
    if (!src || !src->pixels || src->width <= 0 || src->height <= 0)
        return NULL;
    if (height_only_fit) {
        if (src->height <= max_h)
            return NULL;
    } else if (src->width <= max_w && src->height <= max_h) {
        return NULL;
    }

    float scale_x = (float)max_w / (float)src->width;
    float scale_y = (float)max_h / (float)src->height;
    float scale = height_only_fit ? scale_y : fminf(scale_x, scale_y);

    int dst_w = (int)(src->width * scale + 0.5f);
    int dst_h = (int)(src->height * scale + 0.5f);
    if (dst_w <= 0)
        dst_w = 1;
    if (dst_h <= 0)
        dst_h = 1;

    vlog("Downscale: src=%dx%d max=%dx%d scale=%.3f dst=%dx%d\n",
         src->width, src->height, max_w, max_h, scale, dst_w, dst_h);

    uint8_t *dst_pixels = calloc((size_t)dst_w * dst_h, 4);
    if (!dst_pixels)
        return NULL;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy0 = dy * src->height / dst_h;
        int sy1 = (dy + 1) * src->height / dst_h;
        if (sy1 > src->height)
            sy1 = src->height;
        if (sy0 == sy1)
            sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = dx * src->width / dst_w;
            int sx1 = (dx + 1) * src->width / dst_w;
            if (sx1 > src->width)
                sx1 = src->width;
            if (sx0 == sx1)
                sx1 = sx0 + 1;

            float pr_sum = 0, pg_sum = 0, pb_sum = 0, a_sum = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    uint8_t *p = src->pixels + (sy * src->width + sx) * 4;
                    float a = p[3] / 255.0f;
                    pr_sum += p[0] * a;
                    pg_sum += p[1] * a;
                    pb_sum += p[2] * a;
                    a_sum += p[3];
                    count++;
                }
            }
            if (count > 0) {
                uint8_t *dp = dst_pixels + (dy * dst_w + dx) * 4;
                float avg_a = a_sum / count;
                if (avg_a > 0.5f) {
                    float inv = 255.0f / a_sum;
                    dp[0] = (uint8_t)fminf(pr_sum * inv + 0.5f, 255.0f);
                    dp[1] = (uint8_t)fminf(pg_sum * inv + 0.5f, 255.0f);
                    dp[2] = (uint8_t)fminf(pb_sum * inv + 0.5f, 255.0f);
                } else {
                    dp[0] = dp[1] = dp[2] = 0;
                }
                dp[3] = (uint8_t)(avg_a + 0.5f);
            }
        }
    }

    GlyphBitmap *result = malloc(sizeof(GlyphBitmap));
    if (!result) {
        free(dst_pixels);
        return NULL;
    }
    result->pixels = dst_pixels;
    result->width = dst_w;
    result->height = dst_h;
    result->x_offset = (int)(src->x_offset * scale + 0.5f);
    result->y_offset = (int)(src->y_offset * scale + 0.5f);
    result->advance = (int)(src->advance * scale + 0.5f);
    result->glyph_id = src->glyph_id;

    return result;
}

// Symbol/dingbat codepoints whose glyphs frequently exceed the text cell.
// Box Drawing (0x2500-0x257F) and Block Elements (0x2580-0x259F) are drawn
// procedurally elsewhere and intentionally excluded.
bool rend_is_symbol_cell_cp(uint32_t cp)
{
    return is_emoji_presentation(cp) || is_regional_indicator(cp) ||
           (cp >= 0x2300 && cp <= 0x23FF) || // Misc Technical
           (cp >= 0x25A0 && cp <= 0x25FF) || // Geometric Shapes
           (cp >= 0x2900 && cp <= 0x297F) || // Supplemental Arrows-B
           (cp >= 0x2B00 && cp <= 0x2BFF);   // Misc Symbols and Arrows
}

bool rend_is_color_font(FontBackend *font, FontStyle style)
{
    return style == FONT_STYLE_EMOJI || font_style_has_colr(font, style);
}

bool rend_should_use_emoji(const uint32_t *cps, int cp_count,
                           bool emoji_font_available, bool emoji_has_glyph)
{
    if (!emoji_font_available || cp_count <= 0)
        return false;

    uint32_t cp0 = cps[0];

    bool has_vs15 = false;
    bool has_vs16 = false;
    for (int i = 1; i < cp_count; i++) {
        if (cps[i] == UNICODE_VARIATION_SELECTOR_15)
            has_vs15 = true;
        else if (cps[i] == UNICODE_VARIATION_SELECTOR_16)
            has_vs16 = true;
    }

    if (has_vs15 && has_vs16) {
        return emoji_has_glyph;
    }

    if (!has_vs15 && (has_vs16 || is_regional_indicator(cp0))) {
        return true;
    }

    if (is_emoji_presentation(cp0)) {
        return emoji_has_glyph;
    }

    return false;
}

// =============================================================================
// NERD FONTS V2 -> V3 CODEPOINT TRANSLATION HACK
// =============================================================================
//
// Nerd Fonts v3.0 removed all Material Design Icons from the U+F900-U+FAFF
// range because it conflicts with Unicode's CJK Compatibility Ideographs.
// Terminal emulators hard-code this range as double-width characters.
//
// Many applications (e.g., goread, starship, various TUI tools) still use the
// old v2 codepoints. This translation layer maps them to their v3 equivalents
// in the Supplementary Private Use Area (U+F0000+).
//
// Generated from nerd-fonts v2.3.3 -> v3.4.0 mapping.
// See: https://github.com/ryanoasis/nerd-fonts/issues/1190
//      https://github.com/loichyan/nerdfix
// =============================================================================

static const struct
{
    uint32_t old_cp;
    uint32_t new_cp;
} nf_v2_to_v3_map[] = {
    { 0xF900, 0xF0401 }, // mdi-pig -> md-pig
    { 0xF901, 0xF0402 }, // mdi-pill -> md-pill
    { 0xF902, 0xF0403 }, // mdi-pin -> md-pin
    { 0xF903, 0xF0404 }, // mdi-pin_off -> md-pin_off
    { 0xF904, 0xF0405 }, // mdi-pine_tree -> md-pine_tree
    { 0xF905, 0xF0406 }, // mdi-pine_tree_box -> md-pine_tree_box
    { 0xF906, 0xF0407 }, // mdi-pinterest -> md-pinterest
    { 0xF908, 0xF0409 }, // mdi-pizza -> md-pizza
    { 0xF909, 0xF040A }, // mdi-play -> md-play
    { 0xF90A, 0xF040B }, // mdi-play_box_outline -> md-play_box_outline
    { 0xF90B, 0xF040C }, // mdi-play_circle -> md-play_circle
    { 0xF90C, 0xF040D }, // mdi-play_circle_outline -> md-play_circle_outline
    { 0xF90D, 0xF040E }, // mdi-play_pause -> md-play_pause
    { 0xF90E, 0xF040F }, // mdi-play_protected_content -> md-play_protected_content
    { 0xF90F, 0xF0410 }, // mdi-playlist_minus -> md-playlist_minus
    { 0xF910, 0xF0411 }, // mdi-playlist_play -> md-playlist_play
    { 0xF911, 0xF0412 }, // mdi-playlist_plus -> md-playlist_plus
    { 0xF912, 0xF0413 }, // mdi-playlist_remove -> md-playlist_remove
    { 0xF914, 0xF0415 }, // mdi-plus -> md-plus
    { 0xF915, 0xF0416 }, // mdi-plus_box -> md-plus_box
    { 0xF916, 0xF0417 }, // mdi-plus_circle -> md-plus_circle
    { 0xF917, 0xF0418 }, // mdi-plus_circle_multiple_outline
    { 0xF918, 0xF0419 }, // mdi-plus_circle_outline -> md-plus_circle_outline
    { 0xF919, 0xF041A }, // mdi-plus_network -> md-plus_network
    { 0xF91C, 0xF041D }, // mdi-pokeball -> md-pokeball
    { 0xF91D, 0xF041E }, // mdi-polaroid -> md-polaroid
    { 0xF91E, 0xF041F }, // mdi-poll -> md-poll
    { 0xF920, 0xF0421 }, // mdi-polymer -> md-polymer
    { 0xF921, 0xF0422 }, // mdi-popcorn -> md-popcorn
    { 0xF922, 0xF0423 }, // mdi-pound -> md-pound
    { 0xF923, 0xF0424 }, // mdi-pound_box -> md-pound_box
    { 0xF924, 0xF0425 }, // mdi-power -> md-power
    { 0xF925, 0xF0426 }, // mdi-power_settings -> md-power_settings
    { 0xF926, 0xF0427 }, // mdi-power_socket -> md-power_socket
    { 0xF927, 0xF0428 }, // mdi-presentation -> md-presentation
    { 0xF928, 0xF0429 }, // mdi-presentation_play -> md-presentation_play
    { 0xF929, 0xF042A }, // mdi-printer -> md-printer
    { 0xF92A, 0xF042B }, // mdi-printer_3d -> md-printer_3d
    { 0xF92B, 0xF042C }, // mdi-printer_alert -> md-printer_alert
    { 0xF92C, 0xF042D }, // mdi-professional_hexagon -> md-professional_hexagon
    { 0xF92D, 0xF042E }, // mdi-projector -> md-projector
    { 0xF92E, 0xF042F }, // mdi-projector_screen -> md-projector_screen
    { 0xF92F, 0xF0430 }, // mdi-pulse -> md-pulse
    { 0xF930, 0xF0431 }, // mdi-puzzle -> md-puzzle
    { 0xF931, 0xF0432 }, // mdi-qrcode -> md-qrcode
    { 0xF932, 0xF0433 }, // mdi-qrcode_scan -> md-qrcode_scan
    { 0xF933, 0xF0434 }, // mdi-quadcopter -> md-quadcopter
    { 0xF934, 0xF0435 }, // mdi-quality_high -> md-quality_high
    { 0xF936, 0xF0437 }, // mdi-radar -> md-radar
    { 0xF937, 0xF0438 }, // mdi-radiator -> md-radiator
    { 0xF938, 0xF0439 }, // mdi-radio -> md-radio
    { 0xF939, 0xF043A }, // mdi-radio_handheld -> md-radio_handheld
    { 0xF93A, 0xF043B }, // mdi-radio_tower -> md-radio_tower
    { 0xF93B, 0xF043C }, // mdi-radioactive -> md-radioactive
    { 0xF93D, 0xF043E }, // mdi-radiobox_marked -> md-radiobox_marked
    { 0xF93F, 0xF0440 }, // mdi-ray_end -> md-ray_end
    { 0xF940, 0xF0441 }, // mdi-ray_end_arrow -> md-ray_end_arrow
    { 0xF941, 0xF0442 }, // mdi-ray_start -> md-ray_start
    { 0xF942, 0xF0443 }, // mdi-ray_start_arrow -> md-ray_start_arrow
    { 0xF943, 0xF0444 }, // mdi-ray_start_end -> md-ray_start_end
    { 0xF944, 0xF0445 }, // mdi-ray_vertex -> md-ray_vertex
    { 0xF945, 0xF0446 }, // mdi-lastpass -> md-lastpass
    { 0xF946, 0xF0447 }, // mdi-read -> md-read
    { 0xF947, 0xF0448 }, // mdi-youtube_tv -> md-youtube_tv
    { 0xF948, 0xF0449 }, // mdi-receipt -> md-receipt
    { 0xF949, 0xF044A }, // mdi-record -> md-record
    { 0xF94A, 0xF044B }, // mdi-record_rec -> md-record_rec
    { 0xF94B, 0xF044C }, // mdi-recycle -> md-recycle
    { 0xF94C, 0xF044D }, // mdi-reddit -> md-reddit
    { 0xF94D, 0xF044E }, // mdi-redo -> md-redo
    { 0xF94E, 0xF044F }, // mdi-redo_variant -> md-redo_variant
    { 0xF94F, 0xF0450 }, // mdi-refresh -> md-refresh
    { 0xF950, 0xF0451 }, // mdi-regex -> md-regex
    { 0xF951, 0xF0452 }, // mdi-relative_scale -> md-relative_scale
    { 0xF952, 0xF0453 }, // mdi-reload -> md-reload
    { 0xF953, 0xF0454 }, // mdi-remote -> md-remote
    { 0xF954, 0xF0455 }, // mdi-rename_box -> md-rename_box
    { 0xF955, 0xF0456 }, // mdi-repeat -> md-repeat
    { 0xF956, 0xF0457 }, // mdi-repeat_off -> md-repeat_off
    { 0xF957, 0xF0458 }, // mdi-repeat_once -> md-repeat_once
    { 0xF958, 0xF0459 }, // mdi-replay -> md-replay
    { 0xF959, 0xF045A }, // mdi-reply -> md-reply
    { 0xF95A, 0xF045B }, // mdi-reply_all -> md-reply_all
    { 0xF95B, 0xF045C }, // mdi-reproduction -> md-reproduction
    { 0xF95C, 0xF045D }, // mdi-resize_bottom_right -> md-resize_bottom_right
    { 0xF95D, 0xF045E }, // mdi-responsive -> md-responsive
    { 0xF95E, 0xF045F }, // mdi-rewind -> md-rewind
    { 0xF95F, 0xF0460 }, // mdi-ribbon -> md-ribbon
    { 0xF960, 0xF0461 }, // mdi-road -> md-road
    { 0xF961, 0xF0462 }, // mdi-road_variant -> md-road_variant
    { 0xF962, 0xF0463 }, // mdi-rocket -> md-rocket
    { 0xF963, 0xF0EC7 }, // mdi-rotate_3d -> md-rotate_3d
    { 0xF964, 0xF0465 }, // mdi-rotate_left -> md-rotate_left
    { 0xF965, 0xF0466 }, // mdi-rotate_left_variant -> md-rotate_left_variant
    { 0xF966, 0xF0467 }, // mdi-rotate_right -> md-rotate_right
    { 0xF967, 0xF0468 }, // mdi-rotate_right_variant -> md-rotate_right_variant
    { 0xF968, 0xF0469 }, // mdi-router_wireless -> md-router_wireless
    { 0xF969, 0xF046A }, // mdi-routes -> md-routes
    { 0xF96A, 0xF046B }, // mdi-rss -> md-rss
    { 0xF96B, 0xF046C }, // mdi-rss_box -> md-rss_box
    { 0xF96C, 0xF046D }, // mdi-ruler -> md-ruler
    { 0xF96D, 0xF046E }, // mdi-run_fast -> md-run_fast
    { 0xF96E, 0xF046F }, // mdi-sale -> md-sale
    { 0xF96F, 0xF0470 }, // mdi-satellite -> md-satellite
    { 0xF970, 0xF0471 }, // mdi-satellite_variant -> md-satellite_variant
    { 0xF971, 0xF0472 }, // mdi-scale -> md-scale
    { 0xF972, 0xF0473 }, // mdi-scale_bathroom -> md-scale_bathroom
    { 0xF973, 0xF0474 }, // mdi-school -> md-school
    { 0xF974, 0xF0475 }, // mdi-screen_rotation -> md-screen_rotation
    { 0xF975, 0xF0478 }, // mdi-screen_rotation_lock -> md-screen_rotation_lock
    { 0xF976, 0xF0476 }, // mdi-screwdriver -> md-screwdriver
    { 0xF977, 0xF0BC1 }, // mdi-script -> md-script
    { 0xF978, 0xF0479 }, // mdi-sd -> md-sd
    { 0xF979, 0xF047A }, // mdi-seal -> md-seal
    { 0xF97A, 0xF047B }, // mdi-seat_flat -> md-seat_flat
    { 0xF97B, 0xF047C }, // mdi-seat_flat_angled -> md-seat_flat_angled
    { 0xF97C, 0xF047D }, // mdi-seat_individual_suite -> md-seat_individual_suite
    { 0xF97D, 0xF047E }, // mdi-seat_legroom_extra -> md-seat_legroom_extra
    { 0xF97E, 0xF047F }, // mdi-seat_legroom_normal -> md-seat_legroom_normal
    { 0xF97F, 0xF0480 }, // mdi-seat_legroom_reduced -> md-seat_legroom_reduced
    { 0xF980, 0xF0481 }, // mdi-seat_recline_extra -> md-seat_recline_extra
    { 0xF981, 0xF0482 }, // mdi-seat_recline_normal -> md-seat_recline_normal
    { 0xF982, 0xF0483 }, // mdi-security -> md-security
    { 0xF983, 0xF0484 }, // mdi-security_network -> md-security_network
    { 0xF984, 0xF0485 }, // mdi-select -> md-select
    { 0xF985, 0xF0486 }, // mdi-select_all -> md-select_all
    { 0xF986, 0xF0487 }, // mdi-select_inverse -> md-select_inverse
    { 0xF987, 0xF0488 }, // mdi-select_off -> md-select_off
    { 0xF988, 0xF0489 }, // mdi-selection -> md-selection
    { 0xF989, 0xF048A }, // mdi-send -> md-send
    { 0xF98A, 0xF048B }, // mdi-server -> md-server
    { 0xF98B, 0xF048C }, // mdi-server_minus -> md-server_minus
    { 0xF98C, 0xF048D }, // mdi-server_network -> md-server_network
    { 0xF98D, 0xF048E }, // mdi-server_network_off -> md-server_network_off
    { 0xF98E, 0xF048F }, // mdi-server_off -> md-server_off
    { 0xF98F, 0xF0490 }, // mdi-server_plus -> md-server_plus
    { 0xF990, 0xF0491 }, // mdi-server_remove -> md-server_remove
    { 0xF991, 0xF0492 }, // mdi-server_security -> md-server_security
    { 0xF994, 0xF0495 }, // mdi-shape_plus -> md-shape_plus
    { 0xF995, 0xF0496 }, // mdi-share -> md-share
    { 0xF996, 0xF0497 }, // mdi-share_variant -> md-share_variant
    { 0xF997, 0xF0498 }, // mdi-shield -> md-shield
    { 0xF998, 0xF0499 }, // mdi-shield_outline -> md-shield_outline
    { 0xF999, 0xF049A }, // mdi-shopping -> md-shopping
    { 0xF99A, 0xF049B }, // mdi-shopping_music -> md-shopping_music
    { 0xF99B, 0xF049C }, // mdi-shredder -> md-shredder
    { 0xF99C, 0xF049D }, // mdi-shuffle -> md-shuffle
    { 0xF99D, 0xF049E }, // mdi-shuffle_disabled -> md-shuffle_disabled
    { 0xF99E, 0xF049F }, // mdi-shuffle_variant -> md-shuffle_variant
    { 0xF99F, 0xF04A0 }, // mdi-sigma -> md-sigma
    { 0xF9A0, 0xF04A1 }, // mdi-sign_caution -> md-sign_caution
    { 0xF9A1, 0xF04A2 }, // mdi-signal -> md-signal
    { 0xF9A2, 0xF04A3 }, // mdi-silverware -> md-silverware
    { 0xF9A3, 0xF04A4 }, // mdi-silverware_fork -> md-silverware_fork
    { 0xF9A4, 0xF04A5 }, // mdi-silverware_spoon -> md-silverware_spoon
    { 0xF9A5, 0xF04A6 }, // mdi-silverware_variant -> md-silverware_variant
    { 0xF9A6, 0xF04A7 }, // mdi-sim -> md-sim
    { 0xF9A7, 0xF04A8 }, // mdi-sim_alert -> md-sim_alert
    { 0xF9A8, 0xF04A9 }, // mdi-sim_off -> md-sim_off
    { 0xF9A9, 0xF04AA }, // mdi-sitemap -> md-sitemap
    { 0xF9AA, 0xF04AB }, // mdi-skip_backward -> md-skip_backward
    { 0xF9AB, 0xF04AC }, // mdi-skip_forward -> md-skip_forward
    { 0xF9AC, 0xF04AD }, // mdi-skip_next -> md-skip_next
    { 0xF9AD, 0xF04AE }, // mdi-skip_previous -> md-skip_previous
    { 0xF9AE, 0xF04AF }, // mdi-skype -> md-skype
    { 0xF9AF, 0xF04B0 }, // mdi-skype_business -> md-skype_business
    { 0xF9B0, 0xF04B1 }, // mdi-slack -> md-slack
    { 0xF9B1, 0xF04B2 }, // mdi-sleep -> md-sleep
    { 0xF9B2, 0xF04B3 }, // mdi-sleep_off -> md-sleep_off
    { 0xF9B3, 0xF04B4 }, // mdi-smoking -> md-smoking
    { 0xF9B4, 0xF04B5 }, // mdi-smoking_off -> md-smoking_off
    { 0xF9B5, 0xF04B6 }, // mdi-snapchat -> md-snapchat
    { 0xF9B6, 0xF04B7 }, // mdi-snowman -> md-snowman
    { 0xF9B7, 0xF04B8 }, // mdi-soccer -> md-soccer
    { 0xF9B8, 0xF04B9 }, // mdi-sofa -> md-sofa
    { 0xF9B9, 0xF04BA }, // mdi-sort -> md-sort
    { 0xF9BB, 0xF04BC }, // mdi-sort_ascending -> md-sort_ascending
    { 0xF9BC, 0xF04BD }, // mdi-sort_descending -> md-sort_descending
    { 0xF9BE, 0xF04BF }, // mdi-sort_variant -> md-sort_variant
    { 0xF9BF, 0xF04C0 }, // mdi-soundcloud -> md-soundcloud
    { 0xF9C0, 0xF04C1 }, // mdi-source_fork -> md-source_fork
    { 0xF9C1, 0xF04C2 }, // mdi-source_pull -> md-source_pull
    { 0xF9C2, 0xF04C3 }, // mdi-speaker -> md-speaker
    { 0xF9C3, 0xF04C4 }, // mdi-speaker_off -> md-speaker_off
    { 0xF9C4, 0xF04C5 }, // mdi-speedometer -> md-speedometer
    { 0xF9C5, 0xF04C6 }, // mdi-spellcheck -> md-spellcheck
    { 0xF9C6, 0xF04C7 }, // mdi-spotify -> md-spotify
    { 0xF9C7, 0xF04C8 }, // mdi-spotlight -> md-spotlight
    { 0xF9C8, 0xF04C9 }, // mdi-spotlight_beam -> md-spotlight_beam
    { 0xF9CB, 0xF04CC }, // mdi-stack_overflow -> md-stack_overflow
    { 0xF9CC, 0xF04CD }, // mdi-stairs -> md-stairs
    { 0xF9CD, 0xF04CE }, // mdi-star -> md-star
    { 0xF9CE, 0xF04CF }, // mdi-star_circle -> md-star_circle
    { 0xF9CF, 0xF0246 }, // mdi-star_half -> md-star_half
    { 0xF9D0, 0xF04D1 }, // mdi-star_off -> md-star_off
    { 0xF9D1, 0xF04D2 }, // mdi-star_outline -> md-star_outline
    { 0xF9D2, 0xF04D3 }, // mdi-steam -> md-steam
    { 0xF9D3, 0xF04D4 }, // mdi-steering -> md-steering
    { 0xF9D4, 0xF04D5 }, // mdi-step_backward -> md-step_backward
    { 0xF9D5, 0xF04D6 }, // mdi-step_backward_2 -> md-step_backward_2
    { 0xF9D6, 0xF04D7 }, // mdi-step_forward -> md-step_forward
    { 0xF9D7, 0xF04D8 }, // mdi-step_forward_2 -> md-step_forward_2
    { 0xF9D8, 0xF04D9 }, // mdi-stethoscope -> md-stethoscope
    { 0xF9D9, 0xF04DA }, // mdi-stocking -> md-stocking
    { 0xF9DA, 0xF04DB }, // mdi-stop -> md-stop
    { 0xF9DB, 0xF04DC }, // mdi-store -> md-store
    { 0xF9DC, 0xF04DD }, // mdi-store_24_hour -> md-store_24_hour
    { 0xF9DD, 0xF04DE }, // mdi-stove -> md-stove
    { 0xF9DE, 0xF04DF }, // mdi-subway_variant -> md-subway_variant
    { 0xF9DF, 0xF04E0 }, // mdi-sunglasses -> md-sunglasses
    { 0xF9E0, 0xF04E1 }, // mdi-swap_horizontal -> md-swap_horizontal
    { 0xF9E1, 0xF04E2 }, // mdi-swap_vertical -> md-swap_vertical
    { 0xF9E2, 0xF04E3 }, // mdi-swim -> md-swim
    { 0xF9E3, 0xF04E4 }, // mdi-switch -> md-switch
    { 0xF9E4, 0xF04E5 }, // mdi-sword -> md-sword
    { 0xF9E5, 0xF04E6 }, // mdi-sync -> md-sync
    { 0xF9E6, 0xF04E7 }, // mdi-sync_alert -> md-sync_alert
    { 0xF9E7, 0xF04E8 }, // mdi-sync_off -> md-sync_off
    { 0xF9E8, 0xF04E9 }, // mdi-tab -> md-tab
    { 0xF9E9, 0xF04EA }, // mdi-tab_unselected -> md-tab_unselected
    { 0xF9EA, 0xF04EB }, // mdi-table -> md-table
    { 0xF9EB, 0xF04EC }, // mdi-table_column_plus_after
    { 0xF9EC, 0xF04ED }, // mdi-table_column_plus_before
    { 0xF9ED, 0xF04EE }, // mdi-table_column_remove -> md-table_column_remove
    { 0xF9EE, 0xF04EF }, // mdi-table_column_width -> md-table_column_width
    { 0xF9EF, 0xF04F0 }, // mdi-table_edit -> md-table_edit
    { 0xF9F0, 0xF04F1 }, // mdi-table_large -> md-table_large
    { 0xF9F1, 0xF04F2 }, // mdi-table_row_height -> md-table_row_height
    { 0xF9F2, 0xF04F3 }, // mdi-table_row_plus_after -> md-table_row_plus_after
    { 0xF9F3, 0xF04F4 }, // mdi-table_row_plus_before
    { 0xF9F4, 0xF04F5 }, // mdi-table_row_remove -> md-table_row_remove
    { 0xF9F5, 0xF04F6 }, // mdi-tablet -> md-tablet
    { 0xF9F6, 0xF04F7 }, // mdi-tablet_android -> md-tablet_android
    { 0xF9F8, 0xF04F9 }, // mdi-tag -> md-tag
    { 0xF9F9, 0xF04FA }, // mdi-tag_faces -> md-tag_faces
    { 0xF9FA, 0xF04FB }, // mdi-tag_multiple -> md-tag_multiple
    { 0xF9FB, 0xF04FC }, // mdi-tag_outline -> md-tag_outline
    { 0xF9FC, 0xF04FD }, // mdi-tag_text_outline -> md-tag_text_outline
    { 0xF9FD, 0xF04FE }, // mdi-target -> md-target
    { 0xF9FE, 0xF04FF }, // mdi-taxi -> md-taxi
    { 0xF9FF, 0xF0500 }, // mdi-teamviewer -> md-teamviewer
    { 0xFA01, 0xF0502 }, // mdi-television -> md-television
    { 0xFA02, 0xF0503 }, // mdi-television_guide -> md-television_guide
    { 0xFA03, 0xF0504 }, // mdi-temperature_celsius -> md-temperature_celsius
    { 0xFA04, 0xF0505 }, // mdi-temperature_fahrenheit -> md-temperature_fahrenheit
    { 0xFA05, 0xF0506 }, // mdi-temperature_kelvin -> md-temperature_kelvin
    { 0xFA06, 0xF0DA0 }, // mdi-tennis -> md-tennis
    { 0xFA07, 0xF0508 }, // mdi-tent -> md-tent
    { 0xFA09, 0xF050A }, // mdi-text_to_speech -> md-text_to_speech
    { 0xFA0A, 0xF050B }, // mdi-text_to_speech_off -> md-text_to_speech_off
    { 0xFA0B, 0xF050C }, // mdi-texture -> md-texture
    { 0xFA0C, 0xF050D }, // mdi-theater -> md-theater
    { 0xFA0D, 0xF050E }, // mdi-theme_light_dark -> md-theme_light_dark
    { 0xFA0E, 0xF050F }, // mdi-thermometer -> md-thermometer
    { 0xFA0F, 0xF0510 }, // mdi-thermometer_lines -> md-thermometer_lines
    { 0xFA10, 0xF0511 }, // mdi-thumb_down -> md-thumb_down
    { 0xFA11, 0xF0512 }, // mdi-thumb_down_outline -> md-thumb_down_outline
    { 0xFA12, 0xF0513 }, // mdi-thumb_up -> md-thumb_up
    { 0xFA13, 0xF0514 }, // mdi-thumb_up_outline -> md-thumb_up_outline
    { 0xFA14, 0xF0515 }, // mdi-thumbs_up_down -> md-thumbs_up_down
    { 0xFA15, 0xF0516 }, // mdi-ticket -> md-ticket
    { 0xFA16, 0xF0517 }, // mdi-ticket_account -> md-ticket_account
    { 0xFA17, 0xF0518 }, // mdi-ticket_confirmation -> md-ticket_confirmation
    { 0xFA18, 0xF0519 }, // mdi-tie -> md-tie
    { 0xFA19, 0xF051A }, // mdi-timelapse -> md-timelapse
    { 0xFA1A, 0xF13AB }, // mdi-timer -> md-timer
    { 0xFA1B, 0xF051C }, // mdi-timer_10 -> md-timer_10
    { 0xFA1C, 0xF051D }, // mdi-timer_3 -> md-timer_3
    { 0xFA1D, 0xF13AC }, // mdi-timer_off -> md-timer_off
    { 0xFA1E, 0xF051F }, // mdi-timer_sand -> md-timer_sand
    { 0xFA1F, 0xF0520 }, // mdi-timetable -> md-timetable
    { 0xFA20, 0xF0521 }, // mdi-toggle_switch -> md-toggle_switch
    { 0xFA21, 0xF0522 }, // mdi-toggle_switch_off -> md-toggle_switch_off
    { 0xFA22, 0xF0523 }, // mdi-tooltip -> md-tooltip
    { 0xFA23, 0xF0524 }, // mdi-tooltip_edit -> md-tooltip_edit
    { 0xFA24, 0xF0525 }, // mdi-tooltip_image -> md-tooltip_image
    { 0xFA25, 0xF0526 }, // mdi-tooltip_outline -> md-tooltip_outline
    { 0xFA27, 0xF0528 }, // mdi-tooltip_text -> md-tooltip_text
    { 0xFA28, 0xF08C3 }, // mdi-tooth -> md-tooth
    { 0xFA2A, 0xF052B }, // mdi-traffic_light -> md-traffic_light
    { 0xFA2B, 0xF052C }, // mdi-train -> md-train
    { 0xFA2C, 0xF052D }, // mdi-tram -> md-tram
    { 0xFA2D, 0xF052E }, // mdi-transcribe -> md-transcribe
    { 0xFA2E, 0xF052F }, // mdi-transcribe_close -> md-transcribe_close
    { 0xFA2F, 0xF1065 }, // mdi-transfer -> md-transfer
    { 0xFA30, 0xF0531 }, // mdi-tree -> md-tree
    { 0xFA31, 0xF0532 }, // mdi-trello -> md-trello
    { 0xFA32, 0xF0533 }, // mdi-trending_down -> md-trending_down
    { 0xFA33, 0xF0534 }, // mdi-trending_neutral -> md-trending_neutral
    { 0xFA34, 0xF0535 }, // mdi-trending_up -> md-trending_up
    { 0xFA35, 0xF0536 }, // mdi-triangle -> md-triangle
    { 0xFA36, 0xF0537 }, // mdi-triangle_outline -> md-triangle_outline
    { 0xFA37, 0xF0538 }, // mdi-trophy -> md-trophy
    { 0xFA38, 0xF0539 }, // mdi-trophy_award -> md-trophy_award
    { 0xFA39, 0xF053A }, // mdi-trophy_outline -> md-trophy_outline
    { 0xFA3A, 0xF053B }, // mdi-trophy_variant -> md-trophy_variant
    { 0xFA3B, 0xF053C }, // mdi-trophy_variant_outline -> md-trophy_variant_outline
    { 0xFA3C, 0xF053D }, // mdi-truck -> md-truck
    { 0xFA3D, 0xF053E }, // mdi-truck_delivery -> md-truck_delivery
    { 0xFA3E, 0xF0A7B }, // mdi-tshirt_crew -> md-tshirt_crew
    { 0xFA3F, 0xF0A7C }, // mdi-tshirt_v -> md-tshirt_v
    { 0xFA42, 0xF0543 }, // mdi-twitch -> md-twitch
    { 0xFA43, 0xF0544 }, // mdi-twitter -> md-twitter
    { 0xFA47, 0xF0548 }, // mdi-ubuntu -> md-ubuntu
    { 0xFA48, 0xF0549 }, // mdi-umbraco -> md-umbraco
    { 0xFA49, 0xF054A }, // mdi-umbrella -> md-umbrella
    { 0xFA4A, 0xF054B }, // mdi-umbrella_outline -> md-umbrella_outline
    { 0xFA4B, 0xF054C }, // mdi-undo -> md-undo
    { 0xFA4C, 0xF054D }, // mdi-undo_variant -> md-undo_variant
    { 0xFA4D, 0xF054E }, // mdi-unfold_less_horizontal
    { 0xFA4E, 0xF054F }, // mdi-unfold_more_horizontal
    { 0xFA4F, 0xF0550 }, // mdi-ungroup -> md-ungroup
    { 0xFA51, 0xF0552 }, // mdi-upload -> md-upload
    { 0xFA52, 0xF0553 }, // mdi-usb -> md-usb
    { 0xFA53, 0xF0554 }, // mdi-vector_arrange_above -> md-vector_arrange_above
    { 0xFA54, 0xF0555 }, // mdi-vector_arrange_below -> md-vector_arrange_below
    { 0xFA55, 0xF0556 }, // mdi-vector_circle -> md-vector_circle
    { 0xFA56, 0xF0557 }, // mdi-vector_circle_variant -> md-vector_circle_variant
    { 0xFA57, 0xF0558 }, // mdi-vector_combine -> md-vector_combine
    { 0xFA58, 0xF0559 }, // mdi-vector_curve -> md-vector_curve
    { 0xFA59, 0xF055A }, // mdi-vector_difference -> md-vector_difference
    { 0xFA5A, 0xF055B }, // mdi-vector_difference_ab -> md-vector_difference_ab
    { 0xFA5B, 0xF055C }, // mdi-vector_difference_ba -> md-vector_difference_ba
    { 0xFA5C, 0xF055D }, // mdi-vector_intersection -> md-vector_intersection
    { 0xFA5D, 0xF055E }, // mdi-vector_line -> md-vector_line
    { 0xFA5E, 0xF055F }, // mdi-vector_point -> md-vector_point
    { 0xFA5F, 0xF0560 }, // mdi-vector_polygon -> md-vector_polygon
    { 0xFA60, 0xF0561 }, // mdi-vector_polyline -> md-vector_polyline
    { 0xFA61, 0xF0562 }, // mdi-vector_selection -> md-vector_selection
    { 0xFA62, 0xF0563 }, // mdi-vector_triangle -> md-vector_triangle
    { 0xFA63, 0xF0564 }, // mdi-vector_union -> md-vector_union
    { 0xFA65, 0xF0566 }, // mdi-vibrate -> md-vibrate
    { 0xFA66, 0xF0567 }, // mdi-video -> md-video
    { 0xFA67, 0xF0568 }, // mdi-video_off -> md-video_off
    { 0xFA68, 0xF0569 }, // mdi-video_switch -> md-video_switch
    { 0xFA69, 0xF056A }, // mdi-view_agenda -> md-view_agenda
    { 0xFA6A, 0xF056B }, // mdi-view_array -> md-view_array
    { 0xFA6B, 0xF056C }, // mdi-view_carousel -> md-view_carousel
    { 0xFA6C, 0xF056D }, // mdi-view_column -> md-view_column
    { 0xFA6D, 0xF056E }, // mdi-view_dashboard -> md-view_dashboard
    { 0xFA6E, 0xF056F }, // mdi-view_day -> md-view_day
    { 0xFA6F, 0xF0570 }, // mdi-view_grid -> md-view_grid
    { 0xFA70, 0xF0571 }, // mdi-view_headline -> md-view_headline
    { 0xFA71, 0xF0572 }, // mdi-view_list -> md-view_list
    { 0xFA72, 0xF0573 }, // mdi-view_module -> md-view_module
    { 0xFA73, 0xF0574 }, // mdi-view_quilt -> md-view_quilt
    { 0xFA74, 0xF0575 }, // mdi-view_stream -> md-view_stream
    { 0xFA75, 0xF0576 }, // mdi-view_week -> md-view_week
    { 0xFA76, 0xF0577 }, // mdi-vimeo -> md-vimeo
    { 0xFA7B, 0xF057C }, // mdi-vlc -> md-vlc
    { 0xFA7C, 0xF057D }, // mdi-voicemail -> md-voicemail
    { 0xFA7D, 0xF057E }, // mdi-volume_high -> md-volume_high
    { 0xFA7E, 0xF057F }, // mdi-volume_low -> md-volume_low
    { 0xFA7F, 0xF0580 }, // mdi-volume_medium -> md-volume_medium
    { 0xFA80, 0xF0581 }, // mdi-volume_off -> md-volume_off
    { 0xFA81, 0xF0582 }, // mdi-vpn -> md-vpn
    { 0xFA82, 0xF0583 }, // mdi-walk -> md-walk
    { 0xFA83, 0xF0584 }, // mdi-wallet -> md-wallet
    { 0xFA84, 0xF0585 }, // mdi-wallet_giftcard -> md-wallet_giftcard
    { 0xFA85, 0xF0586 }, // mdi-wallet_membership -> md-wallet_membership
    { 0xFA86, 0xF0587 }, // mdi-wallet_travel -> md-wallet_travel
    { 0xFA87, 0xF0588 }, // mdi-wan -> md-wan
    { 0xFA88, 0xF0589 }, // mdi-watch -> md-watch
    { 0xFA89, 0xF058A }, // mdi-watch_export -> md-watch_export
    { 0xFA8A, 0xF058B }, // mdi-watch_import -> md-watch_import
    { 0xFA8B, 0xF058C }, // mdi-water -> md-water
    { 0xFA8C, 0xF058D }, // mdi-water_off -> md-water_off
    { 0xFA8D, 0xF058E }, // mdi-water_percent -> md-water_percent
    { 0xFA8E, 0xF058F }, // mdi-water_pump -> md-water_pump
    { 0xFA8F, 0xF0590 }, // mdi-weather_cloudy -> md-weather_cloudy
    { 0xFA90, 0xF0591 }, // mdi-weather_fog -> md-weather_fog
    { 0xFA91, 0xF0592 }, // mdi-weather_hail -> md-weather_hail
    { 0xFA92, 0xF0593 }, // mdi-weather_lightning -> md-weather_lightning
    { 0xFA93, 0xF0594 }, // mdi-weather_night -> md-weather_night
    { 0xFA95, 0xF0596 }, // mdi-weather_pouring -> md-weather_pouring
    { 0xFA96, 0xF0597 }, // mdi-weather_rainy -> md-weather_rainy
    { 0xFA97, 0xF0598 }, // mdi-weather_snowy -> md-weather_snowy
    { 0xFA98, 0xF0599 }, // mdi-weather_sunny -> md-weather_sunny
    { 0xFA99, 0xF059A }, // mdi-weather_sunset -> md-weather_sunset
    { 0xFA9A, 0xF059B }, // mdi-weather_sunset_down -> md-weather_sunset_down
    { 0xFA9B, 0xF059C }, // mdi-weather_sunset_up -> md-weather_sunset_up
    { 0xFA9C, 0xF059D }, // mdi-weather_windy -> md-weather_windy
    { 0xFA9D, 0xF059E }, // mdi-weather_windy_variant -> md-weather_windy_variant
    { 0xFA9E, 0xF059F }, // mdi-web -> md-web
    { 0xFA9F, 0xF05A0 }, // mdi-webcam -> md-webcam
    { 0xFAA0, 0xF05A1 }, // mdi-weight -> md-weight
    { 0xFAA1, 0xF05A2 }, // mdi-weight_kilogram -> md-weight_kilogram
    { 0xFAA2, 0xF05A3 }, // mdi-whatsapp -> md-whatsapp
    { 0xFAA3, 0xF05A4 }, // mdi-wheelchair_accessibility
    { 0xFAA4, 0xF05A5 }, // mdi-white_balance_auto -> md-white_balance_auto
    { 0xFAA5, 0xF05A6 }, // mdi-white_balance_incandescent
    { 0xFAA6, 0xF05A7 }, // mdi-white_balance_iridescent
    { 0xFAA7, 0xF05A8 }, // mdi-white_balance_sunny -> md-white_balance_sunny
    { 0xFAA8, 0xF05A9 }, // mdi-wifi -> md-wifi
    { 0xFAA9, 0xF05AA }, // mdi-wifi_off -> md-wifi_off
    { 0xFAAB, 0xF05AC }, // mdi-wikipedia -> md-wikipedia
    { 0xFAAC, 0xF05AD }, // mdi-window_close -> md-window_close
    { 0xFAAD, 0xF05AE }, // mdi-window_closed -> md-window_closed
    { 0xFAAE, 0xF05AF }, // mdi-window_maximize -> md-window_maximize
    { 0xFAAF, 0xF05B0 }, // mdi-window_minimize -> md-window_minimize
    { 0xFAB0, 0xF05B1 }, // mdi-window_open -> md-window_open
    { 0xFAB1, 0xF05B2 }, // mdi-window_restore -> md-window_restore
    { 0xFAB3, 0xF05B4 }, // mdi-wordpress -> md-wordpress
    { 0xFAB5, 0xF05B6 }, // mdi-wrap -> md-wrap
    { 0xFAB6, 0xF05B7 }, // mdi-wrench -> md-wrench
    { 0xFABF, 0xF05C0 }, // mdi-xml -> md-xml
    { 0xFAC0, 0xF05C1 }, // mdi-yeast -> md-yeast
    { 0xFAC3, 0xF05C4 }, // mdi-zip_box -> md-zip_box
    { 0xFAC4, 0xF05C5 }, // mdi-surround_sound -> md-surround_sound
    { 0xFAC5, 0xF05C6 }, // mdi-vector_rectangle -> md-vector_rectangle
    { 0xFAC6, 0xF05C7 }, // mdi-playlist_check -> md-playlist_check
    { 0xFAC7, 0xF05C8 }, // mdi-format_line_style -> md-format_line_style
    { 0xFAC8, 0xF05C9 }, // mdi-format_line_weight -> md-format_line_weight
    { 0xFAC9, 0xF05CA }, // mdi-translate -> md-translate
    { 0xFACB, 0xF05CC }, // mdi-opacity -> md-opacity
    { 0xFACC, 0xF05CD }, // mdi-near_me -> md-near_me
    { 0xFACD, 0xF0955 }, // mdi-clock_alert -> md-clock_alert
    { 0xFACE, 0xF05CF }, // mdi-human_pregnant -> md-human_pregnant
    { 0xFACF, 0xF1364 }, // mdi-sticker -> md-sticker
    { 0xFAD0, 0xF05D1 }, // mdi-scale_balance -> md-scale_balance
    { 0xFAD2, 0xF05D3 }, // mdi-account_multiple_minus -> md-account_multiple_minus
    { 0xFAD3, 0xF05D4 }, // mdi-airplane_landing -> md-airplane_landing
    { 0xFAD4, 0xF05D5 }, // mdi-airplane_takeoff -> md-airplane_takeoff
    { 0xFAD5, 0xF05D6 }, // mdi-alert_circle_outline -> md-alert_circle_outline
    { 0xFAD6, 0xF05D7 }, // mdi-altimeter -> md-altimeter
    { 0xFAD7, 0xF05D8 }, // mdi-animation -> md-animation
    { 0xFAD8, 0xF05D9 }, // mdi-book_minus -> md-book_minus
    { 0xFAD9, 0xF05DA }, // mdi-book_open_page_variant -> md-book_open_page_variant
    { 0xFADA, 0xF05DB }, // mdi-book_plus -> md-book_plus
    { 0xFADB, 0xF05DC }, // mdi-boombox -> md-boombox
    { 0xFADC, 0xF05DD }, // mdi-bullseye -> md-bullseye
    { 0xFADD, 0xF05DE }, // mdi-comment_remove -> md-comment_remove
    { 0xFADE, 0xF05DF }, // mdi-camera_off -> md-camera_off
    { 0xFADF, 0xF05E0 }, // mdi-check_circle -> md-check_circle
    { 0xFAE0, 0xF05E1 }, // mdi-check_circle_outline -> md-check_circle_outline
    { 0xFAE1, 0xF05E2 }, // mdi-candle -> md-candle
    { 0xFAE2, 0xF05E3 }, // mdi-chart_bubble -> md-chart_bubble
    { 0xFAE3, 0xF0FF1 }, // mdi-credit_card_off -> md-credit_card_off
    { 0xFAE4, 0xF05E5 }, // mdi-cup_off -> md-cup_off
    { 0xFAE5, 0xF05E6 }, // mdi-copyright -> md-copyright
    { 0xFAE6, 0xF05E7 }, // mdi-cursor_text -> md-cursor_text
    { 0xFAE7, 0xF05E8 }, // mdi-delete_forever -> md-delete_forever
    { 0xFAE8, 0xF05E9 }, // mdi-delete_sweep -> md-delete_sweep
    { 0xFAE9, 0xF1155 }, // mdi-dice_d20 -> md-dice_d20
    { 0xFAEA, 0xF1150 }, // mdi-dice_d4 -> md-dice_d4
    { 0xFAEB, 0xF1151 }, // mdi-dice_d6 -> md-dice_d6
    { 0xFAEC, 0xF1152 }, // mdi-dice_d8 -> md-dice_d8
    { 0xFAEE, 0xF05EF }, // mdi-email_open_outline -> md-email_open_outline
    { 0xFAEF, 0xF05F0 }, // mdi-email_variant -> md-email_variant
    { 0xFAF0, 0xF05F1 }, // mdi-ev_station -> md-ev_station
    { 0xFAF1, 0xF05F2 }, // mdi-food_fork_drink -> md-food_fork_drink
    { 0xFAF2, 0xF05F3 }, // mdi-food_off -> md-food_off
    { 0xFAF3, 0xF05F4 }, // mdi-format_title -> md-format_title
    { 0xFAF4, 0xF05F5 }, // mdi-google_maps -> md-google_maps
    { 0xFAF5, 0xF05F6 }, // mdi-heart_pulse -> md-heart_pulse
    { 0xFAF6, 0xF05F7 }, // mdi-highway -> md-highway
    { 0xFAF7, 0xF05F8 }, // mdi-home_map_marker -> md-home_map_marker
    { 0xFAF8, 0xF05F9 }, // mdi-incognito -> md-incognito
    { 0xFAF9, 0xF05FA }, // mdi-kettle -> md-kettle
    { 0xFAFA, 0xF05FB }, // mdi-lock_plus -> md-lock_plus
    { 0xFAFC, 0xF05FD }, // mdi-logout_variant -> md-logout_variant
    { 0xFAFD, 0xF05FE }, // mdi-music_note_bluetooth -> md-music_note_bluetooth
    { 0xFAFE, 0xF05FF }, // mdi-music_note_bluetooth_off
    { 0xFAFF, 0xF0600 }, // mdi-page_first -> md-page_first
};
#define NF_V2_TO_V3_MAP_SIZE (sizeof(nf_v2_to_v3_map) / sizeof(nf_v2_to_v3_map[0]))

// Translate obsolete Nerd Fonts v2 codepoint to v3 equivalent (binary search)
uint32_t rend_nf_translate_codepoint(uint32_t cp)
{
    // Quick range check: only translate U+F900-U+FAFF
    if (cp < 0xF900 || cp > 0xFAFF)
        return cp;

    // Binary search
    int lo = 0, hi = NF_V2_TO_V3_MAP_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (nf_v2_to_v3_map[mid].old_cp == cp)
            return nf_v2_to_v3_map[mid].new_cp;
        if (nf_v2_to_v3_map[mid].old_cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp; // Not found, return unchanged
}

int rend_display_row_to_unified(int scroll_offset, int display_row)
{
    int scrollback_row = scroll_offset - 1 - display_row;
    if (scrollback_row >= 0) {
        return -(scrollback_row + 1);
    } else {
        return display_row - scroll_offset;
    }
}

void rend_clamp_pixel_to_viewport(int *px, int *py, int viewport_w, int viewport_h)
{
    if (*px < 0)
        *px = 0;
    if (*px >= viewport_w)
        *px = viewport_w - 1;
    if (*py < 0)
        *py = 0;
    if (*py >= viewport_h)
        *py = viewport_h - 1;
}

// =============================================================================
// Glyph atlas packing — GPU-agnostic
// =============================================================================

static uint8_t s_atlas_srgb_to_linear_u8[256];
static bool s_atlas_srgb_lut_ready = false;

static void build_srgb_lut(void)
{
    for (int i = 0; i < 256; i++) {
        int v = (int)(rend_srgb_to_linear((uint8_t)i) * 255.0f + 0.5f);
        s_atlas_srgb_to_linear_u8[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    s_atlas_srgb_lut_ready = true;
}

void rend_linearize_rgba_in_place(uint8_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0)
        return;
    if (!s_atlas_srgb_lut_ready)
        build_srgb_lut();

    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = pixels + i * 4;
        p[0] = s_atlas_srgb_to_linear_u8[p[0]];
        p[1] = s_atlas_srgb_to_linear_u8[p[1]];
        p[2] = s_atlas_srgb_to_linear_u8[p[2]];
        // p[3] (alpha) unchanged
    }
}

static uint32_t atlas_hash(void *font_data, int glyph_id, uint32_t color)
{
    uint32_t hash = 2166136261u;
    uintptr_t ptr = (uintptr_t)font_data;
    for (int i = 0; i < (int)sizeof(ptr); i++) {
        hash ^= (uint8_t)(ptr >> (i * 8));
        hash *= 16777619u;
    }
    uint32_t gid = (uint32_t)glyph_id;
    for (int i = 0; i < 4; i++) {
        hash ^= (uint8_t)(gid >> (i * 8));
        hash *= 16777619u;
    }
    for (int i = 0; i < 4; i++) {
        hash ^= (uint8_t)(color >> (i * 8));
        hash *= 16777619u;
    }
    return hash;
}

static void atlas_clear(RendAtlas *atlas)
{
    memset(atlas->staging, 0,
           (size_t)REND_ATLAS_TEXTURE_SIZE * REND_ATLAS_TEXTURE_SIZE * 4);
    atlas->dirty = true;
    atlas->dirty_rect = (RendAtlasRegion){
        0, 0, REND_ATLAS_TEXTURE_SIZE, REND_ATLAS_TEXTURE_SIZE
    };
}

static void atlas_reset(RendAtlas *atlas)
{
    atlas_clear(atlas);
    atlas->num_shelves = 0;
    atlas->next_shelf_y = 0;
}

static bool atlas_alloc(RendAtlas *atlas, int w, int h,
                        RendAtlasRegion *out)
{
    int padded_w = w + 1;
    int padded_h = h + 1;

    for (int i = 0; i < atlas->num_shelves; i++) {
        RendAtlasShelf *shelf = &atlas->shelves[i];
        if (shelf->height >= h &&
            shelf->cursor_x + padded_w <= REND_ATLAS_TEXTURE_SIZE) {
            out->x = shelf->cursor_x;
            out->y = shelf->y;
            out->w = w;
            out->h = h;
            shelf->cursor_x += padded_w;
            return true;
        }
    }

    if (atlas->num_shelves >= REND_ATLAS_MAX_SHELVES)
        return false;
    if (atlas->next_shelf_y + padded_h > REND_ATLAS_TEXTURE_SIZE)
        return false;

    RendAtlasShelf *shelf = &atlas->shelves[atlas->num_shelves];
    shelf->y = atlas->next_shelf_y;
    shelf->height = h;
    shelf->cursor_x = padded_w;
    atlas->num_shelves++;
    atlas->next_shelf_y += padded_h;

    out->x = 0;
    out->y = shelf->y;
    out->w = w;
    out->h = h;
    return true;
}

static void atlas_evict(RendAtlas *atlas)
{
    vlog("Atlas: evicting all entries (%d entries removed)\n", atlas->entry_count);
    memset(atlas->entries, 0, sizeof(atlas->entries));
    atlas->entry_count = 0;
    atlas_reset(atlas);
    atlas->eviction_occurred = true;
}

void rend_atlas_init(RendAtlas *atlas, uint8_t *staging)
{
    memset(atlas, 0, sizeof(*atlas));
    atlas->staging = staging;
    atlas->current_frame = 0;
    atlas->entry_count = 0;
    atlas->dirty = false;
    atlas->dirty_rect = (RendAtlasRegion){ 0, 0, 0, 0 };
    atlas_clear(atlas);
    atlas->num_shelves = 0;
    atlas->next_shelf_y = 0;
}

void rend_atlas_begin_frame(RendAtlas *atlas)
{
    atlas->current_frame++;
}

RendAtlasEntry *rend_atlas_lookup(RendAtlas *atlas, void *font_data,
                                  int glyph_id, uint32_t color)
{
    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_ATLAS_HASH_SIZE - 1);

    for (int probe = 0; probe < REND_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_ATLAS_HASH_SIZE - 1);
        RendAtlasEntry *e = &atlas->entries[i];
        if (!e->occupied)
            return NULL;
        if (e->font_data == font_data && e->glyph_id == glyph_id &&
            e->color == color) {
            e->last_used_frame = atlas->current_frame;
            return e;
        }
    }
    return NULL;
}

RendAtlasEntry *rend_atlas_insert(RendAtlas *atlas, void *font_data,
                                  int glyph_id, uint32_t color,
                                  GlyphBitmap *bmp, bool is_color,
                                  bool linearize_color)
{
    if (!bmp || bmp->width <= 0 || bmp->height <= 0 || !bmp->pixels)
        return NULL;

    RendAtlasRegion region;

    if (atlas->entry_count >= REND_ATLAS_HASH_SIZE * 3 / 4)
        atlas_evict(atlas);

    if (!atlas_alloc(atlas, bmp->width, bmp->height, &region)) {
        atlas_evict(atlas);
        if (!atlas_alloc(atlas, bmp->width, bmp->height, &region)) {
            vlog("Atlas: glyph %d too large (%dx%d)\n",
                 glyph_id, bmp->width, bmp->height);
            return NULL;
        }
    }

    bool linearize = linearize_color && is_color;
    if (linearize && !s_atlas_srgb_lut_ready)
        build_srgb_lut();

    int staging_pitch = REND_ATLAS_TEXTURE_SIZE * 4;
    int src_pitch = bmp->width * 4;
    for (int y = 0; y < bmp->height; y++) {
        uint8_t *dst = atlas->staging + (region.y + y) * staging_pitch + region.x * 4;
        uint8_t *src = bmp->pixels + y * src_pitch;
        if (linearize) {
            for (int x = 0; x < bmp->width; x++) {
                uint8_t *s = src + x * 4;
                uint8_t *d = dst + x * 4;
                d[0] = s_atlas_srgb_to_linear_u8[s[0]];
                d[1] = s_atlas_srgb_to_linear_u8[s[1]];
                d[2] = s_atlas_srgb_to_linear_u8[s[2]];
                d[3] = s[3];
            }
        } else {
            memcpy(dst, src, src_pitch);
        }
    }

    if (atlas->dirty) {
        int x0 = atlas->dirty_rect.x < region.x ? atlas->dirty_rect.x : region.x;
        int y0 = atlas->dirty_rect.y < region.y ? atlas->dirty_rect.y : region.y;
        int x1 = (atlas->dirty_rect.x + atlas->dirty_rect.w > region.x + region.w)
                     ? atlas->dirty_rect.x + atlas->dirty_rect.w
                     : region.x + region.w;
        int y1 = (atlas->dirty_rect.y + atlas->dirty_rect.h > region.y + region.h)
                     ? atlas->dirty_rect.y + atlas->dirty_rect.h
                     : region.y + region.h;
        atlas->dirty_rect = (RendAtlasRegion){ x0, y0, x1 - x0, y1 - y0 };
    } else {
        atlas->dirty_rect = region;
        atlas->dirty = true;
    }

    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_ATLAS_HASH_SIZE - 1);
    RendAtlasEntry *slot = NULL;

    for (int probe = 0; probe < REND_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_ATLAS_HASH_SIZE - 1);
        RendAtlasEntry *e = &atlas->entries[i];
        if (!e->occupied) {
            slot = e;
            break;
        }
    }

    if (!slot) {
        vlog("Atlas: hash table full\n");
        return NULL;
    }

    slot->font_data = font_data;
    slot->glyph_id = glyph_id;
    slot->color = color;
    slot->region = region;
    slot->x_offset = bmp->x_offset;
    slot->y_offset = bmp->y_offset;
    slot->centered = bmp->centered;
    slot->last_used_frame = atlas->current_frame;
    slot->occupied = true;
    atlas->entry_count++;

    return slot;
}

RendAtlasEntry *rend_atlas_insert_empty(RendAtlas *atlas, void *font_data,
                                        int glyph_id, uint32_t color)
{
    if (atlas->entry_count >= REND_ATLAS_HASH_SIZE * 3 / 4)
        atlas_evict(atlas);

    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_ATLAS_HASH_SIZE - 1);
    RendAtlasEntry *slot = NULL;

    for (int probe = 0; probe < REND_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_ATLAS_HASH_SIZE - 1);
        RendAtlasEntry *e = &atlas->entries[i];
        if (!e->occupied) {
            slot = e;
            break;
        }
    }

    if (!slot)
        return NULL;

    slot->font_data = font_data;
    slot->glyph_id = glyph_id;
    slot->color = color;
    slot->region = (RendAtlasRegion){ 0, 0, 0, 0 };
    slot->x_offset = 0;
    slot->y_offset = 0;
    slot->last_used_frame = atlas->current_frame;
    slot->occupied = true;
    atlas->entry_count++;

    return slot;
}

void rend_atlas_log_stats(RendAtlas *atlas)
{
    if (!atlas)
        return;
    vlog("Atlas stats: frame=%llu entries=%d shelves=%d\n",
         (unsigned long long)atlas->current_frame,
         atlas->entry_count,
         atlas->num_shelves);
}

// =============================================================================
// Font loading orchestration — GPU-agnostic
// =============================================================================

static bool rend_load_font_style(FontResolveBackend *resolve, FontBackend *font,
                                 FontType type, FontStyle style,
                                 const char *font_name, float font_size,
                                 const FontOptions *options, const char *label)
{
    FontResolutionResult result;
    if (font_resolve_find_font(resolve, type, font_name, &result) != 0)
        return false;
    bool ok = font_load_font(font, style, result.font_path, font_size, options);
    if (ok)
        vlog("%s font loaded successfully from %s\n", label, result.font_path);
    else
        vlog("Failed to load %s font from %s\n", label, result.font_path);
    font_resolve_free_result(&result);
    return ok;
}

int rend_load_fonts(RendFontLoadResult *r,
                    FontBackend *font, FontResolveBackend *resolve,
                    float font_size, const char *font_name,
                    int ft_hint_target, float content_scale,
                    const char *hint_name)
{
    r->font = font;
    r->resolve = resolve;
    r->hint_name = hint_name ? hint_name : "unknown";

    FontOptions options = { 0 };
    options.ft_hint_target = ft_hint_target;
    options.dpi_x = 96;
    options.dpi_y = 96;

    if (content_scale > 0.0f) {
        int dpi = (int)(96.0f * content_scale);
        options.dpi_x = dpi;
        options.dpi_y = dpi;
    }
    vlog("Font DPI: %d (content_scale=%.2f)\n", options.dpi_x, content_scale);
    r->font_options = options;

    // Load normal monospace font (required)
    FontResolutionResult result;
    if (font_resolve_find_font(resolve, FONT_TYPE_NORMAL, font_name, &result) != 0) {
        fprintf(stderr, "Failed to load or find normal font\n");
        return -1;
    }
    if (result.size > 0) {
        vlog("Font pattern specifies size %.1f, overriding default %.1f\n",
             result.size, font_size);
        font_size = result.size;
    }
    if (!font_load_font(font, FONT_STYLE_NORMAL, result.font_path, font_size, &options)) {
        fprintf(stderr, "Failed to load normal font from %s\n", result.font_path);
        font_resolve_free_result(&result);
        return -1;
    }
    vlog("Normal font loaded: %s size=%.1f hinting=%s\n",
         result.font_path, font_size, hint_name);
    free(r->font_path);
    r->font_path = result.font_path ? strdup(result.font_path) : NULL;
    font_resolve_free_result(&result);
    r->font_size = font_size;

    // Load optional styles
    rend_load_font_style(resolve, font, FONT_TYPE_BOLD, FONT_STYLE_BOLD,
                         font_name, font_size, &options, "Bold");
    rend_load_font_style(resolve, font, FONT_TYPE_ITALIC, FONT_STYLE_ITALIC,
                         font_name, font_size, &options, "Italic");
    rend_load_font_style(resolve, font, FONT_TYPE_BOLD_ITALIC,
                         FONT_STYLE_BOLD_ITALIC, font_name, font_size, &options,
                         "Bold Italic");
    rend_load_font_style(resolve, font, FONT_TYPE_EMOJI, FONT_STYLE_EMOJI,
                         NULL, font_size * REND_EMOJI_FONT_SCALE, &options, "Emoji");

    // Compute cell metrics from normal font
    const FontMetrics *metrics = font_get_metrics(font, FONT_STYLE_NORMAL);
    if (!metrics) {
        vlog("ERROR: No font available for metrics calculation\n");
        return -1;
    }

    int centered = (metrics->cell_height + metrics->cap_height) / 2;
    int max_ascent = metrics->ascent + metrics->line_gap;
    r->font_ascent = centered < max_ascent ? centered : max_ascent;
    r->font_descent = metrics->descent;
    r->font_cap_height = metrics->cap_height;
    r->char_width = metrics->glyph_width;
    r->char_height = metrics->glyph_height;
    r->cell_width = metrics->cell_width;
    r->cell_height = metrics->cell_height;

    vlog("Font metrics - ascent: %d, descent: %d\n", r->font_ascent, r->font_descent);
    vlog("Cell dimensions - width: %d, height: %d\n", r->cell_width, r->cell_height);

    font_set_target_cell_width(font, r->cell_width);

    vlog("Font loading summary:\n");
    vlog("  Normal: %s\n", font_has_style(font, FONT_STYLE_NORMAL) ? "Loaded" : "Not loaded");
    vlog("  Bold: %s\n", font_has_style(font, FONT_STYLE_BOLD) ? "Loaded" : "Not loaded");
    vlog("  Italic: %s\n", font_has_style(font, FONT_STYLE_ITALIC) ? "Loaded" : "Not loaded");
    vlog("  Bold Italic: %s\n", font_has_style(font, FONT_STYLE_BOLD_ITALIC) ? "Loaded" : "Not loaded");
    vlog("  Emoji: %s\n", font_has_style(font, FONT_STYLE_EMOJI) ? "Loaded" : "Not loaded");

    return 0;
}

// =============================================================================
// Fallback font resolution — GPU-agnostic
// =============================================================================

void rend_fallback_init(RendFallbackState *st)
{
    memset(st, 0, sizeof(*st));
}

void rend_fallback_destroy(RendFallbackState *st, FontBackend *font)
{
    for (int i = 0; i < st->loaded_count; i++) {
        font->destroy_font(font, st->loaded[i].font_data);
        free(st->loaded[i].font_path);
    }
    font->font_data[FONT_STYLE_FALLBACK] = NULL;
    font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);
    for (int i = 0; i < st->cache_count; i++)
        free(st->cache[i].font_path);
    memset(st, 0, sizeof(*st));
}

const char *rend_fallback_lookup(RendFallbackState *st,
                                 FontResolveBackend *resolve,
                                 uint32_t codepoint)
{
    for (int i = 0; i < st->cache_count; i++) {
        if (st->cache[i].codepoint == codepoint)
            return st->cache[i].font_path;
    }

    FontResolutionResult result;
    char *path = NULL;
    if (font_resolve_find_font_for_codepoint(resolve, codepoint, &result) == 0) {
        path = result.font_path;
        result.font_path = NULL;
        font_resolve_free_result(&result);
    }

    if (st->cache_count >= REND_FALLBACK_CACHE_SIZE) {
        free(st->cache[0].font_path);
        memmove(&st->cache[0], &st->cache[1],
                (REND_FALLBACK_CACHE_SIZE - 1) * sizeof(RendFallbackCacheEntry));
        st->cache_count = REND_FALLBACK_CACHE_SIZE - 1;
    }
    st->cache[st->cache_count].codepoint = codepoint;
    st->cache[st->cache_count].font_path = path;
    st->cache_count++;

    return path;
}

bool rend_fallback_ensure(RendFallbackState *st, FontBackend *font,
                          const char *font_path, float font_size,
                          const FontOptions *options, int cell_width)
{
    if (!font_path)
        return false;

    for (int i = 0; i < st->loaded_count; i++) {
        if (strcmp(st->loaded[i].font_path, font_path) == 0) {
            font->font_data[FONT_STYLE_FALLBACK] = st->loaded[i].font_data;
            font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);
            return true;
        }
    }

    if (st->loaded_count >= REND_MAX_LOADED_FALLBACKS) {
        RendLoadedFallbackFont *victim = &st->loaded[0];
        vlog("Fallback cache full, evicting: %s\n", victim->font_path);
        if (font->font_data[FONT_STYLE_FALLBACK] == victim->font_data) {
            font->font_data[FONT_STYLE_FALLBACK] = NULL;
            font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);
        }
        font->destroy_font(font, victim->font_data);
        free(victim->font_path);
        memmove(&st->loaded[0], &st->loaded[1],
                (REND_MAX_LOADED_FALLBACKS - 1) * sizeof(RendLoadedFallbackFont));
        st->loaded_count--;
    }

    void *new_font_data = font->init_font(font, font_path, font_size,
                                          FONT_STYLE_FALLBACK, options);
    if (!new_font_data) {
        vlog("Failed to load fallback font: %s\n", font_path);
        return false;
    }

    if (!font->get_metrics(font, new_font_data,
                           &font->metrics[FONT_STYLE_FALLBACK])) {
        font->destroy_font(font, new_font_data);
        vlog("Failed to get metrics for fallback font: %s\n", font_path);
        return false;
    }

    RendLoadedFallbackFont *entry = &st->loaded[st->loaded_count];
    entry->font_path = strdup(font_path);
    entry->font_data = new_font_data;
    st->loaded_count++;

    font->font_data[FONT_STYLE_FALLBACK] = new_font_data;
    font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);

    font_set_target_cell_width(font, cell_width);
    font_set_variation_axis(font, FONT_STYLE_FALLBACK, "wght", 400);

    vlog("Fallback font loaded and cached (%d/%d): %s\n",
         st->loaded_count, REND_MAX_LOADED_FALLBACKS, font_path);
    return true;
}

// =============================================================================
// Scroll offset management — GPU-agnostic
// =============================================================================

void rend_scroll(RendScrollState *st, TerminalBackend *term, int delta)
{
    int scrollback_lines = terminal_get_scrollback_lines(term);
    int new_offset = st->scroll_offset + delta;
    if (new_offset < 0)
        new_offset = 0;
    if (new_offset > scrollback_lines)
        new_offset = scrollback_lines;
    if (new_offset != st->scroll_offset) {
        st->scroll_offset = new_offset;
        vlog("Scroll offset changed to %d (max: %d)\n",
             st->scroll_offset, scrollback_lines);
    }
}

void rend_reset_scroll(RendScrollState *st)
{
    if (st->overlay)
        return;
    if (st->scroll_offset != 0) {
        st->scroll_offset = 0;
        vlog("Scroll offset reset to 0\n");
    }
}

int rend_get_scroll_offset(RendScrollState *st)
{
    return st->scroll_offset;
}

void rend_set_overlay(RendScrollState *st, TerminalBackend *overlay)
{
    if (!overlay)
        return;
    st->saved_scroll_offset = st->scroll_offset;
    st->overlay = overlay;
    st->scroll_offset = terminal_get_scrollback_lines(overlay);
}

void rend_clear_overlay(RendScrollState *st)
{
    if (!st->overlay)
        return;
    st->overlay = NULL;
    st->scroll_offset = st->saved_scroll_offset;
}

bool rend_has_overlay(RendScrollState *st)
{
    return st->overlay != NULL;
}

// =============================================================================
// Diagnostics helpers — GPU-agnostic
// =============================================================================

bool rend_classify_gpu_driver_libre(const char *driver_name,
                                    const char *driver_info)
{
    return (driver_info && strstr(driver_info, "Mesa")) ||
           (driver_name && strstr(driver_name, "Mesa")) ||
           (driver_name && strstr(driver_name, "open-source")) ||
           (driver_name && strstr(driver_name, "open source"));
}

void rend_format_gpu_driver(char *buf, size_t bufsz,
                            const char *driver_name,
                            const char *driver_info,
                            const char *driver_version)
{
    if (!buf || bufsz == 0)
        return;
    buf[0] = '\0';
    if (!driver_name || !*driver_name)
        return;

    bool libre = rend_classify_gpu_driver_libre(driver_name, driver_info);
    const char *origin = libre ? "open source"
                         : (driver_name && strstr(driver_name, "NVIDIA"))
                             ? "proprietary"
                             : NULL;

    const char *ver = (driver_info && *driver_info) ? driver_info : driver_version;
    char verbuf[96];
    verbuf[0] = '\0';
    if (ver) {
        size_t n = 0;
        while (ver[n] && ver[n] != '\n' && n + 1 < sizeof(verbuf)) {
            verbuf[n] = ver[n];
            n++;
        }
        verbuf[n] = '\0';
    }

    if (origin && verbuf[0])
        snprintf(buf, bufsz, "%s (%s) — %s", driver_name, origin, verbuf);
    else if (origin)
        snprintf(buf, bufsz, "%s (%s)", driver_name, origin);
    else if (verbuf[0])
        snprintf(buf, bufsz, "%s — %s", driver_name, verbuf);
    else
        snprintf(buf, bufsz, "%s", driver_name);
}

// =============================================================================
// Close "×" bitmap helper — shared between SDL3 and Sokol backends
// =============================================================================

// Shortest distance from point (px,py) to the segment (ax,ay)-(bx,by).
static float seg_distance(float px, float py, float ax, float ay, float bx,
                          float by)
{
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float c2 = vx * vx + vy * vy;
    float t = c2 > 0.0f ? (vx * wx + vy * wy) / c2 : 0.0f;
    if (t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

void rend_make_close_x_bitmap(uint8_t *buf, int size)
{
    if (!buf || size <= 0)
        return;

    float inset = (float)size * 0.30f;
    float lo = inset, hi = (float)size - inset;
    float hw = (float)size * 0.06f; // half stroke width
    if (hw < 0.75f)
        hw = 0.75f;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = seg_distance(px, py, lo, lo, hi, hi);  // top-left -> bottom-right
            float d2 = seg_distance(px, py, hi, lo, lo, hi); // top-right -> bottom-left
            if (d2 < d)
                d = d2;
            float cov = hw + 0.5f - d; // 1px anti-aliased edge band
            if (cov < 0.0f)
                cov = 0.0f;
            else if (cov > 1.0f)
                cov = 1.0f;
            uint8_t *p = buf + ((size_t)y * size + x) * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = (uint8_t)(cov * 255.0f + 0.5f);
        }
    }
}
