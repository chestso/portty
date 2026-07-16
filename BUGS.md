# Known Issues

## Wayland: mouse release not detected after dragging out of and back into the window

On Wayland, when the pointer crosses the window border during a selection drag, the compositor sends `wl_pointer.leave` and SDL synthesizes a `BUTTON_UP` event — even though the physical button is still held. portty detects this border artifact and keeps the drag alive so the selection continues to update when the pointer re-enters the window.

However, after the border crossing, SDL has lost all button state (`SDL_GetMouseState` and `SDL_GetGlobalMouseState` both return 0). When the user physically releases the button inside the window, no `BUTTON_UP` event is delivered because SDL already thinks the button is up. The selection drag stays active and continues to follow the mouse until a **click** (starts a new selection, clearing the old one) or a **copy** (right-click or `Ctrl+C`) resets it.

This is a fundamental Wayland protocol limitation ([SDL issue #14980](https://github.com/libsdl-org/SDL/issues/14980), closed as "not our bug"), not a portty bug. The compositor only sends pointer events for surfaces the application owns, and once `wl_pointer.leave` fires, the button state is irrecoverably lost.

**Workaround**: Click anywhere to cancel the stuck selection, then click and drag to start a new one. Right-click copies the current selection and clears it.
