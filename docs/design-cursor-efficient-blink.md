# Design: Efficient Cursor Blink

## Problem

The cursor blink timer fires every 1000 ms (`CURSOR_BLINK_INTERVAL_MS`),
toggles `cursor_blink_visible`, and calls `terminal_mark_dirty(term)`. This
triggers a **full-screen repaint**: the renderer clears the entire render
target, iterates all visible cells, re-stages all glyphs, and re-draws
everything — just to toggle a single cell's cursor overlay.

At idle (no other wakeups), this costs ~0.5-1% CPU: one full repaint per
second. While far less expensive than the Lottie timer (62.5 Hz), it is
still wasteful since only one cell changes.

The specific costs per blink render (from profiling):

| Function                 | CPU % | Purpose                                        |
| ------------------------ | ----- | ---------------------------------------------- |
| `render_visible_cells`   | 2.36% | Iterates all cells (2 passes: populate + draw) |
| `GPU_QueueGeometry`      | 4.20% | SDL3 GPU command buffering for all cell quads  |
| `SDL_ConvertToLinear`    | 1.69% | sRGB→linear conversion of the full frame       |
| `SDL_SetRenderDrawColor` | 1.48% | Per-cell color changes                         |
| `PrepQueueCmdDraw`       | 1.22% | GPU draw command prep                          |
| `terminal_row_iter_next` | 1.02% | Cell iteration overhead                        |
| `SDL_RenderClear`        | —     | Full-screen clear                              |

All of this to change a single ~10×20 pixel cursor overlay.

## Goal

The cursor must blink at the configured rate with minimal CPU cost. The
design must preserve:

- Blink phase visibility (on/off toggle at the configured interval)
- Blink reset on user input (cursor goes solid immediately, timer restarts)
- Solid cursor when window is unfocused or blink is disabled (DECSET 12)
- Correct cursor position tracking (cursor moves as the user types)
- Correct cursor visibility (hidden by application via DECSET 25)
- Scroll-aware cursor (hidden when scrolled back in scrollback)

## Current Architecture

```
Timer fires every 1000ms (CURSOR_BLINK_INTERVAL_MS)
  → SDL_EVENT_USER with code EVENT_CURSOR_BLINK
  → Handler: if terminal_get_cursor_blink(term):
      cursor_blink_visible = !cursor_blink_visible
      terminal_mark_dirty(term)        → d->needs_redraw = true (FULL SCREEN)

Event loop bottom:
  terminal_flush_damage(term)          → cfr_damage_flush → cb_damage
  if terminal_needs_redraw(term):      → d->needs_redraw (true from mark_dirty)
      cursor_vis = !has_focus || !blink || blink_visible
      renderer_draw_terminal(rend, term, cursor_vis)
        → sdl3_draw_terminal
          → populate_atlas (ALL cells, 2 passes)
          → draw_scene_linear
            → SDL_RenderClear (full screen)
            → render_visible_cells (ALL cells)
              → render_cursor: if show_cursor && at cursor pos → draw_rounded_rect
      SDL_RenderPresent
      terminal_clear_redraw(term)      → d->needs_redraw = false
```

Key facts from code analysis:

- `terminal_mark_dirty` sets `d->needs_redraw = true` with no damage
  rectangle — it means "full repaint" (`term_cfr.c:612-617`)
- The damage rect system (`damage_top/bottom/left/right`) tracks coffer's
  internal VT damage, but `terminal_mark_dirty` bypasses it entirely
- The renderer **ignores the damage rect** — `sdl3_draw_terminal` always
  repaints all visible cells via `render_visible_cells` (two passes)
- `terminal_get_damage_rect` exists and is wired, but the SDL3 renderer
  never calls it
- The cursor is drawn as a semi-transparent rounded rectangle overlay at
  the end of `render_cell`, gated by `show_cursor` which combines
  `cursor_visible`, cursor position bounds, scroll offset, and
  `terminal_get_cursor_visible` (`rend_sdl3.c:2162-2168`)

## Design

### Approach: Two-phase cursor render

Instead of a full repaint for cursor blink, render only the cursor overlay
change. The cursor is a single semi-transparent rounded rectangle drawn
over one cell. Blinking it on/off only requires:

1. Erasing the previous cursor state (re-draw the one cell without cursor)
2. Drawing the new cursor state (draw the one cell with cursor)

This can be done as a tiny partial render that touches only 1-2 cells,
without clearing or repainting the full screen.

#### Phase 1: Add a cursor-only render path to the renderer

Add a new renderer function that draws only the cursor cell:

```c
/* rend.h */
/*
 * Redraw only the cursor cell — used for blink-phase toggles where nothing
 * else on screen has changed. Reads the current framebuffer back for the
 * cell area, re-renders that single cell (background + glyph), then draws
 * or omits the cursor overlay. Much cheaper than a full draw_terminal.
 */
void renderer_draw_cursor(RendererBackend *rend, TerminalBackend *term,
                          bool cursor_visible);
```

```c
/* rend.c */
void renderer_draw_cursor(RendererBackend *rend, TerminalBackend *term,
                          bool cursor_visible)
{
    if (!rend || !rend->draw_cursor)
        return;
    rend->draw_cursor(rend, term, cursor_visible);
}
```

```c
/* rend.h — vtable entry */
void (*draw_cursor)(RendererBackend *rend, TerminalBackend *term,
                    bool cursor_visible);
```

#### Phase 2: Implement cursor-only render in rend_sdl3.c

```c
/* rend_sdl3.c */
static void sdl3_draw_cursor(RendererBackend *backend,
                             TerminalBackend *term, bool cursor_visible)
{
    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (!data || !term)
        return;

    /* Cursor is hidden when scrolled back in scrollback. */
    if (data->scroll_offset != 0)
        return;

    /* Get cursor position. */
    TerminalPos cursor_pos = terminal_get_cursor_pos(term);
    int display_rows = data->height / data->cell_height;
    int display_cols = data->width / data->cell_width;
    if (cursor_pos.row < 0 || cursor_pos.row >= display_rows ||
        cursor_pos.col < 0 || cursor_pos.col >= display_cols)
        return;

    bool show_cursor = cursor_visible &&
                       terminal_get_cursor_visible(term);

    /*
     * Render only the cursor cell. We need to:
     * 1. Clear the cell area to the cell's background color
     * 2. Re-draw the cell's glyph
     * 3. Draw the cursor overlay if show_cursor is true
     *
     * This replaces a full-screen repaint (all display_rows × display_cols
     * cells) with a single-cell render.
     *
     * Implementation: set a clip rect to the cursor cell, call
     * SDL_RenderClear to clear just that area, then call render_cell for
     * just that one cell with populate_only=false. The clip rect ensures
     * SDL_RenderClear and render_cell only affect the cursor cell's pixels.
     */
    int px = cursor_pos.col * data->cell_width;
    int py = cursor_pos.row * data->cell_height;
    int pw = data->cell_width;
    int ph = data->cell_height;

    /* Clip to the cursor cell. */
    SDL_Rect clip = { px, py, pw, ph };
    SDL_SetRenderClipRect(data->renderer, &clip);

    /* Clear the cell to black (will be overdrawn by cell bg). */
    SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
    SDL_RenderClear(data->renderer);

    /* Re-render just this cell. render_visible_cells iterates all cells
     * but only the one inside the clip rect will actually write pixels.
     * Alternatively, add a render_single_cell helper that renders exactly
     * one cell without iterating. */
    SDL_SetRenderClipRect(data->renderer, NULL);
}
```

**Note:** The above is a sketch. The actual implementation needs to handle
the linear-light compositing path (`draw_scene_linear` uses a separate
linear render target). The simplest correct approach is:

1. Re-render the single cell into the linear target (with clip rect set)
2. Blit only the affected region from linear target to the screen target
3. Present

Alternatively, a simpler but slightly less efficient approach: skip the
linear target for cursor-only renders (the cursor overlay is a simple
alpha blend, and the single cell underneath is already on screen — just
read it back, toggle the cursor, and write it back). However, SDL3's
GPU renderer may not support efficient framebuffer readback.

**Recommended implementation:** Add a `render_single_cell` helper that
renders exactly one cell (background + glyph + optional cursor overlay)
with proper linear compositing, and call it from both `sdl3_draw_cursor`
and the existing `render_visible_cells` (refactored to iterate and call
`render_single_cell` per cell). This avoids the clip-rect hack and keeps
the linear path correct.

#### Phase 3: Use cursor-only render in the blink handler

In `platform_sdl3.c`, change the `EVENT_CURSOR_BLINK` handler to use
cursor-only render instead of `terminal_mark_dirty`:

```c
// Before (platform_sdl3.c:1220-1225):
case EVENT_CURSOR_BLINK:
    if (terminal_get_cursor_blink(term)) {
        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
        terminal_mark_dirty(term);
    }
    break;

// After:
case EVENT_CURSOR_BLINK:
    if (terminal_get_cursor_blink(term)) {
        /* Erase cursor at old phase. */
        bool old_vis = !ctx->has_focus || !terminal_get_cursor_blink(term)
                       || ctx->cursor_blink_visible;
        renderer_draw_cursor(rend, term, old_vis);

        /* Toggle phase. */
        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;

        /* Draw cursor at new phase. */
        bool new_vis = !ctx->has_focus || !terminal_get_cursor_blink(term)
                       || ctx->cursor_blink_visible;
        renderer_draw_cursor(rend, term, new_vis);

        SDL_RenderPresent(ctx->sdl_renderer);
    }
    break;
```

Wait — this approach has a problem. We need to erase the old cursor and
draw the new one, but both operations render the same cell. The correct
sequence is:

1. Render the cursor cell **without** cursor (erases old cursor)
2. Present
3. Toggle `cursor_blink_visible`
4. Render the cursor cell **with** cursor (draws new cursor)
5. Present

But two presents per blink is wasteful. Better: render the cell once with
the new cursor state, present once. The cell's background and glyph are the
same either way — only the cursor overlay changes. So:

```c
case EVENT_CURSOR_BLINK:
    if (terminal_get_cursor_blink(term)) {
        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
        bool cursor_vis = !ctx->has_focus ||
                          !terminal_get_cursor_blink(term) ||
                          ctx->cursor_blink_visible;
        renderer_draw_cursor(rend, term, cursor_vis);
        SDL_RenderPresent(ctx->sdl_renderer);
    }
    break;
```

This renders the single cursor cell with the new cursor visibility and
presents. The cell's background and glyph are redrawn (overwriting whatever
was there before, including the old cursor), and the new cursor state is
applied. One render, one present, one cell.

**Skip the `terminal_mark_dirty` / `terminal_needs_redraw` / `terminal_flush_damage`
chain entirely** — the blink handler does its own render and present,
bypassing the normal render gate. This is safe because:

- The cursor cell hasn't changed except for the cursor overlay
- No other cells need repainting
- The render gate at the bottom of the event loop will see
  `terminal_needs_redraw() == false` and do nothing

#### Phase 4: Handle cursor position changes

When the cursor moves (user types, PTY output changes cursor position), the
old cursor position needs to be erased and the new one drawn. This is
already handled by the normal render path: PTY output triggers
`terminal_mark_dirty` (via `terminal_flush_damage` → `cb_damage`), which
causes a full repaint that draws all cells correctly including the new
cursor position. The cursor-only path is only used for blink-phase toggles
where nothing else changed.

However, there's a subtle issue: if the cursor moves between blink ticks
(via PTY output), the full repaint at the new position is correct, but the
next blink tick's `renderer_draw_cursor` will render at the **new** cursor
position (queried from `terminal_get_cursor_pos`). This is correct — the
old position was already cleaned up by the full repaint.

#### Phase 5: Keep the blink timer but consider blink rate

The 1000 ms blink interval is standard and matches most terminals. No
change needed to the interval itself. The timer stays as-is — the
optimization is in what happens when it fires (cursor-only render instead
of full repaint).

### Alternative: GPU cursor overlay

A more advanced approach is to render the cursor as a separate texture
layer composited on top of the frame, similar to how hardware mouse
cursors work. The cursor would be a small texture that is drawn or not
drawn each frame, without touching the cell grid at all.

```
Frame composition:
  1. Cell grid (rendered only when content changes — gated by needs_redraw)
  2. Lottie layers (rendered when animations advance)
  3. Sixel images (rendered when images change)
  4. Cursor overlay (rendered every frame, independent of needs_redraw)
```

This would decouple the cursor from the cell grid entirely. Blink would
just toggle whether the cursor texture is drawn, with zero cell re-rendering.

**Pros:**

- Zero cell rendering for blink — just one textured quad
- Cursor can animate independently (smooth fade in/out, pulse, etc.)
- Cursor is always correct — no need to erase old position

**Cons:**

- Requires a separate render pass after the cell grid
- Requires tracking whether the cursor position changed (to know if we
  need to re-render the cell grid to erase the old cursor)
- More complex than the cursor-only render approach
- May interact with the linear compositing path (cursor overlay would need
  to be composited in linear space too)

This is the ideal long-term design but is more invasive. The cursor-only
render approach (Phase 1-5 above) is recommended for the initial
implementation as it requires minimal changes and achieves the primary goal.

### Edge cases

| Scenario                     | Behavior                                                                                                                                                                                                      |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Cursor blink on, idle        | Timer fires at 1 Hz, cursor-only render, 1 cell + 1 present. ~0.02% CPU.                                                                                                                                      |
| Cursor blink off (DECSET 12) | Timer fires, `terminal_get_cursor_blink` returns false, handler does nothing. No render.                                                                                                                      |
| User types (key input)       | Full repaint (via `terminal_mark_dirty` in key handler). Cursor drawn at new position with `cursor_blink_visible = true`. Timer reset.                                                                        |
| PTY output moves cursor      | Full repaint (via damage). Cursor drawn at new position.                                                                                                                                                      |
| Window unfocused             | `cursor_vis = true` (solid). Blink handler renders cursor cell with `cursor_vis = true` every tick — same cell rendered twice (no visual change, but one wasted render). Could skip render when `!has_focus`. |
| Cursor hidden (DECSET 25)    | `terminal_get_cursor_visible` returns false. `show_cursor` in renderer is false. Cursor cell rendered without overlay. No visible change, but one wasted render. Could skip render when cursor not visible.   |
| Scrolled back in scrollback  | `scroll_offset != 0`. `show_cursor` is false in renderer. Cursor-only render should skip entirely (return early when `scroll_offset != 0`).                                                                   |
| Resize                       | Full repaint. Cursor-only path not involved.                                                                                                                                                                  |
| Pager overlay active         | `data->overlay` is set in `sdl3_draw_terminal`. Cursor-only render should check for overlay and skip (pager has no cursor).                                                                                   |

### Optimization: skip render when nothing visible changes

Add early-return checks to the blink handler to avoid even the cursor-only
render when the cursor state doesn't produce a visual change:

```c
case EVENT_CURSOR_BLINK:
    if (!terminal_get_cursor_blink(term))
        break;                          /* blink disabled */
    if (!ctx->has_focus)
        break;                          /* unfocused → solid, no toggle */
    if (data->scroll_offset != 0)       /* scrolled back → cursor hidden */
        break;
    if (!terminal_get_cursor_visible(term))
        break;                          /* cursor hidden by app */

    ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
    bool cursor_vis = ctx->cursor_blink_visible;
    renderer_draw_cursor(rend, term, cursor_vis);
    SDL_RenderPresent(ctx->sdl_renderer);
    break;
```

Note: `data->scroll_offset` is on the renderer side. The platform layer
would need a way to query it, or the renderer's `draw_cursor` impl can
check it internally and no-op. The latter is cleaner — the platform layer
calls `renderer_draw_cursor`, and the renderer decides whether to do
anything based on scroll offset, overlay state, etc.

## Changes Required

### portty

| File                  | Change                                                                                                                                                                                 |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/rend.h`          | Add `draw_cursor` vtable entry; add `renderer_draw_cursor` declaration                                                                                                                 |
| `src/rend.c`          | Add `renderer_draw_cursor` dispatch wrapper                                                                                                                                            |
| `src/rend_sdl3.c`     | Implement `sdl3_draw_cursor` (single-cell render with cursor overlay); add `render_single_cell` helper (refactored from `render_visible_cells`); wire vtable entry                     |
| `src/platform_sdl3.c` | Change `EVENT_CURSOR_BLINK` handler to use `renderer_draw_cursor` + `SDL_RenderPresent` instead of `terminal_mark_dirty`; add early-return checks for unfocused/hidden/scrolled states |

### No changes needed

| File                                           | Reason                                                                            |
| ---------------------------------------------- | --------------------------------------------------------------------------------- |
| `src/timer.c` / `src/timer.h`                  | Timer interval (1000 ms) is fine; `timer_reset` for input reset works as-is       |
| `src/term.c` / `src/term.h` / `src/term_cfr.c` | No terminal backend changes — cursor position/visibility/blink APIs already exist |
| `src/common.h`                                 | `CURSOR_BLINK_INTERVAL_MS` unchanged                                              |
| `coffer`                                       | No changes needed                                                                 |

### Complexity note

The `render_single_cell` refactor is the most involved part. The current
`render_visible_cells` function (`rend_sdl3.c:~1700-2160`) is ~460 lines
that handles background colors, glyphs, emoji, grapheme clusters, VS16
width adjustments, selection highlighting, hyperlink underlines, and the
cursor overlay. Extracting a single-cell version requires either:

1. **Clip-rect approach** (simpler): call `render_visible_cells` with a
   clip rect set to the cursor cell. All cells are iterated but only one
   writes pixels. Overhead: ~3000 cell iterations (loop overhead only,
   no GPU work for clipped cells). CPU cost: negligible (~0.01% per blink).

2. **True single-cell render** (optimal): extract the cell-rendering logic
   into a `render_single_cell(data, term, row, col, cursor_visible)` that
   renders exactly one cell without iterating. Requires refactoring
   `render_visible_cells` to call `render_single_cell` in its loop. More
   code change but zero wasted iteration.

The clip-rect approach is recommended for the initial implementation — it's
a 5-line change (set clip, clear, render, unset clip, present) with no
refactoring of the complex `render_visible_cells` function. The iteration
overhead of ~3000 cells is trivial (a few microseconds of branch checks,
no GPU commands emitted for clipped cells).

## Expected Impact

| Scenario                       | Before                                    | After                                          |
| ------------------------------ | ----------------------------------------- | ---------------------------------------------- |
| Idle with blink                | 1 full repaint/s (~0.5-1% CPU)            | 1 single-cell render/s (~0.01% CPU)            |
| Idle without blink (DECSET 12) | Timer fires, handler no-ops (minimal)     | Same (early return, no change)                 |
| Typing (active input)          | Full repaint per input event (unchanged)  | Same (full repaint needed for content change)  |
| Unfocused                      | Full repaint at 1 Hz (cursor stays solid) | No render (early return, cursor already solid) |

Combined with the Lottie adaptive timer design, idle CPU should drop from
~3-4% to effectively 0% (both the 62.5 Hz Lottie wakeups and the 1 Hz
full-screen blink repaints are eliminated).
