#ifndef PNG_MODE_H
#define PNG_MODE_H

// Render text to a PNG file using an offscreen SDL window.
// Returns 0 on success, non-zero on failure.
int png_render_text(const char *text, const char *output_path,
                    const char *font_name, int ft_hint_target);

// Spawn `sh -c cmd` on a PTY at `cols x rows`, drain its output for up to
// `wait_ms` (or until the child exits), then render the visible grid to PNG.
// Used for visual A/B comparison of real-app output across VT backends.
// Returns 0 on success, non-zero on failure.
int png_render_exec(const char *cmd, int wait_ms, int cols, int rows,
                    const char *output_path, const char *font_name,
                    int ft_hint_target);

#endif /* PNG_MODE_H */
