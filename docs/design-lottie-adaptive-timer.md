# Design: Adaptive Lottie Animation Timer

## Problem

The Lottie animation tick timer fires every 16 ms (~62.5 Hz) for the entire
lifetime of the event loop, regardless of whether any Lottie animations are
active. Each tick:

1. SDLTimer thread: `timer_sdl_callback` → `SDL_PushEvent` →
   `Wayland_SendWakeupEvent` (cross-thread futex)
2. Main thread: `SDL_WaitEvent` wakes → `SDL_PumpEvents` (4-6 zero-timeout
   poll rounds on Wayland/dbus/SDL fds) → dispatch `EVENT_LOTTIE_TICK` →
   `terminal_lottie_tick` → `cfr_lottie_tick` → `rec_count == 0` → early bail
3. `terminal_flush_damage` → `terminal_needs_redraw` → false → back to sleep

This is ~62.5 wakeups/second × ~14 syscalls/wakeup = ~875 syscalls/second
of pure overhead doing zero useful work. The SDLTimer thread alone accounts
for 12.85% of idle CPU. Combined with the main-thread polling overhead, the
Lottie timer is the single largest contributor to portty's 3-4% idle CPU
(vs. ptyxis at ~0.3%).

The render gate (`terminal_needs_redraw()`) correctly prevents unnecessary
GPU work — `cfr_lottie_tick` returns false when no animations advance, so
`terminal_mark_dirty` is not called and no render happens. The cost is
entirely in the wakeup/event-dispatch path, not in rendering.

## Goal

Lottie animations must play at full framerate (60 FPS) when active, with zero
wakeups when no animations are registered. The design must handle:

- Animations loaded at runtime (via APC/OSC 5555 protocol)
- Animations deleted at runtime
- Non-looping animations that finish playback (`playing = false`)
- Finished animations that get replayed (`cmd: "play"`)
- Animations with different framerates

## Current Architecture

```
platform_sdl3.c:1161    timer_add(16ms, repeat, EVENT_LOTTIE_TICK)
                                ↓ 62.5 Hz
platform_sdl3.c:1234    EVENT_LOTTIE_TICK handler
                                ↓
term.c:757              terminal_lottie_tick(term, now_us)
                                ↓
term_cfr.c:565          cfr_back_lottie_tick → cfr_lottie_tick(d->vt, now_us)
                                ↓
coffer/lottie.c:1613    if (!st || st->rec_count == 0) return false;  ← no-op bail
                        for each rec: if playing → advance frame → rasterize
                        return any_advanced
                                ↓
platform_sdl3.c:1236    if (returned true) terminal_mark_dirty(term)
```

Key facts from code analysis:

- `cfr_lottie_tick` already bails early when `rec_count == 0`
  (`coffer/lottie.c:1613`)
- `rec_count` counts all records (playing or not); finished non-looping
  animations stay in the array with `playing = false`
- There is no coffer callback to the host when animations are loaded or
  deleted — the host must poll
- `cfr_get_lotties(vt, &count)` returns a snapshot of all animations with
  their `.playing` field, but it allocates/copies into a scratch buffer each
  call — too expensive for a no-animation check
- The timer system has no `timer_set_interval` — only `timer_add` /
  `timer_remove` / `timer_reset` (reset reuses the same interval)

## Design

### Approach: Dynamic timer add/remove with a lightweight active-query

The timer is removed when no animations are active and re-added at 16 ms when
animations exist. The key challenge is knowing when to re-add the timer after
animations are loaded, since coffer does not notify the host.

#### New coffer API: `cfr_lottie_active_count`

Add a lightweight function to coffer that returns the count of registered
animations **that have at least one placement on the visible screen**:

```c
/* coffer/include/coffer/coffer.h */
int cfr_lottie_active_count(CfrTerm *vt);
```

```c
/* coffer/src/lottie.c */
int cfr_lottie_active_count(CfrTerm *vt)
{
    struct CfrLottieState *st = vt->lottie;
    if (!st)
        return 0;
    return st->rec_count;
}
```

This is a single field read — no allocation, no copy, no iteration. It returns
the total record count (playing + paused + finished). The host uses this to
decide whether the timer should exist at all.

**Why not just `playing` count?** A paused animation (`playing = false`) might
be resumed by the shell at any time via `cmd: "play"`. If we removed the timer
when all animations are paused, we'd need to detect the play command to
re-add it. By keeping the timer alive while any animation record exists
(playing or not), we avoid needing coffer to notify us on play/pause/seek.
The timer only goes away when all animations are deleted/evicted.

**Why not a coffer callback?** A callback (e.g. `cb_lottie_changed`) would be
the most efficient — the host would be notified exactly when animations
appear or disappear, with zero polling. This is the ideal long-term solution.
However, it requires changes to coffer's callback struct (`CfrCallbacks`),
wiring in `term_cfr.c`, and careful threading (the callback fires on the PTY
reader thread, not the main thread). The poll approach is simpler and the
poll cost is negligible (one integer comparison per PTY data event).

#### New terminal backend API

```c
/* term.h */
int terminal_lottie_count(TerminalBackend *term);
```

```c
/* term.c */
int terminal_lottie_count(TerminalBackend *term)
{
    if (!term || !term->lottie_count)
        return 0;
    return term->lottie_count(term);
}
```

```c
/* term.h — vtable entry */
int (*lottie_count)(TerminalBackend *term);
```

```c
/* term_cfr.c */
static int cfr_back_lottie_count(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return cfr_lottie_active_count(d->vt);
}
```

```c
/* term_cfr.c — vtable wiring */
.lottie_count = cfr_back_lottie_count,
```

#### Timer lifecycle in platform_sdl3.c

**Remove the always-on timer from `sdl3_run` startup:**

```c
// Before (platform_sdl3.c:1160-1162):
ctx->lottie_timer = timer_add(ctx->timers, 16, true,
                              EVENT_LOTTIE_TICK, NULL);

// After: do NOT start the timer here. It is started on demand.
```

**Check after every PTY data event:**

The only way animations can appear is through PTY data (the shell sends
APC/OSC 5555 sequences). After processing `EVENT_PTY_DATA`, check if
animations now exist and the timer isn't running:

```c
// In the EVENT_PTY_DATA handler (platform_sdl3.c:1199-1208), after
// renderer_process_pty_data and before the break:

if (ctx->lottie_timer == TIMER_INVALID &&
    terminal_lottie_count(term) > 0) {
    ctx->lottie_timer = timer_add(ctx->timers, 16, true,
                                  EVENT_LOTTIE_TICK, NULL);
}
```

**Check after every Lottie tick:**

After `EVENT_LOTTIE_TICK` processing, check if all animations have been
deleted (e.g. the shell sent a delete command, or animations were evicted
by scroll-off). If none remain, remove the timer:

```c
// In the EVENT_LOTTIE_TICK handler (platform_sdl3.c:1234-1237):

case EVENT_LOTTIE_TICK:
    if (terminal_lottie_tick(term, SDL_GetTicksNS() / 1000))
        terminal_mark_dirty(term);
    if (terminal_lottie_count(term) == 0) {
        timer_remove(ctx->timers, ctx->lottie_timer);
        ctx->lottie_timer = TIMER_INVALID;
    }
    break;
```

**Cleanup at event loop exit:**

```c
// After the event loop, alongside cursor blink timer cleanup:
if (ctx->lottie_timer != TIMER_INVALID) {
    timer_remove(ctx->timers, ctx->lottie_timer);
    ctx->lottie_timer = TIMER_INVALID;
}
```

### Why not use a coffer callback instead of polling?

A `cb_lottie_changed` callback in `CfrCallbacks` would be the ideal solution:
coffer calls it when an animation is loaded or deleted, and the host
adds/removes the timer in response. No polling needed at all.

This is the recommended long-term improvement. The polling approach above is
chosen for the initial implementation because:

1. The poll is cheap (one integer read) and only happens on PTY data events
   (not every frame), so the cost is negligible
2. A callback fires on the PTY reader thread, requiring thread-safe
   communication with the main thread (an atomic flag or an extra SDL event),
   adding complexity
3. The callback requires coffer API changes (new `CfrCallbacks` field) and
   careful versioning

If a callback is added to coffer later, the poll in the PTY data handler can
simply be removed and replaced with the callback-driven approach.

### Why not adaptive interval instead of add/remove?

An alternative to add/remove is keeping the timer always alive but changing
its interval: 16 ms when animations are active, e.g. 5000 ms when idle (as
a poll for new animations). This avoids the `SDL_RemoveTimer` /
`SDL_AddTimer` churn but wastes 0.2 wakeups/second on idle polls. The
add/remove approach achieves true zero wakeups when idle, at the cost of
one `SDL_RemoveTimer` + one `SDL_AddTimer` per animation lifecycle
transition (rare event).

The add/remove approach is preferred because animation load/delete is
infrequent (a few times per session at most), while idle is the common case.

### Animation framerate consideration

The current 16 ms interval (~62.5 Hz) is hardcoded. Lottie animations have
a `frame_fr` field (frames per second) stored in each `LtRec`. The tick
function already handles per-animation framerate correctly: it computes
`frame_delta = elapsed * speed * frame_fr / 1e6`, so a 30 FPS animation
simply advances fewer frames per tick than a 60 FPS one.

If all active animations have `frame_fr <= 30`, the timer could run at 33 ms
instead of 16 ms to halve the wakeup rate. However, this optimization adds
complexity (need to check all animation framerates on every load/delete) for
marginal benefit. The 16 ms interval is kept as-is for simplicity.

### Edge cases

| Scenario                          | Behavior                                                                                                                                                                                                  |
| --------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Shell loads animation via APC     | PTY data handler detects `lottie_count > 0`, adds 16 ms timer                                                                                                                                             |
| Animation finishes (non-looping)  | `playing = false`, but `rec_count` still > 0, timer stays alive. Next tick is a no-op (`cfr_lottie_tick` skips non-playing recs). No render. Cost: 62.5 no-op ticks/s while animation is paused/finished. |
| Shell replays finished animation  | `cmd: "play"` sets `playing = true`. Timer is already alive. Next tick advances frames normally.                                                                                                          |
| Shell deletes all animations      | `rec_count == 0`. Lottie tick handler detects this, removes timer. Zero wakeups thereafter.                                                                                                               |
| Animation scrolled off screen     | coffer's `lt_rec_release` removes the record, `rec_count` drops. If it was the last one, the next tick removes the timer.                                                                                 |
| Multiple animations, some finish  | Timer stays alive (rec_count > 0). Finished ones are no-ops in `cfr_lottie_tick`. Playing ones advance normally.                                                                                          |
| PTY data arrives but no animation | `lottie_count` check returns 0, no timer added. Cost: one integer comparison per PTY data event.                                                                                                          |
| Portty startup (no animations)    | Timer is not started. Zero Lottie wakeups until the shell loads an animation.                                                                                                                             |

### The paused-animation cost

When animations exist but none are playing (all finished or paused), the
timer still fires at 62.5 Hz and each tick is a no-op in `cfr_lottie_tick`
(the loop skips `!r->playing` records). This costs ~62.5 wakeups/s of
overhead. This is acceptable because:

1. A paused animation is visible on screen — the user is looking at it
2. The user or shell may resume playback at any time
3. The alternative (removing the timer when all are paused) requires either
   a coffer callback on play/pause, or polling `CfrLottie.playing` on every
   tick (which requires calling `cfr_get_lotties` — expensive due to scratch
   buffer allocation)

If this cost becomes a concern, a future optimization could add a
`cfr_lottie_playing_count` to coffer (single iteration over `recs[]`
checking `playing`) and use it to remove the timer when all animations are
paused. But for now, the dominant win is eliminating the timer when no
animations exist at all (the common case).

## Changes Required

### coffer (external dependency)

| File                      | Change                                                      |
| ------------------------- | ----------------------------------------------------------- |
| `include/coffer/coffer.h` | Add `int cfr_lottie_active_count(CfrTerm *vt);` declaration |
| `src/lottie.c`            | Add `cfr_lottie_active_count` function (3 lines)            |

### portty

| File                  | Change                                                                                                                                                                                     |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/term.h`          | Add `int (*lottie_count)(TerminalBackend *term);` vtable entry; add `int terminal_lottie_count(TerminalBackend *term);` declaration                                                        |
| `src/term.c`          | Add `terminal_lottie_count` dispatch wrapper                                                                                                                                               |
| `src/term_cfr.c`      | Add `cfr_back_lottie_count` implementation; wire in vtable                                                                                                                                 |
| `src/platform_sdl3.c` | Remove unconditional `timer_add` for Lottie at startup; add conditional timer start in PTY data handler; add conditional timer stop in Lottie tick handler; add timer cleanup at loop exit |

### No changes needed

| File                          | Reason                                                                                                     |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `src/timer.c` / `src/timer.h` | Existing `timer_add` / `timer_remove` API is sufficient                                                    |
| `src/rend_sdl3.c`             | Renderer already handles zero animations correctly (`render_lottie_layer` returns early when `count == 0`) |
| `src/common.h`                | No new constants needed (16 ms interval stays inline)                                                      |

## Expected Impact

| Scenario                     | Before                                                         | After                                          |
| ---------------------------- | -------------------------------------------------------------- | ---------------------------------------------- |
| Idle (no animations)         | 62.5 wakeups/s, ~12.85% SDLTimer + ~15% PumpEvents = ~3-4% CPU | 0 wakeups, 0% CPU from Lottie                  |
| Active animation             | 62.5 wakeups/s (same)                                          | 62.5 wakeups/s (same — no change)              |
| Animation paused/finished    | 62.5 wakeups/s                                                 | 62.5 wakeups/s (no change — timer stays alive) |
| After all animations deleted | 62.5 wakeups/s                                                 | 0 wakeups                                      |

The idle case (the common case) goes from ~3-4% CPU to whatever the cursor
blink timer costs (~0.5% — see the cursor design document).
