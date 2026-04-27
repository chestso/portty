/*
 * test_dmabuf_copy.c — Standalone DMA-BUF zero-copy reproduction test
 *
 * Tests whether glCopyImageSubData and glBlitFramebuffer can copy from a
 * regular GL texture to a GBM/EGLImage-backed texture across multiple
 * GBM pixel formats.  Isolates the DMA-BUF copy path from SDL and GTK4.
 *
 * Build:
 *   gcc -o test_dmabuf_copy tests/test_dmabuf_copy.c \
 *       -lEGL -lgbm $(pkg-config --cflags --libs libdrm) -ldl
 *
 * Run:
 *   ./test_dmabuf_copy
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

/* ------------------------------------------------------------------ */
/* GL defines (avoid pulling in full GL headers)                      */
/* ------------------------------------------------------------------ */
#define GL_TEXTURE_2D              0x0DE1
#define GL_RGBA                    0x1908
#define GL_RGBA8                   0x8058
#define GL_UNSIGNED_BYTE           0x1401
#define GL_FRAMEBUFFER             0x8D40
#define GL_READ_FRAMEBUFFER        0x8CA8
#define GL_DRAW_FRAMEBUFFER        0x8CA9
#define GL_COLOR_ATTACHMENT0       0x8CE0
#define GL_FRAMEBUFFER_COMPLETE    0x8CD5
#define GL_COLOR_BUFFER_BIT        0x00004000
#define GL_NEAREST                 0x2600
#define GL_LINEAR                  0x2601
#define GL_TEXTURE_MIN_FILTER      0x2800
#define GL_TEXTURE_MAG_FILTER      0x2801
#define GL_NO_ERROR                0
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_RED_SIZE        0x805C
#define GL_TEXTURE_GREEN_SIZE      0x805D
#define GL_TEXTURE_BLUE_SIZE       0x805E
#define GL_TEXTURE_ALPHA_SIZE      0x805F

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef void GLvoid;

/* GL function pointer types */
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (*PFNGLTEXIMAGE2DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                    GLint, GLenum, GLenum, const GLvoid *);
typedef void (*PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum,
                                              GLuint, GLint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (*PFNGLCLEARCOLORPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLCLEARPROC)(GLuint);
typedef void (*PFNGLREADPIXELSPROC)(GLint, GLint, GLsizei, GLsizei,
                                    GLenum, GLenum, GLvoid *);
typedef void (*PFNGLFINISHPROC)(void);
typedef GLenum (*PFNGLGETERRORPROC)(void);
typedef void (*PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFNGLGETTEXLEVELPARAMETERIVPROC)(GLenum, GLint, GLenum, GLint *);

typedef void (*PFNGLCOPYIMAGESUBDATAPROC)(GLuint, GLenum, GLint, GLint,
                                          GLint, GLint, GLuint, GLenum,
                                          GLint, GLint, GLint, GLint,
                                          GLsizei, GLsizei, GLsizei);
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint,
                                         GLint, GLint, GLint, GLint,
                                         GLuint, GLenum);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void *);

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static int drm_fd = -1;
static struct gbm_device *gbm_dev = NULL;
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLConfig egl_cfg;

/* GL function pointers */
static PFNGLGENTEXTURESPROC glGenTextures;
static PFNGLDELETETEXTURESPROC glDeleteTextures;
static PFNGLBINDTEXTUREPROC glBindTexture;
static PFNGLTEXIMAGE2DPROC glTexImage2D;
static PFNGLTEXPARAMETERIPROC glTexParameteri;
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
static PFNGLCLEARCOLORPROC glClearColor;
static PFNGLCLEARPROC glClear;
static PFNGLREADPIXELSPROC glReadPixels;
static PFNGLFINISHPROC glFinish;
static PFNGLGETERRORPROC glGetError;
static PFNGLVIEWPORTPROC glViewport;
static PFNGLGETTEXLEVELPARAMETERIVPROC glGetTexLevelParameteriv;
static PFNGLCOPYIMAGESUBDATAPROC glCopyImageSubData;
static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

/* Source texture + FBO */
static GLuint src_tex, src_fbo;

#define TEX_W 64
#define TEX_H 64

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *gl_format_name(GLint fmt)
{
    switch (fmt) {
    case 0x8058:
        return "GL_RGBA8";
    case 0x8059:
        return "GL_RGB10_A2";
    case 0x881A:
        return "GL_RGBA16F";
    case 0x8814:
        return "GL_RGBA32F";
    case 0x8051:
        return "GL_RGB8";
    case 0x8C43:
        return "GL_SRGB8_ALPHA8";
    case 0x1908:
        return "GL_RGBA";
    case 0x1907:
        return "GL_RGB";
    default:
        return NULL;
    }
}

static void report_tex_format(const char *label, GLuint tex)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint ifmt = 0, r = 0, g = 0, b = 0, a = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &ifmt);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &r);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_GREEN_SIZE, &g);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_BLUE_SIZE, &b);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &a);
    glBindTexture(GL_TEXTURE_2D, 0);

    const char *name = gl_format_name(ifmt);
    if (name)
        printf("  %s: internal_format=%s (0x%04X) R%d G%d B%d A%d\n",
               label, name, ifmt, r, g, b, a);
    else
        printf("  %s: internal_format=0x%04X R%d G%d B%d A%d\n",
               label, ifmt, r, g, b, a);
}

static const char *gbm_format_name(uint32_t fmt)
{
    switch (fmt) {
    case GBM_FORMAT_ABGR8888:
        return "ABGR8888";
    case GBM_FORMAT_XBGR8888:
        return "XBGR8888";
    case GBM_FORMAT_ARGB8888:
        return "ARGB8888";
    case GBM_FORMAT_XRGB8888:
        return "XRGB8888";
    default:
        return "UNKNOWN";
    }
}

static void drain_gl_errors(void)
{
    while (glGetError() != GL_NO_ERROR)
        ;
}

/* ------------------------------------------------------------------ */
/* DRM / EGL / GBM setup                                              */
/* ------------------------------------------------------------------ */

static bool open_drm_render_node(const char *preferred_driver)
{
    /* First pass: look for preferred driver; second pass: take anything */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 128; i < 136; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            drmVersionPtr v = drmGetVersion(fd);
            if (!v) {
                close(fd);
                continue;
            }
            bool match = (pass == 1) ||
                         (preferred_driver &&
                          strcmp(v->name, preferred_driver) == 0);
            if (match) {
                printf("DRM render node: %s (%s)\n", path, v->name);
                drmFreeVersion(v);
                drm_fd = fd;
                return true;
            }
            drmFreeVersion(v);
            close(fd);
        }
        /* If no preferred driver requested, first pass suffices */
        if (!preferred_driver)
            break;
    }
    fprintf(stderr, "No usable DRM render node found\n");
    return false;
}

static bool create_egl_context(void)
{
    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        fprintf(stderr, "gbm_create_device failed\n");
        return false;
    }

    /* Use GBM platform */
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (eglGetPlatformDisplayEXT) {
        egl_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA,
                                           gbm_dev, NULL);
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        egl_dpy = eglGetDisplay((EGLNativeDisplayType)gbm_dev);
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }
    printf("EGL %d.%d\n", major, minor);

    const char *extensions = eglQueryString(egl_dpy, EGL_EXTENSIONS);
    printf("EGL_EXT_image_dma_buf_import: %s\n",
           (extensions && strstr(extensions, "EGL_EXT_image_dma_buf_import"))
               ? "yes"
               : "NO");
    printf("EGL_EXT_image_dma_buf_import_modifiers: %s\n",
           (extensions && strstr(extensions,
                                 "EGL_EXT_image_dma_buf_import_modifiers"))
               ? "yes"
               : "NO");

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "eglBindAPI(GLES) failed\n");
        return false;
    }

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n_cfgs = 0;
    if (!eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &n_cfgs) ||
        n_cfgs == 0) {
        fprintf(stderr, "eglChooseConfig failed\n");
        return false;
    }

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return false;
    }

    printf("EGL context created (surfaceless)\n");
    return true;
}

static bool load_gl_functions(void)
{
#define LOAD(name, type)                                \
    do {                                                \
        name = (type)eglGetProcAddress(#name);          \
        if (!name) {                                    \
            fprintf(stderr, "Missing GL: " #name "\n"); \
            return false;                               \
        }                                               \
    } while (0)
#define LOAD_OPT(name, type) \
    name = (type)eglGetProcAddress(#name)

    LOAD(glGenTextures, PFNGLGENTEXTURESPROC);
    LOAD(glDeleteTextures, PFNGLDELETETEXTURESPROC);
    LOAD(glBindTexture, PFNGLBINDTEXTUREPROC);
    LOAD(glTexImage2D, PFNGLTEXIMAGE2DPROC);
    LOAD(glTexParameteri, PFNGLTEXPARAMETERIPROC);
    LOAD(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
    LOAD(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);
    LOAD(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    LOAD(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
    LOAD(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC);
    LOAD(glClearColor, PFNGLCLEARCOLORPROC);
    LOAD(glClear, PFNGLCLEARPROC);
    LOAD(glReadPixels, PFNGLREADPIXELSPROC);
    LOAD(glFinish, PFNGLFINISHPROC);
    LOAD(glGetError, PFNGLGETERRORPROC);
    LOAD(glViewport, PFNGLVIEWPORTPROC);
    LOAD(glGetTexLevelParameteriv, PFNGLGETTEXLEVELPARAMETERIVPROC);
    LOAD(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
    LOAD(glEGLImageTargetTexture2DOES, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC);

    LOAD_OPT(glCopyImageSubData, PFNGLCOPYIMAGESUBDATAPROC);
    printf("glCopyImageSubData: %s\n",
           glCopyImageSubData ? "available" : "NOT available");

#undef LOAD
#undef LOAD_OPT
    return true;
}

/* ------------------------------------------------------------------ */
/* Source texture (simulates SDL render target)                        */
/* ------------------------------------------------------------------ */

static bool create_source_texture(void)
{
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_W, TEX_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &src_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, src_tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Source FBO incomplete: 0x%x\n", status);
        return false;
    }

    /* Clear to red */
    glViewport(0, 0, TEX_W, TEX_H);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* Verify source content */
    uint8_t px[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (px[0] < 200 || px[1] > 50 || px[2] > 50) {
        fprintf(stderr, "Source clear failed: got [%d,%d,%d,%d]\n",
                px[0], px[1], px[2], px[3]);
        return false;
    }

    printf("Source texture: %dx%d cleared to red [%d,%d,%d,%d]\n",
           TEX_W, TEX_H, px[0], px[1], px[2], px[3]);
    report_tex_format("source", src_tex);
    return true;
}

/* ------------------------------------------------------------------ */
/* GBM export buffer creation                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    struct gbm_bo *bo;
    int fd;
    int stride;
    uint32_t fourcc;
    uint64_t modifier;
    EGLImage image;
    GLuint tex;
    GLuint fbo;
} ExportBuffer;

static bool create_export_buffer(uint32_t gbm_fmt, ExportBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->image = EGL_NO_IMAGE;

    /* Try with explicit linear modifier first */
    uint64_t linear_mod = DRM_FORMAT_MOD_LINEAR;
    out->bo = gbm_bo_create_with_modifiers2(
        gbm_dev, TEX_W, TEX_H, gbm_fmt, &linear_mod, 1,
        GBM_BO_USE_RENDERING);
    if (!out->bo) {
        out->bo = gbm_bo_create(gbm_dev, TEX_W, TEX_H, gbm_fmt,
                                GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!out->bo) {
        printf("  gbm_bo_create(%s) failed: %s\n",
               gbm_format_name(gbm_fmt), strerror(errno));
        return false;
    }

    out->fd = gbm_bo_get_fd(out->bo);
    if (out->fd < 0) {
        printf("  gbm_bo_get_fd failed\n");
        gbm_bo_destroy(out->bo);
        out->bo = NULL;
        return false;
    }

    out->stride = (int)gbm_bo_get_stride(out->bo);
    out->fourcc = gbm_bo_get_format(out->bo);
    out->modifier = gbm_bo_get_modifier(out->bo);
    if (out->modifier == DRM_FORMAT_MOD_INVALID)
        out->modifier = DRM_FORMAT_MOD_LINEAR;

    /* Import DMA-BUF as EGLImage */
    EGLAttrib img_attrs[] = {
        EGL_WIDTH, TEX_W,
        EGL_HEIGHT, TEX_H,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)out->fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, out->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, out->stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        (EGLAttrib)(out->modifier & 0xFFFFFFFF),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        (EGLAttrib)(out->modifier >> 32),
        EGL_NONE
    };
    out->image = eglCreateImage(egl_dpy, EGL_NO_CONTEXT,
                                EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (out->image == EGL_NO_IMAGE) {
        printf("  eglCreateImage failed: 0x%x\n", eglGetError());
        close(out->fd);
        out->fd = -1;
        gbm_bo_destroy(out->bo);
        out->bo = NULL;
        return false;
    }

    /* Create GL texture backed by EGLImage */
    glGenTextures(1, &out->tex);
    glBindTexture(GL_TEXTURE_2D, out->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (void *)out->image);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Create FBO */
    glGenFramebuffers(1, &out->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, out->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, out->tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        printf("  Export FBO incomplete: 0x%x\n", status);
        /* Still return true — we can report the FBO status */
    }

    printf("  GBM buffer: fourcc=%s stride=%d modifier=0x%lx fd=%d\n",
           gbm_format_name(out->fourcc), out->stride,
           (unsigned long)out->modifier, out->fd);
    report_tex_format("export", out->tex);

    return true;
}

static void destroy_export_buffer(ExportBuffer *buf)
{
    if (buf->fbo) {
        glDeleteFramebuffers(1, &buf->fbo);
        buf->fbo = 0;
    }
    if (buf->tex) {
        glDeleteTextures(1, &buf->tex);
        buf->tex = 0;
    }
    if (buf->image != EGL_NO_IMAGE) {
        eglDestroyImage(egl_dpy, buf->image);
        buf->image = EGL_NO_IMAGE;
    }
    if (buf->fd >= 0) {
        close(buf->fd);
        buf->fd = -1;
    }
    if (buf->bo) {
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Copy tests                                                         */
/* ------------------------------------------------------------------ */

/* Read back center pixel from export FBO, return true if it matches red */
static bool verify_export_content(ExportBuffer *buf, const char *method,
                                  uint8_t out_px[4])
{
    glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        printf("    %s: FBO incomplete (0x%x), cannot readback\n",
               method, status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        memset(out_px, 0, 4);
        return false;
    }

    glReadPixels(TEX_W / 2, TEX_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                 out_px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    bool ok = (out_px[0] > 200 && out_px[1] < 50 && out_px[2] < 50);
    printf("    %s: [%3d, %3d, %3d, %3d] — %s\n", method,
           out_px[0], out_px[1], out_px[2], out_px[3],
           ok ? "PASS (red)" : "FAIL");
    return ok;
}

/* Clear export FBO to black before each test */
static void clear_export(ExportBuffer *buf)
{
    glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, TEX_W, TEX_H);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static bool try_copy_image_sub_data(ExportBuffer *buf)
{
    if (!glCopyImageSubData) {
        printf("    glCopyImageSubData: not available, skipped\n");
        return false;
    }

    clear_export(buf);
    drain_gl_errors();

    glCopyImageSubData(
        src_tex, GL_TEXTURE_2D, 0, 0, 0, 0,
        buf->tex, GL_TEXTURE_2D, 0, 0, 0, 0,
        TEX_W, TEX_H, 1);
    glFinish();

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("    glCopyImageSubData: GL error 0x%x\n", err);
        return false;
    }

    uint8_t px[4];
    return verify_export_content(buf, "glCopyImageSubData", px);
}

static bool try_fbo_blit(ExportBuffer *buf)
{
    clear_export(buf);
    drain_gl_errors();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, buf->fbo);

    GLenum rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (rs != GL_FRAMEBUFFER_COMPLETE || ds != GL_FRAMEBUFFER_COMPLETE) {
        printf("    glBlitFramebuffer: FBO not complete "
               "(read=0x%x draw=0x%x)\n",
               rs, ds);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBlitFramebuffer(0, 0, TEX_W, TEX_H,
                      0, 0, TEX_W, TEX_H,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();

    GLenum err = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (err != GL_NO_ERROR) {
        printf("    glBlitFramebuffer: GL error 0x%x\n", err);
        return false;
    }

    uint8_t px[4];
    return verify_export_content(buf, "glBlitFramebuffer", px);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* Usage: test_dmabuf_copy [driver]  — e.g. "amdgpu", "nouveau", "i915" */
    const char *driver = (argc > 1) ? argv[1] : NULL;

    printf("=== DMA-BUF copy test ===\n");
    if (driver)
        printf("Preferred driver: %s\n", driver);
    printf("\n");

    if (!open_drm_render_node(driver))
        return 1;
    if (!create_egl_context())
        return 1;
    if (!load_gl_functions())
        return 1;
    if (!create_source_texture())
        return 1;

    printf("\n");

    /* Formats to test — same set used by bloom-terminal and common compositors */
    static const uint32_t formats[] = {
        GBM_FORMAT_ABGR8888,
        GBM_FORMAT_XBGR8888,
        GBM_FORMAT_ARGB8888,
        GBM_FORMAT_XRGB8888,
    };
    static const int n_formats = sizeof(formats) / sizeof(formats[0]);

    int total_pass = 0, total_fail = 0, total_skip = 0;

    for (int i = 0; i < n_formats; i++) {
        printf("--- Format: %s (0x%08x) ---\n",
               gbm_format_name(formats[i]), formats[i]);

        ExportBuffer buf;
        if (!create_export_buffer(formats[i], &buf)) {
            printf("  Skipped (buffer creation failed)\n\n");
            total_skip++;
            continue;
        }

        bool copy_ok = try_copy_image_sub_data(&buf);
        bool blit_ok = try_fbo_blit(&buf);

        if (copy_ok)
            total_pass++;
        else if (glCopyImageSubData)
            total_fail++;
        else
            total_skip++;
        if (blit_ok)
            total_pass++;
        else
            total_fail++;

        destroy_export_buffer(&buf);
        printf("\n");
    }

    /* Summary */
    printf("=== Summary: %d passed, %d failed, %d skipped ===\n",
           total_pass, total_fail, total_skip);

    /* Cleanup */
    glDeleteFramebuffers(1, &src_fbo);
    glDeleteTextures(1, &src_tex);
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    gbm_device_destroy(gbm_dev);
    close(drm_fd);

    return (total_fail > 0) ? 1 : 0;
}
