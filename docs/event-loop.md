# Event Loop Architecture

This document describes bloom-terminal's event loop design, which efficiently handles both PTY (shell I/O) and display events using a thread-based architecture.

## Overview

bloom-terminal uses a single-threaded event loop that must handle two independent event sources:

1. **PTY file descriptor** - Shell output (stdout/stderr from the child process)
2. **SDL events** - User input events (keyboard, mouse, window resize)

The challenge is efficiently processing activity from both sources without introducing latency.

## Architecture

The current implementation uses a thread-based approach:

### PTY Reader Thread

A dedicated background thread (`pty_reader_thread_func` in `src/event_loop_sdl3.c:78`) handles all PTY I/O:

- Uses `poll()` on PTY fd with infinite timeout (`-1`)
- Reads data from PTY and pushes `SDL_EVENT_USER` events to the main thread
- Handles SIGCHLD notifications via signal pipe
- Manages wakeup pipe for graceful shutdown

### Main Event Thread

The main thread uses `SDL_WaitEvent()` to wait for events from either source:

- SDL events (keyboard, mouse, window, etc.)
- Custom PTY data events from the reader thread
- Timer events for cursor blinking
- No artificial timing delays

## Event Flow

```
┌────────────────┐    ┌────────────────┐
│   PTY Reader   │    │   Main Thread  │
│    Thread      │    │     (SDL)      │
└────────┬───────┘    └────────┬───────┘
         │                     │
         │ 1. poll(pty_fd, -1) │
         │     (blocks until   │
         │      PTY data)      │
         │                     │
         │ 2. pty_read()       │
         │     (reads data)    │
         │                     │
         │ 3. Push SDL_EVENT_  │
         │    USER event       │
         │                     │
         └─────────────────────┘
```

## Key Implementation Details

### PTY Reader Thread (`src/event_loop_sdl3.c:78`)

```c
while (SDL_GetAtomicInt(&ctx->running)) {
    struct pollfd pfds[3];
    // Poll PTY fd, signal fd, and wakeup fd with infinite timeout
    int poll_ret = poll(pfds, nfds, -1);  // Blocks indefinitely

    // Handle PTY data by pushing SDL events to main thread
    SDL_Event event = { 0 };
    event.type = SDL_EVENT_USER;
    event.user.code = EVENT_PTY_DATA;
    SDL_PushEvent(&event);
}
```

### Main Event Loop (`src/event_loop_sdl3.c:347`)

```c
while (!SDL_GetAtomicInt(&ctx->quit_requested)) {
    // Wait for events - truly event-driven, no timeout
    if (!SDL_WaitEvent(&event)) {
        break;
    }

    // Process events including custom PTY data events
    if (event.type == SDL_EVENT_USER) {
        switch (event.user.code) {
        case EVENT_PTY_DATA:
            // Process PTY data via terminal backend
            terminal_process_input(term, payload->data, payload->len);
            break;
        // ... other event types
        }
    }
    // ... other SDL event handling

    // Render only when needed
    if (terminal_needs_redraw(term) || ctx->force_redraw) {
        renderer_draw_terminal(rend, term, cursor_vis);
        SDL_RenderPresent(ctx->sdl_renderer);
    }
}
```

### VSync and Rendering

- VSync is disabled for lowest input latency: `SDL_SetRenderVSync(sdl_rend, 0)`
- Rendering is event-driven, only occurs when terminal needs redraw
- No artificial 60 FPS timing or delays

## Benefits

1. **Zero Latency**: PTY data is processed immediately when available, no polling delays
2. **Event-Driven**: Main thread responds instantly to user input
3. **Thread Safety**: PTY I/O is isolated in dedicated thread
4. **Low CPU Usage**: Main thread blocks on `SDL_WaitEvent()` when idle
5. **Graceful Shutdown**: Uses wakeup pipe for clean thread termination

## Implementation Location

- Main loop: `src/event_loop_sdl3.c:314` (`sdl3_run` function)
- PTY reader thread: `src/event_loop_sdl3.c:78` (`pty_reader_thread_func`)
- Event processing: `src/event_loop_sdl3.c:356-521` (event switch statement)
- Rendering: `src/event_loop_sdl3.c:525-535` (conditional rendering)
