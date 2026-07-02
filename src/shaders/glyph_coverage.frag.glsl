#version 450

// portty glyph coverage shader for SDL3's GPU 2D renderer.
//
// Bound (via SDL_GPURenderState) only for the non-color glyph blit, replacing
// SDL's default textured fragment shader. It reproduces the default behavior
// `texture * vertex_color`, then reshapes the resulting coverage (alpha) with
// the text_composition_strategy gamma/contrast curve — the same curve as
// build_coverage_lut() in font_ft.c — but scales the curve's STRENGTH by the
// foreground-vs-background luminance:
//
//   * full strength when fg is darker than bg (dark-on-light / reverse video),
//   * tapering to identity when fg is lighter than bg (normal light-on-dark).
//
// So washed-out reverse-video text thickens while normal text stays neutral.
//
// Only the ALPHA is changed; RGB is emitted exactly as the default shader would
// (texture * vertex_color), so the linear-light pipeline and color space are
// untouched (alpha is gamma-agnostic). For color glyphs the foreground (vertex
// color) is white, so strength -> 0 and the curve is a no-op anyway; the
// renderer additionally bypasses this shader for color glyphs.
//
// SPIR-V resource model for SDL_gpu fragment shaders (see SDL_gpu.h):
//   set 2 = sampled textures (binding 0 = the active draw texture / glyph atlas)
//   set 3 = uniform buffers   (binding 0 = our params = fragment uniform slot 0)

layout(location = 0) in vec4 v_color; // vertex color = foreground (colormod)
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(set = 3, binding = 0) uniform Params {
    float gamma;    // > 1 thickens strokes (text_composition_strategy gamma)
    float contrast; // 0..1, already divided by 100
    float bg_luma;  // perceptual luma of the cell background, 0..1
    float _pad;
} u;

void main()
{
    vec4 base = texture(u_tex, v_uv) * v_color; // == SDL default shader output
    float cov = base.a;

    // Luminance direction of this cell. strength == 1 when fg is much darker
    // than bg, 0 when fg is lighter; matched on the CPU side (bg_luma) using the
    // same Rec.709 weights.
    float fg_luma = dot(v_color.rgb, vec3(0.2126, 0.7152, 0.0722));
    float strength = clamp(u.bg_luma - fg_luma, 0.0, 1.0);

    // Per-pixel coverage curve: pow(cov, 1/gamma) then contrast around the
    // midpoint. Mirrors build_coverage_lut() exactly.
    float g = max(u.gamma, 1e-3);
    float curved = pow(cov, 1.0 / g);
    if (u.contrast > 0.0)
        curved = clamp(0.5 + (curved - 0.5) * (1.0 + u.contrast), 0.0, 1.0);

    // Blend raw <-> curved by luminance strength, so light-on-dark stays neutral.
    float adj = mix(cov, curved, strength);

    o_color = vec4(base.rgb, adj);
}
