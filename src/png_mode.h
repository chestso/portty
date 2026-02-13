#ifndef PNG_MODE_H
#define PNG_MODE_H

// Render text to a PNG file using an offscreen SDL window.
// Returns 0 on success, non-zero on failure.
int png_render_text(const char *text, const char *output_path,
                    const char *font_name, int ft_hint_target);

#endif /* PNG_MODE_H */
