# TODO - portty

---

## 0. HiDPI / Display-Migration Follow-ups

**Out of scope (flagged, not doing now):** re-rasterizing fonts when the window
moves between displays of different DPI. The content scale is recomputed from
`portty_compute_content_scale()` (`--dpi-scale` × compositor scale) at startup
and on resize; `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` refreshes only the
cached compositor scale, which the next resize then folds into the content
scale. Font faces are loaded and sized once by `load_fonts()`, and nothing
re-sizes them, so after a migration to a display with a different scale factor
cell metrics and glyph rasterization stay sized for the original display for
the life of the process. A full fix needs a font-reload pass on
`SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`.

---

## 1. Variable Font Axes

The variation axis infrastructure is fully implemented: `cache_mm_var()` caches axis
metadata, `ft_set_variation_axis()` / `ft_set_variation_axes()` set values, and
`apply_font_variations()` applies per-style overrides. The `wght` axis is used for bold,
and `ital`/`slnt` axes are used for italic rendering. Remaining work is to wire up
additional axes at the application level.

### Optical Size (opsz)

**Range:** 6pt to 144pt
**Use Case:** Optimize rendering for different font sizes. Small terminals get more open spacing, large terminals get finer details.

```c
void set_optical_size(FtFontData *ft_data, float font_size_pt) {
    ft_set_axis_value(ft_data, "opsz", font_size_pt);
}
```

### Grade (GRAD)

**Range:** -200 to 200
**Use Case:** Adjust weight without changing metrics (for accessibility). User preference for "high contrast mode" without changing cell sizes.

```c
void adjust_for_readability(FtFontData *ft_data, int grade) {
    ft_set_axis_value(ft_data, "GRAD", (float)grade);
}
```

### Custom Axes

Support for font-specific axes (e.g., "ROND" for roundness, "SOFT" for softness).

```c
bool ft_set_custom_axis(FtFontData *ft_data, uint32_t tag, float value) {
    for (int i = 0; i < ft_data->num_axes; i++) {
        if (ft_data->axes[i].tag == tag) {
            if (value < ft_data->axes[i].min_value ||
                value > ft_data->axes[i].max_value) {
                return false;
            }

            ft_data->axes[i].current_value = value;

            FT_Fixed *coords = build_coordinate_array(ft_data);
            FT_Set_Var_Design_Coordinates(ft_data->ft_face, ft_data->num_axes, coords);
            hb_ft_font_changed(ft_data->hb_font);
            free(coords);

            return true;
        }
    }
    return false;
}
```

### Configuration Interface

Font axis settings would extend the existing `portty.conf` config file (parsed by
`portty_conf.h/c`). Example additions:

```ini
font = Cascadia Code-14
optical_size = auto
grade = 0

# Per-style overrides
bold_weight = 700
italic_slant = -12
```

---

## 2. Bidirectional Text (BiDi) Support

### Goals

- Full Unicode BiDi Algorithm (UAX #9)
- Mixed LTR/RTL text on same line
- Proper cursor movement in RTL text

### Technical Design

#### BiDi Resolution with FriBidi

```c
#include <fribidi.h>

typedef struct BiDiRun {
    int start;
    int length;
    hb_direction_t dir;
    uint8_t level;
} BiDiRun;

BiDiRun *analyze_bidi(uint32_t *codepoints, int count, int *num_runs) {
    FriBidiCharType *types = malloc(count * sizeof(FriBidiCharType));
    FriBidiLevel *levels = malloc(count * sizeof(FriBidiLevel));

    fribidi_get_bidi_types(codepoints, count, types);

    FriBidiLevel base_level = FRIBIDI_TYPE_LTR;
    fribidi_get_par_embedding_levels(types, count, &base_level, levels);

    BiDiRun *runs = malloc(count * sizeof(BiDiRun));
    int run_count = 0;

    int run_start = 0;
    FriBidiLevel current_level = levels[0];

    for (int i = 1; i <= count; i++) {
        if (i == count || levels[i] != current_level) {
            runs[run_count].start = run_start;
            runs[run_count].length = i - run_start;
            runs[run_count].level = current_level;
            runs[run_count].dir = (current_level % 2) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
            run_count++;

            if (i < count) {
                run_start = i;
                current_level = levels[i];
            }
        }
    }

    free(types);
    free(levels);

    *num_runs = run_count;
    return runs;
}
```

#### Rendering BiDi Text

```c
void render_bidi_line(Renderer *rend, uint32_t *codepoints, int count,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    int num_runs;
    BiDiRun *runs = analyze_bidi(codepoints, count, &num_runs);

    for (int i = 0; i < num_runs; i++) {
        BiDiRun *run = &runs[i];

        ShapedGlyphRun *shaped = shape_text(
            ft_data,
            &codepoints[run->start],
            run->length,
            run->dir
        );

        renderer_draw_shaped_text(rend, shaped, ft_data, x, y, r, g, b);
        x += shaped->total_advance;

        free_shaped_run(shaped);
    }

    free(runs);
}
```

#### Cursor Movement in BiDi

```c
int cursor_move_visual(BiDiContext *ctx, int current_pos, int direction) {
    int *visual_to_logical = malloc(ctx->length * sizeof(int));
    fribidi_reorder_line(ctx->levels, ctx->length, 0, ctx->base_dir,
                         NULL, NULL, NULL, visual_to_logical);

    int visual_pos = -1;
    for (int i = 0; i < ctx->length; i++) {
        if (visual_to_logical[i] == current_pos) {
            visual_pos = i;
            break;
        }
    }

    int new_visual_pos = visual_pos + direction;
    if (new_visual_pos < 0 || new_visual_pos >= ctx->length) {
        free(visual_to_logical);
        return current_pos;
    }

    int new_logical_pos = visual_to_logical[new_visual_pos];
    free(visual_to_logical);

    return new_logical_pos;
}
```

### External Dependencies

- **GNU FriBidi:** BiDi algorithm implementation
- **HarfBuzz:** Already supports RTL shaping
- **FreeType:** No changes needed

### Implementation Plan

1. Add FriBidi dependency to build system
2. Implement BiDi analysis function
3. Modify renderer to handle multiple runs per line
4. Add cursor movement logic
5. Test with Arabic/Hebrew test files

---

## 3. Unimplemented Terminfo Capabilities

These capabilities reach portty through `coffer-vty-256color` (which is
`use=xterm-256color`), but they have no implementation anywhere in the
pipeline: the cell fields exist, nothing sets them, and nothing reads them.

- **font (SGR 10–19)** — `CfrCell.font` is bridged to `TerminalCellAttr.font:4`
  and printed by `dump.c`, but no code assigns it: coffer does not parse
  SGR 10–19, and portty loads no alternative faces.
- **dwl (DECDWL, `ESC # 6`)** — `CFR_ATTR_DWL` is bridged to
  `TerminalCellAttr.dwl:1` and dumped, but nothing raises the bit: coffer does
  not handle `ESC #`, and the renderer never reads it.
- **dhl (DECDHL, `ESC # 3` / `ESC # 4`)** — the same for
  `CFR_ATTR_DHL_TOP` / `CFR_ATTR_DHL_BOTTOM` → `TerminalCellAttr.dhl:2`.

Implementing any of these spans both repositories: coffer must parse the
sequence and raise the attribute, and the renderer must act on it — width-scaled
glyph runs for DWL, a clipped top or bottom half for DHL, and alternate faces
from the font resolver for font select.

---

## 4. Popular Unsupported Capabilities

None of these is implemented. Some are inherited from `xterm-256color` as
accepted-but-ignored SGR codes, the rest are absent from the terminfo entry
entirely; all are commonly expected by modern TUIs.

### OSC 10 / OSC 11 set (default foreground / background)

The query form is answered: coffer's `osc_color_query()` replies to
`OSC 10 ; ?` and `OSC 11 ; ?` with the defaults from its Dracula palette, which
is what image viewers (chafa, viu, lsix) read before alpha-compositing. The set
form (`OSC 10 ; rgb:R/G/B`) is parsed and dropped, so an application cannot
recolor the terminal. portty reads its colors once at startup through
`cfr_default_fg_rgb()` / `cfr_default_bg_rgb()` (`term_colors.c`); no
configuration key overrides them.

### OSC 12 (set cursor color)

Not handled. The cursor color is fixed: `term_colors.c` uses palette entry 4
(Dracula purple).

### OSC 112 (reset cursor color)

Not handled — the same reset as `Cr=\E]112\007` in xterm-256color. It is only
observable once OSC 12 can set a cursor color, so the two go together.

### OSC 4 / OSC 104 (palette set / reset)

Not implemented; coffer logs a one-time warning for each (`CFR_LOGGED_OSC4`,
`CFR_LOGGED_OSC104`). The 256-color palette is compile-time in coffer.

### SGR 53 (overline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Would need a
new `CFR_ATTR_OVERLINE` and renderer support (draw a line above the cell).
Note: terminfo has no standard cap for overline; it is a SGR-only feature.

### SGR 21 (double underline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Could map to
underline style 2 (double) which coffer already supports via SGR 4:2.
Alternatively, add `case 21:` that sets `pen.underline = CFR_UL_DOUBLE`.

### DECSCA (Select Character Protection, CSI " q)

Not in terminfo, not implemented. Used for protected characters that survive
erase operations. Would require a per-cell protection attribute and
modification of all erase operations to check it.

### SGR font selection (10-19)

The cell fields exist but neither end of the pipeline uses them — see §3.
