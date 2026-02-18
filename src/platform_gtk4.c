#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "platform_gtk4.h"
#include <SDL3/SDL.h>
#include <adwaita.h>
#include <errno.h>
#include <glib-unix.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_EGL_DMABUF
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
// GL function pointer types (loaded via eglGetProcAddress, avoids -lGL)
typedef void(EGLAPIENTRYP PFNGLFINISHPROC)(void);
typedef void(EGLAPIENTRYP PFNGLGENTEXTURESPROC)(int n, unsigned int *textures);
typedef void(EGLAPIENTRYP PFNGLBINDTEXTUREPROC)(unsigned int target,
                                                unsigned int texture);
typedef void(EGLAPIENTRYP PFNGLDELETETEXTURESPROC)(int n,
                                                   const unsigned int *textures);
typedef void(EGLAPIENTRYP PFNGLTEXPARAMETERIPROC)(unsigned int target,
                                                  unsigned int pname,
                                                  int param);
typedef void(EGLAPIENTRYP PFNGLGENFRAMEBUFFERSPROC)(int n,
                                                    unsigned int *framebuffers);
typedef void(EGLAPIENTRYP PFNGLBINDFRAMEBUFFERPROC)(unsigned int target,
                                                    unsigned int framebuffer);
typedef void(EGLAPIENTRYP PFNGLFRAMEBUFFERTEXTURE2DPROC)(
    unsigned int target, unsigned int attachment, unsigned int textarget,
    unsigned int texture, int level);
typedef void(EGLAPIENTRYP PFNGLDELETEFRAMEBUFFERSPROC)(
    int n, const unsigned int *framebuffers);
typedef void(EGLAPIENTRYP PFNGLBLITFRAMEBUFFERPROC)(
    int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
    int dstX1, int dstY1, unsigned int mask, unsigned int filter);
typedef unsigned int(EGLAPIENTRYP PFNGLCHECKFRAMEBUFFERSTATUSPROC)(
    unsigned int target);
typedef unsigned int(EGLAPIENTRYP PFNGLGETERRORPROC)(void);
typedef void(EGLAPIENTRYP PFNGLGETINTEGERVPROC)(unsigned int pname,
                                                int *params);
// glEGLImageTargetTexture2DOES — bind EGLImage to GL texture
typedef void(EGLAPIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(
    unsigned int target, void *image);
// glCopyImageSubData — copy between textures without FBO binding (GL 4.3)
typedef void(EGLAPIENTRYP PFNGLCOPYIMAGESUBDATAPROC)(
    unsigned int srcName, unsigned int srcTarget, int srcLevel, int srcX,
    int srcY, int srcZ, unsigned int dstName, unsigned int dstTarget,
    int dstLevel, int dstX, int dstY, int dstZ, int srcWidth, int srcHeight,
    int srcDepth);
// glGetFramebufferAttachmentParameteriv — query FBO attachments
typedef void(EGLAPIENTRYP PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC)(
    unsigned int target, unsigned int attachment, unsigned int pname,
    int *params);
#define GL_TEXTURE_2D                         0x0DE1
#define GL_TEXTURE_MIN_FILTER                 0x2801
#define GL_TEXTURE_MAG_FILTER                 0x2800
#define GL_LINEAR                             0x2601
#define GL_NEAREST                            0x2600
#define GL_RGBA                               0x1908
#define GL_RGBA8                              0x8058
#define GL_UNSIGNED_BYTE                      0x1401
#define GL_FRAMEBUFFER                        0x8D40
#define GL_READ_FRAMEBUFFER                   0x8CA8
#define GL_DRAW_FRAMEBUFFER                   0x8CA9
#define GL_FRAMEBUFFER_BINDING                0x8CA6
#define GL_COLOR_ATTACHMENT0                  0x8CE0
#define GL_COLOR_BUFFER_BIT                   0x00004000
#define GL_FRAMEBUFFER_COMPLETE               0x8CD5
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#endif

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
    AdwWindowTitle *window_title;
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
    bool pty_paused;
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

#ifdef HAVE_EGL_DMABUF
    // Zero-copy DMA-BUF rendering state (GBM import approach)
    bool zero_copy;
    EGLDisplay egl_display;
    GdkTexture *prev_texture;

    // GBM state
    int drm_fd; // DRM render node fd
    struct gbm_device *gbm_dev;
    struct gbm_bo *gbm_bo; // current buffer object
    int dmabuf_fd;         // DMA-BUF fd from GBM (persistent, dup'd for GTK)
    int dmabuf_stride;
    int dmabuf_offset;
    uint32_t dmabuf_fourcc;
    uint64_t dmabuf_modifier;

    // GL resources (texture + FBO backed by GBM buffer via EGLImage import)
    EGLImage egl_image;      // imported from DMA-BUF (persistent per resize)
    unsigned int export_tex; // GL texture bound to EGLImage
    unsigned int export_fbo; // FBO with export_tex as color attachment

    // EGL context state (saved for direct eglMakeCurrent restoration,
    // since SDL_GL_MakeCurrent doesn't work after GTK changes the context)
    EGLContext egl_ctx;
    EGLSurface egl_draw;
    EGLSurface egl_read;

    // SDL render target's GL texture ID (queried from FBO attachment)
    unsigned int sdl_target_gl_tex;

    // GL function pointers
    PFNGLFINISHPROC glFinish;
    PFNGLGENTEXTURESPROC glGenTextures;
    PFNGLBINDTEXTUREPROC glBindTexture;
    PFNGLDELETETEXTURESPROC glDeleteTextures;
    PFNGLTEXPARAMETERIPROC glTexParameteri;
    PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
    PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
    PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
    PFNGLGETERRORPROC glGetError;
    PFNGLGETINTEGERVPROC glGetIntegerv;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLCOPYIMAGESUBDATAPROC glCopyImageSubData; // optional
    PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC
    glGetFramebufferAttachmentParameteriv;
#endif
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

#ifdef HAVE_EGL_DMABUF
// Load a GL/EGL function via eglGetProcAddress
#define LOAD_GL(ctx, name, type) \
    ctx->name = (type)eglGetProcAddress(#name)

// Find and open a DRM render node (/dev/dri/renderD128, etc.)
static int open_drm_render_node(void)
{
    for (int i = 128; i < 136; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            vlog("Opened DRM render node: %s\n", path);
            return fd;
        }
    }
    return -1;
}

// Initialize GBM + EGL for DMA-BUF import — call after GL renderer creation
static bool init_dmabuf_export(GTK4PlatformData *ctx)
{
    ctx->egl_display = eglGetCurrentDisplay();
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        vlog("No EGL display available\n");
        return false;
    }

    // Check for EGL_EXT_image_dma_buf_import (needed to import DMA-BUF)
    const char *extensions = eglQueryString(ctx->egl_display, EGL_EXTENSIONS);
    if (!extensions ||
        !strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
        vlog("EGL_EXT_image_dma_buf_import not available\n");
        return false;
    }

    // Open DRM render node for GBM
    ctx->drm_fd = open_drm_render_node();
    if (ctx->drm_fd < 0) {
        vlog("No DRM render node available\n");
        return false;
    }

    // Create GBM device
    ctx->gbm_dev = gbm_create_device(ctx->drm_fd);
    if (!ctx->gbm_dev) {
        vlog("gbm_create_device failed\n");
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return false;
    }

    // Save EGL context and surfaces for direct restoration.
    // SDL_GL_MakeCurrent doesn't work after GTK makes its own context current,
    // so we call eglMakeCurrent directly in the snapshot callback.
    ctx->egl_ctx = eglGetCurrentContext();
    ctx->egl_draw = eglGetCurrentSurface(EGL_DRAW);
    ctx->egl_read = eglGetCurrentSurface(EGL_READ);

    // Load GL functions (required)
    LOAD_GL(ctx, glFinish, PFNGLFINISHPROC);
    LOAD_GL(ctx, glGenTextures, PFNGLGENTEXTURESPROC);
    LOAD_GL(ctx, glBindTexture, PFNGLBINDTEXTUREPROC);
    LOAD_GL(ctx, glDeleteTextures, PFNGLDELETETEXTURESPROC);
    LOAD_GL(ctx, glTexParameteri, PFNGLTEXPARAMETERIPROC);
    LOAD_GL(ctx, glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
    LOAD_GL(ctx, glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    LOAD_GL(ctx, glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
    LOAD_GL(ctx, glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);
    LOAD_GL(ctx, glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
    LOAD_GL(ctx, glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC);
    LOAD_GL(ctx, glGetError, PFNGLGETERRORPROC);
    LOAD_GL(ctx, glGetIntegerv, PFNGLGETINTEGERVPROC);
    LOAD_GL(ctx, glEGLImageTargetTexture2DOES,
            PFNGLEGLIMAGETARGETTEXTURE2DOESPROC);
    LOAD_GL(ctx, glGetFramebufferAttachmentParameteriv,
            PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC);
    // Optional: glCopyImageSubData (GL 4.3) — preferred over FBO blit
    LOAD_GL(ctx, glCopyImageSubData, PFNGLCOPYIMAGESUBDATAPROC);

    if (!ctx->glFinish || !ctx->glGenTextures || !ctx->glBindTexture ||
        !ctx->glDeleteTextures || !ctx->glTexParameteri ||
        !ctx->glGenFramebuffers || !ctx->glBindFramebuffer ||
        !ctx->glFramebufferTexture2D || !ctx->glDeleteFramebuffers ||
        !ctx->glBlitFramebuffer || !ctx->glCheckFramebufferStatus ||
        !ctx->glGetError || !ctx->glGetIntegerv ||
        !ctx->glEGLImageTargetTexture2DOES ||
        !ctx->glGetFramebufferAttachmentParameteriv) {
        vlog("Failed to load GL function pointers\n");
        gbm_device_destroy(ctx->gbm_dev);
        ctx->gbm_dev = NULL;
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return false;
    }

    ctx->gbm_bo = NULL;
    ctx->dmabuf_fd = -1;
    ctx->egl_image = EGL_NO_IMAGE;
    ctx->export_tex = 0;
    ctx->export_fbo = 0;
    ctx->sdl_target_gl_tex = 0;
    vlog("GBM DMA-BUF import initialized (glCopyImageSubData: %s)\n",
         ctx->glCopyImageSubData ? "yes" : "no");
    return true;
}

// Create or resize the GBM-backed GL texture + FBO.
// Allocates a GBM buffer, exports its DMA-BUF fd, imports into EGL/GL.
static bool setup_export_resources(GTK4PlatformData *ctx, int w, int h)
{
    // Clean up previous resources (reverse order of creation)
    if (ctx->export_fbo) {
        ctx->glDeleteFramebuffers(1, &ctx->export_fbo);
        ctx->export_fbo = 0;
    }
    if (ctx->export_tex) {
        ctx->glDeleteTextures(1, &ctx->export_tex);
        ctx->export_tex = 0;
    }
    if (ctx->egl_image != EGL_NO_IMAGE) {
        eglDestroyImage(ctx->egl_display, ctx->egl_image);
        ctx->egl_image = EGL_NO_IMAGE;
    }
    if (ctx->dmabuf_fd >= 0) {
        close(ctx->dmabuf_fd);
        ctx->dmabuf_fd = -1;
    }
    if (ctx->gbm_bo) {
        gbm_bo_destroy(ctx->gbm_bo);
        ctx->gbm_bo = NULL;
    }

    // 1. Allocate GBM buffer with explicit linear modifier
    uint64_t linear_mod = DRM_FORMAT_MOD_LINEAR;
    ctx->gbm_bo = gbm_bo_create_with_modifiers2(
        ctx->gbm_dev, (uint32_t)w, (uint32_t)h, GBM_FORMAT_ABGR8888,
        &linear_mod, 1, GBM_BO_USE_RENDERING);
    if (!ctx->gbm_bo) {
        // Fallback: try gbm_bo_create with LINEAR flag
        ctx->gbm_bo = gbm_bo_create(ctx->gbm_dev, (uint32_t)w, (uint32_t)h,
                                    GBM_FORMAT_ABGR8888,
                                    GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!ctx->gbm_bo) {
        vlog("gbm_bo_create(%dx%d) failed\n", w, h);
        return false;
    }

    // 2. Get DMA-BUF fd and metadata from GBM
    ctx->dmabuf_fd = gbm_bo_get_fd(ctx->gbm_bo);
    if (ctx->dmabuf_fd < 0) {
        vlog("gbm_bo_get_fd failed\n");
        gbm_bo_destroy(ctx->gbm_bo);
        ctx->gbm_bo = NULL;
        return false;
    }
    ctx->dmabuf_stride = (int)gbm_bo_get_stride(ctx->gbm_bo);
    ctx->dmabuf_offset = 0;
    ctx->dmabuf_fourcc = gbm_bo_get_format(ctx->gbm_bo);
    ctx->dmabuf_modifier = gbm_bo_get_modifier(ctx->gbm_bo);
    // If GBM reports INVALID modifier but we requested linear, use linear
    if (ctx->dmabuf_modifier == DRM_FORMAT_MOD_INVALID)
        ctx->dmabuf_modifier = DRM_FORMAT_MOD_LINEAR;

    // 3. Import DMA-BUF into EGL as an EGLImage (with explicit modifier)
    EGLAttrib img_attrs[] = {
        EGL_WIDTH, w,
        EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)ctx->dmabuf_fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, ctx->dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, ctx->dmabuf_stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        (EGLAttrib)(ctx->dmabuf_modifier & 0xFFFFFFFF),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        (EGLAttrib)(ctx->dmabuf_modifier >> 32),
        EGL_NONE
    };
    ctx->egl_image = eglCreateImage(ctx->egl_display, EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (ctx->egl_image == EGL_NO_IMAGE) {
        vlog("eglCreateImage(DMA-BUF import) failed: 0x%x\n", eglGetError());
        close(ctx->dmabuf_fd);
        ctx->dmabuf_fd = -1;
        gbm_bo_destroy(ctx->gbm_bo);
        ctx->gbm_bo = NULL;
        return false;
    }

    // 4. Create GL texture backed by the EGLImage
    ctx->glGenTextures(1, &ctx->export_tex);
    ctx->glBindTexture(GL_TEXTURE_2D, ctx->export_tex);
    ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ctx->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                      (void *)ctx->egl_image);
    ctx->glBindTexture(GL_TEXTURE_2D, 0);

    // 5. Create FBO with the EGLImage-backed texture
    ctx->glGenFramebuffers(1, &ctx->export_fbo);
    ctx->glBindFramebuffer(GL_FRAMEBUFFER, ctx->export_fbo);
    ctx->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, ctx->export_tex, 0);
    unsigned int status = ctx->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    ctx->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        vlog("Export FBO incomplete: 0x%x\n", status);
        ctx->glDeleteFramebuffers(1, &ctx->export_fbo);
        ctx->export_fbo = 0;
        ctx->glDeleteTextures(1, &ctx->export_tex);
        ctx->export_tex = 0;
        eglDestroyImage(ctx->egl_display, ctx->egl_image);
        ctx->egl_image = EGL_NO_IMAGE;
        close(ctx->dmabuf_fd);
        ctx->dmabuf_fd = -1;
        gbm_bo_destroy(ctx->gbm_bo);
        ctx->gbm_bo = NULL;
        return false;
    }

    vlog("GBM export resources: bo=%p tex=%u fbo=%u fd=%d "
         "fourcc=0x%x modifier=0x%lx stride=%d (%dx%d)\n",
         (void *)ctx->gbm_bo, ctx->export_tex, ctx->export_fbo,
         ctx->dmabuf_fd, ctx->dmabuf_fourcc,
         (unsigned long)ctx->dmabuf_modifier, ctx->dmabuf_stride, w, h);
    return true;
}

// GDestroyNotify callback to close dup'd fd when GTK is done with texture
static void close_dmabuf_fd(gpointer data)
{
    int fd = (int)(intptr_t)data;
    if (fd >= 0)
        close(fd);
}
#endif // HAVE_EGL_DMABUF

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

#ifdef HAVE_EGL_DMABUF
    // If nothing changed and we have a cached texture, reuse it
    if (!needs_render && ctx->prev_texture) {
        gtk_snapshot_append_texture(
            snapshot, ctx->prev_texture,
            &GRAPHENE_RECT_INIT(0, 0, width, height));
        return;
    }
#endif

    if (!needs_render)
        return;

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
#ifdef HAVE_EGL_DMABUF
        // Recreate export resources for new size
        if (ctx->zero_copy) {
            g_clear_object(&ctx->prev_texture);
            if (!setup_export_resources(ctx, phys_w, phys_h)) {
                vlog("Export resource setup failed, disabling zero-copy\n");
                ctx->zero_copy = false;
            }

            // Query the SDL render target's GL texture ID from the FBO
            // attachment. SDL's offscreen renderer doesn't expose it via
            // properties, but we can read it from the FBO color attachment.
            ctx->sdl_target_gl_tex = 0;
            SDL_SetRenderTarget(ctx->sdl_renderer, ctx->render_target);
            int fbo_id = 0;
            ctx->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_id);
            if (fbo_id > 0) {
                int tex_name = 0;
                ctx->glGetFramebufferAttachmentParameteriv(
                    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex_name);
                if (tex_name > 0)
                    ctx->sdl_target_gl_tex = (unsigned int)tex_name;
            }
            SDL_SetRenderTarget(ctx->sdl_renderer, NULL);
            while (ctx->glGetError() != 0)
                ;
            vlog("SDL render target GL texture: %u\n",
                 ctx->sdl_target_gl_tex);
        }
#endif
        if (!ctx->render_target) {
            vlog("Failed to create render target %dx%d: %s\n", phys_w, phys_h,
                 SDL_GetError());
            return;
        }
    }

    // Render terminal into SDL's render target texture.
#ifdef HAVE_EGL_DMABUF
    if (ctx->zero_copy) {
        // GTK4 makes its own EGL context current for GL rendering.
        // SDL_GL_MakeCurrent doesn't restore ours, so call eglMakeCurrent
        // directly with the saved EGL context and surfaces.
        eglMakeCurrent(ctx->egl_display, ctx->egl_draw, ctx->egl_read,
                       ctx->egl_ctx);
    }
#endif
    SDL_SetRenderTarget(ctx->sdl_renderer, NULL);
#ifdef HAVE_EGL_DMABUF
    if (ctx->zero_copy) {
        while (ctx->glGetError() != 0)
            ;
    }
#endif
    if (!SDL_SetRenderTarget(ctx->sdl_renderer, ctx->render_target)) {
        vlog("SDL_SetRenderTarget failed: %s\n", SDL_GetError());
        return;
    }

    bool cursor_vis =
        !terminal_get_cursor_blink(ctx->term) || ctx->cursor_blink_visible;
    renderer_draw_terminal(ctx->rend, ctx->term, cursor_vis);

#ifdef HAVE_EGL_DMABUF
    if (ctx->zero_copy) {
        // Flush SDL draw commands to GL
        SDL_FlushRenderer(ctx->sdl_renderer);

        // Copy SDL render target → GBM-backed export texture
        if (ctx->glCopyImageSubData && ctx->sdl_target_gl_tex) {
            // glCopyImageSubData copies between textures directly —
            // no FBO binding needed, avoids FBO completeness issues
            ctx->glCopyImageSubData(
                ctx->sdl_target_gl_tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                ctx->export_tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                phys_w, phys_h, 1);
        } else {
            // Fallback: blit via FBOs
            int sdl_fbo = 0;
            ctx->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sdl_fbo);
            ctx->glBindFramebuffer(GL_READ_FRAMEBUFFER,
                                   (unsigned int)sdl_fbo);
            ctx->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->export_fbo);
            ctx->glBlitFramebuffer(0, 0, phys_w, phys_h, 0, 0, phys_w,
                                   phys_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            ctx->glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)sdl_fbo);
        }
        ctx->glFinish();

        // dup() the persistent DMA-BUF fd for this frame's GTK texture.
        // GTK closes the fd via close_dmabuf_fd when done.
        int fd = dup(ctx->dmabuf_fd);
        if (fd < 0) {
            vlog("dup(dmabuf_fd) failed: %s\n", strerror(errno));
            goto readback_fallback;
        }

        GdkDmabufTextureBuilder *builder =
            gdk_dmabuf_texture_builder_new();
        gdk_dmabuf_texture_builder_set_display(
            builder, gtk_widget_get_display(widget));
        gdk_dmabuf_texture_builder_set_width(builder, phys_w);
        gdk_dmabuf_texture_builder_set_height(builder, phys_h);
        gdk_dmabuf_texture_builder_set_fourcc(builder, ctx->dmabuf_fourcc);
        gdk_dmabuf_texture_builder_set_modifier(builder, ctx->dmabuf_modifier);
        gdk_dmabuf_texture_builder_set_n_planes(builder, 1);
        gdk_dmabuf_texture_builder_set_fd(builder, 0, fd);
        gdk_dmabuf_texture_builder_set_stride(builder, 0, ctx->dmabuf_stride);
        gdk_dmabuf_texture_builder_set_offset(builder, 0, ctx->dmabuf_offset);
        gdk_dmabuf_texture_builder_set_premultiplied(builder, TRUE);

        if (ctx->prev_texture)
            gdk_dmabuf_texture_builder_set_update_texture(builder,
                                                          ctx->prev_texture);

        GError *error = NULL;
        GdkTexture *texture = gdk_dmabuf_texture_builder_build(
            builder, close_dmabuf_fd, (gpointer)(intptr_t)fd, &error);
        g_object_unref(builder);

        if (texture) {
            gtk_snapshot_append_texture(
                snapshot, texture,
                &GRAPHENE_RECT_INIT(0, 0, width, height));
            g_clear_object(&ctx->prev_texture);
            ctx->prev_texture = texture;

            terminal_clear_redraw(ctx->term);
            ctx->force_redraw = false;
            return;
        }

        // DMA-BUF texture build failed
        vlog("GdkDmabufTextureBuilder failed: %s\n",
             error ? error->message : "unknown");
        g_clear_error(&error);
        close(fd);
    }

readback_fallback:
#endif // HAVE_EGL_DMABUF

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

static void gtk4_set_window_title(PlatformBackend *plat, const char *title);

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

            // Update window title if changed
            gtk4_set_window_title(ctx->plat, terminal_get_title(ctx->term));

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
static void gtk4_pause_pty(PlatformBackend *plat);
static void gtk4_resume_pty(PlatformBackend *plat);

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

#ifdef HAVE_EGL_DMABUF
    // Try to create an OpenGL-backed offscreen renderer for zero-copy DMA-BUF.
    // Strategy: offscreen driver + OpenGL window/renderer, with fallbacks.
    bool got_gl = false;
    // Attempt 1: offscreen driver with OpenGL renderer
    ctx->sdl_window = SDL_CreateWindow("bloom-terminal-offscreen", 800, 600,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (ctx->sdl_window) {
        SDL_PropertiesID rprops = SDL_CreateProperties();
        SDL_SetStringProperty(rprops, SDL_PROP_RENDERER_CREATE_NAME_STRING,
                              "opengl");
        SDL_SetPointerProperty(rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
                               ctx->sdl_window);
        ctx->sdl_renderer = SDL_CreateRendererWithProperties(rprops);
        SDL_DestroyProperties(rprops);

        if (ctx->sdl_renderer) {
            const char *rname = SDL_GetRendererName(ctx->sdl_renderer);
            if (rname &&
                (strcmp(rname, "opengl") == 0 ||
                 strcmp(rname, "opengles2") == 0)) {
                got_gl = true;

                vlog("SDL GL renderer (%s) on offscreen driver\n", rname);
            } else {
                vlog("Got non-GL renderer '%s', retrying\n",
                     rname ? rname : "(null)");
                SDL_DestroyRenderer(ctx->sdl_renderer);
                ctx->sdl_renderer = NULL;
            }
        }
        if (!got_gl) {
            SDL_DestroyWindow(ctx->sdl_window);
            ctx->sdl_window = NULL;
        }
    }

    // Attempt 2: default video driver with hidden GL window
    if (!got_gl) {
        SDL_Quit();
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "");
        if (SDL_Init(SDL_INIT_VIDEO)) {
            ctx->sdl_window =
                SDL_CreateWindow("bloom-terminal-offscreen", 800, 600,
                                 SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
            if (ctx->sdl_window) {
                SDL_PropertiesID rprops = SDL_CreateProperties();
                SDL_SetStringProperty(
                    rprops, SDL_PROP_RENDERER_CREATE_NAME_STRING, "opengl");
                SDL_SetPointerProperty(
                    rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
                    ctx->sdl_window);
                ctx->sdl_renderer =
                    SDL_CreateRendererWithProperties(rprops);
                SDL_DestroyProperties(rprops);

                if (ctx->sdl_renderer) {
                    const char *rname =
                        SDL_GetRendererName(ctx->sdl_renderer);
                    if (rname &&
                        (strcmp(rname, "opengl") == 0 ||
                         strcmp(rname, "opengles2") == 0)) {
                        got_gl = true;

                        vlog("SDL GL renderer (%s) on default driver\n",
                             rname);
                    } else {
                        SDL_DestroyRenderer(ctx->sdl_renderer);
                        ctx->sdl_renderer = NULL;
                    }
                }
                if (!got_gl) {
                    SDL_DestroyWindow(ctx->sdl_window);
                    ctx->sdl_window = NULL;
                }
            }
        }
        // Reinitialize with offscreen driver for fallback
        if (!got_gl) {
            SDL_Quit();
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                fprintf(stderr,
                        "ERROR: Failed to reinitialize SDL video: %s\n",
                        SDL_GetError());
                free(ctx);
                return false;
            }
        }
    }
#endif // HAVE_EGL_DMABUF

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

#ifdef HAVE_EGL_DMABUF
    // Initialize DMA-BUF export if we got a GL renderer
    if (got_gl) {
        // Force a render to ensure GL context is current
        SDL_SetRenderDrawColor(ctx->sdl_renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx->sdl_renderer);
        SDL_RenderPresent(ctx->sdl_renderer);

        if (init_dmabuf_export(ctx)) {
            ctx->zero_copy = true;
            vlog("Zero-copy DMA-BUF rendering enabled (EGL ctx=%p)\n",
                 (void *)ctx->egl_ctx);
        } else {
            vlog("DMA-BUF export init failed, using readback\n");
        }
    } else {
        vlog("No GL renderer, using readback rendering\n");
    }
#endif

    vlog("GTK4 platform initialized (zero_copy=%s, renderer=%s)\n",
#ifdef HAVE_EGL_DMABUF
         ctx->zero_copy ? "yes" : "no",
#else
         "no",
#endif
         SDL_GetRendererName(ctx->sdl_renderer));

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

#ifdef HAVE_EGL_DMABUF
    // Destroy GBM/EGL/GL export resources (reverse order of creation)
    g_clear_object(&ctx->prev_texture);
    if (ctx->export_fbo && ctx->glDeleteFramebuffers) {
        ctx->glDeleteFramebuffers(1, &ctx->export_fbo);
        ctx->export_fbo = 0;
    }
    if (ctx->export_tex && ctx->glDeleteTextures) {
        ctx->glDeleteTextures(1, &ctx->export_tex);
        ctx->export_tex = 0;
    }
    if (ctx->egl_image != EGL_NO_IMAGE && ctx->egl_display != EGL_NO_DISPLAY) {
        eglDestroyImage(ctx->egl_display, ctx->egl_image);
        ctx->egl_image = EGL_NO_IMAGE;
    }
    if (ctx->dmabuf_fd >= 0) {
        close(ctx->dmabuf_fd);
        ctx->dmabuf_fd = -1;
    }
    if (ctx->gbm_bo) {
        gbm_bo_destroy(ctx->gbm_bo);
        ctx->gbm_bo = NULL;
    }
    if (ctx->gbm_dev) {
        gbm_device_destroy(ctx->gbm_dev);
        ctx->gbm_dev = NULL;
    }
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
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
    if (ctx->sdl_window) {
        SDL_DestroyWindow(ctx->sdl_window);
        ctx->sdl_window = NULL;
    }

    free(ctx->last_title);
    free(ctx);
    plat->backend_data = NULL;

    SDL_Quit();
}

static void on_new_terminal_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GTK4PlatformData *ctx = (GTK4PlatformData *)user_data;

    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len <= 0)
        return;
    exe_path[exe_len] = '\0';

    char cwd_path[PATH_MAX] = "";
    if (ctx->pty && ctx->pty->child_pid > 0) {
        char proc_cwd[64];
        snprintf(proc_cwd, sizeof(proc_cwd), "/proc/%d/cwd",
                 ctx->pty->child_pid);
        ssize_t cwd_len =
            readlink(proc_cwd, cwd_path, sizeof(cwd_path) - 1);
        if (cwd_len > 0)
            cwd_path[cwd_len] = '\0';
        else
            cwd_path[0] = '\0';
    }

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid > 0) {
        // Parent: reap intermediate child
        waitpid(pid, NULL, 0);
        return;
    }
    // Intermediate child: fork again to avoid zombies
    pid_t pid2 = fork();
    if (pid2 < 0)
        _exit(1);
    if (pid2 > 0)
        _exit(0); // Intermediate exits immediately, reaped above
    // Grandchild: detach and exec
    setsid();
    if (cwd_path[0])
        (void)chdir(cwd_path);
    char *argv[] = { "bloom-terminal", "--gtk4", NULL };
    execv(exe_path, argv);
    _exit(1);
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

#ifdef HAVE_EGL_DMABUF
    // Wrap in GtkGraphicsOffload for potential compositor direct scanout
    if (ctx->zero_copy) {
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
#else
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view),
                                 ctx->drawing_area);
#endif
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

__attribute__((visibility("default")))
PlatformBackend *
bloom_platform_gtk4_get(void)
{
    return &platform_backend_gtk4;
}
