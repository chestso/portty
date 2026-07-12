#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "common.h"
#include "diag.h"
#include "font_ft_internal.h"
#include "font_resolve.h"
#include "portty_conf.h"
#include "portty_pty.h"
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
#include "pager.h"
#include "platform.h"
#include "platform_sdl3.h"
#include "png_mode.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_cfr.h"
#include <SDL3/SDL.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else

#include <unistd.h>
#endif

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;
float portty_text_gamma = 1.0f;               /* neutral (pure linear-correct) */
float portty_text_contrast = 0.0f;            /* neutral */
bool portty_notification_transparent = false; /* opaque notification panel by default */

/* ASan/UBSan runtime defaults. Only compiled in when the binary is
 * built with -fsanitize=address. detect_leaks=0 silences Mesa exit
 * leaks; abort_on_error=1 produces a core file via systemd-coredump so
 * the trace survives the GUI shutdown; log_path writes reports to
 * /tmp/portty-asan.<pid> in case stderr is lost.
 */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
const char *__asan_default_options(void)
{
    return "abort_on_error=1:disable_coredump=0:detect_leaks=0:"
           "log_path=/tmp/portty-asan:print_module_map=1";
}
const char *__ubsan_default_options(void)
{
    return "abort_on_error=1:print_stacktrace=1:log_path=/tmp/portty-ubsan";
}
#endif

// Function prototypes
static void print_usage(const char *progname);
static void print_version(void);

// Context passed to platform callbacks
typedef struct
{
    TerminalBackend *term;
    RendererBackend *rend;
    PtyContext *pty;
    PlatformBackend *plat;
    const PorttyConf *conf;  // loaded config, for the diagnostics report
    const char *font_source; // where the effective font came from (diagnostics)
    Pager *pager;            // internal pager overlay (diagnostics report, etc.)
    bool drag_pending;
    int drag_start_row; // unified row
    int drag_start_col; // display col
    // Drag-autoscroll: last raw mouse pixel pos while a selection drag is
    // active; the autoscroll tick reads this to know which direction to
    // scroll and where to re-extend the selection.
    int drag_last_pixel_x;
    int drag_last_pixel_y;
    bool autoscroll_active;
    // +1 = scroll down (pointer above the top edge), -1 = scroll up
    // (pointer below the bottom edge), 0 = inactive. Stored explicitly so
    // the autoscroll tick doesn't need to re-derive it from pixel coords
    // that may be clamped to the viewport boundary (e.g. on Wayland where
    // SDL_CaptureMouse is a no-op and the last motion event's coords are
    // at the edge but still inside the viewport).
    int autoscroll_direction;
    // True while the pointer is outside the window (between MOUSE_LEAVE and
    // MOUSE_ENTER). When set, the motion handler skips autoscroll updates
    // because no motion events arrive — the autoscroll timer owns scrolling.
    bool pointer_outside;
    // True while the left mouse button is held down (press → release).
    // Used to distinguish an active drag from a lingering selection that
    // was already released — on_mouse_leave must only start autoscroll
    // when a drag is in progress, not merely when the selection is active.
    bool drag_in_progress;
} MainContext;

// Convert pixel coordinates to display row/col
static bool pixel_to_cell(MainContext *ctx, int pixel_x, int pixel_y, int *out_row, int *out_col)
{
    int cell_w, cell_h;
    if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return false;
    *out_col = pixel_x / cell_w;
    *out_row = pixel_y / cell_h;
    return true;
}

// Convert display row to unified row (scrollback rows are negative)
static int display_row_to_unified(RendererBackend *rend, int display_row)
{
    int scroll_offset = renderer_get_scroll_offset(rend);
    int scrollback_row = scroll_offset - 1 - display_row;
    if (scrollback_row >= 0) {
        return -(scrollback_row + 1);
    } else {
        return display_row - scroll_offset;
    }
}

// Selection change callback — pauses/resumes PTY during selection
static void on_selection_change(bool active, void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    if (active)
        platform_pause_pty(ctx->plat);
    else
        platform_resume_pty(ctx->plat);
}

// Enable / disable the platform drag-autoscroll tick, mirroring local state.
// When disabling, clears the scroll direction. When enabling, the caller must
// set ctx->autoscroll_direction beforehand.
static void set_autoscroll(MainContext *ctx, bool enabled)
{
    if (ctx->autoscroll_active == enabled)
        return;
    ctx->autoscroll_active = enabled;
    if (!enabled)
        ctx->autoscroll_direction = 0;
    platform_set_autoscroll(ctx->plat, enabled);
}

// Re-extend the active selection to a raw pixel position (clamped into the
// viewport). Shared by the motion handler and the autoscroll tick.
static void extend_selection_from_pixel(MainContext *ctx, int pixel_x, int pixel_y)
{
    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return;
    int viewport_w = term_cols * cell_w;
    int viewport_h = term_rows * cell_h;
    if (pixel_x < 0)
        pixel_x = 0;
    if (pixel_x >= viewport_w)
        pixel_x = viewport_w - 1;
    if (pixel_y < 0)
        pixel_y = 0;
    if (pixel_y >= viewport_h)
        pixel_y = viewport_h - 1;

    int display_row, display_col;
    if (!pixel_to_cell(ctx, pixel_x, pixel_y, &display_row, &display_col))
        return;
    if (display_row >= term_rows)
        display_row = term_rows - 1;
    if (display_row < 0)
        display_row = 0;
    if (display_col < 0)
        display_col = 0;
    int unified_row = display_row_to_unified(ctx->rend, display_row);
    display_col = terminal_vis_col_to_vt_col(ctx->term, unified_row, display_col);
    if (display_col >= term_cols)
        display_col = term_cols - 1;
    if (display_col < 0)
        display_col = 0;
    terminal_selection_update(ctx->term, unified_row, display_col);
}

// Drag-autoscroll tick — invoked by the platform timer while the user is
// holding a selection drag past the top/bottom edge of the viewport.
// Uses the stored autoscroll_direction (set when autoscroll was started)
// rather than re-deriving from drag_last_pixel_y, which may be clamped to
// the viewport boundary on platforms where SDL_CaptureMouse is a no-op.
static void on_autoscroll_tick(void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    if (!terminal_selection_active(ctx->term)) {
        set_autoscroll(ctx, false);
        return;
    }

    int dir = ctx->autoscroll_direction;
    if (dir == 0) {
        set_autoscroll(ctx, false);
        return;
    }

    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return;
    (void)term_cols;
    int viewport_h = term_rows * cell_h;
    int py = ctx->drag_last_pixel_y;

    int delta;
    if (dir > 0) {
        // Scroll down (pointer above the top edge)
        int overshoot = (py < 0) ? -py : 0;
        delta = 1 + overshoot / cell_h;
        if (delta > 5)
            delta = 5;
    } else {
        // Scroll up (pointer below the bottom edge)
        int overshoot = (py >= viewport_h) ? (py - (viewport_h - 1)) : 0;
        delta = -(1 + overshoot / cell_h);
        if (delta < -5)
            delta = -5;
    }

    renderer_scroll(ctx->rend, ctx->term, delta);
    extend_selection_from_pixel(ctx, ctx->drag_last_pixel_x, ctx->drag_last_pixel_y);
}

// Mouse left the window during a drag — start autoscroll if a drag is
// in progress and the pointer exited through the top or bottom edge. On
// Wayland, SDL_CaptureMouse is a no-op, so this is the only signal we get
// that the pointer has exited while the user is dragging.
static void on_mouse_leave(void *user_data, int pixel_x, int pixel_y)
{
    MainContext *ctx = (MainContext *)user_data;
    ctx->pointer_outside = true;

    if (!ctx->drag_in_progress)
        return;

    ctx->drag_last_pixel_x = pixel_x;
    ctx->drag_last_pixel_y = pixel_y;

    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_h <= 0)
        return;
    (void)term_cols;
    int viewport_h = term_rows * cell_h;

    // The last in-window position tells us which edge the pointer crossed.
    // If it was near the top, the user is dragging upward → scroll down.
    // If near the bottom, dragging downward → scroll up. If the pointer
    // exited through the left/right edges, no vertical autoscroll is needed.
    if (pixel_y < cell_h) {
        ctx->autoscroll_direction = 1;
        set_autoscroll(ctx, true);
    } else if (pixel_y >= viewport_h - cell_h) {
        ctx->autoscroll_direction = -1;
        set_autoscroll(ctx, true);
    }
}

// Mouse re-entered the window — stop autoscroll. Motion events will resume
// driving selection updates normally. On Wayland, if the user released the
// button while outside, the platform backend synthesizes a BUTTON_UP event
// (since SDL_CaptureMouse is a no-op and no real BUTTON_UP was delivered),
// which clears drag_in_progress via the normal on_mouse path.
static void on_mouse_enter(void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    ctx->pointer_outside = false;
    set_autoscroll(ctx, false);
}

// OSC 52 set-clipboard callback. SDL/GDK clipboard APIs take a NUL-
// terminated UTF-8 string; an interior NUL inside the decoded payload
// would silently truncate, so we rely on that here too.
static void on_clipboard_set(const char *text, size_t len, void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    char *buf = malloc(len + 1);
    if (!buf)
        return;
    if (len)
        memcpy(buf, text, len);
    buf[len] = '\0';
    platform_clipboard_set(ctx->plat, buf);
    free(buf);
}

// OSC 7 / OSC 9;9 working-directory callback. The shell emits these
// sequences on every directory change so the terminal can track CWD
// for Ctrl+Shift+N (spawn new terminal in same directory).
static void on_cwd_change(const char *dir, void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    platform_set_working_dir(ctx->plat, dir);
}

// Build the diagnostics document and show it in the internal pager. Rendered by
// portty itself (not an external pager), so OSC 8 links stay clickable
// and nothing is injected into the shell.
static void show_diagnostics_report(MainContext *ctx)
{
    const PorttyConf *c = ctx->conf;
    RendererDiag rd = { 0 };
    renderer_get_diag(ctx->rend, &rd);

    int rows = 0, cols = 0;
    terminal_get_dimensions(ctx->term, &rows, &cols);

    // GPU + driver, reported via the renderer (SDL_GetGPUDeviceProperties).
    const char *gpu_device = NULL, *gpu_driver = NULL;
    bool gpu_libre = false;
    if (!platform_get_gpu_info(ctx->plat, &gpu_device, &gpu_driver, &gpu_libre)) {
        gpu_device = rd.gpu_device;
        gpu_driver = rd.gpu_driver;
        gpu_libre = rd.gpu_driver_libre;
    }

    DiagSources src = {
        .renderer_name = rd.renderer_name,
        .gpu_device = gpu_device,
        .gpu_driver = gpu_driver,
        .gpu_driver_libre = gpu_libre,
        .linear_light = rd.linear_light,
        .glyph_shader = rd.glyph_shader,
        .content_scale = rd.content_scale,
        .pixel_width = rd.pixel_width,
        .pixel_height = rd.pixel_height,
        .cell_width = rd.cell_width,
        .cell_height = rd.cell_height,
        .cols = cols,
        .rows = rows,
        .config_path = c ? c->source_path : NULL,
        .font_pattern = c ? c->font : NULL,
        .font_path = rd.font_path,
        .font_source = ctx->font_source,
        // Effective hinting from the renderer (what fonts were actually loaded
        // with), not the raw config value — so the default resolves to "light"
        // rather than "(default)".
        .hinting = rd.hinting,
        .scrollback = terminal_get_scrollback_capacity(ctx->term),
        .text_gamma = portty_text_gamma,
        .text_contrast = portty_text_contrast,
        .word_chars = c ? c->word_chars : NULL,
        .platform_name = platform_get_name(ctx->plat),
        // TERM/COLORTERM are what pty.c advertises to the shell (set post-fork,
        // so not visible via the host's getenv) — keep in sync with pty.c.
        .term_env = "portty-vty-256color",
        .colorterm_env = "truecolor",
        .lang_env = getenv("LANG"),
        .title = terminal_get_title(ctx->term),
        .altscreen = terminal_is_altscreen(ctx->term),
        .mouse_mode = terminal_get_mouse_mode(ctx->term),
        // VT engine features — runtime modes are queried from coffer so
        // the report reflects live state. Always-on capabilities (sixel,
        // OSC 8, grapheme clusters, reflow) are listed by diag.c.
        .vt_backend = ctx->term->name,
        .lottie_rasterizer = cfr_have_lottie(),
        .osc52 = ctx->term->clipboard_set_cb != NULL,
        .bracketed_paste = terminal_get_mode(ctx->term, CFR_MODE_BRACKETED_PASTE),
        .sync_output = terminal_get_mode(ctx->term, CFR_MODE_SYNC_OUTPUT),
        .focus_reporting = terminal_get_mode(ctx->term, CFR_MODE_FOCUS_REPORTING),
        .sixel_scrolling = terminal_get_mode(ctx->term, CFR_MODE_SIXEL_SCROLLING),
#ifdef PORTTY_HARDEN_HEAP
        .hardened_heap = true,
#else
        .hardened_heap = false,
#endif
    };

    char *report = diag_build_report(&src);
    if (!report)
        return;

    // Show the report in the internal pager (it copies the text). The pager
    // pauses the PTY for the duration and renders the document itself, so the
    // clickable OSC 8 issues link survives regardless of the user's $PAGER.
    if (cols > 0 && rows > 0)
        pager_open(ctx->pager, report, cols, rows);
    free(report);
}

// Key callback — receives TERM_KEY_* and TERM_MOD_* (platform-independent)
static KeyboardResult on_key(void *user_data, int key, int mod,
                             uint32_t codepoint)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    // The internal pager is modal: while open it consumes every keystroke
    // (scroll/close) so nothing reaches the shell. Closing keys (q/Esc) make it
    // inactive here, so the next keystroke flows normally.
    if (pager_active(ctx->pager)) {
        result.handled = pager_key(ctx->pager, key, mod, codepoint);
        return result;
    }

    // Ctrl+C with active selection → copy and cancel (not SIGINT)
    if (codepoint == 'c' && (mod & TERM_MOD_CTRL) && !(mod & TERM_MOD_SHIFT) &&
        terminal_selection_active(ctx->term)) {
        char *text = terminal_selection_get_text(ctx->term);
        if (text) {
            platform_clipboard_set(ctx->plat, text);
            free(text);
        }
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+C → copy
    if (key == TERM_KEY_NONE && codepoint == 'C' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                platform_clipboard_set(ctx->plat, text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
            result.force_redraw = true;
        }
        result.handled = true;
        return result;
    }
    // Any key with active selection → cancel selection
    if (terminal_selection_active(ctx->term)) {
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
    }

    // Ctrl+Shift+V → paste
    if (key == TERM_KEY_NONE && codepoint == 'V' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        // Paste synchronously
        if (!platform_clipboard_paste_async(ctx->plat, ctx->term, ctx->pty)) {
            char *clipboard = platform_clipboard_get(ctx->plat);
            if (clipboard && clipboard[0] != '\0')
                platform_paste_text(ctx->term, ctx->pty, clipboard, strlen(clipboard));
            platform_clipboard_free(ctx->plat, clipboard);
        }
        result.handled = true;
        return result;
    }

    // Shift+PageUp/Down: scrollback navigation (normal screen only)
    if ((mod & TERM_MOD_SHIFT) && !terminal_is_altscreen(ctx->term)) {
        if (key == TERM_KEY_PAGEUP || key == TERM_KEY_PAGEDOWN) {
            int rows, cols;
            terminal_get_dimensions(ctx->term, &rows, &cols);
            renderer_scroll(ctx->rend, ctx->term, key == TERM_KEY_PAGEUP ? rows : -rows);
            result.force_redraw = true;
            result.handled = true;
            return result;
        }
    }

    // Ctrl+Shift+F6 → diagnostics report in the internal pager (mirrors kitty's
    // debug_config). The pager is a self-contained modal overlay that pauses the
    // PTY, so it works over the alt screen too.
    if (key == TERM_KEY_F6 && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        show_diagnostics_report(ctx);
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+N → spawn a new terminal window in the shell's CWD
    if (key == TERM_KEY_NONE && codepoint == 'N' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        platform_spawn_new_terminal(ctx->plat);
        result.handled = true;
        return result;
    }

    // Special keys
    if (key != TERM_KEY_NONE) {
        terminal_send_key(ctx->term, key, mod);
        result.handled = true;
        return result;
    }

    // Ctrl/Alt + printable
    if (codepoint && (mod & (TERM_MOD_CTRL | TERM_MOD_ALT))) {
        // Pass Ctrl and Alt to libvterm but not Shift (already baked into resolved char)
        int mod_no_shift = mod & ~TERM_MOD_SHIFT;
        terminal_send_char(ctx->term, codepoint, mod_no_shift);
        result.handled = true;
    }

    return result;
}

// Text input callback — pure UTF-8 from IME/compose
static KeyboardResult on_text(void *user_data, const char *text)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    // While the pager is open, plain printables arrive here (the platforms
    // route non-modified letters through IME/text, not on_key). Feed each as a
    // character shortcut (q close, j/k/g/G/b/space scroll); everything else is
    // swallowed so nothing reaches the shell.
    if (pager_active(ctx->pager)) {
        for (const char *t = text; *t; t++)
            pager_key(ctx->pager, TERM_KEY_NONE, TERM_MOD_NONE, (unsigned char)*t);
        result.handled = true;
        return result;
    }

    if (terminal_selection_active(ctx->term)) {
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
    }

    size_t text_len = strlen(text);
    if (text_len > 0 && text_len < sizeof(result.data)) {
        memcpy(result.data, text, text_len);
        result.len = text_len;
    }

    return result;
}

// Window resize callback
static void on_resize(void *user_data, int pixel_w, int pixel_h)
{
    MainContext *ctx = (MainContext *)user_data;

    terminal_selection_clear(ctx->term);
    renderer_resize(ctx->rend, pixel_w, pixel_h);

    int cell_w, cell_h;
    if (renderer_get_cell_size(ctx->rend, &cell_w, &cell_h)) {
        terminal_set_cell_px(ctx->term, cell_w, cell_h);
        int cols = pixel_w / cell_w;
        int rows = pixel_h / cell_h;
        if (cols > 0 && rows > 0) {
            terminal_resize(ctx->term, cols, rows);
            pty_resize(ctx->pty, rows, cols);
            // Rebuild the pager overlay at the new size if it is open.
            if (pager_active(ctx->pager))
                pager_resize(ctx->pager, cols, rows);
        }
    }
}

// Scroll callback — the single dispatch point for wheel scrolling.
// ticks is the number of whole wheel ticks (positive = up, negative = down).
// The platform layer handles sub-tick accumulation; this function decides
// what the ticks do: pager scroll, altscreen arrow keys, or scrollback.
static void on_scroll(void *user_data, int ticks)
{
    MainContext *ctx = (MainContext *)user_data;
    if (ticks == 0)
        return;

    if (pager_active(ctx->pager)) {
        pager_scroll(ctx->pager, ticks * SCROLL_LINES_PER_TICK);
        return;
    }

    // In altscreen with no mouse mode, convert wheel to arrow keys so
    // apps like less/man scroll. This was previously handled in on_mouse
    // with a hardcoded 3; it is now consolidated here using the constant.
    if (terminal_is_altscreen(ctx->term) && terminal_get_mouse_mode(ctx->term) == 0) {
        int key = (ticks > 0) ? TERM_KEY_UP : TERM_KEY_DOWN;
        int count = abs(ticks) * SCROLL_LINES_PER_TICK;
        if (count < 1)
            count = 1;
        for (int i = 0; i < count; i++)
            terminal_send_key(ctx->term, key, TERM_MOD_NONE);
        return;
    }

    renderer_scroll(ctx->rend, ctx->term, ticks * SCROLL_LINES_PER_TICK);
}

// Output callback for terminal - sends data to PTY
static void term_output_to_pty(const char *data, size_t len, void *user)
{
    PtyContext *pty = (PtyContext *)user;
    if (pty) {
        pty_write(pty, data, len);
    }
}

// Resolve the OSC-8 hyperlink id at a pixel position. Returns 0 if no link
// or out-of-bounds. Also returns the resolved (unified_row, vt_col) for
// callers that want to fetch the URI without redoing the math.
static uint16_t hyperlink_id_at(MainContext *ctx, int pixel_x, int pixel_y,
                                int *out_unified_row, int *out_vt_col)
{
    int display_row, display_col;
    if (!pixel_to_cell(ctx, pixel_x, pixel_y, &display_row, &display_col))
        return 0;
    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    if (display_row < 0 || display_row >= term_rows ||
        display_col < 0 || display_col >= term_cols)
        return 0;
    int unified_row = display_row_to_unified(ctx->rend, display_row);
    int vt_col = terminal_vis_col_to_vt_col(ctx->term, unified_row, display_col);
    if (vt_col < 0)
        vt_col = 0;
    if (vt_col >= term_cols)
        vt_col = term_cols - 1;

    TerminalCell cell;
    int rc = (unified_row >= 0)
                 ? terminal_get_cell(ctx->term, unified_row, vt_col, &cell)
                 : terminal_get_scrollback_cell(ctx->term, -(unified_row + 1),
                                                vt_col, &cell);
    if (rc < 0)
        return 0;
    if (out_unified_row)
        *out_unified_row = unified_row;
    if (out_vt_col)
        *out_vt_col = vt_col;
    return cell.hyperlink_id;
}

// Resolve the OSC-8 hyperlink under a pixel position and update hover state +
// cursor shape. px<0 (or out-of-bounds) resolves to "no link". Returns true if
// the hovered id changed (caller should trigger a redraw). Shared by on_mouse
// (motion) and on_revalidate_hover (post-PTY re-resolution at the live pointer).
static bool resolve_link_hover(MainContext *ctx, int px, int py)
{
    int link_row = 0, link_col = 0;
    uint16_t hid = (px < 0) ? 0 : hyperlink_id_at(ctx, px, py, &link_row, &link_col);
    if (hid == terminal_hovered_hyperlink(ctx->term))
        return false;
    terminal_set_hovered_hyperlink(ctx->term, hid);
    platform_set_cursor(ctx->plat,
                        hid != 0 ? PLATFORM_CURSOR_POINTER : PLATFORM_CURSOR_TEXT);
    // Reveal the real target URI in a transient hover hint, positioned clear
    // of the link via the pixel-Y anchor. hid == 0 (pointer off any link)
    // hides it.
    if (hid != 0) {
        char url[4096];
        size_t n = terminal_cell_get_hyperlink(ctx->term, link_row, link_col, url,
                                               sizeof(url));
        platform_set_link_hint(ctx->plat, n > 0 ? url : NULL, py);
    } else {
        platform_set_link_hint(ctx->plat, NULL, py);
    }
    return true;
}

// Mouse callback — mod uses TERM_MOD_* flags (platform-independent)
static bool on_mouse(void *user_data, int pixel_x, int pixel_y, int button, bool pressed,
                     int clicks, int mod)
{
    MainContext *ctx = (MainContext *)user_data;

    // The pager is modal: it handles hover, Ctrl+click-to-open, and text
    // selection, and consumes all mouse events while open.
    if (pager_active(ctx->pager))
        return pager_mouse(ctx->pager, pixel_x, pixel_y, button, pressed, clicks, mod);

    // The SDL3 notification panel floats above the terminal. While the pointer
    // is over it, the panel owns the cursor (pointer over the close button, with
    // a hover highlight), suppresses link hover underneath, and consumes the
    // event so the terminal doesn't react beneath it.
    int notif_hit = renderer_notification_hit(ctx->rend, pixel_x, pixel_y);
    if (notif_hit != 0) {
        // Pointer is over the panel — no link underneath it to hint.
        platform_set_link_hint(ctx->plat, NULL, 0);
        if (pressed && button == 1 && notif_hit == 2) {
            platform_notify_dismiss(ctx->plat);
            platform_set_cursor(ctx->plat, PLATFORM_CURSOR_TEXT);
            return true;
        }
        platform_set_cursor(ctx->plat, notif_hit == 2 ? PLATFORM_CURSOR_POINTER
                                                      : PLATFORM_CURSOR_TEXT);
        if (renderer_set_notification_hover(ctx->rend, notif_hit == 2))
            terminal_mark_dirty(ctx->term);
        if (terminal_hovered_hyperlink(ctx->term) != 0) {
            terminal_set_hovered_hyperlink(ctx->term, 0);
            terminal_mark_dirty(ctx->term);
        }
        return true;
    }
    // Pointer left the panel — clear any lingering close-button hover highlight.
    if (renderer_set_notification_hover(ctx->rend, false))
        terminal_mark_dirty(ctx->term);

    int mouse_mode = terminal_get_mouse_mode(ctx->term);
    bool shift_held = (mod & TERM_MOD_SHIFT) != 0;

    // OSC-8 hyperlink hover: only meaningful when the terminal is not
    // forwarding mouse events to the running app (or Shift overrides).
    // We update the cursor + hover-id and trigger a redraw on changes.
    bool hover_changed = false;
    if (mouse_mode == 0 || shift_held) {
        // Update hover state, cursor shape, and the link-hint (centralized in
        // resolve_link_hover so the post-PTY revalidation path stays in sync).
        hover_changed = resolve_link_hover(ctx, pixel_x, pixel_y);

        // Ctrl + left-click on a link cell: open URL, swallow the event so
        // it does not start a selection.
        if (button == 1 && pressed && (mod & TERM_MOD_CTRL)) {
            int link_row, link_col;
            uint16_t hid = hyperlink_id_at(ctx, pixel_x, pixel_y, &link_row, &link_col);
            if (hid != 0) {
                char url[4096];
                size_t n = terminal_cell_get_hyperlink(ctx->term, link_row,
                                                       link_col, url, sizeof(url));
                if (n > 0 && terminal_hyperlink_is_safe(url)) {
                    vlog("Opening OSC-8 URL: %s\n", url);
                    char err[256];
                    if (!platform_open_url(ctx->plat, url, err, sizeof(err))) {
                        // Bound the URL so a very long link can't blow up (or
                        // truncate) the one-line title.
                        char title[256];
                        char body[320];
                        snprintf(title, sizeof(title), "Couldn't open %.200s", url);
                        snprintf(body, sizeof(body), "Error is: %.256s", err);
                        fprintf(stderr, "ERROR: %s — %s\n", title, body);
                        platform_notify(ctx->plat, title, body, PLATFORM_NOTIFY_ERROR);
                    }
                } else if (n > 0) {
                    char title[256];
                    snprintf(title, sizeof(title), "Refusing to open %.200s", url);
                    fprintf(stderr, "WARNING: %s (disallowed scheme)\n", title);
                    platform_notify(ctx->plat, title, "Disallowed URL scheme",
                                    PLATFORM_NOTIFY_WARNING);
                }
                return true;
            }
        }
    } else {
        // A mouse-mode app owns the pointer (and Shift isn't overriding) — drop
        // any lingering link hint from a prior hover.
        platform_set_link_hint(ctx->plat, NULL, 0);
    }

    // Forward to terminal if mouse mode is active and Shift is not held
    bool in_altscreen = terminal_is_altscreen(ctx->term);

    if (mouse_mode > 0 && !shift_held) {
        bool should_forward = false;

        if (button == 4 || button == 5) {
            should_forward = in_altscreen || (mouse_mode > 0);
        } else if (button > 0) {
            should_forward = true;
        } else {
            should_forward = (mouse_mode >= 2 && pressed) || (mouse_mode >= 3);
        }

        if (should_forward) {
            int cell_w, cell_h;
            if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 ||
                cell_h <= 0)
                return false;
            int col = pixel_x / cell_w;
            int row = pixel_y / cell_h;

            int term_rows, term_cols;
            terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
            if (col >= term_cols)
                col = term_cols - 1;
            if (row >= term_rows)
                row = term_rows - 1;
            if (col < 0)
                col = 0;
            if (row < 0)
                row = 0;

            terminal_send_mouse_event(ctx->term, row, col, button, pressed, mod);
            return true;
        }

        if (button == 4 || button == 5)
            return false;
    }

    // Wheel events not consumed by terminal mouse-mode forwarding fall
    // through to on_scroll, which handles altscreen arrows and scrollback.
    if (button == 4 || button == 5)
        return false;

    // In altscreen the application owns the display.  When no mouse protocol
    // is active, the app hasn't claimed the pointer — but terminal-level
    // selection can still clobber the app's own clipboard operations (OSC 52)
    // and produce confusing visual artifacts over the app's UI.  Shift
    // overrides, giving the user an escape hatch (matches kitty, VTE, and
    // Alacritty convention for Shift-to-select).  Right-click paste is
    // allowed through since it's useful even in altscreen apps.
    if (in_altscreen && !shift_held && mouse_mode == 0 && button != 3) {
        return hover_changed;
    }

    int display_row, display_col;
    if (!pixel_to_cell(ctx, pixel_x, pixel_y, &display_row, &display_col))
        return false;

    // Clamp to terminal dimensions (display_row in display space; display_col
    // is a visual column that gets translated to libvterm space below).
    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    if (display_row >= term_rows)
        display_row = term_rows - 1;
    if (display_row < 0)
        display_row = 0;
    if (display_col < 0)
        display_col = 0;

    int unified_row = display_row_to_unified(ctx->rend, display_row);

    // Translate visual column → libvterm column for VS16-widened rows. The
    // selection API and mouse-event reporting both work in libvterm space.
    display_col = terminal_vis_col_to_vt_col(ctx->term, unified_row, display_col);
    if (display_col >= term_cols)
        display_col = term_cols - 1;
    if (display_col < 0)
        display_col = 0;

    // Left button press — start selection (or defer for char mode)
    if (button == 1 && pressed) {
        ctx->drag_in_progress = true;
        if (clicks >= 3) {
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_LINE);
        } else if (clicks == 2) {
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_WORD);
        } else if (terminal_selection_active(ctx->term)) {
            ctx->drag_pending = false;
            terminal_selection_clear(ctx->term);
        } else {
            // Defer char selection until drag — don't pause PTY on stray clicks
            ctx->drag_pending = true;
            ctx->drag_start_row = unified_row;
            ctx->drag_start_col = display_col;
        }
        return true;
    }

    // Left button release — cancel pending drag if no motion occurred
    if (button == 1 && !pressed) {
        ctx->drag_in_progress = false;
        ctx->drag_pending = false;
        ctx->pointer_outside = false;
        set_autoscroll(ctx, false);
        return false;
    }

    // Motion with button held — start or update selection
    if (button == 0 && pressed) {
        if (ctx->drag_pending) {
            // First motion after click — start char selection from saved position
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, ctx->drag_start_row, ctx->drag_start_col,
                                     TERM_SELECT_CHAR);
            terminal_selection_update(ctx->term, unified_row, display_col);
        } else if (terminal_selection_active(ctx->term)) {
            terminal_selection_update(ctx->term, unified_row, display_col);
        } else {
            // Button-down outside a selection (e.g. middle/right) — nothing to do
            return hover_changed;
        }

        // Update autoscroll state from the raw (unclamped) pointer pos.
        ctx->drag_last_pixel_x = pixel_x;
        ctx->drag_last_pixel_y = pixel_y;
        int cell_w, cell_h;
        if (renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) && cell_h > 0) {
            int viewport_h = term_rows * cell_h;
            if (pixel_y < 0) {
                ctx->autoscroll_direction = 1;
                set_autoscroll(ctx, true);
            } else if (pixel_y >= viewport_h) {
                ctx->autoscroll_direction = -1;
                set_autoscroll(ctx, true);
            } else {
                // Pointer inside viewport — autoscroll no longer needed.
                set_autoscroll(ctx, false);
            }
        }
        return true;
    }

    // Any non-drag mouse activity — stop autoscroll
    if (button != 0 && pressed)
        set_autoscroll(ctx, false);

    // Right button press — copy selection if active, otherwise paste
    if (button == 3 && pressed) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                platform_clipboard_set(ctx->plat, text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
        } else {
            // Paste synchronously
            if (!platform_clipboard_paste_async(ctx->plat, ctx->term, ctx->pty)) {
                char *clipboard = platform_clipboard_get(ctx->plat);
                if (clipboard && clipboard[0] != '\0') {
                    terminal_start_paste(ctx->term);
                    pty_write(ctx->pty, clipboard, strlen(clipboard));
                    terminal_end_paste(ctx->term);
                }
                platform_clipboard_free(ctx->plat, clipboard);
            }
        }
        return true;
    }

    return hover_changed;
}

// Re-resolve OSC-8 hover at the live pointer after PTY output redrew the screen.
// terminal_process_input clears the hovered id on every batch; without a fresh
// motion event the underline would drop to idle until the user jiggles the
// mouse — visible as flicker under a continuously-redrawing app. We restore it
// here before the frame paints. Gated to the no-mouse-mode case (the app is not
// grabbing the pointer) and disabled while the pager is modal.
static bool on_revalidate_hover(void *user_data, int px, int py)
{
    MainContext *ctx = (MainContext *)user_data;
    if (pager_active(ctx->pager))
        return false;
    if (terminal_get_mouse_mode(ctx->term) != 0)
        return false;
    return resolve_link_hover(ctx, px, py);
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    /* GUI subsystem has no console — reattach to the parent's console
     * so stdout/stderr work when launched from cmd.exe.  For short-lived
     * commands (--list-fonts, -h, -v) we keep the console so output
     * reaches the user.  For the long-running GUI we detach after
     * initial output so closing the parent shell doesn't kill us. */
    bool console_attached = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        fflush(stdout);
        fflush(stderr);
        console_attached = true;
    }
#endif

    TerminalBackend *term;
    RendererBackend *rend = NULL;
    PtyContext *pty = NULL;
    PlatformBackend *plat = NULL;
    int running = 1;
    int opt;

    // Parse command line arguments
    int list_fonts = 0;
    int ft_hint_target = FT_LOAD_TARGET_LIGHT; // Default: light hinting
    char *png_text = NULL;
    const char *png_exec = NULL;
    int png_wait_ms = 200;
    char *demo_text = NULL;
    const char *font_name = NULL;
    int font_from_flag = 0; // -f given on the CLI (overrides config + desktop)
    // Where the effective font came from, surfaced in the diagnostics report.
    const char *font_source = NULL;
    const char *colr_debug_path = NULL;
    char **exec_argv = NULL;
    const float font_size = 12.0f;
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;
    int init_scrollback = -1;

    static struct option long_options[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "demo", required_argument, NULL, 'd' },
        { "exec", required_argument, NULL, 'X' },
        { "wait", required_argument, NULL, 'W' },
        { "scrollback", required_argument, NULL, 's' },
        { NULL, 0, NULL, 0 }
    };

    /* Load config file (CLI flags below will override) */
    PorttyConf conf;
    portty_conf_init(&conf);
    portty_conf_load(&conf);

    if (conf.verbose == 1)
        verbose = 1;
    if (conf.font)
        font_name = conf.font;
    if (conf.cols > 0)
        init_cols = conf.cols;
    if (conf.rows > 0)
        init_rows = conf.rows;
    if (conf.hinting != PORTTY_HINT_UNSET) {
        static const int hint_map[] = { FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT,
                                        FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_MONO };
        ft_hint_target = hint_map[conf.hinting];
    }
    if (conf.scrollback >= 0)
        init_scrollback = conf.scrollback;
    /* kitty-style text_composition_strategy curve (unset = neutral). */
    if (conf.text_gamma > 0.0f)
        portty_text_gamma = conf.text_gamma;
    if (conf.text_contrast >= 0.0f)
        portty_text_contrast = conf.text_contrast;
    if (conf.notification_transparency == 1)
        portty_notification_transparent = true;

    while ((opt = getopt_long(argc, argv, "hvVf:g:P:D:s:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'V':
            print_version();
            return 0;
        case 'v':
            verbose = 1;
            break;
        case 'd':
            demo_text = optarg;
            break;
        case 'f':
            font_name = optarg;
            font_from_flag = 1;
            break;
        case 'L':
            list_fonts = 1;
            break;
        case 'H':
            if (strcmp(optarg, "none") == 0) {
                ft_hint_target = FT_LOAD_NO_HINTING;
            } else if (strcmp(optarg, "light") == 0) {
                ft_hint_target = FT_LOAD_TARGET_LIGHT;
            } else if (strcmp(optarg, "normal") == 0) {
                ft_hint_target = FT_LOAD_TARGET_NORMAL;
            } else if (strcmp(optarg, "mono") == 0) {
                ft_hint_target = FT_LOAD_TARGET_MONO;
            } else {
                fprintf(stderr, "ERROR: Invalid hinting target: %s (use none, light, normal, mono)\n", optarg);
                return 1;
            }
            break;
        case 'g':
        {
            int w = 0, h = 0;
            if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                init_cols = w;
                init_rows = h;
            } else {
                fprintf(stderr, "ERROR: Invalid geometry: %s (use COLSxROWS, e.g. 120x40)\n", optarg);
                return 1;
            }
            break;
        }
        case 'P':
            png_text = optarg;
            break;
        case 'X':
            png_exec = optarg;
            break;
        case 'W':
            png_wait_ms = atoi(optarg);
            if (png_wait_ms <= 0) {
                fprintf(stderr, "ERROR: --wait requires a positive millisecond value\n");
                return 1;
            }
            break;
        case 'D':
            colr_debug_path = optarg;
            break;
            break;
        case 's':
        {
            char *end = NULL;
            long n = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n < 0 || n > INT_MAX) {
                fprintf(stderr,
                        "ERROR: Invalid scrollback: %s (use a non-negative integer)\n",
                        optarg);
                return 1;
            }
            init_scrollback = (int)n;
            break;
        }
        case '?':
            print_usage(argv[0]);
            return 1;
        }
    }

    // After getopt, remaining args (after --) are the command to execute
    if (optind < argc) {
        exec_argv = &argv[optind];
    } else if (conf.shell) {
        // No -- args given: use the shell from config (overrides $SHELL/COMSPEC).
        // Split on whitespace to support arguments, e.g. "bash --norc".
        char *shell_copy = strdup(conf.shell);
        if (!shell_copy) {
            fprintf(stderr, "ERROR: Out of memory\n");
            return 1;
        }
        char *tokens[64];
        int ntok = 0;
        char *tok = strtok(shell_copy, " \t");
        while (tok && ntok < 63) {
            tokens[ntok++] = tok;
            tok = strtok(NULL, " \t");
        }
        tokens[ntok] = NULL;

        char **shell_argv = calloc(ntok + 1, sizeof(char *));
        if (!shell_argv) {
            free(shell_copy);
            fprintf(stderr, "ERROR: Out of memory\n");
            return 1;
        }
        for (int i = 0; i < ntok; i++)
            shell_argv[i] = strdup(tokens[i]);
        shell_argv[ntok] = NULL;
        free(shell_copy);
        exec_argv = shell_argv;
    }

    // List monospace fonts and exit
    if (list_fonts) {
        FontResolveBackend *resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            return 1;
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        return 0;
    }

    // Set COLR layer debug prefix if specified
    if (colr_debug_path) {
        colr_set_debug_prefix(colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", colr_debug_path);
    }

    // PNG render mode: skip interactive mode, render to PNG and exit.
    // Two flavors:
    //   -P "text" out.png             — feed literal text into the term
    //   -P "" --exec CMD [--wait MS] out.png  — spawn CMD on a PTY, drain
    //                                            its output for MS ms (default
    //                                            200), then render.
    if (png_text || png_exec) {
        if (optind >= argc) {
            fprintf(stderr, "ERROR: -P requires output PNG path as positional argument\n");
            fprintf(stderr, "Usage: %s -P \"text\" output.png\n", argv[0]);
            fprintf(stderr, "       %s -P \"\" --exec 'cmd' [--wait 500] output.png\n", argv[0]);
            return 1;
        }
        if (png_exec) {
            return png_render_exec(png_exec, png_wait_ms, init_cols, init_rows,
                                   argv[optind], font_name, ft_hint_target);
        }
        return png_render_text(png_text, argv[optind], font_name, ft_hint_target);
    }

    // Select and initialize platform backend
    PlatformBackend *selected_backend = &platform_backend_sdl3;
    plat = platform_init(selected_backend);
    if (!plat) {
        fprintf(stderr, "ERROR: Failed to initialize platform\n");
        return 1;
    }

    // FreeType is initialized in renderer_init, not here
    vlog("FreeType will be initialized in renderer\n");

    // Initialize terminal. Cell pixel size defaults to 10x20; updated
    // once font loading completes.  Reflow is enabled by default.
    TerminalBackend *vt_backend = &terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = init_rows;
    cfg.cols = init_cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    cfg.reflow = true;
    term = terminal_init(vt_backend, &cfg);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        platform_destroy(plat);
        return 1;
    }

    if (conf.word_chars)
        terminal_selection_set_word_chars(term, conf.word_chars);

    if (init_scrollback >= 0)
        terminal_set_scrollback_size(term, init_scrollback);

    // Provide the exe path to the terminal backend so that the OSC 7 / OSC
    // 9;9 handlers can convert MSYS2/Unix paths to native Windows paths.
    term->exe_path = platform_get_exe_path(plat);

    // Only create window and renderer if we're going to run the event loop
    if (running) {
#ifdef _WIN32
        // When launched from MSYS2/Cygwin bash, the shell uses POSIX
        // waitpid() semantics — it blocks until the child exits, even
        // for GUI-subsystem binaries.  cmd.exe and PowerShell check
        // the PE subsystem and return immediately for GUI apps, but
        // MSYS2 bash does not.  Re-exec ourselves with DETACHED_PROCESS
        // so the original process can exit and unblock the parent shell.
        // A sentinel env var prevents infinite re-exec.
        if (console_attached && !getenv("_PORTTY_DETACHED")) {
            const char *msystem = getenv("MSYSTEM");
            const char *msys = getenv("MSYS");
            if (msystem || msys) {
                char exe[MAX_PATH];
                GetModuleFileNameA(NULL, exe, MAX_PATH);
                // Build command line from original argv
                char cmdline[32768];
                char *cp = cmdline;
                for (int i = 0; i < argc; i++) {
                    if (i > 0)
                        *cp++ = ' ';
                    const char *s = argv[i];
                    BOOL need_quote = FALSE;
                    for (const char *c = s; *c; c++)
                        if (*c == ' ' || *c == '\t' || *c == '"')
                            need_quote = TRUE;
                    if (need_quote) {
                        *cp++ = '"';
                        while (*s) {
                            if (*s == '"')
                                *cp++ = '"';
                            *cp++ = *s;
                            s++;
                        }
                        *cp++ = '"';
                    } else {
                        while (*s)
                            *cp++ = *s++;
                    }
                }
                *cp = '\0';
                // Set sentinel so the re-exec'd child skips this block
                SetEnvironmentVariableA("_PORTTY_DETACHED", "1");
                STARTUPINFOA si;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi;
                ZeroMemory(&pi, sizeof(pi));
                if (CreateProcessA(exe, cmdline, NULL, NULL, FALSE,
                                   DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                                   NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    FreeConsole();
                    return 0;
                }
                // If CreateProcess failed, fall through to the
                // normal FreeConsole path — at least we won't
                // block on console close.
            }
        }
        // Detach from the parent console now that startup output is done.
        // The GUI process must not stay attached or closing the parent
        // shell will kill it.  Short-lived commands (--list-fonts, -h)
        // already returned above while the console was still attached.
        if (console_attached)
            FreeConsole();
#endif
        // Create window (placeholder size; will be resized after font loading)
        if (!platform_create_window(plat, "portty", 800, 600)) {
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }

        // Initialize renderer using SDL handles from platform
        rend = renderer_init(&renderer_backend_sdl3,
                             platform_get_sdl_window(plat),
                             platform_get_sdl_renderer(plat));
        if (!rend) {
            fprintf(stderr, "Failed to initialize renderer\n");
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }

        // Query desktop environment for preferred monospace font
        char *desktop_font = NULL;
        if (!font_name) {
            desktop_font = platform_get_default_font(plat);
            if (desktop_font)
                font_name = desktop_font;
        }

        // Record where the effective font came from, for the diagnostics
        // report. With no explicit font and no platform default (the SDL
        // backend has no desktop integration), font_name stays NULL and the
        // resolver falls back to fontconfig's generic "monospace" alias — this
        // is by design, so name it plainly rather than leaving it a mystery.
        if (font_from_flag)
            font_source = "-f flag";
        else if (conf.font)
            font_source = "config file";
        else if (desktop_font)
            font_source = "desktop default";
        else
#ifdef _WIN32
            font_source = "system default (no console font set)";
#else
            font_source = "fontconfig generic (no desktop default)";
#endif

        // Set content scale before font loading so FreeType uses correct DPI
        float display_scale = platform_get_display_scale(plat);
        if (display_scale > 0.0f)
            renderer_set_content_scale(rend, display_scale);

        // Load fonts
        if (renderer_load_fonts(rend, font_size, font_name, ft_hint_target) < 0) {
            fprintf(stderr, "Failed to load fonts\n");
            free(desktop_font);
            renderer_destroy(rend);
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }
        free(desktop_font);

        // Derive window size from font cell dimensions
        int cell_w, cell_h;
        int win_w = 800, win_h = 600;
        if (renderer_get_cell_size(rend, &cell_w, &cell_h)) {
            // Tell the VT engine the cell pixel size so it can place sixel
            // images in text rows.
            terminal_set_cell_px(term, cell_w, cell_h);
            win_w = init_cols * cell_w;
            win_h = init_rows * cell_h;
            vlog("Derived window size from font: %dx%d (%d cols * %d px, %d rows * %d px)\n",
                 win_w, win_h, init_cols, cell_w, init_rows, cell_h);

            // Clamp to display bounds so the WM doesn't have to force-resize
            int disp_w, disp_h;
            if (platform_get_display_size(plat, &disp_w, &disp_h)) {
                if (win_w > disp_w || win_h > disp_h) {
                    if (win_w > disp_w)
                        win_w = disp_w;
                    if (win_h > disp_h)
                        win_h = disp_h;
                    init_cols = win_w / cell_w;
                    init_rows = win_h / cell_h;
                    if (init_cols < 1)
                        init_cols = 1;
                    if (init_rows < 1)
                        init_rows = 1;
                    win_w = init_cols * cell_w;
                    win_h = init_rows * cell_h;
                    vlog("Clamped to display %dx%d: %dx%d (%d cols, %d rows)\n",
                         disp_w, disp_h, win_w, win_h, init_cols, init_rows);
                    terminal_resize(term, init_cols, init_rows);
                }
            }
        }
        platform_set_window_size(plat, win_w, win_h);
        renderer_resize(rend, win_w, win_h);
        platform_show_window(plat);

        if (demo_text) {
            // Demo mode: feed text directly into terminal, no PTY needed
            terminal_process_input(term, demo_text, strlen(demo_text));
            vlog("Demo mode: fed %zu bytes into terminal\n", strlen(demo_text));
        } else {
            // Initialize signal handling before creating PTY
            if (pty_signal_init() < 0) {
                fprintf(stderr, "WARNING: Failed to initialize SIGCHLD handling\n");
            }

            // Create PTY and spawn shell (or custom command)
            pty = pty_create(init_rows, init_cols, exec_argv);
            if (!pty) {
                fprintf(stderr, "ERROR: Failed to create PTY\n");
                pty_signal_cleanup();
                renderer_destroy(rend);
                terminal_destroy(term);
                platform_destroy(plat);
                return 1;
            }

            // Register PTY with platform
            if (!platform_register_pty(plat, pty)) {
                fprintf(stderr, "ERROR: Failed to register PTY with platform\n");
                pty_destroy(pty);
                pty_signal_cleanup();
                renderer_destroy(rend);
                terminal_destroy(term);
                platform_destroy(plat);
                return 1;
            }
        }
    }

    // Only enter event loop if running
    if (running) {
        MainContext main_ctx = {
            .term = term,
            .rend = rend,
            .pty = pty,
            .plat = plat,
            .conf = &conf,
            .font_source = font_source,
        };
        main_ctx.pager = pager_create(rend, plat);

        PlatformCallbacks callbacks = {
            .on_key = on_key,
            .on_text = on_text,
            .on_resize = on_resize,
            .on_scroll = on_scroll,
            .on_mouse = on_mouse,
            .on_autoscroll_tick = on_autoscroll_tick,
            .on_mouse_leave = on_mouse_leave,
            .on_mouse_enter = on_mouse_enter,
            .on_revalidate_hover = on_revalidate_hover,
            .user_data = &main_ctx,
        };

        // Connect terminal output to PTY (for mouse escape sequences)
        terminal_set_output_callback(term, term_output_to_pty, pty);

        // Pause/resume PTY on selection changes in alt screen
        terminal_set_selection_callback(term, on_selection_change, &main_ctx);

        // OSC 52: route application clipboard-set requests to the OS clipboard
        terminal_set_clipboard_set_callback(term, on_clipboard_set, &main_ctx);

        // OSC 7 / OSC 9;9 CWD tracking for Ctrl+Shift+N
        terminal_set_cwd_callback(term, on_cwd_change, &main_ctx);

        // Run the event loop (blocks)
        platform_run(plat, term, rend, &callbacks);

        pager_destroy(main_ctx.pager);
    }

    // Cleanup
    if (pty)
        pty_destroy(pty);
    pty_signal_cleanup();
    if (rend)
        renderer_destroy(rend);
    terminal_destroy(term);
    portty_conf_free(&conf);
    platform_destroy(plat);

    return 0;
}

void vlog_impl(const char *file, const char *func, int line, const char *format, ...)
{
    if (!verbose)
        return;

    // Extract basename from file path
    const char *basename = strrchr(file, '/');
#ifdef _WIN32
    const char *basename2 = strrchr(file, '\\');
    if (!basename || (basename2 && basename2 > basename))
        basename = basename2;
#endif
    basename = basename ? basename + 1 : file;

    // Get current time with milliseconds
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    int hour = st.wHour, min = st.wMinute, sec = st.wSecond;
    long ms = st.wMilliseconds;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int hour = tm.tm_hour, min = tm.tm_min, sec = tm.tm_sec;
    long ms = ts.tv_nsec / 1000000;
#endif

    va_list args;
    va_start(args, format);
    fprintf(stderr, "DEBUG [%02d:%02d:%02d.%03ld] %s:%d %s(): ",
            hour, min, sec, ms, basename, line, func);
    vfprintf(stderr, format, args);
    va_end(args);
}

static void print_usage(const char *progname)
{
    printf("Usage: %s [OPTIONS] [-- COMMAND [ARGS...]]\n", progname);
    printf("Terminal emulator using coffer and SDL3\n\n");
    printf("Options:\n");
    printf("  -h, --help          Show this help message and exit\n");
    printf("  -V, --version       Show version and build info and exit\n");
    printf("  -v                  Verbose output (debug information)\n");
    printf("  -f PATTERN          Font (fontconfig pattern, default: monospace)\n");
    printf("                      Size is part of the pattern, e.g. -f monospace-16\n");
    printf("                      Examples: -f \"Cascadia Code-14\", -f monospace-24\n");
    printf("  -g COLSxROWS        Initial terminal size, e.g. 120x40 (default: 80x24)\n");
    printf("  -s, --scrollback N  Scrollback history lines (default: 1000, 0 to disable)\n");
    printf("  --ft-hinting S      FreeType hinting: none, light, normal, mono (default: light)\n");
    printf("  --list-fonts        List available monospace fonts and exit\n");
    printf("  --demo TEXT         Display TEXT in terminal without spawning a shell (for testing)\n");
    printf("  -P TEXT             Render TEXT to a PNG file (output path as positional arg)\n");
    printf("  -D PREFIX           Debug COLR layers: save each layer as PREFIX_layer00.png, etc.\n");
    printf("\n");
    printf("Command execution:\n");
    printf("  Use -- to separate options from command. Without --, runs default shell.\n");
    printf("  Examples:\n");
    printf("    %s                              # Run default shell\n", progname);
    printf("    %s -- htop                      # Run htop directly\n", progname);
    printf("    %s -- sh -c 'echo hello'        # Run shell command\n", progname);
    printf("\n");
    printf("Runtime controls:\n");
    printf("  Ctrl+Shift+C    Copy selection to clipboard\n");
    printf("  Ctrl+Shift+V    Paste from clipboard\n");
    printf("  Shift+PgUp/Dn   Scroll through scrollback\n");
    printf("\n");
    printf("Run '%s --version' for build and version info.\n", progname);
}

static void print_version(void)
{
    printf("portty %s\n", PORTTY_VERSION);
    printf("Copyright (C) 2026 Thomas Christensen\n");
    printf("License MIT: <https://opensource.org/licenses/MIT>\n");
    printf("This is free software: you are free to change and redistribute it.\n");
    printf("There is NO WARRANTY, to the extent permitted by law.\n");
    printf("\n");
    printf("Built with: %s\n", BUILD_CC);
    printf("coffer %s, SDL3 %s, FreeType %s\n",
           DEP_COFFER_VERSION, DEP_SDL3_VERSION, DEP_FREETYPE_VERSION);
    printf("HarfBuzz %s, libpng %s\n",
           DEP_HARFBUZZ_VERSION, DEP_LIBPNG_VERSION);
#ifdef DEP_FONTCONFIG_VERSION
    printf("Fontconfig %s\n", DEP_FONTCONFIG_VERSION);
#endif
#ifdef __APPLE__
    printf("Font resolver: Core Text (system)\n");
#elif defined(_WIN32)
    printf("Font resolver: W32 native\n");
#endif
}
