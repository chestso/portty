# portty architecture

How portty is put together and why. For build instructions and the test
workflow see [CONTRIBUTING.md](CONTRIBUTING.md); for a feature list see
[README.md](README.md). Image protocols have their own document:
[docs/inline-image-architecture.md](docs/inline-image-architecture.md).

- [Layers](#layers)
- [Source map](#source-map)
- [Backends](#backends)
- [Startup](#startup)
- [Threads and the event loop](#threads-and-the-event-loop)
- [Timers are events](#timers-are-events)
- [Rendering](#rendering)
- [Animation pacing](#animation-pacing)
- [The coffer boundary](#the-coffer-boundary)
- [Scales and coordinates](#scales-and-coordinates)
- [Debug scripts and tests](#debug-scripts-and-tests)
- [Invariants worth knowing](#invariants-worth-knowing)

## Layers

```
 child process ── PTY ──┐
                        ▼
              ┌──────────────────┐   events    ┌──────────────────────┐
              │  PTY reader      │────────────▶│  main thread         │
              │  thread          │             │  (event loop)        │
              └──────────────────┘             │                      │
                                               │  PorttyApp (policy)  │
   SDL timers ── EVENT_TIMER_FIRE ────────────▶│  TerminalBackend     │──▶ coffer (VT engine)
   SDL input / window events ─────────────────▶│  Renderer + Fonts    │──▶ SDL3 GPU renderer
                                               └──────────────────────┘
```

portty is a C99 terminal emulator. It owns the window, input, rendering, fonts
and the PTY, and delegates all terminal emulation (parsing, grid, scrollback,
reflow, graphics decoding, Lottie rasterization) to **coffer**, an external
library consumed through pkg-config. SDL3 provides the window, input, timers
and a GPU renderer; FreeType and HarfBuzz rasterize and shape text; Fontconfig
(Linux), Core Text (macOS) or GDI (Windows) find fonts.

Everything the program draws or decides runs on **one main thread**. The only
other threads are the PTY reader and SDL's timer thread, and both do nothing
but post events to the main thread.

## Source map

| Area               | Files                                                                             |
| ------------------ | --------------------------------------------------------------------------------- |
| Entry, config      | `main.c`, `portty_conf.c`                                                         |
| Application policy | `portty_app.c` (keys, mouse, selection, clipboard, panels, diagnostics, reset)    |
| Platform + loop    | `backend_sdl3.c` (window, event loop, timers glue, pacing, debug-script hooks)    |
| Timers             | `timer.c` (pure slot table, no SDL)                                               |
| Animation pacing   | `frame_pacer.c` (pure, no SDL), `portty_pace_trace.c`                             |
| Terminal bridge    | `term.c`, `term_cfr.c` (the `TerminalBackend` interface and its coffer binding)   |
| Embedded terminals | `embedded_term.c` (a PTY-less coffer terminal: pager overlay, panel content)      |
| Rendering          | `rend_sdl3.c`, `rend_sdl3_atlas.c`, `rend_common.c`, `rend_boxdraw.c`, `shaders/` |
| Fonts              | `font.c`, `font_ft.c`, `colr.c`, `font_resolve*.c`                                |
| PTY                | `pty.c` (POSIX), `pty_w32.c` (ConPTY)                                             |
| Debug scripts      | `portty_script.c` (UI extension commands; the engine lives in coffer)             |
| Diagnostics        | `diag.c`, `dump.c`, `pager.c`                                                     |
| Panels             | `portty_panel.c` (geometry and state; drawn by the renderer)                      |

## Backends

Every subsystem that could have more than one implementation uses the same
pattern: a header defines a struct of function pointers, a small `.c` file
provides thin wrappers, and implementation files fill the pointers.

```
portty_backend.h  PorttyBackend   backend_sdl3.c   (window, input, loop, clipboard, PTY pump)
term.h            TerminalBackend term.c + term_cfr.c   (the coffer binding)
font.h            FontBackend     font.c + font_ft.c
font_resolve.h    resolver        font_resolve_{fc,ct,w32}.c
```

Adding a method means: the pointer in the struct, a wrapper in the generic
`.c` file, and an implementation in each backend (at least the one in use).
`PorttyApp` and the renderer only see the interfaces, so nothing outside
`backend_sdl3.c` includes SDL's windowing headers for decisions of policy.

Rendering decisions that do not depend on the GPU (emoji coverage routing,
downscale policy, Nerd Font v2 → v3 translation, font-style cascades) live in
`rend_common.c` and are shared; `rend_sdl3.c` only differs in atlas insertion
and final placement.

## Startup

```
config → platform → terminal → window → renderer → font loading → PTY
```

The window is created hidden at a placeholder size, the fonts are loaded, and
the grid size is derived from the cell size before the window is shown. The
font loader needs the compositor's scale, which is why the window precedes the
fonts.

## Threads and the event loop

**PTY reader thread.** Blocks in `poll()` on the PTY master, a signal fd and a
wakeup pipe. It reads a chunk and posts `EVENT_PTY_DATA` (carrying the bytes)
to the main thread. Closing and child exit arrive as `EVENT_PTY_CLOSED` and
`EVENT_PTY_CHILD_EXIT`. The reader can be paused (`pause_pty`) by the flood
guard and by an active selection, each holding its own reference.

**Main thread.** The loop in `sdl3_run()` is event-driven: it blocks in
`SDL_WaitEvent()` and does nothing while idle. One pass:

```
 1  script step            (debug scripts only; one command per pass)
 2  sdl3_pace_update()     (enter or leave paced mode; see Animation pacing)
 3  block                  (SDL_WaitEvent, unless paced)
 4  drain events           (PTY data, keys, mouse, window, timer fires)
 5  release the flood pause if the queue emptied
 6  render step            (flush damage; draw and present if something changed)
 7  post-render captures   (screendump, renderdump, frame recording)
```

Events that matter:

| Event                                         | Source             | What the loop does                                      |
| --------------------------------------------- | ------------------ | ------------------------------------------------------- |
| `EVENT_PTY_DATA`                              | reader thread      | feed coffer; may scroll, set the title, trigger replies |
| `EVENT_TIMER_FIRE` (slot, epoch)              | SDL's timer thread | `timer_dispatch()` → `sdl3_handle_timer()`              |
| `EVENT_WAKE`                                  | any thread         | nothing; ends the blocking wait so the loop re-checks   |
| `SDL_EVENT_KEY_*`, mouse, text input          | SDL                | `PorttyApp` decides; bytes go to the PTY                |
| window resize, display scale, focus, exposure | SDL                | re-derive the grid, reload fonts, mark dirty            |
| `OCCLUDED`, `MINIMIZED`, `HIDDEN`, `SHOWN`    | SDL                | track visibility (pacing stops while not visible)       |

**Output flood guard.** A program that writes faster than the loop can parse
would otherwise starve input and timers. One pass may spend at most 25 ms
(`PTY_FLOOD_PARSE_NS`) parsing; then the reader is paused so the child blocks on
the PTY buffer, the pass ends, and input and timers get serviced. While a
backlog is draining, presents are coalesced to at most one per 12 ms
(`FLOOD_RENDER_MIN_NS`); a one-shot timer forces the final frame out when the
backlog is gone.

**Redraw model.** Rendering happens only when something changed. coffer
accumulates damage; the loop folds it into one dirty flag with
`terminal_flush_damage()` once per pass, and changes coffer cannot see (cursor
blink, selection, scroll position, focus, resize) call `terminal_mark_dirty()`.
An idle terminal does no rendering work and no wakeups beyond the cursor blink.

## Timers are events

Every deadline in the backend is an SDL timer that posts an event. Nothing
counts time down, and nothing polls a clock to decide whether a timer is due.
The users are: cursor blink, autoscroll while dragging a selection, the record
tick, the resize-overlay and status-message hides, the flood-hold release, the
script `wait` deadline, and the free-running animation frame tick.

`timer.c` is a slot table (`TIMER_MAX` slots) owned by the main thread. It does
not know about SDL: a `TimerScheduler` supplied by the backend arms and cancels
the real timers, which is what lets `test_timer` run against a fake.

```
main thread                            SDL timer thread
-----------                            ----------------
timer_add_ns() ── arm(slot, epoch) ──▶ SDL_AddTimerNS
                                       ... deadline passes ...
                                       callback: queue ONE EVENT_TIMER_FIRE(slot, epoch)
                                                 return next - now  (periodic)
EVENT_TIMER_FIRE
  timer_dispatch(slot, epoch)
    stale?  → drop
    else    → TimerEvent.code → sdl3_handle_timer()
```

- **Epochs.** A timer is armed with an epoch that every re-arm, `timer_reset`
  and `timer_remove` bumps. A timer can be removed or reset after its event is
  already queued; the main thread compares the event's epoch with the slot's
  and drops a stale one. A `TimerId` additionally carries a slot generation, so
  a stale id cannot touch the timer that reused the slot.
- **No drift.** A periodic callback keeps an absolute deadline
  (`timer_next_deadline`: `prev + period`) and returns the time remaining to
  it, so a late wake does not push later ticks back. If it is more than a
  period behind it skips to the next grid point instead of firing a burst.
- **No pile-up.** The callback queues one event per slot at a time
  (`timer_pending[slot]` holds the epoch of the queued event, cleared by the
  main thread when it handles it). A busy main thread therefore never
  accumulates cursor-blink events.
- **Arm records.** SDL does not promise that a cancelled timer's callback has
  returned, so the records the callbacks read are handed out round-robin from a
  ring four times the slot count rather than reused immediately.
- **Script waits.** A script step runs one command. After it the loop arms a
  one-shot for the current wait's remaining time, and posts `EVENT_WAKE` when
  the next command is ready to run, so a script is neither slowed by nor
  dependent on unrelated events.

## Rendering

The render step draws one frame when `terminal_needs_redraw()` says so.
`rend_sdl3_draw_terminal()` has three phases:

1. **Populate the atlas.** Walk the visible cells and cache any missing glyph;
   nothing is drawn yet. Room comes from entries the current frame has not
   touched, so a frame that fits needs one pass. Only a full reset of the atlas
   makes the pass restart.
2. **Flush staging.** Upload the staged glyph pixels to the GPU texture in one
   batch. The draw pass never inserts a glyph: one cached after this flush would
   sample texels the GPU does not have yet, so a draw-time miss stays blank for
   that frame.
3. **Draw** (`render_scene()`).

**One render path.** `render_scene()` serves every surface: the main view, the
pager overlay, PNG export, and panel content. It clears to the destination's
base colour, draws the cells (glyphs, underline spans, cursor, selection),
draws Lottie layers and sixel/iTerm2/kitty images (kitty `z < 0` behind the
text, everything else on top), composites the panel chrome, and blits the
result out with a single sRGB encode.

**Linear light.** The scene is drawn into a grow-only `RGBA64_FLOAT` /
`SRGB_LINEAR` target (`linear_target`), so glyph coverage blends in linear
light. The last step is one full-window blit onto the sRGB destination
(`blit_linear_to_output`), which does the encode. The renderer falls back to
plain sRGB blending inside `render_scene()` if the target cannot be made, so
gamma correctness is structural, not something each caller has to remember.
Details: [docs/embedded-terminals-linear-light.md](docs/embedded-terminals-linear-light.md).

**The glyph atlas** is 2048×2048 with shelf packing, FNV-1a lookup and
frame-LRU eviction: entries the current frame has not touched are dropped
oldest first and their regions recycled through a free list. It is keyed on
`(font_data, glyph_id, colour)`, where `font_data` is a stable identity rather
than a face address (the four main styles key on the loaded face and a font
reload empties the atlas; fallback glyphs key on a per-font-path record that
outlives face eviction). Fallback routing (which font covers a codepoint) is
memoised per codepoint for the life of the fallback state.

**Held presents.** The linear target keeps the last window scene. If nothing
changed, `rend_sdl3_present_held()` redraws the window from it with one blit
and no scene pass. The `scene_valid` flag says the target still holds the
window scene; `render_scene()` clears it (every use overwrites the target),
`rend_sdl3_draw_terminal()` sets it for the window scene, and the public
mutators that change the scene's inputs (resize, scale, overlay, scroll,
panels, font reload) clear it. The hold is also refused when the output size
differs from the one the scene was drawn at.

**Font reload.** When the window moves to a display with another scale, fonts
are re-rasterized for the new DPI. The atlas must be emptied _before_ the new
faces exist, because the allocator can hand the new face the address of the one
it replaces and a stale entry would answer with the old bitmap. The order is
fallback faces → font faces → atlas → resolver, and a failed reload restores
the previous scale.

## Animation pacing

A Lottie animation is a stream of frames that must reach the screen at a steady
rate. The goal is that each frame stays on screen for the same number of
display refreshes: 30 fps content on a 60 Hz display is always 2 refreshes, 60
fps is always 1, and 24 fps is the unavoidable 2:3 pattern. portty does not get
there by ticking at the content rate. It makes the display's refresh the clock.

**Principle 1: the frame is a function of time.** coffer anchors an animation
at the first presentation time it sees after it starts playing, resumes, seeks
or changes speed, and then computes

```
position(t) = anchor + (t - anchor_time) × speed × frame_rate
frame       = in_point + floor(position)         (loop: wrap; otherwise clamp and stop)
```

with no state carried between calls. How often the host calls, or how late a
call runs, changes nothing about which frame is chosen. The call is
`cfr_lottie_present(vt, present_us, hold_us)`: the frame about to be drawn
reaches the screen at `present_us` and stays for `hold_us` (one display
period), and the engine evaluates the position at **`present_us + hold_us / 2`**,
the middle of the frame's time on screen.

The middle matters. At 30 fps on a 60 Hz display every frame boundary lands on
a refresh. Sampling on the refresh would put the position exactly on an
integer, where rounding noise decides whether a frame is held for 2 refreshes
or 3. Sampling mid-refresh leaves a quarter frame of margin either way.

**Principle 2: the clock is the display.** While an animation plays in a
visible window, the loop is in _paced mode_ (`sdl3_pace_update()`):

- vsync is turned on (`SDL_SetRenderVSync`), and turned off again afterwards;
- the loop does not wait in `SDL_WaitEvent`; instead it presents **once per
  refresh**, and the present blocks until the next refresh;
- events are still drained every pass, so input is serviced at most one refresh
  late;
- an unchanged frame is presented too, by re-blitting the cached scene (a
  _held present_). This is not optional: a present blocks only while the swap
  queue is full, and a 30 fps animation that presented only on change would
  drain the queue and get no timing information from the present.

A held present costs about 0.08 ms. A full frame costs the scene pass plus the
ThorVG raster, 4.5 ms in a release build.

**`FramePacer`** (`frame_pacer.c`) turns that into sample times. It reads no
clock and links no SDL; every time is passed in as nanoseconds.

```
pacer_begin(now)               → the refresh the next present will occupy
pacer_cost(work)               → budget for preparing a frame
pacer_end(before, after)       → learn from how long the present blocked
```

- **The sample time is a counter, not a measurement.** `pacer_begin()` returns
  the previous value plus exactly one period. Re-deriving "the next refresh"
  from the current time flips between two adjacent refreshes whenever the
  present's return jitters across a grid point (returns jitter by about
  ±0.3 ms), and every flip is a dropped or doubled frame.
- **The period** starts from the display mode (the reported rational, so 59.94 Hz
  is not rounded to 60) and is refined from the interval over a long run of
  presents that returned exactly one period apart, clamped to ±0.5% of nominal.
  A long baseline is stable to about 1e-4, so media time does not slip against
  the display.
- **Compositor stalls.** About 1–4% of presents return a whole refresh late: the
  compositor repeats a frame on screen no matter what the application does.
  The sample time lags by a period each time. Once it lags by more than one
  period plus the frame cost, the pacer skips ahead by whole periods and counts
  a _miss_, which keeps media time on the clock instead of letting it fall
  behind. A miss shows up as one hold of 1 and one of 3 in a per-present
  histogram.
- **Blocked or not.** A present counts as blocked when it took at least a
  quarter period. The pacer is `PACER_PROBING` at first, `PACER_LOCKED` after 4
  blocked presents in a row, and `PACER_FREE` after 8 presents in a row that did
  not block. (The first presents return at once while the swap queue fills,
  which is why giving up takes 8.) `PACER_FREE` is sticky until pacing stops.
  Detection looks at duration, not at the vsync flag: the software renderer on
  Wayland reports vsync off yet blocks on the compositor's frame callback.

**The free-running grid.** In `PACER_FREE` presents do not block, so there is no
display clock to follow. Causes: vsync not honoured, a compositor without frame
callbacks, a headless run. The pacer then makes its own clock: a sequence of
sample times one period apart (about 16.67 ms at 60 Hz) that advances one period
per tick, driven by a periodic event timer (`TCODE_FRAME`). The loop goes back
to `SDL_WaitEvent`. Each tick samples the animation for the next slot of the
grid (with the same mid-slot hold) and marks the screen dirty if the frame
changed; the normal render step presents it.

The grid keeps timing exact on average, because the timer is accurate to
nanoseconds, but nothing ties it to the real display's refresh edges. A frame
committed near an edge can land on either side, so cadence is no better than
before (20–30% off at 30 and 60 fps in measurements). It is a fallback so that
a platform whose presents do not block still gets exact timers and time-correct
sampling. It also uses the period as the hold, not zero: a grid whose period
divides the frame period would otherwise sample exactly on a frame boundary.

**Visibility.** Pacing stops when the window is occluded, minimized or hidden (a
vsynced present on a hidden surface can be throttled) and when the display
changes (the refresh period may differ). Media time keeps running by the clock,
and on return the sampler lands on the right frame with no catch-up burst.

**What is not paced.** An idle terminal, and any terminal without a playing
animation, uses none of this: vsync stays off and there are no wakeups beyond
the cursor blink. A resize clears every animation in the engine, so pacing stops
with it.

**Observing it.** `PORTTY_PACE_TRACE=file` writes one line per present (start
and end time, the sample time, locked or not, the miss count, whether it was a
held present, and the frame each playing animation showed).
`scripts/pace-report.sh file` turns that into a histogram of how many
refreshes each frame stayed. `scripts/pace-lottie.py` writes a test animation
and the debug script that plays it. `PORTTY_PACE_FREE=1` forces the free-running
mode. The diagnostics report has a `lottie pacing` row. The `present-held`
script command re-presents the cached scene, for checking that a held frame is
byte-identical to the frame it repeats.

## The coffer boundary

coffer is a separate library; portty bridges it through `TerminalBackend`
(`term_cfr.c`). A rendering symptom is often an engine one: cell width,
grapheme clustering, reflow, scrollback, alt-screen and cursor save/restore
live in coffer, as do the sixel, iTerm2, kitty and Lottie decoders. Glyph
raster, emoji routing, the atlas, box drawing, selection UX and clipboard live
in portty.

- Cell widths are computed by coffer at insertion time. The renderer treats a
  width-0 cell as a continuation.
- Images (sixel, iTerm2, kitty, Lottie) are owned by the engine and fetched per
  frame (`terminal_get_images`, `terminal_get_lotties`).
  The host owns only a texture cache keyed by the engine's stable id and
  version.
- **Client pixels are logical.** Every pixel size a client hands the engine is
  logical, and the engine multiplies by the content scale once on the way out.
  Everything the terminal measures back to the client (`TIOCGWINSZ` pixel
  fields, `CSI 14/16 t`, XTSMGRAPHICS, `OSC 1337;ReportCellSize`) is logical
  too. This is the iTerm2 convention; kitty, foot and wezterm report device
  pixels, and nothing in the protocol can tell a client which one it is
  talking to except the 1337 reply, which carries the scale.
- The engine's cell-width contract is declared to child processes as
  `PORTTY_WIDTH=uts51` (and `PORTTY=1`).
- A heap allocator (`heap_harden.c`) wraps coffer's allocator with a magic
  value, a canary, poison-on-free and a backtrace ring. It is on in debug and
  sanitizer builds; a canary mismatch calls `PORTTY_BUG_ABORT`, which dumps the
  heap statistics and registered state and aborts.

## Scales and coordinates

There are two scales, and mixing them is the classic bug here:

- **`window_scale`** is the compositor's scale for the window. Mouse and window
  coordinates convert with it and nothing else.
- **`content_scale`** is `window_scale × user_scale`. It sizes fonts and
  therefore cells, and nothing else.

A cell hit is `logical_mouse × window_scale / (base_cell × content_scale)`.
Using `content_scale` in the numerator cancels under `--dpi-scale` and puts the
selection in the wrong place.

Rows come in three spaces: **display rows** (0 is the top visible row),
**unified rows** (negative is scrollback, non-negative is the visible area; -1
is the newest scrollback line) and the **scrollback index** (0 is the newest,
counting up to older). Mouse, cursor and selection coordinates share a single
column space.

## Debug scripts and tests

**Debug scripts** (`portty -S FILE`) run headless, scriptable scenarios: send
input, wait for output, assert on the buffer, move the mouse, resize, change the
display scale, and capture the presented frame (`screendump`), an offscreen
render (`renderdump`) or a recording. The engine, parser and core commands live
in coffer; `portty_script.c` adds the UI commands. They are the fastest way to
reproduce rendering, GPU and timing bugs, because the unit tests cannot see
compositing. Two things to remember when writing one: `send` does not append a
newline (use `sendln`), and `wait-for TEXT` also matches the echoed command
line, so put the marker text together at the shell (`echo ZZLOA''DED`).

**Tests** are small C programs under `tests/` with a custom assertion header
(`test_helpers.h`) and stubs that isolate a unit from SDL and the terminal
(`sdl_stubs.c`, `term_stubs.c`). They compile the sources they test directly
from `src/`. Modules written to be testable without a GPU are kept free of SDL
on purpose:

| Module                   | Test                               | What it checks                                     |
| ------------------------ | ---------------------------------- | -------------------------------------------------- |
| `timer.c`                | `test_timer`                       | epochs, stale events, one-shots, the deadline grid |
| `frame_pacer.c`          | `test_frame_pacer`                 | cadence against a simulated display, modes, stalls |
| `portty_pace_trace.c`    | `test_pace_trace`                  | trace format and file round trip                   |
| `scripts/pace-report.sh` | `test-pace-report.sh`              | the hold histogram on hand-built traces            |
| Lottie bridge            | `test_lottie`, `test_lottie_cache` | APC to placements; sampling by presentation time   |
| script parser            | `test_debug_script`                | extension commands against a fake host             |

Run the suite with `make check` from a build directory. Anything that depends
on compositing or on a real display's timing is checked with a debug script and
the pace trace, not a unit test.

## Invariants worth knowing

- **One main thread owns all state.** Other threads only post events.
  `SDL_PushEvent` is the one call they make.
- **A timer event is a hint, not a fact.** It may be stale; `timer_dispatch()`
  decides.
- **`render_scene()` is the only way to draw a terminal surface**, and every use
  of it invalidates the held scene.
- **The draw pass never inserts into the atlas.** A miss stays blank for a
  frame rather than sampling a texel that was never uploaded.
- **The atlas is emptied before a font reload creates new faces.**
- **The pacer's sample time is a counter.** Do not recompute it from the clock.
- **Mouse uses `window_scale`; cells use `content_scale`.**
- **A hold never replaces a full draw when anything changed.** `needs_redraw`
  wins, then `scene_valid` and the output size are checked.
- **Comments and docs describe current behaviour.** History belongs in the git
  log.
