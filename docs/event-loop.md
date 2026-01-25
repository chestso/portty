# Event Loop Architecture

This document describes bloom-term's unified event loop design, which efficiently handles both PTY (shell I/O) and display (X11/Wayland) events with minimal latency.

## Overview

bloom-term uses a single-threaded event loop that must handle two independent event sources:

1. **PTY file descriptor** - Shell output (stdout/stderr from the child process)
2. **Display file descriptor** - User input events (keyboard, mouse, window resize)

The challenge is efficiently waiting for activity from _either_ source without introducing latency.

## The Problem

A naive approach has latency issues because we can only block on one event source at a time:

```c
// Approach 1: Block on PTY, poll SDL
poll(pty_fd, 16ms);      // Waits up to 16ms if no shell output
SDL_PollEvent();         // SDL events delayed up to 16ms

// Approach 2: Block on SDL, poll PTY
SDL_WaitEventTimeout(16ms);  // Waits up to 16ms if no user input
poll(pty_fd, 0);             // Non-blocking check, may miss output
```

Either approach introduces up to 16ms of latency for one event source, which is noticeable when:

- Typing rapidly (characters appear delayed)
- Running commands with streaming output (output appears chunky)
- Resizing window while shell is outputting

## Solution: Unified poll() on Both File Descriptors

SDL3 provides access to the underlying display system's file descriptor via window properties. By including both the PTY fd and display fd in a single `poll()` call, we wake immediately when _either_ has activity.

```c
struct pollfd pfds[2];
int nfds = 0;

pfds[nfds++] = (struct pollfd){ .fd = pty_fd, .events = POLLIN };
if (display_fd >= 0) {
    pfds[nfds++] = (struct pollfd){ .fd = display_fd, .events = POLLIN };
}

poll(pfds, nfds, 16);  // Wakes on EITHER source
```

## Platform Details

### X11

```c
#include <X11/Xlib.h>

Display *xdisplay = (Display *)SDL_GetPointerProperty(
    SDL_GetWindowProperties(window),
    SDL_PROP_WINDOW_X11_DISPLAY_POINTER,
    NULL
);
int x11_fd = ConnectionNumber(xdisplay);
```

The `ConnectionNumber()` macro (from `<X11/Xlib.h>`) extracts the socket file descriptor from the X11 `Display*` structure.

### Wayland

```c
#include <wayland-client.h>

struct wl_display *wl_dpy = (struct wl_display *)SDL_GetPointerProperty(
    SDL_GetWindowProperties(window),
    SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER,
    NULL
);
int wl_fd = wl_display_get_fd(wl_dpy);
```

Wayland has additional requirements from the [Wayland protocol](https://wayland-book.com/wayland-display/event-loop.html):

1. **Before blocking**: Call `wl_display_flush()` to send any pending requests to the compositor
2. **After poll returns**: Call `wl_display_dispatch_pending()` to process any queued events

```c
// Before poll
wl_display_flush(wl_dpy);

poll(pfds, nfds, timeout);

// After poll
wl_display_dispatch_pending(wl_dpy);
SDL_PumpEvents();
```

### Fallback

If the display fd is unavailable (other video drivers, or missing X11/Wayland libraries), we fall back to short timeout polling:

```c
if (display_fd < 0) {
    // 1ms timeout - slightly higher CPU usage but still responsive
    poll(&pty_pfd, 1, 1);
}
```

## SDL3 Properties API

SDL3 exposes underlying display handles via `SDL_GetWindowProperties()`:

| Property                                  | Type                 | Description                |
| ----------------------------------------- | -------------------- | -------------------------- |
| `SDL_PROP_WINDOW_X11_DISPLAY_POINTER`     | `Display*`           | X11 display connection     |
| `SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER` | `struct wl_display*` | Wayland display connection |

Detect the active video driver with `SDL_GetCurrentVideoDriver()`:

```c
const char *driver = SDL_GetCurrentVideoDriver();
if (strcmp(driver, "x11") == 0) {
    // Use X11 properties
} else if (strcmp(driver, "wayland") == 0) {
    // Use Wayland properties
}
```

## Event Flow Diagram

```
┌─────────────┐     ┌─────────────┐
│   PTY fd    │     │ Display fd  │
│ (shell I/O) │     │(X11/Wayland)│
└──────┬──────┘     └──────┬──────┘
       │                   │
       └─────────┬─────────┘
                 │
                 ▼
    ┌────────────────────────┐
    │  wl_display_flush()    │  (Wayland only)
    └───────────┬────────────┘
                │
                ▼
       poll(fds, 2, timeout)
                │
    ┌───────────┴───────────┐
    │                       │
    ▼                       ▼
┌────────────┐    ┌─────────────────────┐
│ PTY ready? │    │ wl_display_dispatch │
│            │    │ _pending()          │
│ pty_read() │    │ (Wayland only)      │
│     │      │    └──────────┬──────────┘
│     ▼      │               │
│ terminal_  │               ▼
│ process()  │    ┌─────────────────────┐
└────────────┘    │  SDL_PumpEvents()   │
       │          │  SDL_PollEvent()    │
       │          └──────────┬──────────┘
       │                     │
       └──────────┬──────────┘
                  │
                  ▼
           ┌────────────┐
           │  render()  │
           │     │      │
           │     ▼      │
           │ SDL_Render │
           │ Present()  │
           └────────────┘
```

## Implementation

The implementation lives in `src/main.c`:

- `DisplayContext` struct holds the display fd and backend type
- `display_context_init()` detects the video driver and extracts the display fd
- `display_context_flush()` flushes Wayland display before blocking
- `display_context_dispatch_pending()` dispatches Wayland events after poll

Build configuration:

- `configure.ac` checks for `x11` and `wayland-client` as optional dependencies
- `HAVE_X11` and `HAVE_WAYLAND` macros enable the respective code paths
- Falls back gracefully if neither is available

## Testing

```bash
# Test on X11
SDL_VIDEODRIVER=x11 ./build/src/bloom-term -v

# Test on Wayland
SDL_VIDEODRIVER=wayland ./build/src/bloom-term -v
```

Verify low latency:

- Type rapidly - characters should appear instantly
- Run `cat` and paste large text - should stream smoothly
- Resize window while shell is outputting - both should work simultaneously

## References

- [SDL3 SDL_GetWindowProperties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties)
- [SDL Forum: Using select()/poll()](https://discourse.libsdl.org/t/using-select-poll/11521)
- [Wayland Book: Event Loop Integration](https://wayland-book.com/wayland-display/event-loop.html)
- [wl_display_get_fd man page](https://www.systutorials.com/docs/linux/man/3-wl_display_get_fd/)
