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

### Diagonal Box-Drawing Padding Explained

#### The Problem

When diagonal box-drawing characters (╱ U+2571, ╲ U+2572, ╳ U+2573) are stacked vertically in a terminal, a visible brightness dip appears at every row boundary. Measured on a 20×44 cell (≈192 DPI):

```
y=42: avg_R=2.25   ← normal
y=43: avg_R=0.96   ← 57% drop
y=44: avg_R=1.41   ← row boundary
y=45: avg_R=1.71   ← recovery
```

This happens because the anti-aliased line converges toward a corner of the cell, leaving fewer lit pixels near the edge.

#### What "Padding" Means

Each diagonal glyph is rendered into a bitmap slightly larger than the cell. For a 20×44 cell with `pad=1`, the bitmap is 22×46 — one extra pixel on each side:

```
┌──────────────────────┐ ← padding (1px)
│ ┌──────────────────┐ │
│ │   visible cell   │ │
│ │     (20×44)      │ │
│ └──────────────────┘ │
└──────────────────────┘ ← padding (1px)
      22×46 bitmap
```

The backend places this bitmap offset by `-pad` so it extends 1px beyond the cell boundary. This overhang lets AA pixels from one cell bleed into its neighbor's area.

#### What "Overhang" Means

Overhang is the portion of a glyph's bitmap that extends **outside** its assigned cell rectangle and paints into neighboring cells' areas. It happens because the bitmap is larger than the cell and is placed with a negative offset:

```
         cell N              cell N+1
    ┌──────────────┐     ┌──────────────┐
    │              │     │              │
    │   ╱╱╱╱╱╱╱   │     │   ╱╱╱╱╱╱╱   │
    │ ╱╱╱╱╱╱╱╱╱╱╱│╱╱╱╱╱│╱╱╱╱╱╱╱╱╱╱╱ │
    │╱╱╱╱╱╱╱╱╱╱╱╱│╱╱╱╱╱│╱╱╱╱╱╱╱╱╱╱╱╱│
    ├──────────────┤←boundary──────────┤
    │▓▓▓▓▓▓▓▓▓▓▓▓▓│                     ← overhang from cell N
    │              │▓▓▓▓▓▓▓▓▓▓▓▓▓       ← overhang from cell N+1
    │              │                     (extends upward into cell N)
    └──────────────┘     └──────────────┘
         ▲                      ▲
    bitmap placed at        bitmap placed at
    cell_x - pad            cell_x - pad
```

Without overhang, each cell's diagonal would be clipped exactly at the cell edge. The last scanline of a ╱ near the bottom-left corner would have only a single pixel at x=0, then nothing — creating a hard brightness drop. With overhang, the adjacent cell's bitmap paints its top-right corner pixels into this cell's bottom area (and vice versa), filling in some of the gap.

The overhang is invisible for non-diagonal glyphs because their bitmaps are exactly cell-sized (`pad=0`). Only diagonals use `pad > 0`, making them the only glyphs that intentionally paint outside their cell bounds.

#### Why Pad Should Scale With DPI (But Can't)

Cell dimensions scale with display DPI, and so does line thickness (`w/5`):

| DPI | Cell Size | Thickness | Pad | Ratio |
| --- | --------- | --------- | --- | ----- |
| 96  | 10×22     | 2         | 1   | 0.50  |
| 144 | 15×33     | 3         | 1   | 0.33  |
| 192 | 20×44     | 4         | 1   | 0.25  |
| 288 | 30×66     | 6         | 1   | 0.17  |

In theory, pad should scale proportionally to thickness so the overhang remains effective relative to the line width. At high DPI the ratio drops from 0.5 to 0.17, meaning the 1px overhang covers a shrinking fraction of the line's AA spread.

In practice, increasing pad makes the seam **worse**. The diagonal exits through a corner, so near that corner the line occupies only 1-2 pixels horizontally. Larger padding pushes those pixels further into the overhang zone (outside the visible cell), leaving the visible area emptier near the boundary. This is true regardless of whether lines are drawn to bitmap corners or extrapolated from cell corners — the geometry is fundamentally hostile to scaling pad.

Pad is therefore pinned at 1 as the least-bad option: it provides some overhang benefit without making the corner-exit problem worse. Fixing the seam properly likely requires a different approach entirely (e.g., drawing across cell boundaries at render time rather than baking overhang into the glyph bitmap).

#### Why Simply Increasing Pad Doesn't Work

Two approaches were tried:

##### Approach A: Bitmap-corner-to-bitmap-corner with larger pad

Lines drawn from `(0, bmp_h-1)` to `(bmp_w-1, 0)`. With pad=2, the line near the bottom of the visible cell is at bitmap x≈0.5, which maps to screen x ≈ cell_x0 - 1.5 — mostly in the overhang, not the visible cell. **Result: worse gap** because the line exits the visible area too early.

##### Approach B: Cell-corner-to-cell-corner with extrapolation

Lines drawn through cell corners `(pad, pad+h)` to `(pad+w, pad)`, extrapolated to bitmap edges. Same problem: near the cell corner, the line is in the padding zone rather than the visible area. **Result: worse gap.**

##### Root cause

The ╱ character exits through the bottom-left corner. Near that corner, regardless of padding strategy, the line occupies only 1-2 pixels horizontally. More padding pushes those pixels further into the overhang zone, making the visible cell area emptier near the boundary.

#### Current State

- `pad = 1` (fixed, does not scale with DPI)
- Lines drawn bitmap-corner to bitmap-corner
- Backend offsets placement by `-pad` for overhang
- Produces a measurable but small brightness dip at row boundaries
- Adjacent cells' overhangs partially compensate for each other
- At higher DPI, the dip may become more visible as the pad/thickness ratio decreases

### Affected Files

- `src/backend_sokol.c` — centered glyph placement logic for padded bitmaps
- `src/rend_boxdraw.c` — diagonal bitmap rendering with padding
- `src/rend_sdl3_boxdraw.c` — same for SDL3 backend
- `tests/diagonal_seam.txt` — visual test script for seam verification

---

## 4. Unimplemented Terminfo Capabilities

These capabilities are inherited from `xterm-256color` via `use=xterm-256color`
but are not yet fully implemented. coffer now has explicit case handlers (no
more silent `default: break` fallthrough) with logging for unimplemented
sequences. portty renders none of these yet.

### Stateful (parsed and stored in coffer, renderer noop)

These follow the blink pattern: `CFR_ATTR_BLINK` is parsed and stored in
coffer, portty maps it to `TerminalCellAttr.blink`, but neither renderer reads
it.

| Capability                   | Sequence    | coffer status                                               | portty renderer                                               |
| ---------------------------- | ----------- | ----------------------------------------------------------- | ------------------------------------------------------------- |
| `dim` (SGR 2)                | `\E[2m`     | `CFR_ATTR_DIM` bit set, cleared by SGR 22                   | Noop (not read by either backend)                             |
| `invis` (SGR 8)              | `\E[8m`     | `CFR_ATTR_INVIS` bit set, cleared by SGR 28                 | Noop (not read by either backend)                             |
| `blink` (SGR 5)              | `\E[5m`     | `CFR_ATTR_BLINK` bit set, cleared by SGR 25                 | Noop (not read by either backend)                             |
| `flash` (DECSCNM, ?5)        | `\E[?5h`    | `CFR_MODE_REVERSE_VIDEO` tracked, `set_mode` callback fired | Noop (screen-level reverse not handled; per-cell SGR 7 works) |
| `smm`/`rmm` (meta, ?1034)    | `\E[?1034h` | `CFR_MODE_META` tracked, logged once                        | Noop                                                          |
| `smglr`/`mgc` (margins, ?69) | `\E[?69h`   | `CFR_MODE_LEFT_RIGHT_MARGINS` tracked, logged once          | Noop                                                          |

### Full implementation design for left/right margins (?69)

DECSET ?69 enables left/right margin support via DECSLRM (`CSI Pl;Pr s`).
Full implementation requires:

- **New fields in CfrTerm:** `int margin_left`, `margin_right` (default 0 and
  `cols - 1`).
- **DECSLRM dispatch** (`CSI Pl;Pr s` with `?69`): Parse and validate left/right
  margins. Only act when `CFR_MODE_LEFT_RIGHT_MARGINS` is on.
- **Print path:** Autowrap checks against `margin_right` instead of `cols - 1`.
- **Cursor movement:** Clamp cursor column to `[margin_left, margin_right]`.
- **Erase operations:** `EL` (erase in line) and `ED` (erase in display)
  respect left/right margins when origin mode + margins are on.
- **Scroll operations:** `IL`/`DL`/`SU`/`SD` respect all four margins.
- ** DECSTBM interaction:** Top/bottom margins (`scroll_top`/`scroll_bottom`)
  already work; left/right would need similar clamping in the same code paths.
- ** DECSET ?69l (reset):** Restore margins to full width.
- **Origin mode (DECOM):** When both DECOM and ?69 are on, cursor coordinates
  are relative to the top-left of the margin region.

### Noop + log (no state to track)

| Capability                | Sequence    | coffer status                        |
| ------------------------- | ----------- | ------------------------------------ |
| `mc0`/`mc4`/`mc5` (CSI i) | `\E[i` etc. | Logged once per CfrTerm, then silent |
| `meml` (ESC l)            | `\El`       | Logged once per CfrTerm, then silent |
| `memu` (ESC m)            | `\Em`       | Logged once per CfrTerm, then silent |
| `initc` (OSC 4)           | `\E]4;...`  | Logged once per CfrTerm, then silent |
| `oc` (OSC 104)            | `\E]104`    | Logged once per CfrTerm, then silent |

### Fully implemented in coffer

| Capability         | Sequence    | coffer status                                        |
| ------------------ | ----------- | ---------------------------------------------------- |
| `rep` (REP, CSI b) | `\E[%p1%db` | Fully implemented: repeats last printed char N times |

### Backend-specific rendering gaps

Some attributes are parsed and stored by coffer but only rendered by one
backend. These are not terminfo concerns (both backends handle the same VT
input), but are important to track for visual parity.

**Rendered by SDL3 only (not Sokol):**

| Attribute                                                   | SDL3                  | Sokol           |
| ----------------------------------------------------------- | --------------------- | --------------- |
| underline (5 styles: single, double, curly, dotted, dashed) | `rend_sdl3.c:124-234` | Not implemented |
| strikethrough                                               | `rend_sdl3.c:240`     | Not implemented |
| `ul_color` (underline color)                                | `rend_sdl3.c:1051`    | Not implemented |

**Rendered differently between backends:**

| Attribute       | SDL3                                    | Sokol                                                    |
| --------------- | --------------------------------------- | -------------------------------------------------------- |
| reverse (SGR 7) | Pre-swaps fg/bg in `term_cfr.c:511-516` | Reads `attrs.reverse` directly at `backend_sokol.c:1694` |

**Not rendered by either backend:**

| Attribute                    | Stored in `TerminalCellAttr` | SDL3        | Sokol       |
| ---------------------------- | ---------------------------- | ----------- | ----------- |
| blink (SGR 5)                | `blink:1`                    | Not read    | Not read    |
| dim (SGR 2)                  | `dim:1`                      | Not read    | Not read    |
| invis (SGR 8)                | `invis:1`                    | Not read    | Not read    |
| font (SGR 10-19)             | `font:4`                     | Not read    | Not read    |
| dwl (DECDWL)                 | `dwl:1`                      | Not read    | Not read    |
| dhl (DECDHL)                 | `dhl:2`                      | Not read    | Not read    |
| DECSCNM (screen reverse, ?5) | N/A (mode, not cell attr)    | Not handled | Not handled |

Note: cursor blink (DECSET ?12) is separate from text blink (SGR 5) and is
fully implemented in both backends via timer-based cursor visibility toggling.

---

## 5. Popular Unsupported Capabilities

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
