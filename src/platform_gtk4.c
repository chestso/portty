#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "platform_gtk4.h"
#include <SDL3/SDL.h>
#include <adwaita.h>
#include <cairo.h>
#include <errno.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Cursor blink interval in milliseconds
#define CURSOR_BLINK_INTERVAL_MS 1000

// GDK keyval -> terminal key mapping
static const struct
{
    uint32_t gdk_key;
    int term_key;
} gdk_key_map[] = {
    { GDK_KEY_Return, TERM_KEY_ENTER },
    { GDK_KEY_KP_Enter, TERM_KEY_KP_ENTER },
    { GDK_KEY_BackSpace, TERM_KEY_BACKSPACE },
    { GDK_KEY_Escape, TERM_KEY_ESCAPE },
    { GDK_KEY_Tab, TERM_KEY_TAB },
    { GDK_KEY_ISO_Left_Tab, TERM_KEY_TAB }, // Shift+Tab
    { GDK_KEY_Up, TERM_KEY_UP },
    { GDK_KEY_Down, TERM_KEY_DOWN },
    { GDK_KEY_Right, TERM_KEY_RIGHT },
    { GDK_KEY_Left, TERM_KEY_LEFT },
    { GDK_KEY_Home, TERM_KEY_HOME },
    { GDK_KEY_End, TERM_KEY_END },
    { GDK_KEY_Insert, TERM_KEY_INS },
    { GDK_KEY_Delete, TERM_KEY_DEL },
    { GDK_KEY_Page_Up, TERM_KEY_PAGEUP },
    { GDK_KEY_Page_Down, TERM_KEY_PAGEDOWN },
    { GDK_KEY_F1, TERM_KEY_F1 },
    { GDK_KEY_F2, TERM_KEY_F2 },
    { GDK_KEY_F3, TERM_KEY_F3 },
    { GDK_KEY_F4, TERM_KEY_F4 },
    { GDK_KEY_F5, TERM_KEY_F5 },
    { GDK_KEY_F6, TERM_KEY_F6 },
    { GDK_KEY_F7, TERM_KEY_F7 },
    { GDK_KEY_F8, TERM_KEY_F8 },
    { GDK_KEY_F9, TERM_KEY_F9 },
    { GDK_KEY_F10, TERM_KEY_F10 },
    { GDK_KEY_F11, TERM_KEY_F11 },
    { GDK_KEY_F12, TERM_KEY_F12 },
    { GDK_KEY_KP_0, TERM_KEY_KP_0 },
    { GDK_KEY_KP_1, TERM_KEY_KP_1 },
    { GDK_KEY_KP_2, TERM_KEY_KP_2 },
    { GDK_KEY_KP_3, TERM_KEY_KP_3 },
    { GDK_KEY_KP_4, TERM_KEY_KP_4 },
    { GDK_KEY_KP_5, TERM_KEY_KP_5 },
    { GDK_KEY_KP_6, TERM_KEY_KP_6 },
    { GDK_KEY_KP_7, TERM_KEY_KP_7 },
    { GDK_KEY_KP_8, TERM_KEY_KP_8 },
    { GDK_KEY_KP_9, TERM_KEY_KP_9 },
    { GDK_KEY_KP_Multiply, TERM_KEY_KP_MULTIPLY },
    { GDK_KEY_KP_Add, TERM_KEY_KP_PLUS },
    { GDK_KEY_KP_Separator, TERM_KEY_KP_COMMA },
    { GDK_KEY_KP_Subtract, TERM_KEY_KP_MINUS },
    { GDK_KEY_KP_Decimal, TERM_KEY_KP_PERIOD },
    { GDK_KEY_KP_Divide, TERM_KEY_KP_DIVIDE },
    { GDK_KEY_KP_Equal, TERM_KEY_KP_EQUAL },
};

// Backend-specific context
typedef struct
{
    // GTK
    GtkWindow *window;
    GtkWidget *drawing_area;
    GtkWidget *header_bar;
    GtkIMContext *im_context;
    GMainLoop *main_loop;

    // SDL (offscreen)
    SDL_Window *sdl_window;
    SDL_Renderer *sdl_renderer;

    // Render target texture (sized to drawing area)
    SDL_Texture *render_target;
    int target_w, target_h;

    // PTY
    PtyContext *pty;
    GIOChannel *pty_channel;
    guint pty_watch_id;
    GIOChannel *signal_channel;
    guint signal_watch_id;

    // Timer
    guint cursor_blink_timer_id;
    bool cursor_blink_visible;

    // Render state
    bool force_redraw;
    bool has_focus;
    char *last_title;

    // Stored for draw_func access
    TerminalBackend *term;
    RendererBackend *rend;
    PlatformCallbacks *callbacks;
    PlatformBackend *plat;

    int scale_factor;

    // Content size (logical pixels, set by set_window_size)
    int content_width;
    int content_height;

    // Unix signal watch IDs (0 = already removed)
    guint sigint_id;
    guint sigterm_id;
} GTK4PlatformData;

// Convert GDK modifier flags to TERM_MOD_* flags
static int gdk_mod_to_term(GdkModifierType state)
{
    int m = TERM_MOD_NONE;
    if (state & GDK_SHIFT_MASK)
        m |= TERM_MOD_SHIFT;
    if (state & GDK_ALT_MASK)
        m |= TERM_MOD_ALT;
    if (state & GDK_CONTROL_MASK)
        m |= TERM_MOD_CTRL;
    return m;
}

// Helper: process a keyboard result from callbacks
static void handle_keyboard_result(GTK4PlatformData *ctx, KeyboardResult *result)
{
    if (result->request_quit) {
        g_main_loop_quit(ctx->main_loop);
        return;
    }

    if (result->force_redraw) {
        ctx->force_redraw = true;
        gtk_widget_queue_draw(ctx->drawing_area);
        return;
    }

    if (result->handled || result->len > 0) {
        // Reset scroll position when typing
        if (renderer_get_scroll_offset(ctx->rend) != 0) {
            renderer_reset_scroll(ctx->rend);
            ctx->force_redraw = true;
        }

        // Reset cursor blink on user input
        ctx->cursor_blink_visible = true;
        ctx->force_redraw = true;

        // Write to PTY if callback provided raw data
        if (result->len > 0 && !result->handled) {
            ssize_t written = pty_write(ctx->pty, result->data, result->len);
            if (written < 0) {
                vlog("PTY write failed: %s\n", strerror(errno));
            }
        }

        gtk_widget_queue_draw(ctx->drawing_area);
    }
}

// Draw function for GtkDrawingArea
static void draw_func(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer user_data)
{
    (void)area;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    if (!ctx->rend || !ctx->term || !ctx->sdl_renderer)
        return;

    // Always fill background black (avoids white flash on resize)
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    // Only render terminal content if needed
    if (!terminal_needs_redraw(ctx->term) && !ctx->force_redraw)
        return;

    // Update window title if changed
    const char *title = terminal_get_title(ctx->term);
    if (title) {
        if (!ctx->last_title || strcmp(ctx->last_title, title) != 0) {
            free(ctx->last_title);
            ctx->last_title = strdup(title);
            gtk_window_set_title(ctx->window, title);
            adw_header_bar_set_title_widget(
                ADW_HEADER_BAR(ctx->header_bar),
                GTK_WIDGET(adw_window_title_new(title, NULL)));
        }
    }

    int scale = ctx->scale_factor;
    int phys_w = width * scale;
    int phys_h = height * scale;

    if (phys_w <= 0 || phys_h <= 0)
        return;

    // Ensure renderer internal state matches drawing area
    renderer_resize(ctx->rend, width, height);

    // Create/resize render target texture if needed
    if (!ctx->render_target || ctx->target_w != phys_w ||
        ctx->target_h != phys_h) {
        if (ctx->render_target)
            SDL_DestroyTexture(ctx->render_target);
        ctx->render_target = SDL_CreateTexture(
            ctx->sdl_renderer, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, phys_w, phys_h);
        ctx->target_w = phys_w;
        ctx->target_h = phys_h;
        if (!ctx->render_target) {
            vlog("Failed to create render target %dx%d: %s\n", phys_w, phys_h,
                 SDL_GetError());
            return;
        }
        vlog("Created render target texture %dx%d\n", phys_w, phys_h);
    }

    // Render terminal into the render target texture
    SDL_SetRenderTarget(ctx->sdl_renderer, ctx->render_target);

    bool cursor_vis =
        !terminal_get_cursor_blink(ctx->term) || ctx->cursor_blink_visible;
    renderer_draw_terminal(ctx->rend, ctx->term, cursor_vis);

    // Read pixels back from the render target
    SDL_Surface *surface = SDL_RenderReadPixels(ctx->sdl_renderer, NULL);
    SDL_SetRenderTarget(ctx->sdl_renderer, NULL);

    if (!surface) {
        vlog("SDL_RenderReadPixels failed: %s\n", SDL_GetError());
        return;
    }

    // Convert to ARGB32 (Cairo uses ARGB32 premultiplied)
    SDL_Surface *argb_surface =
        SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surface);
    if (!argb_surface) {
        vlog("SDL_ConvertSurface failed: %s\n", SDL_GetError());
        return;
    }

    // Create Cairo surface from pixel data
    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        (unsigned char *)argb_surface->pixels, CAIRO_FORMAT_ARGB32,
        argb_surface->w, argb_surface->h, argb_surface->pitch);

    // Scale from physical to logical pixels and paint
    cairo_save(cr);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr, cairo_surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(cairo_surface);
    SDL_DestroySurface(argb_surface);

    terminal_clear_redraw(ctx->term);
    ctx->force_redraw = false;
}

// Key press handler
static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks)
        return FALSE;

    int tmod = gdk_mod_to_term(state);

    // Look up special keys
    int term_key = TERM_KEY_NONE;
    for (int i = 0; i < (int)(sizeof(gdk_key_map) / sizeof(gdk_key_map[0])); i++) {
        if (gdk_key_map[i].gdk_key == keyval) {
            term_key = gdk_key_map[i].term_key;
            break;
        }
    }

    if (term_key != TERM_KEY_NONE) {
        // Special key found
        if (ctx->callbacks->on_key) {
            KeyboardResult result =
                ctx->callbacks->on_key(ctx->callbacks->user_data, term_key, tmod, 0);
            handle_keyboard_result(ctx, &result);
        }
        return TRUE;
    }

    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
        // Ctrl/Alt + printable: resolve codepoint
        // Use the unmodified keyval (lowercase unless shift held)
        uint32_t cp = gdk_keyval_to_unicode(keyval);
        if (cp >= 32 && cp < 127) {
            // Lowercase if Shift not held
            if (cp >= 'A' && cp <= 'Z' && !(state & GDK_SHIFT_MASK))
                cp = cp - 'A' + 'a';
            if (ctx->callbacks->on_key) {
                KeyboardResult result = ctx->callbacks->on_key(
                    ctx->callbacks->user_data, TERM_KEY_NONE, tmod, cp);
                handle_keyboard_result(ctx, &result);
            }
            return TRUE;
        }
    }

    // Let IME handle it
    if (gtk_im_context_filter_keypress(ctx->im_context,
                                       gtk_event_controller_get_current_event(
                                           GTK_EVENT_CONTROLLER(controller)))) {
        return TRUE;
    }

    return FALSE;
}

// Key release handler (for IME)
static void on_key_released(GtkEventControllerKey *controller,
                            guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data)
{
    (void)keyval;
    (void)keycode;
    (void)state;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    gtk_im_context_filter_keypress(ctx->im_context,
                                   gtk_event_controller_get_current_event(
                                       GTK_EVENT_CONTROLLER(controller)));
}

// IME commit handler (text input)
static void on_im_commit(GtkIMContext *im_context, const char *text,
                         gpointer user_data)
{
    (void)im_context;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks || !ctx->callbacks->on_text)
        return;

    KeyboardResult result =
        ctx->callbacks->on_text(ctx->callbacks->user_data, text);
    handle_keyboard_result(ctx, &result);
}

// Mouse click handler
static void on_click_pressed(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data)
{
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks || !ctx->callbacks->on_mouse)
        return;

    int button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(gesture));
    int tmod = gdk_mod_to_term(state);

    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, (int)x, (int)y,
                                 button, true, n_press, tmod)) {
        ctx->force_redraw = true;
        gtk_widget_queue_draw(ctx->drawing_area);
    }
}

static void on_click_released(GtkGestureClick *gesture, int n_press,
                              double x, double y, gpointer user_data)
{
    (void)n_press;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks || !ctx->callbacks->on_mouse)
        return;

    int button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(gesture));
    int tmod = gdk_mod_to_term(state);

    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, (int)x, (int)y,
                                 button, false, 0, tmod)) {
        ctx->force_redraw = true;
        gtk_widget_queue_draw(ctx->drawing_area);
    }
}

// Mouse motion handler
static void on_motion(GtkEventControllerMotion *controller, double x, double y,
                      gpointer user_data)
{
    (void)controller;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks || !ctx->callbacks->on_mouse)
        return;

    GdkModifierType state =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(controller));
    int tmod = gdk_mod_to_term(state);

    // Check if any button is pressed
    bool any_pressed = (state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) != 0;

    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, (int)x, (int)y,
                                 0, any_pressed, 0, tmod)) {
        ctx->force_redraw = true;
        gtk_widget_queue_draw(ctx->drawing_area);
    }
}

// Scroll handler
static gboolean on_scroll(GtkEventControllerScroll *controller,
                          double dx, double dy, gpointer user_data)
{
    (void)controller;
    (void)dx;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (!ctx->callbacks)
        return FALSE;

    if (dy == 0.0)
        return FALSE;

    // Try forwarding as mouse event first (for mouse mode)
    bool consumed = false;
    if (ctx->callbacks->on_mouse) {
        int button = (dy < 0) ? 4 : 5;
        GdkModifierType state =
            gtk_event_controller_get_current_event_state(
                GTK_EVENT_CONTROLLER(controller));
        int tmod = gdk_mod_to_term(state);

        // Get mouse position relative to drawing area
        double mx = 0, my = 0;
        GdkSurface *surface = gtk_native_get_surface(
            gtk_widget_get_native(ctx->drawing_area));
        if (surface) {
            GdkDevice *pointer = gdk_seat_get_pointer(
                gdk_display_get_default_seat(gdk_display_get_default()));
            if (pointer) {
                gdk_surface_get_device_position(surface, pointer, &mx, &my, NULL);
                // Adjust for header bar offset using non-deprecated API
                graphene_point_t src_pt = GRAPHENE_POINT_INIT((float)mx, (float)my);
                graphene_point_t dst_pt;
                if (gtk_widget_compute_point(GTK_WIDGET(ctx->window),
                                             ctx->drawing_area, &src_pt, &dst_pt)) {
                    mx = dst_pt.x;
                    my = dst_pt.y;
                }
            }
        }

        int clicks_count = (int)(fabs(dy));
        if (clicks_count < 1)
            clicks_count = 1;
        for (int i = 0; i < clicks_count && !consumed; i++) {
            consumed = ctx->callbacks->on_mouse(ctx->callbacks->user_data,
                                                (int)mx, (int)my, button,
                                                true, 0, tmod);
        }
    }

    // Fallback to scroll callback
    if (!consumed && ctx->callbacks->on_scroll) {
        ctx->callbacks->on_scroll(ctx->callbacks->user_data, (int)(-dy * 3));
    }

    ctx->force_redraw = true;
    gtk_widget_queue_draw(ctx->drawing_area);
    return TRUE;
}

// Focus handlers
static void on_focus_enter(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    (void)controller;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    ctx->has_focus = true;
    ctx->force_redraw = true;
    gtk_im_context_focus_in(ctx->im_context);
    gtk_widget_queue_draw(ctx->drawing_area);
}

static void on_focus_leave(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    (void)controller;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    ctx->has_focus = false;
    ctx->force_redraw = true;
    if (ctx->im_context && ctx->drawing_area &&
        gtk_widget_get_mapped(ctx->drawing_area))
        gtk_im_context_focus_out(ctx->im_context);
    gtk_widget_queue_draw(ctx->drawing_area);
}

// Drawing area resize handler
static void on_drawing_area_resize(GtkDrawingArea *area, int width, int height,
                                   gpointer user_data)
{
    (void)area;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    vlog("Drawing area resized to %dx%d (logical)\n", width, height);

    // Update scale factor
    ctx->scale_factor = gtk_widget_get_scale_factor(ctx->drawing_area);
    renderer_set_pixel_density(ctx->rend, (float)ctx->scale_factor);

    // Notify main.c callback (updates terminal dimensions, PTY, renderer)
    if (ctx->callbacks && ctx->callbacks->on_resize)
        ctx->callbacks->on_resize(ctx->callbacks->user_data, width, height);
    ctx->force_redraw = true;
}

// PTY I/O watch callback
static gboolean on_pty_data(GIOChannel *source, GIOCondition condition,
                            gpointer user_data)
{
    (void)source;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        vlog("PTY closed (condition=0x%x)\n", condition);
        g_main_loop_quit(ctx->main_loop);
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_IN) {
        char buf[4096];
        ssize_t n = pty_read(ctx->pty, buf, sizeof(buf));
        if (n > 0) {
            int scroll_off = renderer_get_scroll_offset(ctx->rend);
            int old_sb = 0;
            if (scroll_off > 0)
                old_sb = terminal_get_scrollback_lines(ctx->term);

            terminal_process_input(ctx->term, buf, (size_t)n);

            // If user is scrolled up, compensate for new scrollback
            if (scroll_off > 0) {
                int delta = terminal_get_scrollback_lines(ctx->term) - old_sb;
                if (delta > 0)
                    renderer_scroll(ctx->rend, ctx->term, delta);
            }

            ctx->force_redraw = true;
            gtk_widget_queue_draw(ctx->drawing_area);
        } else if (n == 0) {
            vlog("PTY EOF\n");
            g_main_loop_quit(ctx->main_loop);
            return G_SOURCE_REMOVE;
        } else if (errno != EAGAIN && errno != EINTR) {
            vlog("PTY read error: %s\n", strerror(errno));
            g_main_loop_quit(ctx->main_loop);
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

// SIGCHLD watch callback
static gboolean on_sigchld(GIOChannel *source, GIOCondition condition,
                           gpointer user_data)
{
    (void)source;
    (void)condition;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    pty_signal_drain();

    if (!pty_is_running(ctx->pty)) {
        vlog("Child process has exited\n");
        g_main_loop_quit(ctx->main_loop);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

// Cursor blink timer
static gboolean on_cursor_blink(gpointer user_data)
{
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (ctx->term && terminal_get_cursor_blink(ctx->term)) {
        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
        ctx->force_redraw = true;
        gtk_widget_queue_draw(ctx->drawing_area);
    }
    return G_SOURCE_CONTINUE;
}

// Unix signal handler (SIGINT, SIGTERM)
static gboolean on_unix_signal(gpointer user_data)
{
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    vlog("Received signal, quitting\n");
    // Clear both IDs since G_SOURCE_REMOVE auto-removes this source
    ctx->sigint_id = 0;
    ctx->sigterm_id = 0;
    g_main_loop_quit(ctx->main_loop);
    return G_SOURCE_REMOVE;
}

// Window close handler
static gboolean on_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    vlog("Window close requested\n");
    g_main_loop_quit(ctx->main_loop);
    return TRUE; // We handle it
}

// Clipboard paste callback (async)
typedef struct
{
    GTK4PlatformData *ctx;
} ClipboardPasteData;

static void clipboard_read_callback(GObject *source_object, GAsyncResult *res,
                                    gpointer user_data)
{
    ClipboardPasteData *paste_data = (ClipboardPasteData *)user_data;
    GTK4PlatformData *ctx = paste_data->ctx;
    free(paste_data);

    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    if (text && text[0] != '\0') {
        terminal_start_paste(ctx->term);
        pty_write(ctx->pty, text, strlen(text));
        terminal_end_paste(ctx->term);
    }
    g_free(text);
}

// Forward declarations
static bool gtk4_plat_init(PlatformBackend *plat);
static void gtk4_plat_destroy(PlatformBackend *plat);
static bool gtk4_create_window(PlatformBackend *plat, const char *title,
                               int width, int height);
static void gtk4_show_window(PlatformBackend *plat);
static void gtk4_set_window_size(PlatformBackend *plat, int width, int height);
static void gtk4_set_window_title(PlatformBackend *plat, const char *title);
static void *gtk4_get_sdl_renderer(PlatformBackend *plat);
static void *gtk4_get_sdl_window(PlatformBackend *plat);
static char *gtk4_clipboard_get(PlatformBackend *plat);
static bool gtk4_clipboard_set(PlatformBackend *plat, const char *text);
static void gtk4_clipboard_free(PlatformBackend *plat, char *text);
static bool gtk4_clipboard_paste_async(PlatformBackend *plat,
                                       TerminalBackend *term, PtyContext *pty);
static bool gtk4_register_pty(PlatformBackend *plat, PtyContext *pty);
static void gtk4_run(PlatformBackend *plat, TerminalBackend *term,
                     RendererBackend *rend, PlatformCallbacks *callbacks);
static void gtk4_request_quit(PlatformBackend *plat);

// Backend definition
PlatformBackend platform_backend_gtk4 = {
    .name = "gtk4",
    .backend_data = NULL,
    .init = gtk4_plat_init,
    .destroy = gtk4_plat_destroy,
    .create_window = gtk4_create_window,
    .show_window = gtk4_show_window,
    .set_window_size = gtk4_set_window_size,
    .set_window_title = gtk4_set_window_title,
    .get_sdl_renderer = gtk4_get_sdl_renderer,
    .get_sdl_window = gtk4_get_sdl_window,
    .clipboard_get = gtk4_clipboard_get,
    .clipboard_set = gtk4_clipboard_set,
    .clipboard_free = gtk4_clipboard_free,
    .clipboard_paste_async = gtk4_clipboard_paste_async,
    .register_pty = gtk4_register_pty,
    .run = gtk4_run,
    .request_quit = gtk4_request_quit,
};

static bool gtk4_plat_init(PlatformBackend *plat)
{
    vlog("Initializing GTK4/libadwaita platform\n");

    // Initialize libadwaita (also initializes GTK4)
    adw_init();

    // Initialize SDL video (needed for offscreen rendering)
    if (!SDL_SetAppMetadata("bloom-terminal", "1.0.0", "bloom-terminal")) {
        fprintf(stderr, "WARNING: Failed to set SDL app metadata: %s\n",
                SDL_GetError());
    }

    // Set SDL to use offscreen driver to avoid conflict with GTK's display
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "ERROR: Failed to initialize SDL video: %s\n",
                SDL_GetError());
        return false;
    }

    // Allocate context
    GTK4PlatformData *ctx = calloc(1, sizeof(GTK4PlatformData));
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate platform context\n");
        SDL_Quit();
        return false;
    }

    ctx->cursor_blink_visible = true;
    ctx->has_focus = true;
    ctx->force_redraw = true;
    ctx->scale_factor = 1;

    // Create hidden SDL window for offscreen rendering
    ctx->sdl_window = SDL_CreateWindow("bloom-terminal-offscreen", 800, 600,
                                       SDL_WINDOW_HIDDEN);
    if (!ctx->sdl_window) {
        fprintf(stderr, "ERROR: Failed to create offscreen SDL window: %s\n",
                SDL_GetError());
        free(ctx);
        SDL_Quit();
        return false;
    }

    ctx->sdl_renderer = SDL_CreateRenderer(ctx->sdl_window, NULL);
    if (!ctx->sdl_renderer) {
        fprintf(stderr, "ERROR: Failed to create SDL renderer: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(ctx->sdl_window);
        free(ctx);
        SDL_Quit();
        return false;
    }

    // Disable VSync for offscreen rendering
    SDL_SetRenderVSync(ctx->sdl_renderer, 0);

    vlog("GTK4 platform initialized with offscreen SDL renderer\n");

    plat->backend_data = ctx;
    return true;
}

static void gtk4_plat_destroy(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    // Remove watches
    if (ctx->pty_watch_id) {
        g_source_remove(ctx->pty_watch_id);
        ctx->pty_watch_id = 0;
    }
    if (ctx->signal_watch_id) {
        g_source_remove(ctx->signal_watch_id);
        ctx->signal_watch_id = 0;
    }
    if (ctx->cursor_blink_timer_id) {
        g_source_remove(ctx->cursor_blink_timer_id);
        ctx->cursor_blink_timer_id = 0;
    }

    // Destroy GIO channels
    if (ctx->pty_channel) {
        g_io_channel_unref(ctx->pty_channel);
        ctx->pty_channel = NULL;
    }
    if (ctx->signal_channel) {
        g_io_channel_unref(ctx->signal_channel);
        ctx->signal_channel = NULL;
    }

    // Destroy IM context
    if (ctx->im_context) {
        g_object_unref(ctx->im_context);
        ctx->im_context = NULL;
    }

    // Destroy main loop
    if (ctx->main_loop) {
        g_main_loop_unref(ctx->main_loop);
        ctx->main_loop = NULL;
    }

    // Destroy GTK window
    if (ctx->window) {
        gtk_window_destroy(ctx->window);
        ctx->window = NULL;
    }

    // Destroy SDL resources
    if (ctx->render_target) {
        SDL_DestroyTexture(ctx->render_target);
        ctx->render_target = NULL;
    }
    if (ctx->sdl_renderer) {
        SDL_DestroyRenderer(ctx->sdl_renderer);
        ctx->sdl_renderer = NULL;
    }
    if (ctx->sdl_window) {
        SDL_DestroyWindow(ctx->sdl_window);
        ctx->sdl_window = NULL;
    }

    free(ctx->last_title);
    free(ctx);
    plat->backend_data = NULL;

    SDL_Quit();
}

static bool gtk4_create_window(PlatformBackend *plat, const char *title,
                               int width, int height)
{
    if (!plat || !plat->backend_data)
        return false;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    // Create AdwWindow (single integrated CSD header bar)
    ctx->window = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(ctx->window, title);
    gtk_window_set_default_size(ctx->window, width, height);

    // Create header bar
    ctx->header_bar = adw_header_bar_new();
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(ctx->header_bar),
        GTK_WIDGET(adw_window_title_new(title, NULL)));

    // Create drawing area for terminal content
    ctx->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(ctx->drawing_area, TRUE);
    gtk_widget_set_vexpand(ctx->drawing_area, TRUE);
    gtk_widget_set_focusable(ctx->drawing_area, TRUE);
    gtk_widget_set_can_focus(ctx->drawing_area, TRUE);

    // Use AdwToolbarView to integrate header bar with content
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view),
                                 ctx->header_bar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view),
                                 ctx->drawing_area);
    adw_window_set_content(ADW_WINDOW(ctx->window), toolbar_view);

    // Set draw function
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ctx->drawing_area),
                                   draw_func, ctx, NULL);

    // Connect resize signal
    g_signal_connect(ctx->drawing_area, "resize",
                     G_CALLBACK(on_drawing_area_resize), ctx);

    // Connect close request
    g_signal_connect(ctx->window, "close-request",
                     G_CALLBACK(on_close_request), ctx);

    // Set up IM context
    ctx->im_context = gtk_im_multicontext_new();
    g_signal_connect(ctx->im_context, "commit",
                     G_CALLBACK(on_im_commit), ctx);

    // Store initial content size
    ctx->content_width = width;
    ctx->content_height = height;

    vlog("GTK4 window created (%dx%d)\n", width, height);
    return true;
}

static void gtk4_show_window(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (ctx->window) {
        gtk_window_present(ctx->window);
        // Focus the drawing area
        gtk_widget_grab_focus(ctx->drawing_area);
    }
}

static void gtk4_set_window_size(PlatformBackend *plat, int width, int height)
{
    if (!plat || !plat->backend_data)
        return;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    ctx->content_width = width;
    ctx->content_height = height;
    if (ctx->window) {
        gtk_window_set_default_size(ctx->window, width, height);
    }
}

static void gtk4_set_window_title(PlatformBackend *plat, const char *title)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    // Skip if title unchanged
    if (ctx->last_title && title && strcmp(ctx->last_title, title) == 0)
        return;
    if (!ctx->last_title && !title)
        return;

    free(ctx->last_title);
    ctx->last_title = title ? strdup(title) : NULL;

    if (ctx->window) {
        gtk_window_set_title(ctx->window, title ? title : "bloom-terminal");
        if (ctx->header_bar) {
            adw_header_bar_set_title_widget(
                ADW_HEADER_BAR(ctx->header_bar),
                GTK_WIDGET(adw_window_title_new(
                    title ? title : "bloom-terminal", NULL)));
        }
    }
}

static void *gtk4_get_sdl_renderer(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return NULL;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    return ctx->sdl_renderer;
}

static void *gtk4_get_sdl_window(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return NULL;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    return ctx->sdl_window;
}

static char *gtk4_clipboard_get(PlatformBackend *plat)
{
    // Synchronous clipboard get is not ideal in GTK4, but needed for
    // on_key/on_mouse paste path. Return NULL here — Ctrl+Shift+V paste
    // is handled asynchronously via the clipboard_read_callback.
    // Right-click paste also uses async path.
    (void)plat;
    return NULL;
}

static bool gtk4_clipboard_set(PlatformBackend *plat, const char *text)
{
    if (!plat || !plat->backend_data || !text)
        return false;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    GdkClipboard *clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(ctx->window)));
    gdk_clipboard_set_text(clipboard, text);
    return true;
}

static void gtk4_clipboard_free(PlatformBackend *plat, char *text)
{
    (void)plat;
    // Our clipboard_get returns NULL, nothing to free.
    // If we ever return g_strdup'd text, use g_free here.
    (void)text;
}

static bool gtk4_clipboard_paste_async(PlatformBackend *plat,
                                       TerminalBackend *term, PtyContext *pty)
{
    if (!plat || !plat->backend_data)
        return false;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    (void)term;
    (void)pty;

    GdkClipboard *clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(ctx->window)));

    ClipboardPasteData *paste_data = malloc(sizeof(ClipboardPasteData));
    if (!paste_data)
        return false;
    paste_data->ctx = ctx;

    gdk_clipboard_read_text_async(clipboard, NULL, clipboard_read_callback,
                                  paste_data);
    return true;
}

static bool gtk4_register_pty(PlatformBackend *plat, PtyContext *pty)
{
    if (!plat || !plat->backend_data || !pty)
        return false;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    ctx->pty = pty;
    return true;
}

static void gtk4_run(PlatformBackend *plat, TerminalBackend *term,
                     RendererBackend *rend, PlatformCallbacks *callbacks)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    if (!ctx->pty) {
        fprintf(stderr, "ERROR: No PTY registered with platform\n");
        return;
    }

    // Store references for draw_func and event handlers
    ctx->term = term;
    ctx->rend = rend;
    ctx->callbacks = callbacks;
    ctx->plat = plat;

    // Get scale factor
    ctx->scale_factor = gtk_widget_get_scale_factor(ctx->drawing_area);
    renderer_set_pixel_density(rend, (float)ctx->scale_factor);
    vlog("GTK4 scale factor: %d\n", ctx->scale_factor);

    // Set up event controllers on drawing area
    // Keyboard
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed",
                     G_CALLBACK(on_key_pressed), ctx);
    g_signal_connect(key_controller, "key-released",
                     G_CALLBACK(on_key_released), ctx);
    gtk_widget_add_controller(ctx->drawing_area, key_controller);

    // Set IM context client widget
    gtk_im_context_set_client_widget(ctx->im_context, ctx->drawing_area);

    // Mouse click (all buttons)
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); // all buttons
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), ctx);
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), ctx);
    gtk_widget_add_controller(ctx->drawing_area, GTK_EVENT_CONTROLLER(click));

    // Mouse motion
    GtkEventController *motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_motion), ctx);
    gtk_widget_add_controller(ctx->drawing_area, motion_controller);

    // Scroll
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
        GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll), ctx);
    gtk_widget_add_controller(ctx->drawing_area, scroll_controller);

    // Focus
    GtkEventController *focus_controller = gtk_event_controller_focus_new();
    g_signal_connect(focus_controller, "enter",
                     G_CALLBACK(on_focus_enter), ctx);
    g_signal_connect(focus_controller, "leave",
                     G_CALLBACK(on_focus_leave), ctx);
    gtk_widget_add_controller(ctx->drawing_area, focus_controller);

    // Set up PTY I/O watch
    int pty_fd = pty_get_master_fd(ctx->pty);
    ctx->pty_channel = g_io_channel_unix_new(pty_fd);
    g_io_channel_set_encoding(ctx->pty_channel, NULL, NULL);
    g_io_channel_set_buffered(ctx->pty_channel, FALSE);
    g_io_channel_set_flags(ctx->pty_channel,
                           g_io_channel_get_flags(ctx->pty_channel) | G_IO_FLAG_NONBLOCK,
                           NULL);
    ctx->pty_watch_id = g_io_add_watch(ctx->pty_channel,
                                       G_IO_IN | G_IO_ERR | G_IO_HUP,
                                       on_pty_data, ctx);

    // Set up SIGCHLD watch
    int signal_fd = pty_signal_get_fd();
    if (signal_fd >= 0) {
        ctx->signal_channel = g_io_channel_unix_new(signal_fd);
        g_io_channel_set_encoding(ctx->signal_channel, NULL, NULL);
        g_io_channel_set_buffered(ctx->signal_channel, FALSE);
        ctx->signal_watch_id = g_io_add_watch(ctx->signal_channel,
                                              G_IO_IN, on_sigchld, ctx);
    }

    // Start cursor blink timer
    ctx->cursor_blink_visible = true;
    ctx->cursor_blink_timer_id =
        g_timeout_add(CURSOR_BLINK_INTERVAL_MS, on_cursor_blink, ctx);

    // Handle SIGINT/SIGTERM for clean shutdown (e.g. Ctrl+C in parent shell)
    ctx->sigint_id = g_unix_signal_add(SIGINT, on_unix_signal, ctx);
    ctx->sigterm_id = g_unix_signal_add(SIGTERM, on_unix_signal, ctx);

    // Create main loop and run
    ctx->main_loop = g_main_loop_new(NULL, FALSE);
    vlog("GTK4 event loop starting\n");
    g_main_loop_run(ctx->main_loop);
    vlog("GTK4 event loop exiting\n");

    // Cleanup signal watches and timer
    if (ctx->sigint_id)
        g_source_remove(ctx->sigint_id);
    if (ctx->sigterm_id)
        g_source_remove(ctx->sigterm_id);
    if (ctx->cursor_blink_timer_id) {
        g_source_remove(ctx->cursor_blink_timer_id);
        ctx->cursor_blink_timer_id = 0;
    }
}

static void gtk4_request_quit(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (ctx->main_loop)
        g_main_loop_quit(ctx->main_loop);
}
