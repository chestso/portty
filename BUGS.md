# Known Issues

## Wayland: mouse release not detected after dragging out of and back into the window

On Wayland, when the pointer crosses the window border during a selection drag, the compositor sends `wl_pointer.leave` and SDL synthesizes a `BUTTON_UP` event — even though the physical button is still held. portty detects this border artifact and keeps the drag alive so the selection continues to update when the pointer re-enters the window.

However, after the border crossing, SDL has lost all button state (`SDL_GetMouseState` and `SDL_GetGlobalMouseState` both return 0). When the user physically releases the button inside the window, no `BUTTON_UP` event is delivered because SDL already thinks the button is up. The selection drag stays active and continues to follow the mouse until a **click** (starts a new selection, clearing the old one) or a **copy** (right-click or `Ctrl+C`) resets it.

This is a fundamental Wayland protocol limitation ([SDL issue #14980](https://github.com/libsdl-org/SDL/issues/14980), closed as "not our bug"), not a portty bug. The compositor only sends pointer events for surfaces the application owns, and once `wl_pointer.leave` fires, the button state is irrecoverably lost.

**Workaround**: Click anywhere to cancel the stuck selection, then click and drag to start a new one. Right-click copies the current selection and clears it.

## Wayland: SDL3 interactive window resize is laggy

When dragging the window border to resize on Wayland, the resize feels sluggish. Programmatic resize via `SDL_SetWindowSize` (used by the `winsize` debug command) is smooth and immediate, but manual dragging via the compositor's `xdg_toplevel` configure events has noticeable lag.

The root cause is the Wayland resize protocol interaction with SDL3's event loop:

1. The compositor sends `xdg_surface.configure` with the new size.
2. SDL3 converts this to `SDL_EVENT_WINDOW_RESIZED` / `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`.
3. `SDL_PollEvent` does not dispatch the Wayland display queue on its own — `SDL_PumpEvents` must be called first (fixed in portty v0.5.5).
4. After processing the resize, `SDL_RenderPresent` blocks on the compositor's `wl_surface.frame` callback, which is throttled during continuous resize dragging.

An event-loop-driven frame callback driven by the compositor's vsync would avoid this — events would be processed within the frame callback, with no extra blocking round-trip.

**Related SDL issues:**

- [#15380](https://github.com/libsdl-org/SDL/issues/15380) — `SDL_WaitEventTimeout` can block indefinitely on Wayland due to cursor change race conditions (open, 3.x milestone). The deadlock occurs when cursor changes queue Wayland requests without flushing the write buffer, causing `poll()` to never return.
- [#13763](https://github.com/libsdl-org/SDL/issues/13763) — Extreme lag when resizing: `SDL_PollEvent()` takes 100ms to several seconds during interactive resize (open, labeled `notourbug`).
- [#13272](https://github.com/libsdl-org/SDL/issues/13272) — Event queue fills to 65,535 events on Wayland, causing freeze (closed).
- [#4609](https://github.com/libsdl-org/SDL/issues/4609) — High CPU usage with v-sync on Wayland due to frame callback busy-wait (closed, fixed).
- [SDL Wiki: AppFreezeDuringDrag](https://wiki.libsdl.org/SDL3/AppFreezeDuringDrag) — Documents that `SDL_PollEvent`, `SDL_WaitEvent`, `SDL_WaitEventTimeout`, and `SDL_PumpEvents` may block during resize/drag on some platforms.

**Mitigations applied** (v0.5.5):

- `SDL_PumpEvents` before `SDL_PollEvent` to ensure Wayland configure events are dispatched
- Restructured event loop: `PollEvent` drain → timers → render → `SDL_Delay(2)` idle (no `SDL_WaitEventTimeout`, which can block indefinitely on Wayland per [#15380](https://github.com/libsdl-org/SDL/issues/15380))
- Removed `SDL_RenderPresent` from the resize event handler (let the bottom-of-loop render handle it)

**Remaining limitation**: `SDL_RenderPresent` still blocks on the compositor's frame callback during continuous drag. This is a Wayland protocol limitation — the compositor controls when the client can present frames. A future fix could use `SDL_SetRenderVSync(0)` or a non-blocking present path, but on Wayland the compositor may still throttle.
