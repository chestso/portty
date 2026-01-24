#ifndef FONT_FT_H
#define FONT_FT_H

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"

// FreeType/HarfBuzz font backend implementation
extern FontBackend font_backend_ft;

#endif // FONT_FT_H
