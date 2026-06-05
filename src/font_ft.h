#ifndef FONT_FT_H
#define FONT_FT_H

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"

// FreeType/HarfBuzz font backend implementation
extern FontBackend font_backend_ft;

// Select how the text_composition_strategy coverage curve is applied. When the
// GPU glyph shader is active the renderer applies the (luminance-scaled) curve
// at draw time, so rasterization must bake RAW coverage (active = true). When
// inactive (fallback), rasterization bakes the uniform curve itself. Call once
// at renderer init, before the first glyph is rasterized.
void font_ft_set_shader_curve_active(bool active);

#endif // FONT_FT_H
