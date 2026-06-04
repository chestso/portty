#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_version.h"

#include "bloom_bug.h"
#include "common.h"
#include "gtk4_vulkan.h"
#include "platform_gtk4.h"
#include <SDL3/SDL.h>
#include <adwaita.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#include <errno.h>
#include <glib-unix.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    AdwWindowTitle *window_title;
    GtkIMContext *im_context;
    GMainLoop *main_loop;

    // SDL (offscreen)
    SDL_Window *sdl_window;
    SDL_Renderer *sdl_renderer;

    // Render target texture (sized to drawing area)
    SDL_Texture *render_target;
    int target_w, target_h;

#ifdef HAVE_VULKAN_DMABUF
    // Zero-copy via Vulkan: bloom owns the VkInstance/VkDevice and exports the
    // render target as a DMA-BUF. When true, render_target aliases
    // vk_target.texture (an exportable VkImage wrapped as an SDL render target).
    bool vulkan_dmabuf;
    bool vk_export_verified;
    BloomVk vk;
    BloomVkTarget vk_target;
#endif

    // Last displayed frame: reused on a no-redraw snapshot and passed to GTK
    // as the update-texture hint for the next zero-copy frame.
    GdkTexture *prev_texture;

    // PTY
    PtyContext *pty;
    GIOChannel *pty_channel;
    guint pty_watch_id;
    bool pty_paused;
    GIOChannel *signal_channel;
    guint signal_watch_id;

    // Timer
    guint cursor_blink_timer_id;
    guint autoscroll_timer_id;
    bool cursor_blink_visible;

    // Cached exe path (resolved once at startup, avoids " (deleted)" issue)
    char exe_path[PATH_MAX];

    // Render state
    bool force_redraw;
    bool has_focus;

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

    /* Detection counters. Split into "lifecycle" (just count) and
     * "bug indicators" (the corresponding code path also calls
     * BLOOM_BUG_ABORT). Dumped via bloom_bug on any abort and on
     * clean shutdown. */
    struct
    {
        /* Lifecycle */
        uint64_t frame_count;
        uint32_t readback_path_taken;

        /* Bug indicators — incremented just before BLOOM_BUG_ABORT
         * so the post-mortem dump shows which slot was hit. */
        uint32_t dup_dmabuf_failures;
        uint32_t gdk_builder_failures;
        uint32_t render_target_create_failures;
    } gl_stats;
} GTK4PlatformData;

/* Singleton pointer so the bloom_bug dump hook (no user argument) can
 * reach the live ctx. There is only ever one GTK4 backend per
 * process. */
static GTK4PlatformData *gtk4_singleton = NULL;

/* bloom_bug post-mortem hook — dumps the gl_stats counters so an abort
 * anywhere in the binary leaves a trail of what the GTK4 GL/DMA-BUF
 * lifecycle did in the run-up. Registered from gtk4_plat_init. */
static void gtk4_dump_gl_stats(void)
{
    GTK4PlatformData *ctx = gtk4_singleton;
    if (!ctx)
        return;
    fprintf(stderr,
            "DMABUF_STATS frame=%" PRIu64 " readback=%u\n",
            ctx->gl_stats.frame_count, ctx->gl_stats.readback_path_taken);
    fprintf(stderr,
            "DMABUF_BUGS dup_failures=%u gdk_builder_failures=%u"
            " render_target_failures=%u\n",
            ctx->gl_stats.dup_dmabuf_failures,
            ctx->gl_stats.gdk_builder_failures,
            ctx->gl_stats.render_target_create_failures);
}

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
        if (result->len > 0 && !result->handled && ctx->pty) {
            ssize_t written = pty_write(ctx->pty, result->data, result->len);
            if (written < 0) {
                vlog("PTY write failed: %s\n", strerror(errno));
            }
        }

        gtk_widget_queue_draw(ctx->drawing_area);
    }
}

// GDestroyNotify callback to close dup'd fd when GTK is done with texture
static void close_dmabuf_fd(gpointer data)
{
    int fd = (int)(intptr_t)data;
    if (fd >= 0)
        close(fd);
}

// BloomTerminalArea — GtkDrawingArea subclass with snapshot override

#define BLOOM_TYPE_TERMINAL_AREA (bloom_terminal_area_get_type())
G_DECLARE_FINAL_TYPE(BloomTerminalArea, bloom_terminal_area, BLOOM,
                     TERMINAL_AREA, GtkDrawingArea)

struct _BloomTerminalArea
{
    GtkDrawingArea parent_instance;
    GTK4PlatformData *ctx;
};

G_DEFINE_TYPE(BloomTerminalArea, bloom_terminal_area, GTK_TYPE_DRAWING_AREA)

static void bloom_terminal_area_snapshot(GtkWidget *widget,
                                         GtkSnapshot *snapshot)
{
    BloomTerminalArea *self = BLOOM_TERMINAL_AREA(widget);
    GTK4PlatformData *ctx = self->ctx;

    if (!ctx || !ctx->rend || !ctx->term || !ctx->sdl_renderer)
        return;

    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    int scale = ctx->scale_factor;
    int phys_w = width * scale;
    int phys_h = height * scale;

    // Always paint black background (avoids white flash on resize)
    gtk_snapshot_append_color(
        snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
        &GRAPHENE_RECT_INIT(0, 0, width, height));

    bool needs_render =
        terminal_needs_redraw(ctx->term) || ctx->force_redraw;

    // If nothing changed and we have a cached texture, reuse it
    if (!needs_render && ctx->prev_texture) {
        gtk_snapshot_append_texture(
            snapshot, ctx->prev_texture,
            &GRAPHENE_RECT_INIT(0, 0, width, height));
        return;
    }

    if (!needs_render)
        return;

    if (phys_w <= 0 || phys_h <= 0)
        return;

    // Ensure renderer internal state matches drawing area (physical pixels)
    renderer_resize(ctx->rend, phys_w, phys_h);

    ctx->gl_stats.frame_count++;

    // Create/resize render target texture if needed
    if (!ctx->render_target || ctx->target_w != phys_w ||
        ctx->target_h != phys_h) {
#ifdef HAVE_VULKAN_DMABUF
        if (ctx->vulkan_dmabuf) {
            // The render target is an exportable VkImage wrapped as an SDL
            // texture; bloom_vk_target_create destroys the previous one, so we
            // must not SDL_DestroyTexture(render_target) here (it aliases it).
            g_clear_object(&ctx->prev_texture);
            if (!bloom_vk_target_create(&ctx->vk, ctx->sdl_renderer, phys_w,
                                        phys_h, &ctx->vk_target)) {
                ctx->gl_stats.render_target_create_failures++;
                vlog("Vulkan dmabuf target create failed at %dx%d\n", phys_w,
                     phys_h);
                ctx->render_target = NULL;
                return;
            }
            ctx->render_target = ctx->vk_target.texture;
            ctx->target_w = phys_w;
            ctx->target_h = phys_h;
            goto render_target_ready;
        }
#endif
        if (ctx->render_target)
            SDL_DestroyTexture(ctx->render_target);
        ctx->render_target = SDL_CreateTexture(
            ctx->sdl_renderer, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, phys_w, phys_h);
        // A1.4 — NULL check before any use of render_target.
        if (!ctx->render_target) {
            ctx->gl_stats.render_target_create_failures++;
            vlog("Failed to create render target %dx%d: %s\n", phys_w,
                 phys_h, SDL_GetError());
            return;
        }
        ctx->target_w = phys_w;
        ctx->target_h = phys_h;
    }

#ifdef HAVE_VULKAN_DMABUF
render_target_ready:;
#endif
    // Render terminal into SDL's render target texture.
    SDL_SetRenderTarget(ctx->sdl_renderer, NULL);
    if (!SDL_SetRenderTarget(ctx->sdl_renderer, ctx->render_target)) {
        vlog("SDL_SetRenderTarget failed: %s\n", SDL_GetError());
        return;
    }

    bool cursor_vis =
        !ctx->has_focus || !terminal_get_cursor_blink(ctx->term) || ctx->cursor_blink_visible;
    renderer_draw_terminal(ctx->rend, ctx->term, cursor_vis);

#ifdef HAVE_VULKAN_DMABUF
    if (ctx->vulkan_dmabuf) {
        // Ensure SDL's render into the exportable VkImage completes before the
        // compositor scans out the DMA-BUF.
        SDL_FlushRenderer(ctx->sdl_renderer);
        bloom_vk_finish(&ctx->vk);

        // One-time sanity check: confirm the exportable image actually received
        // content (guards against a silently-blank export, like the GL path).
        if (!ctx->vk_export_verified) {
            ctx->vk_export_verified = true;
            SDL_Surface *s = SDL_RenderReadPixels(ctx->sdl_renderer, NULL);
            if (s) {
                int nonzero = 0;
                const uint8_t *px = s->pixels;
                int n = (s->w < 64 ? s->w : 64) * 4;
                for (int i = 0; i < n; i++)
                    if (px[i])
                        nonzero++;
                if (nonzero == 0)
                    BLOOM_BUG_ABORT(
                        "Vulkan DMA-BUF export verify: blank render at %dx%d",
                        phys_w, phys_h);
                vlog("Vulkan DMA-BUF export verified (%d non-zero bytes)\n",
                     nonzero);
                SDL_DestroySurface(s);
            }
        }

        int fd = dup(ctx->vk_target.dmabuf_fd);
        if (fd < 0) {
            ctx->gl_stats.dup_dmabuf_failures++;
            BLOOM_BUG_ABORT("dup(vk dmabuf_fd=%d) failed: %s",
                            ctx->vk_target.dmabuf_fd, strerror(errno));
        }

        GdkDmabufTextureBuilder *builder = gdk_dmabuf_texture_builder_new();
        gdk_dmabuf_texture_builder_set_display(builder,
                                               gtk_widget_get_display(widget));
        gdk_dmabuf_texture_builder_set_width(builder, phys_w);
        gdk_dmabuf_texture_builder_set_height(builder, phys_h);
        gdk_dmabuf_texture_builder_set_fourcc(builder, ctx->vk_target.fourcc);
        gdk_dmabuf_texture_builder_set_modifier(builder, ctx->vk_target.modifier);
        gdk_dmabuf_texture_builder_set_n_planes(builder, 1);
        gdk_dmabuf_texture_builder_set_fd(builder, 0, fd);
        gdk_dmabuf_texture_builder_set_stride(builder, 0, ctx->vk_target.stride);
        gdk_dmabuf_texture_builder_set_offset(builder, 0, ctx->vk_target.offset);
        gdk_dmabuf_texture_builder_set_premultiplied(builder, TRUE);
        if (ctx->prev_texture)
            gdk_dmabuf_texture_builder_set_update_texture(builder,
                                                          ctx->prev_texture);

        GError *error = NULL;
        GdkTexture *texture = gdk_dmabuf_texture_builder_build(
            builder, close_dmabuf_fd, (gpointer)(intptr_t)fd, &error);
        g_object_unref(builder);

        if (!texture) {
            ctx->gl_stats.gdk_builder_failures++;
            close(fd);
            BLOOM_BUG_ABORT(
                "gdk_dmabuf_texture_builder_build (vulkan) failed at %dx%d "
                "(fourcc=0x%x modifier=0x%llx stride=%u offset=%u): %s",
                phys_w, phys_h, ctx->vk_target.fourcc,
                (unsigned long long)ctx->vk_target.modifier,
                ctx->vk_target.stride, ctx->vk_target.offset,
                error ? error->message : "unknown");
        }

        gtk_snapshot_append_texture(snapshot, texture,
                                    &GRAPHENE_RECT_INIT(0, 0, width, height));
        g_clear_object(&ctx->prev_texture);
        ctx->prev_texture = texture;
        terminal_clear_redraw(ctx->term);
        ctx->force_redraw = false;
        return;
    }
#endif

    // Readback fallback — read pixels and create GdkMemoryTexture
    {
        SDL_Surface *surface = SDL_RenderReadPixels(ctx->sdl_renderer, NULL);
        SDL_SetRenderTarget(ctx->sdl_renderer, NULL);

        if (!surface) {
            vlog("SDL_RenderReadPixels failed: %s\n", SDL_GetError());
            return;
        }

        // SDL renders with premultiplied alpha
        GBytes *bytes = g_bytes_new(surface->pixels,
                                    surface->h * surface->pitch);
        GdkTexture *texture = gdk_memory_texture_new(
            surface->w, surface->h, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED,
            bytes, surface->pitch);
        g_bytes_unref(bytes);
        SDL_DestroySurface(surface);

        gtk_snapshot_append_texture(
            snapshot, texture,
            &GRAPHENE_RECT_INIT(0, 0, width, height));
        g_object_unref(texture);
    }

    terminal_clear_redraw(ctx->term);
    ctx->force_redraw = false;
}

static void bloom_terminal_area_init(BloomTerminalArea *self)
{
    (void)self;
}

static void bloom_terminal_area_measure(GtkWidget *widget,
                                        GtkOrientation orientation,
                                        int for_size, int *minimum,
                                        int *natural, int *minimum_baseline,
                                        int *natural_baseline)
{
    (void)for_size;
    BloomTerminalArea *self = BLOOM_TERMINAL_AREA(widget);
    GTK4PlatformData *ctx = self->ctx;

    *minimum = 1;
    *natural = 1;
    if (ctx) {
        *natural = (orientation == GTK_ORIENTATION_HORIZONTAL)
                       ? ctx->content_width
                       : ctx->content_height;
    }
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

static void bloom_terminal_area_class_init(BloomTerminalAreaClass *klass)
{
    GTK_WIDGET_CLASS(klass)->snapshot = bloom_terminal_area_snapshot;
    GTK_WIDGET_CLASS(klass)->measure = bloom_terminal_area_measure;
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

    int px = (int)(x * ctx->scale_factor);
    int py = (int)(y * ctx->scale_factor);
    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, px, py,
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

    int px = (int)(x * ctx->scale_factor);
    int py = (int)(y * ctx->scale_factor);
    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, px, py,
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

    int px = (int)(x * ctx->scale_factor);
    int py = (int)(y * ctx->scale_factor);
    if (ctx->callbacks->on_mouse(ctx->callbacks->user_data, px, py,
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

        int smx = (int)(mx * ctx->scale_factor);
        int smy = (int)(my * ctx->scale_factor);
        int clicks_count = (int)(fabs(dy));
        if (clicks_count < 1)
            clicks_count = 1;
        for (int i = 0; i < clicks_count && !consumed; i++) {
            consumed = ctx->callbacks->on_mouse(ctx->callbacks->user_data,
                                                smx, smy, button,
                                                true, 0, tmod);
        }
    }

    // Fallback to scroll callback
    if (!consumed && ctx->callbacks->on_scroll) {
        ctx->callbacks->on_scroll(ctx->callbacks->user_data, (int)(-dy * SCROLL_LINES_PER_TICK));
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

    // Update scale factor
    ctx->scale_factor = gtk_widget_get_scale_factor(ctx->drawing_area);
    renderer_set_content_scale(ctx->rend, (float)ctx->scale_factor);

    int phys_w = width * ctx->scale_factor;
    int phys_h = height * ctx->scale_factor;
    vlog("Drawing area resized to %dx%d (logical), %dx%d (physical)\n",
         width, height, phys_w, phys_h);

    // Notify main.c callback with physical dimensions (font metrics are
    // at physical DPI, so cols/rows must be computed in physical pixels)
    if (ctx->callbacks && ctx->callbacks->on_resize)
        ctx->callbacks->on_resize(ctx->callbacks->user_data, phys_w, phys_h);
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
            renderer_process_pty_data(ctx->rend, ctx->term, buf, (size_t)n);

            // Update window title if changed
            platform_set_window_title(ctx->plat, terminal_get_title(ctx->term));

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

    if (ctx->pty && !pty_is_running(ctx->pty)) {
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

// Drag-autoscroll tick (active while selection drag goes past viewport edge)
static gboolean on_autoscroll_tick_gtk(gpointer user_data)
{
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    if (ctx->callbacks && ctx->callbacks->on_autoscroll_tick) {
        ctx->callbacks->on_autoscroll_tick(ctx->callbacks->user_data);
        ctx->force_redraw = true;
        if (ctx->drawing_area)
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
    if (text && text[0] != '\0' && ctx->pty) {
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
static void gtk4_pause_pty(PlatformBackend *plat);
static void gtk4_resume_pty(PlatformBackend *plat);
static char *gtk4_get_default_font(PlatformBackend *plat);
static float gtk4_get_display_scale(PlatformBackend *plat);
static bool gtk4_get_display_size(PlatformBackend *plat, int *width, int *height);
static bool gtk4_open_url(PlatformBackend *plat, const char *url);
static void gtk4_set_cursor(PlatformBackend *plat, PlatformCursor cursor);
static void gtk4_set_autoscroll(PlatformBackend *plat, bool enabled);

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
    .pause_pty = gtk4_pause_pty,
    .resume_pty = gtk4_resume_pty,
    .get_default_font = gtk4_get_default_font,
    .get_display_scale = gtk4_get_display_scale,
    .get_display_size = gtk4_get_display_size,
    .open_url = gtk4_open_url,
    .set_cursor = gtk4_set_cursor,
    .set_autoscroll = gtk4_set_autoscroll,
};

static bool gtk4_plat_init(PlatformBackend *plat)
{
    vlog("Initializing GTK4/libadwaita platform\n");

    // Set program name so GTK4 sets the correct Wayland app_id,
    // allowing GNOME to match the window to bloom-terminal.desktop
    g_set_prgname("bloom-terminal");

    // Initialize libadwaita (also initializes GTK4)
    adw_init();

    // Initialize SDL video (needed for offscreen rendering)
    if (!SDL_SetAppMetadata("bloom-terminal", BLOOM_TERMINAL_VERSION, "bloom-terminal")) {
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
    gtk4_singleton = ctx;
    bloom_bug_register_dump(gtk4_dump_gl_stats);

    ctx->cursor_blink_visible = true;
    ctx->has_focus = true;
    ctx->force_redraw = true;
    ctx->scale_factor = 1;

    // Cache exe path now while the binary still exists on disk.
    // SDL_GetBasePath() caches internally and is platform-independent.
    const char *base = SDL_GetBasePath();
    if (base)
        snprintf(ctx->exe_path, sizeof(ctx->exe_path), "%s" PACKAGE, base);

    // bloom requires gamma-correct (linear-light) glyph blending, which only
    // SDL's Vulkan renderer provides (it linearizes -> blends -> re-encodes via
    // SRGB_LINEAR float targets; the OpenGL renderer blends in sRGB). The
    // Vulkan renderer also exposes the render target's VkImage + VkDevice, used
    // for zero-copy DMA-BUF export. Create a hidden Vulkan window + 'vulkan'
    // renderer under the offscreen video driver (no conflict with GTK's
    // display).
    ctx->sdl_window = SDL_CreateWindow("bloom-terminal-offscreen", 800, 600,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (ctx->sdl_window) {
        SDL_PropertiesID vrp = SDL_CreateProperties();
        if (vrp) {
            SDL_SetStringProperty(vrp, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan");
            SDL_SetPointerProperty(vrp, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
                                   ctx->sdl_window);
#ifdef HAVE_VULKAN_DMABUF
            // bloom owns Vulkan instance/device creation so the external-memory
            // extensions (needed for zero-copy DMA-BUF export) are enabled —
            // SDL's own vulkan device does not enable them.
            if (bloom_vk_init(&ctx->vk, ctx->sdl_window, vrp)) {
                ctx->sdl_renderer = SDL_CreateRendererWithProperties(vrp);
                if (ctx->sdl_renderer)
                    ctx->vulkan_dmabuf = true;
                else
                    bloom_vk_shutdown(&ctx->vk);
            }
#else
            ctx->sdl_renderer = SDL_CreateRendererWithProperties(vrp);
#endif
            SDL_DestroyProperties(vrp);
        }
        if (ctx->sdl_renderer) {
#ifdef HAVE_VULKAN_DMABUF
            vlog("GTK4 using Vulkan renderer (linear-light blending, "
                 "own-device DMA-BUF zero-copy)\n");
#else
            vlog("GTK4 using Vulkan renderer (linear-light blending, readback)\n");
#endif
        } else {
            vlog("Vulkan renderer creation failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(ctx->sdl_window);
            ctx->sdl_window = NULL;
        }
    } else {
        vlog("Vulkan window creation failed: %s\n", SDL_GetError());
    }

    // Fallback: offscreen + any renderer (software)
    if (!ctx->sdl_window) {
        ctx->sdl_window = SDL_CreateWindow("bloom-terminal-offscreen", 800,
                                           600, SDL_WINDOW_HIDDEN);
        if (!ctx->sdl_window) {
            fprintf(stderr,
                    "ERROR: Failed to create offscreen SDL window: %s\n",
                    SDL_GetError());
            free(ctx);
            SDL_Quit();
            return false;
        }
    }
    if (!ctx->sdl_renderer) {
        ctx->sdl_renderer = SDL_CreateRenderer(ctx->sdl_window, NULL);
        if (!ctx->sdl_renderer) {
            fprintf(stderr, "ERROR: Failed to create SDL renderer: %s\n",
                    SDL_GetError());
            SDL_DestroyWindow(ctx->sdl_window);
            free(ctx);
            SDL_Quit();
            return false;
        }
    }

    // Disable VSync for offscreen rendering
    SDL_SetRenderVSync(ctx->sdl_renderer, 0);

    bool zc_enabled = false;
#ifdef HAVE_VULKAN_DMABUF
    zc_enabled = ctx->vulkan_dmabuf;
#endif
    vlog("GTK4 platform initialized (zero_copy=%s, renderer=%s)\n",
         zc_enabled ? "yes" : "no", SDL_GetRendererName(ctx->sdl_renderer));

    plat->backend_data = ctx;
    return true;
}

static void gtk4_plat_destroy(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    /* Dump stats on clean shutdown so successful sessions still surface
     * the readback distribution. Bug counters should all be zero — if any
     * are non-zero we'd have aborted earlier. */
    vlog("DMABUF_STATS frame=%" PRIu64 " readback=%u\n",
         ctx->gl_stats.frame_count, ctx->gl_stats.readback_path_taken);

    /* Clear singleton so the bloom_bug dump hook doesn't dereference
     * an about-to-be-freed ctx if anything aborts during teardown. */
    if (gtk4_singleton == ctx)
        gtk4_singleton = NULL;

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
    if (ctx->autoscroll_timer_id) {
        g_source_remove(ctx->autoscroll_timer_id);
        ctx->autoscroll_timer_id = 0;
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

#ifdef HAVE_VULKAN_DMABUF
    // Tear down the Vulkan export target before the SDL renderer (the renderer
    // runs on our VkDevice, so the device must outlive it — shut it down last).
    if (ctx->vulkan_dmabuf) {
        bloom_vk_finish(&ctx->vk);
        g_clear_object(&ctx->prev_texture);
        bloom_vk_target_destroy(&ctx->vk, &ctx->vk_target);
        ctx->render_target = NULL; // aliased the now-destroyed wrapped texture
    }
#endif

    // Destroy SDL resources
    if (ctx->render_target) {
        SDL_DestroyTexture(ctx->render_target);
        ctx->render_target = NULL;
    }
    if (ctx->sdl_renderer) {
        SDL_DestroyRenderer(ctx->sdl_renderer);
        ctx->sdl_renderer = NULL;
    }
#ifdef HAVE_VULKAN_DMABUF
    if (ctx->vulkan_dmabuf)
        bloom_vk_shutdown(&ctx->vk);
#endif
    if (ctx->sdl_window) {
        SDL_DestroyWindow(ctx->sdl_window);
        ctx->sdl_window = NULL;
    }

    free(ctx);
    plat->backend_data = NULL;

    SDL_Quit();
}

static void on_child_exited(GPid pid, gint status, gpointer user_data)
{
    (void)status;
    (void)user_data;
    g_spawn_close_pid(pid);
}

static void child_setup(gpointer user_data)
{
    (void)user_data;
    setsid();
}

static void on_new_terminal_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    if (!ctx->exe_path[0])
        return;

    char cwd_path[PATH_MAX] = "";
    int pty_child_pid = ctx->pty ? pty_get_child_pid(ctx->pty) : -1;
    if (pty_child_pid > 0) {
        char proc_cwd[64];
        snprintf(proc_cwd, sizeof(proc_cwd), "/proc/%d/cwd",
                 pty_child_pid);
        ssize_t cwd_len =
            readlink(proc_cwd, cwd_path, sizeof(cwd_path) - 1);
        if (cwd_len > 0)
            cwd_path[cwd_len] = '\0';
        else
            cwd_path[0] = '\0';
    }

    char *argv[] = { ctx->exe_path, "--gtk4", NULL };
    GPid child_pid;
    GError *error = NULL;

    gboolean ok = g_spawn_async(
        cwd_path[0] ? cwd_path : NULL, argv, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD, child_setup, NULL, &child_pid, &error);

    if (ok) {
        g_child_watch_add(child_pid, on_child_exited, NULL);
    } else {
        if (error) {
            vlog("Failed to spawn terminal: %s\n", error->message);
            g_error_free(error);
        }
    }

    gtk_widget_grab_focus(ctx->drawing_area);
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
    // Black window background prevents theme color from bleeding through
    // at anti-aliased rounded corners. Override named colors so libadwaita
    // derives backdrop/shade colors correctly, and override the flat
    // headerbar's "background: none" so it uses the headerbar colors.
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css_provider,
        "@define-color window_bg_color black;"
        "@define-color window_fg_color white;"
        "@define-color headerbar_bg_color #2e2e32;"
        "@define-color headerbar_fg_color white;"
        "@define-color headerbar_backdrop_color #2e2e32;"
        "toolbarview > .top-bar headerbar {"
        "  background: @headerbar_bg_color;"
        "  color: @headerbar_fg_color;"
        "}"
        "toolbarview > .top-bar headerbar:backdrop {"
        "  background: @headerbar_backdrop_color;"
        "}");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css_provider);

    // Create header bar with persistent title widget
    ctx->header_bar = adw_header_bar_new();
    ctx->window_title = ADW_WINDOW_TITLE(adw_window_title_new(title, NULL));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(ctx->header_bar),
                                    GTK_WIDGET(ctx->window_title));

    GtkWidget *new_term_btn =
        gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_widget_set_tooltip_text(new_term_btn, "New Terminal");
    gtk_widget_add_css_class(new_term_btn, "flat");
    g_signal_connect(new_term_btn, "clicked",
                     G_CALLBACK(on_new_terminal_clicked), ctx);
    adw_header_bar_pack_start(ADW_HEADER_BAR(ctx->header_bar), new_term_btn);

    // Create drawing area for terminal content (custom subclass for snapshot)
    BloomTerminalArea *term_area =
        g_object_new(BLOOM_TYPE_TERMINAL_AREA, NULL);
    term_area->ctx = ctx;
    ctx->drawing_area = GTK_WIDGET(term_area);
    gtk_widget_set_hexpand(ctx->drawing_area, TRUE);
    gtk_widget_set_vexpand(ctx->drawing_area, TRUE);
    gtk_widget_set_focusable(ctx->drawing_area, TRUE);
    gtk_widget_set_can_focus(ctx->drawing_area, TRUE);

    // Use AdwToolbarView to integrate header bar with content
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view),
                                 ctx->header_bar);

    bool use_offload = false;
#ifdef HAVE_VULKAN_DMABUF
    use_offload = ctx->vulkan_dmabuf;
#endif
    // Wrap in GtkGraphicsOffload for compositor direct scanout of the DMA-BUF.
    if (use_offload) {
        GtkWidget *offload = gtk_graphics_offload_new(ctx->drawing_area);
        gtk_graphics_offload_set_enabled(GTK_GRAPHICS_OFFLOAD(offload),
                                         GTK_GRAPHICS_OFFLOAD_ENABLED);
        gtk_graphics_offload_set_black_background(
            GTK_GRAPHICS_OFFLOAD(offload), TRUE);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), offload);
    } else {
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view),
                                     ctx->drawing_area);
    }
    adw_window_set_content(ADW_WINDOW(ctx->window), toolbar_view);

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

// Deferred window presentation — called from GMainLoop idle to avoid
// blocking startup on Wayland compositor roundtrip (~1.3s cold start).
static gboolean present_window_idle(gpointer user_data)
{
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;
    vlog("gtk_window_present (deferred)\n");
    gtk_window_present(ctx->window);
    gtk_widget_grab_focus(ctx->drawing_area);
    vlog("gtk_window_present done\n");
    return G_SOURCE_REMOVE;
}

static void gtk4_show_window(PlatformBackend *plat)
{
    // No-op: actual presentation is deferred to gtk4_run() via idle callback.
    // This allows PTY creation to happen before the blocking Wayland roundtrip.
    (void)plat;
}

static void gtk4_set_window_size(PlatformBackend *plat, int width, int height)
{
    if (!plat || !plat->backend_data)
        return;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;

    // Font metrics are at physical DPI. Convert to logical for GTK widget.
    float scale = gtk4_get_display_scale(plat);
    if (scale > 1.0f) {
        width = (int)(width / scale);
        height = (int)(height / scale);
    }

    ctx->content_width = width;
    ctx->content_height = height;
}

static void gtk4_set_window_title(PlatformBackend *plat, const char *title)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (ctx->window) {
        const char *t = title ? title : "bloom-terminal";
        gtk_window_set_title(ctx->window, t);
        if (ctx->window_title)
            adw_window_title_set_title(ctx->window_title, t);
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

    // Store references for draw_func and event handlers
    ctx->term = term;
    ctx->rend = rend;
    ctx->callbacks = callbacks;
    ctx->plat = plat;

    // Get scale factor
    ctx->scale_factor = gtk_widget_get_scale_factor(ctx->drawing_area);
    renderer_set_content_scale(rend, (float)ctx->scale_factor);
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

    // Set up PTY I/O watch (skip in demo mode when no PTY)
    if (ctx->pty) {
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

    // Present window from idle callback so the Wayland compositor roundtrip
    // happens inside the event loop instead of blocking startup.
    g_idle_add(present_window_idle, ctx);

    vlog("GTK4 event loop starting\n");
    g_main_loop_run(ctx->main_loop);
    vlog("GTK4 event loop exiting\n");

    // Cleanup signal watches and timer.
    // Zero all IDs so gtk4_plat_destroy won't double-remove sources
    // that were already auto-removed via G_SOURCE_REMOVE in callbacks.
    if (ctx->sigint_id)
        g_source_remove(ctx->sigint_id);
    ctx->sigint_id = 0;
    if (ctx->sigterm_id)
        g_source_remove(ctx->sigterm_id);
    ctx->sigterm_id = 0;
    if (ctx->cursor_blink_timer_id)
        g_source_remove(ctx->cursor_blink_timer_id);
    ctx->cursor_blink_timer_id = 0;
    if (ctx->autoscroll_timer_id)
        g_source_remove(ctx->autoscroll_timer_id);
    ctx->autoscroll_timer_id = 0;
    ctx->pty_watch_id = 0;
    ctx->signal_watch_id = 0;
}

static void gtk4_request_quit(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (ctx->main_loop)
        g_main_loop_quit(ctx->main_loop);
}

static void gtk4_pause_pty(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (ctx->pty_paused)
        return;

    if (ctx->pty_watch_id) {
        g_source_remove(ctx->pty_watch_id);
        ctx->pty_watch_id = 0;
    }
    ctx->pty_paused = true;
    vlog("PTY paused (backpressure)\n");
}

static void gtk4_resume_pty(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (!ctx->pty_paused)
        return;

    ctx->pty_paused = false;
    if (ctx->pty_channel) {
        ctx->pty_watch_id = g_io_add_watch(ctx->pty_channel,
                                           G_IO_IN | G_IO_ERR | G_IO_HUP,
                                           on_pty_data, ctx);
    }
    vlog("PTY resumed\n");
}

static char *gtk4_get_default_font(PlatformBackend *plat)
{
    (void)plat;
    GSettings *settings = g_settings_new("org.gnome.desktop.interface");
    if (!settings)
        return NULL;
    char *value = g_settings_get_string(settings, "monospace-font-name");
    g_object_unref(settings);
    if (!value || value[0] == '\0') {
        g_free(value);
        return NULL;
    }
    // Convert Pango format "Family Name 12" to fontconfig "Family Name-12"
    char *last_space = strrchr(value, ' ');
    if (last_space && last_space[1] >= '0' && last_space[1] <= '9')
        *last_space = '-';
    char *result = strdup(value);
    g_free(value);
    vlog("GNOME monospace font: %s\n", result);
    return result;
}

static float gtk4_get_display_scale(PlatformBackend *plat)
{
    (void)plat;
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return 0.0f;
    GListModel *monitors = gdk_display_get_monitors(display);
    if (!monitors || g_list_model_get_n_items(monitors) == 0)
        return 0.0f;
    GdkMonitor *monitor = g_list_model_get_item(monitors, 0);
    if (!monitor)
        return 0.0f;
    int scale = gdk_monitor_get_scale_factor(monitor);
    g_object_unref(monitor);
    return (float)scale;
}

static bool gtk4_get_display_size(PlatformBackend *plat, int *width, int *height)
{
    (void)plat;
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return false;
    GListModel *monitors = gdk_display_get_monitors(display);
    if (!monitors || g_list_model_get_n_items(monitors) == 0)
        return false;
    GdkMonitor *monitor = g_list_model_get_item(monitors, 0);
    if (!monitor)
        return false;
    GdkRectangle geom;
    gdk_monitor_get_geometry(monitor, &geom);
    int scale = gdk_monitor_get_scale_factor(monitor);
    g_object_unref(monitor);
    if (width)
        *width = geom.width * scale;
    if (height)
        *height = geom.height * scale;
    return true;
}

static bool gtk4_open_url(PlatformBackend *plat, const char *url)
{
    (void)plat;
    if (!url)
        return false;
    /* g_app_info_launch_default_for_uri picks the user's preferred handler
     * via xdg-mime / portals. Async to avoid blocking the GTK main loop. */
    GError *error = NULL;
    gboolean ok = g_app_info_launch_default_for_uri(url, NULL, &error);
    if (!ok) {
        fprintf(stderr, "ERROR: open URL failed: %s\n",
                error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return false;
    }
    return true;
}

static void gtk4_set_cursor(PlatformBackend *plat, PlatformCursor cursor)
{
    if (!plat || !plat->backend_data)
        return;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (!ctx->drawing_area)
        return;
    const char *name = (cursor == PLATFORM_CURSOR_POINTER) ? "pointer" : "text";
    gtk_widget_set_cursor_from_name(ctx->drawing_area, name);
}

static void gtk4_set_autoscroll(PlatformBackend *plat, bool enabled)
{
    if (!plat || !plat->backend_data)
        return;
    GTK4PlatformData *ctx = (GTK4PlatformData *)plat->backend_data;
    if (enabled) {
        if (!ctx->autoscroll_timer_id)
            ctx->autoscroll_timer_id =
                g_timeout_add(AUTOSCROLL_INTERVAL_MS, on_autoscroll_tick_gtk, ctx);
    } else if (ctx->autoscroll_timer_id) {
        g_source_remove(ctx->autoscroll_timer_id);
        ctx->autoscroll_timer_id = 0;
    }
}

__attribute__((visibility("default")))
PlatformBackend *
bloom_platform_gtk4_get(void)
{
    return &platform_backend_gtk4;
}
