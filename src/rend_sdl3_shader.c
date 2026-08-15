/*
 * portty — SDL_GPU glyph-coverage shader plumbing
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

// GPU glyph-coverage shader plumbing. Keeps the SDL_gpu / SDL_GPURenderState
// calls out of rend_sdl3.c. See rend_sdl3_shader.h and
// src/shaders/glyph_coverage.frag.glsl.

#include "rend_sdl3_shader.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "shaders/glyph_coverage_frag_spv.h"

// Fragment uniform block (set 3, binding 0). Mirrors the Params block in
// glyph_coverage.frag.glsl. Kept to one vec4 so std140 alignment is trivial.
typedef struct
{
    float gamma;
    float contrast; // already divided by 100
    float bg_luma;  // 0..1
    float _pad;
} GlyphCurveUniforms;

struct RendShaderState
{
    SDL_Renderer *renderer;
    SDL_GPUDevice *device;
    SDL_GPUShader *shader;
    SDL_GPURenderState *state;
    float gamma;
    float contrast;     // already divided by 100
    float last_bg_luma; // -1 forces the first uniform push
};

RendShaderState *rend_shader_create(SDL_Renderer *renderer, float gamma, float contrast)
{
    if (!renderer)
        return NULL;

    // Neutral curve: nothing to apply, so the baked path (identity) is fine and
    // we avoid the shader entirely.
    if (gamma == 1.0f && contrast == 0.0f) {
        vlog("Glyph shader: neutral curve, using fallback (no shader)\n");
        return NULL;
    }

    const char *env = SDL_getenv("PORTTY_GLYPH_SHADER");
    if (env && strcmp(env, "0") == 0) {
        vlog("Glyph shader: disabled via PORTTY_GLYPH_SHADER=0\n");
        return NULL;
    }

    SDL_GPUDevice *device = SDL_GetGPURendererDevice(renderer);
    if (!device) {
        vlog("Glyph shader: renderer has no GPU device, using fallback\n");
        return NULL;
    }

    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(device);
    if (!(fmts & SDL_GPU_SHADERFORMAT_SPIRV)) {
        vlog("Glyph shader: SPIR-V unsupported on this GPU backend, using fallback\n");
        return NULL;
    }

    SDL_GPUShaderCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.code = glyph_coverage_frag_spv;
    sci.code_size = glyph_coverage_frag_spv_len;
    sci.entrypoint = "main";
    sci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    sci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sci.num_samplers = 1;        // set 2, binding 0 = the active draw texture
    sci.num_uniform_buffers = 1; // set 3, binding 0 = GlyphCurveUniforms

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &sci);
    if (!shader) {
        vlog("Glyph shader: SDL_CreateGPUShader failed (%s), using fallback\n", SDL_GetError());
        return NULL;
    }

    SDL_GPURenderStateCreateInfo rsci;
    memset(&rsci, 0, sizeof(rsci));
    rsci.fragment_shader = shader;

    SDL_GPURenderState *state = SDL_CreateGPURenderState(renderer, &rsci);
    if (!state) {
        vlog("Glyph shader: SDL_CreateGPURenderState failed (%s), using fallback\n", SDL_GetError());
        SDL_ReleaseGPUShader(device, shader);
        return NULL;
    }

    RendShaderState *s = calloc(1, sizeof(*s));
    if (!s) {
        SDL_DestroyGPURenderState(state);
        SDL_ReleaseGPUShader(device, shader);
        return NULL;
    }

    s->renderer = renderer;
    s->device = device;
    s->shader = shader;
    s->state = state;
    s->gamma = gamma;
    s->contrast = contrast / 100.0f;
    s->last_bg_luma = -1.0f;
    return s;
}

void rend_shader_destroy(RendShaderState *s)
{
    if (!s)
        return;
    if (s->state)
        SDL_DestroyGPURenderState(s->state);
    if (s->shader)
        SDL_ReleaseGPUShader(s->device, s->shader);
    free(s);
}

void rend_shader_set_bg_luma(RendShaderState *s, float bg_luma)
{
    if (!s || bg_luma == s->last_bg_luma)
        return;
    GlyphCurveUniforms u = { s->gamma, s->contrast, bg_luma, 0.0f };
    SDL_SetGPURenderStateFragmentUniforms(s->state, 0, &u, sizeof(u));
    s->last_bg_luma = bg_luma;
}

void rend_shader_bind(RendShaderState *s)
{
    if (s)
        SDL_SetGPURenderState(s->renderer, s->state);
}

void rend_shader_unbind(RendShaderState *s)
{
    if (s)
        SDL_SetGPURenderState(s->renderer, NULL);
}
