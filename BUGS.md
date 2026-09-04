# Known Issues

## Wayland: mouse release not detected after dragging out of and back into the window

On Wayland, when the pointer crosses the window border during a selection drag, the compositor sends `wl_pointer.leave` and SDL synthesizes a `BUTTON_UP` event — even though the physical button is still held. portty detects this border artifact and keeps the drag alive so the selection continues to update when the pointer re-enters the window.

However, after the border crossing, SDL has lost all button state (`SDL_GetMouseState` and `SDL_GetGlobalMouseState` both return 0). When the user physically releases the button inside the window, no `BUTTON_UP` event is delivered because SDL already thinks the button is up. The selection drag stays active and continues to follow the mouse until a **click** (starts a new selection, clearing the old one) or a **copy** (right-click or `Ctrl+C`) resets it.

This is a fundamental Wayland protocol limitation ([SDL issue #14980](https://github.com/libsdl-org/SDL/issues/14980), closed as "not our bug"), not a portty bug. The compositor only sends pointer events for surfaces the application owns, and once `wl_pointer.leave` fires, the button state is irrecoverably lost.

**Workaround**: Click anywhere to cancel the stuck selection, then click and drag to start a new one. Right-click copies the current selection and clears it.

## Resolved: Wayland interactive window resize lag

Interactive resize on Wayland was previously sluggish (see [SDL issue #13763](https://github.com/libsdl-org/SDL/issues/13763)). As of SDL3 3.4.x the resize is smooth on the same setup; the upstream Wayland frame-callback/resize handling improved between the 3.2.x and 3.4.x series.

The event-loop mitigations introduced in portty v0.5.5 remain in place, since they are correct regardless of SDL version:

- `SDL_PumpEvents` before `SDL_PollEvent` to ensure Wayland configure events are dispatched
- Event-driven loop with `SDL_WaitEvent` (no `SDL_WaitEventTimeout`, which can block indefinitely on Wayland per [SDL issue #15380](https://github.com/libsdl-org/SDL/issues/15380))
- No `SDL_RenderPresent` in the resize event handler — the bottom-of-loop render handles it
- `SDL_SetRenderVSync(0)` so presents never block on the compositor's frame callback

## Resolved: Kitty (and likely iTerm2/sixel) graphics don't use the full available screen space

Running:

```
chafa --clear --align mid,mid -d 5 -- *.jpg
```

left 2 lines of blank at the bottom of the screen, while kitty left only 1.

The engine moved the cursor down by the image's full row count after placement, instead of kitty's `rows - 1` (plus a column wrap at the right edge). chafa's epilogue newline then hit the bottom margin and scrolled the grid, shifting the image up: the top row clipped off-screen and an extra blank row appeared at the bottom. Fixed in coffer (kitty cursor-advance rule, `c=`/`r=` parsed as display size, `C=1` honored); `chafa --clear --align mid,mid` now renders pixel-identical to kitty.

The single remaining blank row is chafa policy, not a terminal gap: `--margin-bottom` defaults to 1 ("safety margin… prevent images from scrolling out", per `chafa --help`). Use `--margin-bottom=0` to have chafa request the full grid; the image height is still aspect-limited unless `--stretch` is also given.
