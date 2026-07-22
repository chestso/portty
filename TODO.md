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

## 3. Sokol Backend: Diagonal Box-Drawing Seams

### Status: Completed

The diagonal seam problem has been resolved with proportional margins and zero-coverage texel discarding.

### The Solution: Proportional Margins

The key insight from `rectangles.py`:

1. **Glyph size = 125% of cell size** (25% margin on all sides)
2. **Lines drawn across margin bounds**, not cell bounds
3. **When cells tile edge-to-edge, the extended lines connect seamlessly**

Example for a 32×96 cell:

- Margin: 8px horizontal (25% of 32), 24px vertical (25% of 96)
- Glyph bitmap: 40×120 (cell + margins)
- Line: drawn from corner to corner of the full margin bounds

When such glyphs are placed centered at negative offsets (so the cell portion aligns with the cell position), the 25% overhang on each side ensures diagonal lines connect perfectly with adjacent cells.

### Why Previous Approaches Failed

The earlier approaches used fixed pixel padding (1-2px) rather than proportional margins. This caused two problems:

1. **DPI scaling**: Fixed 1px pad becomes proportionally smaller at higher DPI
2. **Incorrect line geometry**: Lines drawn to cell corners rather than margin bounds

A 25% margin scales with cell size at any DPI, and drawing lines across the full margin bounds ensures continuous lines across cell boundaries.

### Implementation

1. `rend_boxdraw.c` now uses 10% proportional margins for diagonal characters (U+2571-U+2573)
2. Lines extend to bitmap edges for continuous coverage at row boundaries
3. Sokol backend discards zero-coverage glyph texels to prevent seams
4. Both backends share the same `rend_boxdraw.c` implementation

### Commits

- `77bc344` - Fix diagonal box-drawing seams with proportional margins
- `46ec8da` - Discard zero-coverage glyph texels to fix Sokol diagonal seams
- `2f59881` - Remove duplicate SDL3 boxdraw implementation (consolidated to common code)

---

## 4. SDL3 Linear-Light Sixel Rendering

### Status: Completed

Sixel images appeared too bright in SDL3 when using linear-light rendering (gpu/vulkan renderers). The fix adds CPU-side sRGB→linear conversion for sixel and lottie textures before upload.

### Root Cause

SDL3's linear-light path (`linear_ok = true`) renders to an `SDL_COLORSPACE_SRGB_LINEAR` float target. SDL linearizes draw/vertex colors but does not decode sampled texture texels. Sixel/lottie textures uploaded as raw sRGB appeared double-encoded (too bright/washed out).

### Solution

1. Added `rend_linearize_rgba_in_place()` in `rend_common.c` - converts RGBA sRGB→linear using the existing atlas LUT
2. Added `linearize_for_upload()` in `rend_sdl3.c` - allocates temp buffer and linearizes when needed
3. Updated `sixel_get_texture()` and `lottie_get_texture()` to linearize before upload

### Commit

- `64c7cd2` - Fix bright sixel images in SDL3 linear-light mode

---

## 5. Unimplemented Terminfo Capabilities

These capabilities are inherited from `xterm-256color` via `use=xterm-256color`
but are not yet fully implemented. coffer now has explicit case handlers (no
more silent `default: break` fallthrough) with logging for unimplemented
sequences.

### Status: Blink and DECSCNM Concluded

Blinking text (SGR 5) and screen-level reverse video (DECSCNM, `?5`) are
deliberately not rendered. Both are considered accessibility hazards —
blinking text causes visual distraction and can trigger seizures, while
screen-level reverse video (distinct from per-cell SGR 7 reverse) is
primarily used for visual bell effects which are better handled by modern
UI patterns. The attributes are parsed and stored for compatibility, but the
visual effects are intentionally omitted. No further work is needed.

| Capability                | Sequence    | coffer status                                               | portty renderer                                       |
| ------------------------- | ----------- | ----------------------------------------------------------- | ----------------------------------------------------- |
| `blink` (SGR 5)           | `\E[5m`     | `CFR_ATTR_BLINK` bit set, cleared by SGR 25                 | Deliberately not rendered (accessibility) — concluded |
| DECSCNM (?5)              | `\E[?5h`    | `CFR_MODE_REVERSE_VIDEO` tracked, `set_mode` callback fired | Deliberately not rendered (accessibility) — concluded |
| `smm`/`rmm` (meta, ?1034) | `\E[?1034h` | `CFR_MODE_META` tracked, logged once                        | Noop                                                  |

### Noop + log (no state to track)

| Capability                | Sequence    | coffer status                        |
| ------------------------- | ----------- | ------------------------------------ |
| `mc0`/`mc4`/`mc5` (CSI i) | `\E[i` etc. | Logged once per CfrTerm, then silent |
| `meml` (ESC l)            | `\El`       | Logged once per CfrTerm, then silent |
| `memu` (ESC m)            | `\Em`       | Logged once per CfrTerm, then silent |
| `initc` (OSC 4)           | `\E]4;...`  | Logged once per CfrTerm, then silent |
| `oc` (OSC 104)            | `\E]104`    | Logged once per CfrTerm, then silent |

### Not rendered by either backend

These attributes are parsed and stored by coffer but not rendered by
either backend. Blink and DECSCNM are deliberately omitted (accessibility)
— see the Concluded section above for rationale.

| Attribute        | Stored in `TerminalCellAttr` | SDL3     | Sokol    |
| ---------------- | ---------------------------- | -------- | -------- |
| font (SGR 10-19) | `font:4`                     | Not read | Not read |
| dwl (DECDWL)     | `dwl:1`                      | Not read | Not read |
| dhl (DECDHL)     | `dhl:2`                      | Not read | Not read |

Note: cursor blink (DECSET ?12) is separate from text blink (SGR 5) and is
fully implemented in both backends via timer-based cursor visibility toggling.

---

## 6. Popular Unsupported Capabilities

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
