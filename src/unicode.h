#ifndef UNICODE_H
#define UNICODE_H

#include <stdbool.h>
#include <stdint.h>

// Unicode codepoint constants
#define UNICODE_ZERO_WIDTH_JOINER      0x200D
#define UNICODE_VARIATION_SELECTOR_16  0xFE0F
#define UNICODE_SKIN_TONE_MIN          0x1F3FB
#define UNICODE_SKIN_TONE_MAX          0x1F3FF
#define UNICODE_REGIONAL_INDICATOR_MIN 0x1F1E6
#define UNICODE_REGIONAL_INDICATOR_MAX 0x1F1FF

// Emoji detection functions
bool is_emoji_base_range(uint32_t cp);
bool is_ambiguous_emoji(uint32_t cp);
bool is_emoji_presentation(uint32_t cp);
bool is_regional_indicator(uint32_t cp);
bool is_zwj(uint32_t cp);
bool is_skin_tone_modifier(uint32_t cp);

// Returns true if any codepoint in the cell's chars[] is U+FE0F (VS16).
// Used to enforce the "VS16 → 2 cells" rule of the emoji width paradigm.
bool unicode_cell_has_vs16(const uint32_t *chars, int max);

// Returns true if the cell's char[] is a VS16-widened emoji: base codepoint
// is in the emoji-presentation set AND the cell contains U+FE0F. These cells
// have a presentation width of 2 even though libvterm reports them as 1.
bool unicode_cell_is_vs16_emoji(const uint32_t *chars, int max);

// UTF-8 conversion
int utf8_to_codepoints(const char *utf8, uint32_t *out, int max_out);

#endif /* UNICODE_H */
