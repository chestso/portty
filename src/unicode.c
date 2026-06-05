#include "unicode.h"
#include <stdbool.h>
#include <stdint.h>

bool is_emoji_presentation(uint32_t cp)
{
    // Codepoints that should prefer the color-emoji font. This is the union of
    // Unicode's Emoji_Presentation=Yes set (default emoji — color without VS16)
    // and the text-default symbol band bloom also colors by default ("color
    // preferred, independent of VS16"). The color decision is still guarded by
    // whether the emoji font actually carries the glyph (see render_cell), so
    // the broad SMP blocks below are intentional supersets — they have no
    // Emoji_Presentation gaps and any over-inclusion is harmless. Regional
    // indicators are handled separately (is_regional_indicator). Tracks Unicode
    // Emoji 17.0 (unicode.org/Public/UCD/latest/ucd/emoji/emoji-data.txt).
    return
        // BMP symbols & dingbats (U+231A..U+2BFF)
        (cp >= 0x231A && cp <= 0x231B) || // ⌚⌛
        cp == 0x2328 ||                   // ⌨
        (cp >= 0x23E9 && cp <= 0x23FA) || // ⏩..⏺ media controls
        (cp >= 0x25FD && cp <= 0x25FE) || // ◽◾
        (cp >= 0x2600 && cp <= 0x27BF) || // ☀..➿ misc symbols & dingbats
        (cp >= 0x2B1B && cp <= 0x2B1C) || // ⬛⬜
        cp == 0x2B50 || cp == 0x2B55 ||   // ⭐ ⭕
        // Enclosed Alphanumeric / Ideographic Supplement (U+1F000..U+1F2FF)
        cp == 0x1F004 || cp == 0x1F0CF ||                    // 🀄 🃏
        cp == 0x1F18E || (cp >= 0x1F191 && cp <= 0x1F19A) || // 🆎 🆑..🆚
        cp == 0x1F201 || cp == 0x1F21A || cp == 0x1F22F ||   // 🈁 🈚 🈯
        (cp >= 0x1F232 && cp <= 0x1F23A) ||                  // 🈲..🈺
        (cp >= 0x1F250 && cp <= 0x1F251) ||                  // 🉐🉑
        // Supplementary Multilingual Plane emoji blocks (supersets)
        (cp >= 0x1F300 && cp <= 0x1F5FF) || // Misc Symbols & Pictographs
        (cp >= 0x1F600 && cp <= 0x1F64F) || // Emoticons
        (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport & Map
        (cp >= 0x1F7E0 && cp <= 0x1F7F0) || // 🟠..🟫 colored shapes, 🟰
        (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental Symbols & Pictographs
        (cp >= 0x1FA70 && cp <= 0x1FAFF);   // Symbols & Pictographs Extended-A
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

bool unicode_cell_is_vs16_emoji(const uint32_t *chars, int max)
{
    if (!chars || max < 1 || chars[0] == 0)
        return false;
    if (!is_emoji_presentation(chars[0]))
        return false;
    return unicode_cell_has_vs16(chars, max);
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
