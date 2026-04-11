#include "unicode.h"
#include <stdbool.h>
#include <stdint.h>

bool is_emoji_base_range(uint32_t cp)
{
    // Proper emoji base ranges from Unicode standard
    return (cp >= 0x1F300 && cp <= 0x1F5FF) || // Miscellaneous Symbols and Pictographs
           (cp >= 0x1F600 && cp <= 0x1F64F) || // Emoticons
           (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport and Map Symbols
           (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental Symbols and Pictographs
           (cp >= 0x1FA70 && cp <= 0x1FAFF);   // Symbols and Pictographs Extended-A
}

bool is_ambiguous_emoji(uint32_t cp)
{
    // Text-default emoji: Emoji=Yes, Emoji_Presentation=No per Unicode.
    // These render as text by default and only as emoji with U+FE0F.
    // Ranges that have Emoji_Presentation=Yes (0x1F300+) belong in
    // is_emoji_base_range() instead.
    return (cp >= 0x2600 && cp <= 0x27BF) || // Miscellaneous Symbols, Dingbats
           (cp >= 0x231A && cp <= 0x231B) || // Watch, Clock
           (cp == 0x2328) ||                 // Keyboard
           (cp >= 0x23E9 && cp <= 0x23FA);   // Media controls
}

bool is_emoji_presentation(uint32_t cp)
{
    // Check if character is in emoji base ranges or ambiguous emoji that needs variation selector
    return is_emoji_base_range(cp) || is_ambiguous_emoji(cp);
}

bool is_regional_indicator(uint32_t cp)
{
    // Regional indicators (U+1F1E6 to U+1F1FF)
    return (cp >= 0x1F1E6 && cp <= 0x1F1FF);
}

bool is_zwj(uint32_t cp)
{
    // Zero Width Joiner
    return (cp == 0x200D);
}

bool is_skin_tone_modifier(uint32_t cp)
{
    // Skin tone modifiers (U+1F3FB to U+1F3FF)
    return (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

bool unicode_cell_has_vs16(const uint32_t *chars, int max)
{
    if (!chars)
        return false;
    for (int i = 0; i < max && chars[i] != 0; i++) {
        if (chars[i] == UNICODE_VARIATION_SELECTOR_16)
            return true;
    }
    return false;
}

/* Convert UTF-8 string to an array of Unicode codepoints.
 * Returns number of codepoints written, or -1 on error. */
int utf8_to_codepoints(const char *utf8, uint32_t *out, int max_out)
{
    int count = 0;
    const uint8_t *s = (const uint8_t *)utf8;

    while (*s && count < max_out) {
        uint32_t cp;
        int len;

        if (s[0] < 0x80) {
            cp = s[0];
            len = 1;
        } else if ((s[0] & 0xE0) == 0xC0) {
            cp = s[0] & 0x1F;
            len = 2;
        } else if ((s[0] & 0xF0) == 0xE0) {
            cp = s[0] & 0x0F;
            len = 3;
        } else if ((s[0] & 0xF8) == 0xF0) {
            cp = s[0] & 0x07;
            len = 4;
        } else {
            return -1; /* invalid UTF-8 */
        }

        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80)
                return -1;
            cp = (cp << 6) | (s[i] & 0x3F);
        }

        out[count++] = cp;
        s += len;
    }
    return count;
}
