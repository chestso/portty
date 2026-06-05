#ifndef REND_SDL3_SHADER_H
#define REND_SDL3_SHADER_H

#include <stdbool.h>

#include <SDL3/SDL.h>

// GPU glyph-coverage shader: applies the text_composition_strategy gamma/
// contrast curve at draw time, scaled by fg/bg luminance, so dark-on-light
// (reverse video) text thickens while light-on-dark stays neutral. See
// src/shaders/glyph_coverage.frag.glsl.
//
// This wraps SDL3's custom-fragment-shader API (SDL_GPURenderState), which is
// only available on the Vulkan-backed "gpu"/"vulkan" renderers with SPIR-V.
// When unavailable the renderer falls back to the atlas-baked uniform curve in
// font_ft.c — so rend_shader_create() simply returns NULL and callers treat
// NULL as "use the fallback".
typedef struct RendShaderState RendShaderState;

// Create the glyph-coverage render state for this renderer, or return NULL to
// signal the caller should fall back to the baked curve. Returns NULL when:
// the curve is neutral (gamma 1, contrast 0); BLOOM_GLYPH_SHADER=0; the
// renderer has no GPU device (software/opengl); SPIR-V is unsupported; or
// shader/render-state creation fails. gamma/contrast are the raw
// bloom_text_gamma / bloom_text_contrast (contrast in 0..100).
RendShaderState *rend_shader_create(SDL_Renderer *renderer, float gamma, float contrast);

void rend_shader_destroy(RendShaderState *s);

// Update the background-luminance uniform for the next glyph. Cheap: only
// pushes to the GPU when the value actually changes, so long runs of identical
// background (the common case) don't break draw batching. bg_luma is 0..1.
void rend_shader_set_bg_luma(RendShaderState *s, float bg_luma);

// Activate / deactivate the custom shader for subsequent draws. Bind around a
// glyph blit, unbind immediately after, so cursor/selection fills and other
// draws keep SDL's default shader.
void rend_shader_bind(RendShaderState *s);
void rend_shader_unbind(RendShaderState *s);

#endif // REND_SDL3_SHADER_H
