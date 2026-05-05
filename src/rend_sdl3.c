#include "rend_sdl3.h"
#include "common.h"
#include "font.h"
#include "font_ft.h"
#include "font_resolve.h"
#ifdef _WIN32
#include "font_resolve_w32.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_w32
#elif defined(__APPLE__)
#include "font_resolve_ct.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_ct
#else
#include "font_resolve_fc.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_fc
#endif
#include "png_writer.h"
#include "rend.h"
#include "rend_sdl3_atlas.h"
#include "rend_sdl3_boxdraw.h"
#include "unicode.h"
#include <SDL3/SDL.h>
#include <stdint.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMOJI_FONT_SCALE     4.0f
#define FALLBACK_CACHE_SIZE  64
#define MAX_LOADED_FALLBACKS 8

typedef struct
{
    uint32_t codepoint;
    char *font_path; // NULL = no font found for this codepoint
} FallbackCacheEntry;

typedef struct
{
    char *font_path;
    void *font_data; // FtFontData*, kept alive for pointer stability
} LoadedFallbackFont;

// Cursor color: Charm signature purple with slight transparency (RGBA)
#define CURSOR_COLOR_R 0x6B
#define CURSOR_COLOR_G 0x50
#define CURSOR_COLOR_B 0xFF
#define CURSOR_COLOR_A 220

// Selection highlight color: muted Dracula comment-style (RGBA)
#define SELECTION_COLOR_R 0x44
#define SELECTION_COLOR_G 0x47
#define SELECTION_COLOR_B 0x5A
#define SELECTION_COLOR_A 180

// Underline color: matches cursor color
#define UNDERLINE_COLOR_R CURSOR_COLOR_R
#define UNDERLINE_COLOR_G CURSOR_COLOR_G
#define UNDERLINE_COLOR_B CURSOR_COLOR_B
#define UNDERLINE_COLOR_A 255

// OSC-8 hyperlink underline: idle is a faint dotted Charm-purple line under
// the cell so the user can see linked text exists without clutter; hover
// brightens to full opacity across the whole run sharing the same id.
#define HYPERLINK_COLOR_R     CURSOR_COLOR_R
#define HYPERLINK_COLOR_G     CURSOR_COLOR_G
#define HYPERLINK_COLOR_B     CURSOR_COLOR_B
#define HYPERLINK_IDLE_ALPHA  110
#define HYPERLINK_HOVER_ALPHA 255

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
static uint32_t nf_translate_codepoint(uint32_t cp)
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

// =============================================================================
// END NERD FONTS HACK
// =============================================================================

// Box-filter downscale a glyph bitmap to fit within max_w x max_h.
// Returns a newly allocated GlyphBitmap, or NULL if no downscale is needed.
static GlyphBitmap *downscale_bitmap(GlyphBitmap *src, int max_w, int max_h)
{
    if (!src || !src->pixels || src->width <= 0 || src->height <= 0)
        return NULL;
    if (src->width <= max_w && src->height <= max_h)
        return NULL;

    float scale_x = (float)max_w / (float)src->width;
    float scale_y = (float)max_h / (float)src->height;
    float scale = fminf(scale_x, scale_y);

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

// Draw a filled rounded rectangle
static void draw_rounded_rect(SDL_Renderer *renderer, float x, float y,
                              float w, float h, float radius)
{
    if (radius <= 0) {
        SDL_FRect rect = { x, y, w, h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    // Clamp radius to half of smallest dimension
    if (radius > w / 2)
        radius = w / 2;
    if (radius > h / 2)
        radius = h / 2;

    // Draw center rectangle (full width, excluding corner rows)
    SDL_FRect center = { x, y + radius, w, h - 2 * radius };
    SDL_RenderFillRect(renderer, &center);

    // Draw top and bottom rectangles (excluding corners)
    SDL_FRect top = { x + radius, y, w - 2 * radius, radius };
    SDL_FRect bottom = { x + radius, y + h - radius, w - 2 * radius, radius };
    SDL_RenderFillRect(renderer, &top);
    SDL_RenderFillRect(renderer, &bottom);

    // Draw corner circles using filled points
    float r2 = radius * radius;
    for (int dy = 0; dy < (int)radius; dy++) {
        for (int dx = 0; dx < (int)radius; dx++) {
            float dist2 = (radius - dx - 0.5f) * (radius - dx - 0.5f) +
                          (radius - dy - 0.5f) * (radius - dy - 0.5f);
            if (dist2 <= r2) {
                // Top-left
                SDL_RenderPoint(renderer, x + dx, y + dy);
                // Top-right
                SDL_RenderPoint(renderer, x + w - 1 - dx, y + dy);
                // Bottom-left
                SDL_RenderPoint(renderer, x + dx, y + h - 1 - dy);
                // Bottom-right
                SDL_RenderPoint(renderer, x + w - 1 - dx, y + h - 1 - dy);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Underline drawing helpers (DPI-aware)
// ---------------------------------------------------------------------------

static void draw_underline_single(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    SDL_FRect rect = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_underline_double(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    int gap = (int)roundf(1.0f * pixel_density);
    if (gap < 1)
        gap = 1;
    SDL_FRect top = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &top);
    SDL_FRect bot = { (float)x, (float)(y + thickness + gap), (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &bot);
}

static void draw_underline_curly(SDL_Renderer *renderer, int x, int y, int width,
                                 float pixel_density, Uint8 cr, Uint8 cg, Uint8 cb)
{
    float amplitude = 1.5f * pixel_density;
    if (amplitude < 1.0f)
        amplitude = 1.0f;
    float wavelength = 8.0f * pixel_density;
    if (wavelength < 4.0f)
        wavelength = 4.0f;
    float thickness = 0.5f * pixel_density;
    if (thickness < 0.5f)
        thickness = 0.5f;
    float center_y = (float)y + amplitude;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int px = 0; px < width; px++) {
        float sine_y = center_y + amplitude * sinf((float)px / wavelength * 2.0f * (float)M_PI);
        int y_min = (int)floorf(sine_y - thickness);
        int y_max = (int)ceilf(sine_y + thickness);
        for (int iy = y_min; iy <= y_max; iy++) {
            float dist = fabsf((float)iy + 0.5f - sine_y);
            float alpha;
            if (dist <= thickness)
                alpha = 1.0f;
            else if (dist <= thickness + 1.0f)
                alpha = 1.0f - (dist - thickness);
            else
                continue;
            Uint8 a = (Uint8)(alpha * 255.0f);
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, a);
            SDL_RenderPoint(renderer, (float)(x + px), (float)iy);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static void draw_underline_dotted(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    float radius = 0.5f * pixel_density;
    if (radius < 0.5f)
        radius = 0.5f;
    float gap = roundf(2.0f * pixel_density);
    if (gap < 2.0f)
        gap = 2.0f;
    float stride = radius * 2.0f + gap;
    float cy = (float)y + radius;

    if (radius < 1.0f) {
        // Low DPI: single-pixel dots
        for (float cx = (float)x; cx < (float)(x + width); cx += stride)
            SDL_RenderPoint(renderer, cx, (float)y);
    } else {
        // HiDPI: filled circles via scanlines
        float r2 = radius * radius;
        for (float cx = (float)x + radius; cx < (float)(x + width); cx += stride) {
            for (float dy = -radius; dy <= radius; dy += 1.0f) {
                float half_w = sqrtf(r2 - dy * dy);
                SDL_FRect span = { cx - half_w, cy + dy, half_w * 2.0f, 1.0f };
                SDL_RenderFillRect(renderer, &span);
            }
        }
    }
}

static void draw_underline_dashed(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    int dash_w = (int)roundf(3.0f * pixel_density);
    if (dash_w < 1)
        dash_w = 1;
    int gap = (int)roundf(2.0f * pixel_density);
    if (gap < 1)
        gap = 1;
    int stride = dash_w + gap;
    for (int px = x; px < x + width; px += stride) {
        int w = dash_w;
        if (px + w > x + width)
            w = x + width - px;
        SDL_FRect rect = { (float)px, (float)y, (float)w, (float)thickness };
        SDL_RenderFillRect(renderer, &rect);
    }
}

// ---------------------------------------------------------------------------
// Strikethrough drawing helper (DPI-aware)
// ---------------------------------------------------------------------------

static void draw_strikethrough(SDL_Renderer *renderer, int x, int y, int width,
                               float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    SDL_FRect rect = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &rect);
}

typedef struct RendererSdl3Data
{
    SDL_Renderer *renderer;
    SDL_Window *window;
    FontBackend *font;
    int cell_width;
    int cell_height;
    int char_width;
    int char_height;
    int font_ascent;
    int font_descent;
    int font_cap_height;
    int width;
    int height;
    int scroll_offset;

    RendSdl3Atlas atlas;

    // Font resolver backend (kept alive for runtime fallback queries)
    FontResolveBackend *resolve;

    // Dynamic font fallback cache
    FallbackCacheEntry fallback_cache[FALLBACK_CACHE_SIZE];
    int fallback_cache_count;
    float font_size;          // saved for loading fallback at same size
    FontOptions font_options; // saved for loading fallback with same options

    // Loaded fallback font cache — keeps multiple fallback fonts alive
    // so their font_data pointers remain stable for atlas cache keys
    LoadedFallbackFont loaded_fallbacks[MAX_LOADED_FALLBACKS];
    int loaded_fallback_count;

    // Content scale factor (physical pixels / logical pixels)
    // Used for both font DPI and decoration scaling
    float content_scale;

    // Sixel texture cache
    struct
    {
        SDL_Texture *texture;
        SixelImage *source; // pointer identity check
    } sixel_cache[TERM_MAX_SIXEL_IMAGES];
    int sixel_cache_count;
} RendererSdl3Data;

// Forward declaration for sixel cache cleanup (used in sdl3_destroy)
static void sixel_cache_clear(RendererSdl3Data *data);

static bool sdl3_init(RendererBackend *backend, void *window_handle, void *renderer_handle)
{
    // Allocate SDL3-specific data
    RendererSdl3Data *data = malloc(sizeof(RendererSdl3Data));
    if (!data)
        return false;

    // Cast handles back to SDL types
    data->window = (SDL_Window *)window_handle;
    data->renderer = (SDL_Renderer *)renderer_handle;

    // Initialize fields
    data->font = NULL;
    data->cell_width = 0;
    data->cell_height = 0;
    data->char_width = 0;
    data->char_height = 0;
    data->font_ascent = 0;
    data->font_descent = 0;
    data->width = 0;
    data->height = 0;
    data->scroll_offset = 0;
    data->resolve = NULL;
    data->fallback_cache_count = 0;
    data->font_size = 0;
    memset(&data->font_options, 0, sizeof(data->font_options));
    memset(data->fallback_cache, 0, sizeof(data->fallback_cache));
    data->loaded_fallback_count = 0;
    memset(data->loaded_fallbacks, 0, sizeof(data->loaded_fallbacks));
    data->content_scale = 1.0f;

    // Initialize glyph atlas
    if (!rend_sdl3_atlas_init(&data->atlas, data->renderer)) {
        vlog("Failed to initialize glyph atlas\n");
        free(data);
        return false;
    }

    // Initialize font backend with FreeType backend
    data->font = &font_backend_ft;
    if (!font_init(data->font)) {
        vlog("Failed to initialize font backend\n");
        rend_sdl3_atlas_destroy(&data->atlas);
        free(data);
        return false;
    }

    // Store in backend
    backend->backend_data = data;

    return true;
}

static void sdl3_destroy(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    // Destroy sixel texture cache
    sixel_cache_clear(data);

    // Destroy glyph atlas
    rend_sdl3_atlas_destroy(&data->atlas);

    // Destroy all cached loaded fallback fonts
    for (int i = 0; i < data->loaded_fallback_count; i++) {
        if (data->loaded_fallbacks[i].font_data) {
            data->font->destroy_font(data->font, data->loaded_fallbacks[i].font_data);
        }
        free(data->loaded_fallbacks[i].font_path);
    }
    data->loaded_fallback_count = 0;

    // Clear the fallback slot pointer so font_destroy() doesn't double-free it
    // (the actual font_data was already destroyed above from the cache)
    data->font->font_data[FONT_STYLE_FALLBACK] = NULL;
    data->font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);

    if (data->font) {
        font_destroy(data->font);
    }

    // Free fallback cache entries
    for (int i = 0; i < data->fallback_cache_count; i++) {
        free(data->fallback_cache[i].font_path);
    }

    // Cleanup font resolver (deferred from sdl3_load_fonts)
    if (data->resolve) {
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
    }

    free(data);
    backend->backend_data = NULL;
}

static bool load_font_style(FontResolveBackend *resolve, FontBackend *font,
                            FontType type, FontStyle style,
                            const char *font_name, float font_size,
                            const FontOptions *options, const char *label,
                            float *out_size)
{
    FontResolutionResult result;
    if (font_resolve_find_font(resolve, type, font_name, &result) != 0)
        return false;

    if (out_size && result.size > 0)
        *out_size = result.size;

    bool ok = font_load_font(font, style, result.font_path, font_size, options);
    if (ok)
        vlog("%s font loaded successfully from %s\n", label, result.font_path);
    else
        vlog("Failed to load %s font from %s\n", label, result.font_path);

    font_resolve_free_result(&result);
    return ok;
}

static int sdl3_load_fonts(RendererBackend *backend, float font_size, const char *font_name, int ft_hint_target)
{
    if (!backend || !backend->backend_data)
        return -1;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    const char *hint_name = "none";
    if (ft_hint_target == FT_LOAD_TARGET_LIGHT)
        hint_name = "light";
    else if (ft_hint_target == FT_LOAD_TARGET_NORMAL)
        hint_name = "normal";
    else if (ft_hint_target == FT_LOAD_TARGET_MONO)
        hint_name = "mono";

    // Initialize font resolver
    data->resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
    if (!data->resolve) {
        fprintf(stderr, "Failed to initialize font resolver\n");
        return -1;
    }

    // Setup DPI options
    FontOptions options = { 0 };
    options.ft_hint_target = ft_hint_target;
    options.dpi_x = 96;
    options.dpi_y = 96;

    // Determine font DPI from content scale.
    // content_scale is pre-set by GTK4 or main.c; also check SDL window.
    float font_scale = data->content_scale;
    if (data->window && font_scale <= 1.0f) {
        float sdl_scale = SDL_GetWindowDisplayScale(data->window);
        if (sdl_scale > 1.0f)
            font_scale = sdl_scale;
    }
    if (font_scale > 0.0f) {
        data->content_scale = font_scale;
        int dpi = (int)(96.0f * font_scale);
        options.dpi_x = dpi;
        options.dpi_y = dpi;
    }
    vlog("Font DPI: %d (content_scale=%.2f)\n", options.dpi_x, data->content_scale);

    // Load normal monospace font (required)
    // Resolve path first, then check for pattern-specified size before loading
    FontResolutionResult result;
    if (font_resolve_find_font(data->resolve, FONT_TYPE_NORMAL, font_name, &result) != 0) {
        fprintf(stderr, "Failed to load or find normal font\n");
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
        return -1;
    }
    if (result.size > 0) {
        vlog("Font pattern specifies size %.1f, overriding default %.1f\n", result.size, font_size);
        font_size = result.size;
    }
    if (!font_load_font(data->font, FONT_STYLE_NORMAL, result.font_path, font_size, &options)) {
        fprintf(stderr, "Failed to load normal font from %s\n", result.font_path);
        font_resolve_free_result(&result);
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
        return -1;
    }
    vlog("Normal font loaded: %s size=%.1f hinting=%s\n", result.font_path, font_size, hint_name);
    font_resolve_free_result(&result);

    // Save font size and options for dynamic fallback loading later
    data->font_size = font_size;
    data->font_options = options;

    // Load bold font (optional)
    load_font_style(data->resolve, data->font, FONT_TYPE_BOLD, FONT_STYLE_BOLD,
                    font_name, font_size, &options, "Bold", NULL);

    // Load italic font (optional)
    load_font_style(data->resolve, data->font, FONT_TYPE_ITALIC, FONT_STYLE_ITALIC,
                    font_name, font_size, &options, "Italic", NULL);

    // Load bold italic font (optional)
    load_font_style(data->resolve, data->font, FONT_TYPE_BOLD_ITALIC, FONT_STYLE_BOLD_ITALIC,
                    font_name, font_size, &options, "Bold Italic", NULL);

    // Load emoji font (optional)
    load_font_style(data->resolve, data->font, FONT_TYPE_EMOJI, FONT_STYLE_EMOJI,
                    NULL, font_size * EMOJI_FONT_SCALE, &options, "Emoji", NULL);

    // NOTE: font_resolver_cleanup() is deferred to sdl3_destroy() so the
    // resolver remains available for runtime dynamic fallback queries.

    // Calculate cell dimensions from font metrics using normal font
    const FontMetrics *metrics = font_get_metrics(data->font, FONT_STYLE_NORMAL);
    if (!metrics) {
        vlog("ERROR: No font available for metrics calculation\n");
        return -1;
    }

    // Center uppercase text (cap_height region) vertically within cell_height.
    // Derivation: we want cap_height midpoint at cell center, so
    //   font_ascent - cap_height/2 = cell_height/2
    //   font_ascent = (cell_height + cap_height) / 2
    // Clamp to ascent+line_gap to prevent descender clipping when cap_height ≈ ascent.
    int centered = (metrics->cell_height + metrics->cap_height) / 2;
    int max_ascent = metrics->ascent + metrics->line_gap;
    data->font_ascent = centered < max_ascent ? centered : max_ascent;
    vlog("font_ascent: centered=%d, max_ascent=%d, chosen=%d\n",
         centered, max_ascent, data->font_ascent);
    data->font_descent = metrics->descent;
    data->font_cap_height = metrics->cap_height;
    data->char_width = metrics->glyph_width;
    data->char_height = metrics->glyph_height;
    data->cell_width = metrics->cell_width;
    data->cell_height = metrics->cell_height;

    vlog("Font metrics - ascent: %d, descent: %d\n",
         data->font_ascent, data->font_descent);
    vlog("Character dimensions - width: %d, height: %d\n",
         data->char_width, data->char_height);
    vlog("Cell dimensions - width: %d, height: %d\n",
         data->cell_width, data->cell_height);

    // Set target cell width on all loaded fonts so oversized glyphs are
    // scaled down during rasterization (before bitmap generation).
    font_set_target_cell_width(data->font, data->cell_width);

    // Log which fonts were successfully loaded
    vlog("Font loading summary:\n");
    vlog("  Normal font: %s\n", font_has_style(data->font, FONT_STYLE_NORMAL) ? "Loaded" : "Not loaded");
    vlog("  Bold font: %s\n", font_has_style(data->font, FONT_STYLE_BOLD) ? "Loaded" : "Not loaded");
    vlog("  Italic font: %s\n", font_has_style(data->font, FONT_STYLE_ITALIC) ? "Loaded" : "Not loaded");
    vlog("  Bold Italic font: %s\n", font_has_style(data->font, FONT_STYLE_BOLD_ITALIC) ? "Loaded" : "Not loaded");
    vlog("  Emoji font: %s\n", font_has_style(data->font, FONT_STYLE_EMOJI) ? "Loaded" : "Not loaded");
    vlog("  Fallback font: (loaded on demand)\n");

    return 0;
}

static RendSdl3AtlasEntry *cache_glyph(RendSdl3Atlas *atlas, void *font_data,
                                       uint32_t glyph_id, uint32_t color_key,
                                       GlyphBitmap *bitmap, bool downscale,
                                       int max_w, int max_h)
{
    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(atlas, font_data, glyph_id, color_key);
    if (entry)
        return entry;

    GlyphBitmap *scaled = NULL;
    if (downscale) {
        vlog("Cache emoji glyph %u: bitmap=%dx%d max=%dx%d\n",
             glyph_id, bitmap->width, bitmap->height, max_w, max_h);
        scaled = downscale_bitmap(bitmap, max_w, max_h);
        // Color emoji are placed by cell-center, not baseline.
        bitmap->centered = true;
        if (scaled)
            scaled->centered = true;
    }
    entry = rend_sdl3_atlas_insert(atlas, font_data, glyph_id, color_key,
                                   scaled ? scaled : bitmap);
    if (scaled) {
        free(scaled->pixels);
        free(scaled);
    }
    return entry;
}

static bool is_color_font(FontBackend *font, FontStyle style)
{
    return style == FONT_STYLE_EMOJI || font_style_has_colr(font, style);
}

static void blit_glyph(SDL_Renderer *renderer, RendSdl3Atlas *atlas,
                       RendSdl3AtlasEntry *entry,
                       int cell_x, int cell_y, int glyph_x_offset, int glyph_y_offset,
                       int avail_w, int avail_h, int font_ascent, bool is_regional,
                       bool color_baked, uint8_t mod_r, uint8_t mod_g, uint8_t mod_b)
{
    if (!entry || entry->region.w <= 0)
        return;

    SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                      (float)entry->region.w, (float)entry->region.h };
    SDL_FRect dst;
    if (entry->centered) {
        // Cell-center placement, used for color emoji (rasterized at native
        // size) and for symbol/emoji glyphs that the rasterizer pre-scaled
        // to fit the cell width. fminf collapses to ~1.0 for pre-fit
        // bitmaps, so no double-scaling occurs.
        float glyph_w = (float)entry->region.w;
        float glyph_h = (float)entry->region.h;
        float scaled_w, scaled_h;

        if (is_regional) {
            // Regional indicators: scale uniformly to fit within a square,
            // preserving aspect ratio.
            float side = fminf((float)avail_w, (float)avail_h);
            float scale = fminf(side / glyph_w, side / glyph_h);
            scaled_w = fminf(glyph_w * scale, side);
            scaled_h = fminf(glyph_h * scale, side);
        } else {
            float scale = fminf((float)avail_w / glyph_w, (float)avail_h / glyph_h);
            scaled_w = glyph_w * scale;
            scaled_h = glyph_h * scale;
        }
        dst = (SDL_FRect){
            floorf((float)cell_x + ((float)avail_w - scaled_w) * 0.5f),
            floorf((float)cell_y + ((float)avail_h - scaled_h) * 0.5f),
            scaled_w, scaled_h
        };
    } else {
        // Trust FreeType's bitmap bounds: anchor at cell_x + bitmap_left and
        // let the glyph overhang the cell. Row draw is two-pass — all cell
        // backgrounds in pass 1 before any glyph in pass 2 — so a small
        // overhang lands on top of an already-painted neighbor background.
        dst = (SDL_FRect){
            (float)cell_x + glyph_x_offset,
            (float)cell_y + font_ascent - glyph_y_offset,
            (float)entry->region.w, (float)entry->region.h
        };
    }
    if (!color_baked)
        SDL_SetTextureColorMod(atlas->texture, mod_r, mod_g, mod_b);
    SDL_RenderTexture(renderer, atlas->texture, &src, &dst);
    if (!color_baked)
        SDL_SetTextureColorMod(atlas->texture, 255, 255, 255);
}

// Look up or query fontconfig for a fallback font covering the given codepoint.
// Returns the cached font_path (may be NULL if no font was found).
static const char *fallback_cache_lookup(RendererSdl3Data *data, uint32_t codepoint)
{
    // Search existing cache
    for (int i = 0; i < data->fallback_cache_count; i++) {
        if (data->fallback_cache[i].codepoint == codepoint)
            return data->fallback_cache[i].font_path;
    }

    // Query font resolver
    FontResolutionResult result;
    char *path = NULL;
    if (font_resolve_find_font_for_codepoint(data->resolve, codepoint, &result) == 0) {
        path = result.font_path;
        result.font_path = NULL; // take ownership
        font_resolve_free_result(&result);
    }

    // Store in cache (evict oldest if full)
    if (data->fallback_cache_count >= FALLBACK_CACHE_SIZE) {
        free(data->fallback_cache[0].font_path);
        memmove(&data->fallback_cache[0], &data->fallback_cache[1],
                (FALLBACK_CACHE_SIZE - 1) * sizeof(FallbackCacheEntry));
        data->fallback_cache_count = FALLBACK_CACHE_SIZE - 1;
    }
    data->fallback_cache[data->fallback_cache_count].codepoint = codepoint;
    data->fallback_cache[data->fallback_cache_count].font_path = path;
    data->fallback_cache_count++;

    return path;
}

// Ensure the FONT_STYLE_FALLBACK slot is loaded with the font at the given path.
// Uses a cache of loaded fallback fonts to keep pointers stable (important for
// atlas cache keys). Never destroys/reloads a font that's already cached.
static bool ensure_fallback_font(RendererSdl3Data *data, const char *font_path)
{
    if (!font_path)
        return false;

    // Search the loaded fallback cache for a match
    for (int i = 0; i < data->loaded_fallback_count; i++) {
        if (strcmp(data->loaded_fallbacks[i].font_path, font_path) == 0) {
            // Found — swap into the fallback slot without destroying anything
            data->font->font_data[FONT_STYLE_FALLBACK] = data->loaded_fallbacks[i].font_data;
            data->font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);
            return true;
        }
    }

    // Not cached — need to load. Evict oldest entry if cache is full.
    if (data->loaded_fallback_count >= MAX_LOADED_FALLBACKS) {
        LoadedFallbackFont *victim = &data->loaded_fallbacks[0];
        vlog("Fallback cache full, evicting: %s\n", victim->font_path);
        // If the evicted font is currently in the fallback slot, clear it
        if (data->font->font_data[FONT_STYLE_FALLBACK] == victim->font_data) {
            data->font->font_data[FONT_STYLE_FALLBACK] = NULL;
            data->font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);
        }
        data->font->destroy_font(data->font, victim->font_data);
        free(victim->font_path);
        memmove(&data->loaded_fallbacks[0], &data->loaded_fallbacks[1],
                (MAX_LOADED_FALLBACKS - 1) * sizeof(LoadedFallbackFont));
        data->loaded_fallback_count--;
    }

    // Load directly via init_font (bypass font_load_font which auto-destroys
    // whatever is in the slot — we manage the slot ourselves)
    void *new_font_data = data->font->init_font(data->font, font_path,
                                                data->font_size, FONT_STYLE_FALLBACK,
                                                &data->font_options);
    if (!new_font_data) {
        vlog("Failed to load fallback font: %s\n", font_path);
        return false;
    }

    // Get metrics for the new font
    if (!data->font->get_metrics(data->font, new_font_data,
                                 &data->font->metrics[FONT_STYLE_FALLBACK])) {
        data->font->destroy_font(data->font, new_font_data);
        vlog("Failed to get metrics for fallback font: %s\n", font_path);
        return false;
    }

    // Store in cache
    LoadedFallbackFont *entry = &data->loaded_fallbacks[data->loaded_fallback_count];
    entry->font_path = strdup(font_path);
    entry->font_data = new_font_data;
    data->loaded_fallback_count++;

    // Set the fallback slot
    data->font->font_data[FONT_STYLE_FALLBACK] = new_font_data;
    data->font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);

    // Set target cell width on the new fallback font for oversized glyph scaling
    font_set_target_cell_width(data->font, data->cell_width);

    // Match fallback font weight to regular (400).  Variable fonts like
    // Noto Sans CJK VF default to weight 100 (Thin) which is unreadable.
    font_set_variation_axis(data->font, FONT_STYLE_FALLBACK, "wght", 400);

    vlog("Fallback font loaded and cached (%d/%d): %s\n",
         data->loaded_fallback_count, MAX_LOADED_FALLBACKS, font_path);
    return true;
}

// Draw cell background only. Cell must be pre-fetched by the caller.
// row:     display row (for draw position).
// vis_col: visual column for draw position (may exceed vt_col on rows
//          with VS16-widened emoji).
// pres_w:  presentation width from the iterator (for bg rect width).
static void render_cell_bg(RendererSdl3Data *data, int row, int vis_col,
                           int pres_w, const TerminalCell *cell)
{
    if (cell->bg.is_default)
        return;
    if (pres_w <= 0)
        pres_w = 1;
    SDL_FRect bg_rect = {
        (float)(vis_col * data->cell_width),
        (float)(row * data->cell_height),
        (float)(pres_w * data->cell_width),
        (float)data->cell_height
    };
    SDL_SetRenderDrawColor(data->renderer, cell->bg.r, cell->bg.g, cell->bg.b, 255);
    SDL_RenderFillRect(data->renderer, &bg_rect);
}

// Render a single cell.
// Cell must be pre-fetched by the caller (typically via TerminalRowIter).
// row:     display row (for draw position and cursor compare).
// vt_col:  libvterm column (for cursor compare, selection check).
// vis_col: visual column for draw position (may differ from vt_col on rows
//          with VS16-widened emoji).
// pres_w:  presentation width in cells (from iterator).
static void render_cell(RendererSdl3Data *data, TerminalBackend *term,
                        int row, int vt_col, int vis_col, int pres_w,
                        const TerminalCell *cell_in, TerminalPos cursor_pos,
                        bool show_cursor, bool populate_only)
{
    TerminalCell cell = *cell_in;
    Uint8 r = cell.fg.r, g = cell.fg.g, b = cell.fg.b;

    int columns_to_consume = pres_w > 0 ? pres_w : 1;

    if (cell.cp == 0) {
        // Empty cell - jump to cursor drawing, then return
        goto render_cursor;
    }

    // Collect the cell's codepoint sequence. Single-codepoint cells —
    // ~99% of the grid — take the fast path with zero backend calls.
    // Multi-codepoint clusters (emoji ZWJ chains, flags, long combining
    // runs) come through `terminal_cell_get_grapheme` which routes to
    // bvt's grapheme arena and returns the full sequence regardless of
    // length. The 32-element local cap matches BVT_CLUSTER_MAX*2 — well
    // above any cluster the parser will commit.
    uint32_t cps[32];
    int cp_count;
    if (cell.grapheme_id == 0) {
        cps[0] = cell.cp;
        cp_count = 1;
    } else {
        int scroll_offset = data->scroll_offset;
        int scrollback_row = scroll_offset - 1 - row;
        int unified_row = (scrollback_row >= 0)
                              ? -(scrollback_row + 1)
                              : (row - scroll_offset);
        size_t n = terminal_cell_get_grapheme(term, unified_row, vt_col,
                                              cps, sizeof(cps) / sizeof(cps[0]));
        if (n == 0) {
            cps[0] = cell.cp;
            n = 1;
        }
        cp_count = (int)n;
    }

    // NERD FONTS HACK: Translate obsolete v2 codepoints to v3 equivalents
    for (int i = 0; i < cp_count; i++)
        cps[i] = nf_translate_codepoint(cps[i]);

    // Procedural box drawing / block elements — bypass font pipeline
    if (cp_count == 1 && rend_sdl3_boxdraw_is_supported(cps[0])) {
        if (!populate_only) {
            rend_sdl3_boxdraw_draw(data->renderer, cps[0],
                                   vis_col * data->cell_width,
                                   row * data->cell_height,
                                   data->cell_width, data->cell_height, r, g, b);
        }
        goto render_cursor;
    }

    // Select font style
    FontStyle style = FONT_STYLE_NORMAL;
    if (cell.attrs.bold && cell.attrs.italic) {
        if (font_has_style(data->font, FONT_STYLE_BOLD_ITALIC))
            style = FONT_STYLE_BOLD_ITALIC;
        else if (font_has_style(data->font, FONT_STYLE_BOLD))
            style = FONT_STYLE_BOLD;
        else if (font_has_style(data->font, FONT_STYLE_ITALIC))
            style = FONT_STYLE_ITALIC;
    } else if (cell.attrs.bold) {
        if (font_has_style(data->font, FONT_STYLE_BOLD))
            style = FONT_STYLE_BOLD;
    } else if (cell.attrs.italic) {
        if (font_has_style(data->font, FONT_STYLE_ITALIC))
            style = FONT_STYLE_ITALIC;
    }
    if (cp_count > 0) {
        bool use_emoji = is_emoji_presentation(cps[0]) || is_regional_indicator(cps[0]);
        if (!use_emoji) {
            for (int i = 1; i < cp_count; i++) {
                if (cps[i] == 0xFE0F) {
                    use_emoji = true;
                    break;
                }
            }
        }
        if (use_emoji && font_has_style(data->font, FONT_STYLE_EMOJI))
            style = FONT_STYLE_EMOJI;
    }

    bool emoji_render = (style == FONT_STYLE_EMOJI);

    // Width is authoritative from cell.width (the term backend enforces the
    // "VS16 → 2 cells" rule in convert_vterm_screen_cell). No render-time
    // width override needed here. See README.md "Emoji Width Paradigm".

    if (cps[0] > 0x7F)
        vlog("render_cell: U+%04X style=%d emoji=%d cols=%d cell.w=%d\n",
             cps[0], style, emoji_render, columns_to_consume, cell.width);

    int cell_x = vis_col * data->cell_width;
    int cell_y = row * data->cell_height;
    int avail_w = columns_to_consume * data->cell_width;
    int avail_h = data->cell_height;

    // Tell the font backend the pixel budget for this glyph so oversized
    // glyphs (e.g. double-advance symbols, CJK via fallback) get scaled.
    for (int s = 0; s < FONT_STYLE_COUNT; s++)
        font_set_presentation_width(data->font, s, avail_w);

    // Emoji: prefer square aspect ratio (avail_h) but never exceed
    // the allocated cell space (columns_to_consume * cell_width).
    if (style == FONT_STYLE_EMOJI && avail_h < avail_w) {
        avail_w = avail_h;
    }

    void *font_data = data->font->font_data[style];
    bool color_baked = is_color_font(data->font, style);
    uint8_t render_r = color_baked ? r : 255;
    uint8_t render_g = color_baked ? g : 255;
    uint8_t render_b = color_baked ? b : 255;
    uint32_t color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                     : 0xFFFFFF;
    bool is_regional = (cp_count > 0 && is_regional_indicator(cps[0]));

    // For regional indicators, cache at square size for consistent high-quality scaling
    int cache_w = avail_w;
    int cache_h = avail_h;
    if (is_regional) {
        int side = avail_w < avail_h ? avail_w : avail_h;
        cache_w = cache_h = side;
    }

    // Shaped rendering path (multiple codepoints)
    if (cp_count > 1 && data->font->render_shaped) {
        ShapedGlyphs *shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                                       render_r, render_g, render_b);

        // Fallback: if shaped rendering fails with selected style, try NORMAL
        if (!shaped && style != FONT_STYLE_NORMAL) {
            style = FONT_STYLE_NORMAL;
            font_data = data->font->font_data[style];
            color_baked = is_color_font(data->font, style);
            render_r = color_baked ? r : 255;
            render_g = color_baked ? g : 255;
            render_b = color_baked ? b : 255;
            color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                    : 0xFFFFFF;
            shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                             render_r, render_g, render_b);
        }

        // Dynamic fallback: try fontconfig-resolved font for the first codepoint
        if (!shaped && cp_count > 0) {
            const char *fb_path = fallback_cache_lookup(data, cps[0]);
            if (fb_path && ensure_fallback_font(data, fb_path)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                // Lazily loaded — set presentation_width now
                font_set_presentation_width(data->font, style, avail_w);
                color_baked = is_color_font(data->font, style);
                render_r = color_baked ? r : 255;
                render_g = color_baked ? g : 255;
                render_b = color_baked ? b : 255;
                color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                        : 0xFFFFFF;
                shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                                 render_r, render_g, render_b);
            }
        }

        if (shaped) {
            for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                uint32_t gid = shaped->glyph_ids[gi];
                if (gid == 0)
                    continue;
                // Tag the gid so 1-cell and 2-cell rasters of the same glyph
                // get separate atlas entries (matches single-glyph path bit 29).
                uint32_t atlas_gid = (columns_to_consume >= 2) ? (gid | (1u << 29)) : gid;
                RendSdl3AtlasEntry *entry =
                    rend_sdl3_atlas_lookup(&data->atlas, font_data, atlas_gid, color_key);
                if (!entry) {
                    GlyphBitmap *gb = font_render_glyph_id(data->font, style, gid,
                                                           render_r, render_g, render_b);
                    if (gb) {
                        entry = cache_glyph(&data->atlas, font_data, atlas_gid, color_key,
                                            gb, emoji_render && color_baked,
                                            cache_w, cache_h);
                        data->font->free_glyph_bitmap(data->font, gb);
                    } else {
                        rend_sdl3_atlas_insert_empty(&data->atlas, font_data, atlas_gid, color_key);
                    }
                }
                if (!populate_only) {
                    int x_off = shaped->x_positions[gi] + (entry ? entry->x_offset : 0);
                    int y_off = entry ? entry->y_offset : 0;
                    blit_glyph(data->renderer, &data->atlas, entry,
                               cell_x, cell_y, x_off,
                               y_off, avail_w, avail_h, data->font_ascent,
                               is_regional, color_baked, r, g, b);
                }
            }
            free(shaped->glyph_ids);
            free(shaped->x_positions);
            free(shaped->y_positions);
            free(shaped->x_advances);
            free(shaped);
            goto render_cursor;
        }
    }

    // Single glyph fallback
    {
        uint32_t codepoint = cps[0];
        uint32_t glyph_index = font_get_glyph_index(data->font, style, codepoint);
        RendSdl3AtlasEntry *entry = NULL;

        // Fallback: if glyph not found in selected style, try NORMAL
        if (glyph_index == 0 && style != FONT_STYLE_NORMAL) {
            style = FONT_STYLE_NORMAL;
            font_data = data->font->font_data[style];
            color_baked = is_color_font(data->font, style);
            render_r = color_baked ? r : 255;
            render_g = color_baked ? g : 255;
            render_b = color_baked ? b : 255;
            color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                    : 0xFFFFFF;
            glyph_index = font_get_glyph_index(data->font, style, codepoint);
        }

        // Dynamic fallback: if still missing, query fontconfig for a covering font
        if (glyph_index == 0) {
            const char *fb_path = fallback_cache_lookup(data, codepoint);
            if (fb_path && ensure_fallback_font(data, fb_path)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                // Lazily loaded — set presentation_width now
                font_set_presentation_width(data->font, style, avail_w);
                color_baked = is_color_font(data->font, style);
                render_r = color_baked ? r : 255;
                render_g = color_baked ? g : 255;
                render_b = color_baked ? b : 255;
                color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                        : 0xFFFFFF;
                glyph_index = font_get_glyph_index(data->font, style, codepoint);
            }
        }

        // Tag glyph_index so glyphs at different presentation widths
        // get separate atlas entries (different rasterization sizes).
        uint32_t atlas_glyph_id = glyph_index;
        if (columns_to_consume >= 2 && atlas_glyph_id != 0)
            atlas_glyph_id |= (1u << 29);

        if (atlas_glyph_id != 0)
            entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, atlas_glyph_id, color_key);
        if (!entry) {
            GlyphBitmap *glyph_bitmap = font_render_glyphs(data->font, style, &codepoint, 1,
                                                           render_r, render_g, render_b);
            if (glyph_bitmap) {
                uint32_t insert_id = atlas_glyph_id ? atlas_glyph_id
                                                    : (uint32_t)glyph_bitmap->glyph_id;
                entry = cache_glyph(&data->atlas, font_data, insert_id, color_key,
                                    glyph_bitmap, emoji_render && color_baked,
                                    cache_w, cache_h);
                data->font->free_glyph_bitmap(data->font, glyph_bitmap);
            } else if (atlas_glyph_id != 0) {
                rend_sdl3_atlas_insert_empty(&data->atlas, font_data, atlas_glyph_id, color_key);
            }
        }
        if (!populate_only)
            blit_glyph(data->renderer, &data->atlas, entry,
                       cell_x, cell_y,
                       entry ? entry->x_offset : 0, entry ? entry->y_offset : 0,
                       avail_w, avail_h, data->font_ascent, is_regional,
                       color_baked, r, g, b);
    }

render_cursor:
    if (!populate_only && show_cursor && row == cursor_pos.row && vt_col == cursor_pos.col) {
        float cx = (float)(vis_col * data->cell_width);
        float cy = (float)(row * data->cell_height);
        float cw = (float)data->cell_width;
        float ch = (float)data->cell_height;

        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(data->renderer, CURSOR_COLOR_R, CURSOR_COLOR_G,
                               CURSOR_COLOR_B, CURSOR_COLOR_A);
        draw_rounded_rect(data->renderer, cx, cy, cw, ch, 2.0f);
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
    }

    // Selection highlight
    if (!populate_only) {
        int scroll_offset = data->scroll_offset;
        int scrollback_row = scroll_offset - 1 - row;
        int unified_row = (scrollback_row >= 0) ? -(scrollback_row + 1) : (row - scroll_offset);
        if (terminal_cell_in_selection(term, unified_row, vt_col)) {
            float sx = (float)(vis_col * data->cell_width);
            float sy = (float)(row * data->cell_height);
            float sw = (float)(columns_to_consume * data->cell_width);
            float sh = (float)data->cell_height;

            SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(data->renderer, SELECTION_COLOR_R, SELECTION_COLOR_G,
                                   SELECTION_COLOR_B, SELECTION_COLOR_A);
            SDL_FRect sel_rect = { sx, sy, sw, sh };
            SDL_RenderFillRect(data->renderer, &sel_rect);
            SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
        }
    }
}

static void render_visible_cells(RendererSdl3Data *data, TerminalBackend *term,
                                 int display_rows, int display_cols,
                                 bool cursor_visible, bool populate_only)
{
    TerminalPos cursor_pos = terminal_get_cursor_pos(term);
    // Hide cursor when scrolled back, when terminal says it's not visible, when cursor_visible is false,
    // or when cursor is outside visible bounds (can happen during resize before shell repositions cursor)
    bool cursor_in_bounds = cursor_pos.row >= 0 && cursor_pos.row < display_rows &&
                            cursor_pos.col >= 0 && cursor_pos.col < display_cols;
    bool show_cursor = cursor_visible && cursor_in_bounds &&
                       (data->scroll_offset == 0) && terminal_get_cursor_visible(term);

    for (int row = 0; row < display_rows; row++) {
        int unified_row = row - data->scroll_offset;
        TerminalRowIter it;

        // Pass 1: draw all cell backgrounds for this row
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            while (terminal_row_iter_next(&it)) {
                render_cell_bg(data, row, it.vis_col, it.pres_w, &it.cell);
            }
        }
        // Pass 2: draw glyphs, cursors, and selection overlays
        terminal_row_iter_init(&it, term, unified_row, display_cols);
        while (terminal_row_iter_next(&it)) {
            render_cell(data, term, row, it.vt_col, it.vis_col, it.pres_w,
                        &it.cell, cursor_pos, show_cursor, populate_only);
        }
        // Pass 3: draw underlines as continuous spans across consecutive cells
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            int vis_run_start = -1;
            int vis_run_end = 0;
            unsigned int run_style = 0;
            Uint8 run_r = 0, run_g = 0, run_b = 0;
            while (terminal_row_iter_next(&it)) {
                unsigned int cs = it.cell.attrs.underline;
                Uint8 cr = it.cell.ul_color.is_default ? UNDERLINE_COLOR_R : it.cell.ul_color.r;
                Uint8 cg = it.cell.ul_color.is_default ? UNDERLINE_COLOR_G : it.cell.ul_color.g;
                Uint8 cb = it.cell.ul_color.is_default ? UNDERLINE_COLOR_B : it.cell.ul_color.b;
                bool same_run = (run_style != 0 && cs == run_style && cr == run_r &&
                                 cg == run_g && cb == run_b);
                if (run_style != 0 && !same_run) {
                    // Flush current run
                    float pd = data->content_scale;
                    int thickness = (int)roundf(1.0f * pd);
                    if (thickness < 1)
                        thickness = 1;
                    int cell_y = row * data->cell_height;
                    int underline_y = cell_y + data->font_ascent + (int)roundf(2.0f * pd);
                    if (underline_y + thickness > cell_y + data->cell_height)
                        underline_y = cell_y + data->cell_height - thickness;
                    int run_x = vis_run_start * data->cell_width;
                    int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                    SDL_SetRenderDrawColor(data->renderer, run_r, run_g, run_b,
                                           UNDERLINE_COLOR_A);
                    switch (run_style) {
                    case 1:
                        draw_underline_single(data->renderer, run_x, underline_y, run_w, pd);
                        break;
                    case 2:
                        draw_underline_double(data->renderer, run_x, underline_y, run_w, pd);
                        break;
                    case 3:
                        draw_underline_curly(data->renderer, run_x, underline_y, run_w, pd,
                                             run_r, run_g, run_b);
                        break;
                    case 4:
                        draw_underline_dotted(data->renderer, run_x, underline_y, run_w, pd);
                        break;
                    case 5:
                        draw_underline_dashed(data->renderer, run_x, underline_y, run_w, pd);
                        break;
                    }
                    run_style = 0;
                }
                if (cs != 0 && run_style == 0) {
                    vis_run_start = it.vis_col;
                    run_style = cs;
                    run_r = cr;
                    run_g = cg;
                    run_b = cb;
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (run_style != 0) {
                // Flush final run
                float pd = data->content_scale;
                int thickness = (int)roundf(1.0f * pd);
                if (thickness < 1)
                    thickness = 1;
                int cell_y = row * data->cell_height;
                int underline_y = cell_y + data->font_ascent + (int)roundf(2.0f * pd);
                if (underline_y + thickness > cell_y + data->cell_height)
                    underline_y = cell_y + data->cell_height - thickness;
                int run_x = vis_run_start * data->cell_width;
                int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                SDL_SetRenderDrawColor(data->renderer, run_r, run_g, run_b, UNDERLINE_COLOR_A);
                switch (run_style) {
                case 1:
                    draw_underline_single(data->renderer, run_x, underline_y, run_w, pd);
                    break;
                case 2:
                    draw_underline_double(data->renderer, run_x, underline_y, run_w, pd);
                    break;
                case 3:
                    draw_underline_curly(data->renderer, run_x, underline_y, run_w, pd, run_r,
                                         run_g, run_b);
                    break;
                case 4:
                    draw_underline_dotted(data->renderer, run_x, underline_y, run_w, pd);
                    break;
                case 5:
                    draw_underline_dashed(data->renderer, run_x, underline_y, run_w, pd);
                    break;
                }
            }
        }
        // Pass 3b: draw OSC-8 hyperlink underlines. Coalesce by hyperlink_id
        // so a single link with adjacent cells sharing the same id renders
        // as one continuous run. Idle is a faint dotted Charm-purple line
        // sitting one pixel below the SGR underline slot; hover (when the
        // cell's id matches the terminal-tracked hovered_hyperlink_id)
        // upgrades the same span to a solid full-alpha line.
        if (!populate_only) {
            uint16_t hovered = terminal_hovered_hyperlink(term);
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            int vis_run_start = -1;
            int vis_run_end = 0;
            uint16_t run_id = 0;
            while (terminal_row_iter_next(&it)) {
                uint16_t hid = it.cell.hyperlink_id;
                bool same_run = (run_id != 0 && hid == run_id);
                if (run_id != 0 && !same_run) {
                    // Flush current run
                    float pd = data->content_scale;
                    int thickness = (int)roundf(1.0f * pd);
                    if (thickness < 1)
                        thickness = 1;
                    int cell_y = row * data->cell_height;
                    int link_y = cell_y + data->font_ascent + (int)roundf(3.0f * pd);
                    if (link_y + thickness > cell_y + data->cell_height)
                        link_y = cell_y + data->cell_height - thickness;
                    int run_x = vis_run_start * data->cell_width;
                    int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                    bool hover = (run_id == hovered);
                    Uint8 a = hover ? HYPERLINK_HOVER_ALPHA : HYPERLINK_IDLE_ALPHA;
                    SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(data->renderer, HYPERLINK_COLOR_R,
                                           HYPERLINK_COLOR_G, HYPERLINK_COLOR_B, a);
                    if (hover)
                        draw_underline_single(data->renderer, run_x, link_y, run_w, pd);
                    else
                        draw_underline_dotted(data->renderer, run_x, link_y, run_w, pd);
                    SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
                    run_id = 0;
                }
                if (hid != 0 && run_id == 0) {
                    vis_run_start = it.vis_col;
                    run_id = hid;
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (run_id != 0) {
                float pd = data->content_scale;
                int thickness = (int)roundf(1.0f * pd);
                if (thickness < 1)
                    thickness = 1;
                int cell_y = row * data->cell_height;
                int link_y = cell_y + data->font_ascent + (int)roundf(3.0f * pd);
                if (link_y + thickness > cell_y + data->cell_height)
                    link_y = cell_y + data->cell_height - thickness;
                int run_x = vis_run_start * data->cell_width;
                int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                bool hover = (run_id == hovered);
                Uint8 a = hover ? HYPERLINK_HOVER_ALPHA : HYPERLINK_IDLE_ALPHA;
                SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(data->renderer, HYPERLINK_COLOR_R,
                                       HYPERLINK_COLOR_G, HYPERLINK_COLOR_B, a);
                if (hover)
                    draw_underline_single(data->renderer, run_x, link_y, run_w, pd);
                else
                    draw_underline_dotted(data->renderer, run_x, link_y, run_w, pd);
                SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
            }
        }

        // Pass 4: draw strikethroughs as continuous spans across consecutive cells
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            int vis_run_start = -1;
            int vis_run_end = 0;
            bool in_run = false;
            Uint8 run_r = 0, run_g = 0, run_b = 0;
            while (terminal_row_iter_next(&it)) {
                bool cs = it.cell.attrs.strikethrough;
                Uint8 cr = it.cell.fg.r, cg = it.cell.fg.g, cb = it.cell.fg.b;
                bool same_run = in_run && cs && cr == run_r && cg == run_g && cb == run_b;
                if (in_run && !same_run) {
                    // Flush current run
                    float pd = data->content_scale;
                    int cell_y = row * data->cell_height;
                    int strike_y = cell_y + data->font_ascent - data->font_cap_height / 2;
                    int run_x = vis_run_start * data->cell_width;
                    int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                    SDL_SetRenderDrawColor(data->renderer, run_r, run_g, run_b, 255);
                    draw_strikethrough(data->renderer, run_x, strike_y, run_w, pd);
                    in_run = false;
                }
                if (cs && !in_run) {
                    vis_run_start = it.vis_col;
                    in_run = true;
                    run_r = cr;
                    run_g = cg;
                    run_b = cb;
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (in_run) {
                float pd = data->content_scale;
                int cell_y = row * data->cell_height;
                int strike_y = cell_y + data->font_ascent - data->font_cap_height / 2;
                int run_x = vis_run_start * data->cell_width;
                int run_w = (vis_run_end - vis_run_start) * data->cell_width;
                SDL_SetRenderDrawColor(data->renderer, run_r, run_g, run_b, 255);
                draw_strikethrough(data->renderer, run_x, strike_y, run_w, pd);
            }
        }
    }
}

// Destroy all cached sixel textures
static void sixel_cache_clear(RendererSdl3Data *data)
{
    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].texture)
            SDL_DestroyTexture(data->sixel_cache[i].texture);
        data->sixel_cache[i].texture = NULL;
        data->sixel_cache[i].source = NULL;
    }
    data->sixel_cache_count = 0;
}

// Find or create an SDL_Texture for a sixel image (by pointer identity)
static SDL_Texture *sixel_get_texture(RendererSdl3Data *data, SixelImage *img)
{
    // Check cache
    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].source == img)
            return data->sixel_cache[i].texture;
    }

    // Create new texture
    SDL_Texture *tex =
        SDL_CreateTexture(data->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                          img->width, img->height);
    if (!tex)
        return NULL;

    SDL_UpdateTexture(tex, NULL, img->pixels, img->width * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    if (data->sixel_cache_count < TERM_MAX_SIXEL_IMAGES) {
        data->sixel_cache[data->sixel_cache_count].texture = tex;
        data->sixel_cache[data->sixel_cache_count].source = img;
        data->sixel_cache_count++;
    }

    return tex;
}

// Render sixel images overlaid on the terminal
static void render_sixel_images(RendererSdl3Data *data, TerminalBackend *term)
{
    if (term->sixel_image_count == 0)
        return;

    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);
    (void)term_cols;

    for (int i = 0; i < term->sixel_image_count; i++) {
        SixelImage *img = term->sixel_images[i];
        if (!img || !img->valid)
            continue;

        // Compute screen position accounting for scroll offset
        int screen_row = img->cursor_row + data->scroll_offset;

        // Pixel position on screen
        int px = img->cursor_col * data->cell_width;
        int py = screen_row * data->cell_height;

        // Skip if completely off-screen
        if (py + img->height <= 0)
            continue;
        if (py >= data->height)
            continue;
        if (px + img->width <= 0)
            continue;
        if (px >= data->width)
            continue;

        SDL_Texture *tex = sixel_get_texture(data, img);
        if (!tex)
            continue;

        SDL_FRect dst = { (float)px, (float)py, (float)img->width, (float)img->height };
        SDL_RenderTexture(data->renderer, tex, NULL, &dst);
    }
}

static void sdl3_draw_terminal(RendererBackend *backend, TerminalBackend *term,
                               bool cursor_visible)
{
    if (!backend || !backend->backend_data || !term)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    if (!font_has_style(data->font, FONT_STYLE_NORMAL)) {
        vlog("Renderer draw terminal failed: invalid parameters\n");
        return;
    }

    rend_sdl3_atlas_begin_frame(&data->atlas);

    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);
    int display_rows = data->height / data->cell_height;
    int display_cols = data->width / data->cell_width;
    if (display_rows > term_rows)
        display_rows = term_rows;
    if (display_cols > term_cols)
        display_cols = term_cols;

    // Two-phase render: populate atlas first (no draw calls), then flush
    // staging buffers to GPU while the render queue is empty, then draw.
    // This avoids the implicit render queue flush inside SDL_UpdateTexture
    // interfering with in-flight draw commands.

    // Phase 1: Populate atlas (insert missing glyphs, no draws)
    data->atlas.eviction_occurred = false;
    render_visible_cells(data, term, display_rows, display_cols, cursor_visible, true);

    // If eviction occurred during Phase 1, flush the partial staging data and
    // re-populate so glyphs that were destroyed by eviction get re-rasterized
    // into staging before the final flush uploads to GPU.
    if (data->atlas.eviction_occurred) {
        rend_sdl3_atlas_flush(&data->atlas);
        data->atlas.eviction_occurred = false;
        render_visible_cells(data, term, display_rows, display_cols, cursor_visible, true);
    }

    // Phase 2: Flush staging buffers to GPU (render queue is empty)
    rend_sdl3_atlas_flush(&data->atlas);

    // Phase 3: Draw (all glyphs cached, texture data is current)
    SDL_SetRenderDrawColor(data->renderer, 0x00, 0x00, 0x00, 255);
    SDL_RenderClear(data->renderer);
    render_visible_cells(data, term, display_rows, display_cols, cursor_visible, false);

    // Phase 4: Overlay sixel images
    render_sixel_images(data, term);
}

static void sdl3_present(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    SDL_RenderPresent(data->renderer);
}

static void sdl3_log_stats(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    rend_sdl3_atlas_log_stats(&data->atlas);
}

static void sdl3_resize(RendererBackend *backend, int width, int height)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    data->width = width;
    data->height = height;
}

static bool sdl3_get_cell_size(RendererBackend *backend, int *cell_width, int *cell_height)
{
    if (!backend || !backend->backend_data)
        return false;
    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (data->cell_width <= 0 || data->cell_height <= 0)
        return false;
    *cell_width = data->cell_width;
    *cell_height = data->cell_height;
    return true;
}

static void sdl3_scroll(RendererBackend *backend, TerminalBackend *term, int delta)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    int scrollback_lines = terminal_get_scrollback_lines(term);

    int new_offset = data->scroll_offset + delta;
    if (new_offset < 0)
        new_offset = 0;
    if (new_offset > scrollback_lines)
        new_offset = scrollback_lines;

    if (new_offset != data->scroll_offset) {
        data->scroll_offset = new_offset;
        vlog("Scroll offset changed to %d (max: %d)\n", data->scroll_offset, scrollback_lines);
    }
}

static void sdl3_reset_scroll(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (data->scroll_offset != 0) {
        data->scroll_offset = 0;
        vlog("Scroll offset reset to 0\n");
    }
}

static int sdl3_get_scroll_offset(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return 0;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    return data->scroll_offset;
}

static int sdl3_render_to_png(RendererBackend *backend, TerminalBackend *term,
                              const char *output_path)
{
    if (!backend || !backend->backend_data || !term || !output_path)
        return -1;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    if (!font_has_style(data->font, FONT_STYLE_NORMAL)) {
        fprintf(stderr, "ERROR: No font loaded for PNG render\n");
        return -1;
    }

    // Get terminal dimensions
    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);

    // Find the rightmost non-empty column across all rows so multi-row
    // outputs (e.g. -P --exec) aren't trimmed to row 0's width.
    int last_col = 0;
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            TerminalCell cell;
            if (terminal_get_cell(term, row, col, &cell) == 0 && cell.cp != 0) {
                int end = col + (cell.width > 0 ? cell.width : 1);
                if (end > last_col)
                    last_col = end;
            }
        }
    }
    if (last_col <= 0)
        last_col = 1;

    int render_cols = last_col;
    int render_rows = term_rows;

    int img_w = render_cols * data->cell_width;
    int img_h = render_rows * data->cell_height;

    vlog("PNG render: %d cols x %d rows = %dx%d pixels\n",
         render_cols, render_rows, img_w, img_h);

    // Create offscreen render target texture
    SDL_Texture *target = SDL_CreateTexture(data->renderer,
                                            SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET,
                                            img_w, img_h);
    if (!target) {
        fprintf(stderr, "ERROR: Failed to create offscreen texture: %s\n", SDL_GetError());
        return -1;
    }

    // Redirect rendering to offscreen texture
    if (!SDL_SetRenderTarget(data->renderer, target)) {
        fprintf(stderr, "ERROR: Failed to set render target: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    rend_sdl3_atlas_begin_frame(&data->atlas);

    // Phase 1: Populate atlas (no draw calls)
    data->atlas.eviction_occurred = false;
    render_visible_cells(data, term, render_rows, render_cols, false, true);

    // If eviction occurred during Phase 1, flush and re-populate
    if (data->atlas.eviction_occurred) {
        rend_sdl3_atlas_flush(&data->atlas);
        data->atlas.eviction_occurred = false;
        render_visible_cells(data, term, render_rows, render_cols, false, true);
    }

    // Phase 2: Flush staging buffers to GPU
    rend_sdl3_atlas_flush(&data->atlas);

    // Phase 3: Clear and draw
    SDL_SetRenderDrawColor(data->renderer, 0x00, 0x00, 0x00, 255);
    SDL_RenderClear(data->renderer);
    render_visible_cells(data, term, render_rows, render_cols, false, false);

    // Read pixels back from the render target
    SDL_Surface *surface = SDL_RenderReadPixels(data->renderer, NULL);

    // Restore default render target
    SDL_SetRenderTarget(data->renderer, NULL);

    if (!surface) {
        fprintf(stderr, "ERROR: Failed to read pixels: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    // Convert surface to RGBA32 if needed
    SDL_Surface *rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);

    if (!rgba_surface) {
        fprintf(stderr, "ERROR: Failed to convert surface to RGBA: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    // Write PNG
    int rc = png_write_rgba(output_path, (const uint8_t *)rgba_surface->pixels,
                            rgba_surface->w, rgba_surface->h);

    SDL_DestroySurface(rgba_surface);
    SDL_DestroyTexture(target);

    if (rc == 0) {
        fprintf(stderr, "STATUS: png_output=%s (%dx%d)\n", output_path, img_w, img_h);
    } else {
        fprintf(stderr, "ERROR: Failed to write PNG to %s\n", output_path);
    }

    return rc;
}

static void sdl3_set_content_scale(RendererBackend *backend, float scale)
{
    if (!backend || !backend->backend_data)
        return;
    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (scale > 0.0f) {
        data->content_scale = scale;
        vlog("Content scale set to %.2f\n", scale);
    }
}

// SDL3 renderer backend instance
RendererBackend renderer_backend_sdl3 = {
    .name = "sdl3",
    .backend_data = NULL,
    .init = sdl3_init,
    .destroy = sdl3_destroy,
    .load_fonts = sdl3_load_fonts,
    .draw_terminal = sdl3_draw_terminal,
    .present = sdl3_present,
    .resize = sdl3_resize,
    .log_stats = sdl3_log_stats,
    .get_cell_size = sdl3_get_cell_size,
    .scroll = sdl3_scroll,
    .reset_scroll = sdl3_reset_scroll,
    .get_scroll_offset = sdl3_get_scroll_offset,
    .render_to_png = sdl3_render_to_png,
    .set_content_scale = sdl3_set_content_scale,
};
