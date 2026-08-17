# TODO - portty

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
`portty_conf.h/c`). Example additions to the `[terminal]` section:

```ini
[terminal]
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

These capabilities are inherited from `xterm-256color` via `use=xterm-256color`
but are not yet rendered by portty.

### Not rendered

These attributes are parsed and stored by coffer but not rendered.

| Attribute        | Stored in `TerminalCellAttr` | Rendered by |
| ---------------- | ---------------------------- | ----------- |
| font (SGR 10-19) | `font:4`                     | Not read    |
| dwl (DECDWL)     | `dwl:1`                      | Not read    |
| dhl (DECDHL)     | `dhl:2`                      | Not read    |

---

## 4. Popular Unsupported Capabilities

These capabilities are not in the terminfo entry, not inherited, and not
implemented, but are commonly expected by modern TUIs.

### OSC 11 (set default foreground color)

Not in terminfo, not implemented. Some TUIs query this to detect background
color for contrast detection. coffer's OSC dispatcher forwards unknown OSCs to
the generic `callbacks.osc` hook, but portty does not handle OSC 11. Full
implementation would require parsing `OSC 11 ; ?` queries (responding with the
current default foreground RGB) and `OSC 11 ; rgb:R/G/B` sets.

### OSC 12 (set default background color)

Not in terminfo, not implemented. Same use as OSC 11 for background color
detection. Would follow the same parse/respond/set pattern.

### OSC 112 (reset cursor color)

Not in terminfo, not implemented. Resets cursor color to default (same as
`Cr=\E]112\007` in xterm-256color, which resets cursor color specifically, as
opposed to OSC 12 which sets it).

### SGR 53 (overline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Would need a
new `CFR_ATTR_OVERLINE` and renderer support (draw a line above the cell).
Note: terminfo has no standard cap for overline; it is a SGR-only feature.

### SGR 21 (double underline)

Inherited from xterm-256color as an accepted-but-ignored SGR. Could map to
underline style 2 (double) which coffer already supports via SGR 4:2.
Alternatively, add `case 21:` that sets `pen.underline = CFR_UL_DOUBLE`.

### XTVERSION (CSI > q)

Not in terminfo, not implemented. TUIs use this to query the terminal's name
and version for feature detection. The `RV` cap in xterm-256color sends
`ESC[>c` but coffer does not respond to `CSI > q` (XTVERSION query). DA2
(`CSI > c`) IS implemented and responds with `ESC[>1;0;0c`.

### E3 (clear scrollback)

Inherited from xterm-256color as `\E[3J`. coffer handles `CSI 3 J` but routes
it to the same code path as `CSI 2 J` (clear visible grid only). The
scrollback buffer is NOT purged. `cfr_scrollback_clear()` exists but is never
called from the ED path. Fix: call `cfr_scrollback_clear(vt)` in the `case 3:`
branch of `cfr_erase_in_display()`.

### DECSCA (Select Character Protection, CSI " q)

Not in terminfo, not implemented. Used for protected characters that survive
erase operations. Would require a per-cell protection attribute and
modification of all erase operations to check it.

### SGR font selection (10-19)

Parsed and stored in `attrs.font` but no renderer reads it. Full
implementation would require loading and selecting alternate font faces via
SGR 10 (primary), 11-19 (alternative fonts).
