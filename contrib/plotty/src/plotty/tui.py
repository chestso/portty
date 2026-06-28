"""plotty.tui — Textual TUI for playing Lottie animations in bloom-terminal."""

from __future__ import annotations

import os

from textual.app import App, ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import Label, Static

from .protocol import (
    apc_delete,
    apc_load,
    apc_pause,
    apc_place,
    apc_play,
    apc_seek,
    apc_stop,
    compute_placement,
    parse_lottie_meta,
    tty_write,
)

# ---------------------------------------------------------------------------
# Custom widgets
# ---------------------------------------------------------------------------


class PlayIndicator(Static):
    """Animated play/pause indicator."""

    playing: reactive[bool] = reactive(True, always_update=True)

    def watch_playing(self, playing: bool) -> None:
        self.set_class(playing, "is-playing")
        self.set_class(not playing, "is-paused")
        self.update("▶ Playing" if playing else "⏸ Paused")


class FrameCounter(Static):
    """Frame progress display."""

    current_frame: reactive[int] = reactive(0, always_update=True)

    def __init__(self, total_frames: int = 60, **kwargs) -> None:
        super().__init__("0/60", **kwargs)
        self.total_frames = total_frames

    def watch_current_frame(self, frame: int) -> None:
        self.update(f"{frame}/{self.total_frames}")


class ProgressBar(Static):
    """Visual frame progress bar with filled/empty segments."""

    current_frame: reactive[int] = reactive(0, always_update=True)

    def __init__(self, total_frames: int = 60, **kwargs) -> None:
        super().__init__("░░░░░░░░░░░░░░░░░░░░", **kwargs)
        self.total_frames = total_frames

    def watch_current_frame(self, frame: int) -> None:
        ratio = frame / max(1, self.total_frames)
        filled = int(ratio * 20)
        bar = "█" * filled + "░" * (20 - filled)
        self.update(bar)


class SpeedDisplay(Static):
    """Speed multiplier display."""

    speed: reactive[float] = reactive(1.0, always_update=True)

    def watch_speed(self, speed: float) -> None:
        self.update(f"{speed:.1f}×")


class LoopIndicator(Static):
    """Loop mode indicator."""

    loop: reactive[bool] = reactive(True, always_update=True)

    def watch_loop(self, loop: bool) -> None:
        self.set_class(loop, "is-loop")
        self.set_class(not loop, "is-once")
        self.update("∞ loop" if loop else "→ once")


class LayerIndicator(Static):
    """Render layer indicator."""

    bg_layer: reactive[bool] = reactive(False, always_update=True)

    def watch_bg_layer(self, bg: bool) -> None:
        self.update("bg" if bg else "fg")


class OpacityDisplay(Static):
    """Opacity percentage display."""

    opacity: reactive[float] = reactive(1.0, always_update=True)

    def watch_opacity(self, opacity: float) -> None:
        self.update(f"{opacity:.0%}")


class InfoBar(Horizontal):
    """Bottom info bar — Charm-style with progress bar inline."""

    DEFAULT_CSS = """
    InfoBar {
        dock: bottom;
        height: 3;
        width: 100%;
        background: #1a1a2e;
        padding: 1 3;
        align: left middle;
    }

    InfoBar > * {
        height: 1;
        width: auto;
    }

    InfoBar > .info-item {
        min-width: 0;
        padding: 0 1;
    }

    InfoBar PlayIndicator {
        color: #04b575;
        text-style: bold;
    }

    InfoBar PlayIndicator.is-paused {
        color: #ecfd65;
    }

    InfoBar ProgressBar {
        color: #7d56f4;
        width: 22;
        padding: 0 1 0 0;
    }

    InfoBar FrameCounter {
        color: #999;
    }

    InfoBar SpeedDisplay {
        color: #ee6ff8;
        text-style: bold;
    }

    InfoBar LoopIndicator.is-loop {
        color: #04b575;
    }

    InfoBar LoopIndicator.is-once {
        color: #ecfd65;
    }

    InfoBar LayerIndicator {
        color: #999;
    }

    InfoBar OpacityDisplay {
        color: #999;
    }
    """

    def __init__(
        self,
        total_frames: int = 60,
        fps: int = 30,
        **kwargs,
    ) -> None:
        super().__init__(**kwargs)
        self.total_frames = total_frames
        self.fps = fps

    def compose(self) -> ComposeResult:
        yield PlayIndicator(classes="info-item")
        yield ProgressBar(total_frames=self.total_frames, classes="info-item")
        yield FrameCounter(total_frames=self.total_frames, classes="info-item")
        yield Label(f"{self.fps}fps", classes="info-item")
        yield SpeedDisplay(classes="info-item")
        yield LoopIndicator(classes="info-item")
        yield LayerIndicator(classes="info-item")
        yield OpacityDisplay(classes="info-item")


class HelpPanel(Vertical):
    """Right-aligned help panel showing keyboard shortcuts."""

    DEFAULT_CSS = """
    HelpPanel {
        dock: right;
        width: 30;
        height: 100%;
        background: #1a1a2e;
        padding: 1 2;
        display: none;
    }

    HelpPanel.visible {
        display: block;
    }

    HelpPanel .help-title {
        color: #ee6ff8;
        text-style: bold;
        height: 1;
        width: 100%;
        margin: 0 0 1 0;
    }

    HelpPanel .shortcut-row {
        height: 1;
        width: 100%;
    }

    HelpPanel .key {
        color: #7d56f4;
        text-style: bold;
    }

    HelpPanel .desc {
        color: #999;
    }

    HelpPanel .help-footer {
        color: #555;
        height: 1;
        width: 100%;
        margin: 1 0 0 0;
    }
    """

    def compose(self) -> ComposeResult:
        yield Label("Shortcuts", classes="help-title")
        for key, desc in [
            ("space", "play / pause"),
            ("← →", "seek -5 / +5"),
            ("+ -", "speed up / down"),
            ("r", "restart"),
            ("L", "toggle loop"),
            ("b", "toggle bg / fg"),
            ("[ ]", "opacity - / +"),
            ("q", "quit"),
        ]:
            yield Label(f"  {key:7s} {desc}", classes="shortcut-row")
        yield Label("  ?       close help", classes="shortcut-row")
        yield Label("press ? to toggle", classes="help-footer")


class LottiePlayerApp(App):
    """A TUI Lottie animation player for bloom-terminal."""

    CSS = """
    Screen {
        background: #0e0e18;
    }

    #anim-area {
        height: 1fr;
        width: 100%;
        align: center middle;
        border: round #7d56f4 40%;
    }

    #title-bar {
        dock: top;
        height: 3;
        width: 100%;
        background: #1a1a2e;
        padding: 0 4;
        align: left middle;
    }

    #title-bar .app-name {
        color: #ee6ff8;
        text-style: bold;
    }

    #title-bar .filename {
        color: #ad58b4;
        text-style: italic;
    }

    #title-bar .spacer {
        width: 1fr;
    }

    #title-bar .anim-info {
        color: #555;
    }

    #title-bar .version {
        color: #3c3c3c;
    }
    """

    BINDINGS = [
        ("space", "toggle_play", "Pause/Play"),
        ("q", "quit", "Quit"),
        ("escape", "quit", "Quit"),
        ("left", "seek_back", "Seek -5"),
        ("right", "seek_forward", "Seek +5"),
        ("plus", "speed_up", "Faster"),
        ("minus", "speed_down", "Slower"),
        ("r", "restart", "Restart"),
        ("L", "toggle_loop", "Toggle loop"),
        ("b", "toggle_layer", "Toggle bg/fg"),
        ("bracketleft", "opacity_down", "Opacity -10%"),
        ("bracketright", "opacity_up", "Opacity +10%"),
        ("question_mark", "toggle_help", "Help"),
    ]

    def __init__(
        self,
        lottie_json: str,
        filepath: str,
        loop: bool = True,
        speed: float = 1.0,
        opacity: float = 1.0,
        bg_layer: bool = False,
    ) -> None:
        super().__init__()
        self.lottie_json = lottie_json
        self.filepath = filepath
        self.lottie_data = __import__("json").loads(lottie_json)
        self.meta = parse_lottie_meta(self.lottie_data)
        self.total_frames = self.meta["op"] - self.meta["ip"]
        if self.total_frames <= 0:
            self.total_frames = 60

        self.playing = True
        self.current_frame = 0
        self.speed = speed
        self.loop = loop
        self.opacity = opacity
        self.bg_layer = bg_layer
        self.layer_str = "background" if bg_layer else "foreground"
        self.help_visible = False

        # Placement (computed on mount when we know terminal size)
        self.place_row = 0
        self.place_col = 0
        self.place_rows = 0
        self.place_cols = 0

        # Frame counter timer
        self._frame_timer = None

    def compose(self) -> ComposeResult:
        with Horizontal(id="title-bar"):
            yield Label("plotty ", classes="app-name")
            yield Label(self.filepath, classes="filename")
            yield Label("", classes="spacer")
            yield Label(
                f"{self.meta['w']}×{self.meta['h']}  {self.meta['fr']}fps",
                classes="anim-info",
            )
            yield Label("  v0.1", classes="version")
        with Container(id="anim-area"):
            yield Label("")
        yield HelpPanel(id="help-panel")
        yield InfoBar(
            total_frames=self.total_frames,
            fps=self.meta["fr"],
            id="info-bar",
        )

    HELP_PANEL_WIDTH = 30

    def _get_term_size(self) -> tuple[int, int]:
        """Get the terminal size in rows/cols."""
        try:
            from .protocol import _get_tty_fd

            fd = _get_tty_fd()
            if fd >= 0:
                ts = os.get_terminal_size(fd)
                return ts.lines, ts.columns
        except (OSError, ValueError):
            pass
        return (
            self.size.height if self.size.height else 24,
            self.size.width if self.size.width else 80,
        )

    def _recompute_placement(self, term_rows: int, term_cols: int) -> None:
        """Recompute animation placement for the current terminal size,
        accounting for whether the help panel is visible."""
        panel_cols = self.HELP_PANEL_WIDTH if self.help_visible else 0
        avail_cols = term_cols - panel_cols
        # chrome_rows: 3 (title bar) + 2 (anim-area border) + 3 (info bar) = 8
        # border_cols: 2 (anim-area left+right border)
        self.place_row, self.place_col, self.place_rows, self.place_cols = (
            compute_placement(
                self.meta["w"],
                self.meta["h"],
                term_rows,
                avail_cols,
                chrome_rows=8,
                border_cols=2,
            )
        )

    def action_toggle_help(self) -> None:
        self.help_visible = not self.help_visible
        panel = self.query_one("#help-panel", HelpPanel)
        panel.set_class(self.help_visible, "visible")
        # Give Textual a moment to relayout before we recompute
        self.set_timer(0.05, self._recenter_animation)

    def _recenter_animation(self) -> None:
        """Recompute placement and reload the animation at the new position."""
        term_rows, term_cols = self._get_term_size()
        self._recompute_placement(term_rows, term_cols)
        # Reload fully — ensures correct rasterization size and position
        apc_delete()
        apc_load(
            self.lottie_json,
            self.place_row,
            self.place_col,
            self.place_rows,
            self.place_cols,
            self.layer_str,
            self.opacity,
            self.speed,
            self.loop,
            autostart=True,
        )
        if not self.playing:
            apc_pause()

    def _replace_placement(self) -> None:
        """Re-issue the place command with current placement coordinates."""
        apc_place(
            self.place_row,
            self.place_col,
            self.place_rows,
            self.place_cols,
            self.layer_str,
            self.opacity,
        )

    def on_mount(self) -> None:
        term_rows, term_cols = self._get_term_size()

        # Compute placement for current terminal size
        self._recompute_placement(term_rows, term_cols)

        # Switch to alt screen and hide cursor
        tty_write(b"\x1b[?1049h")  # alt screen
        tty_write(b"\x1b[?25l")  # hide cursor

        # Load and start the animation
        apc_load(
            self.lottie_json,
            self.place_row,
            self.place_col,
            self.place_rows,
            self.place_cols,
            self.layer_str,
            self.opacity,
            self.speed,
            self.loop,
            autostart=True,
        )

        # Update info bar widgets
        info = self.query_one("#info-bar", InfoBar)
        play_ind = info.query_one(PlayIndicator)
        frame_ctr = info.query_one(FrameCounter)
        progress = info.query_one(ProgressBar)
        speed_disp = info.query_one(SpeedDisplay)
        loop_ind = info.query_one(LoopIndicator)
        layer_ind = info.query_one(LayerIndicator)
        opacity_disp = info.query_one(OpacityDisplay)

        play_ind.playing = self.playing
        frame_ctr.current_frame = 0
        progress.current_frame = 0
        speed_disp.speed = self.speed
        loop_ind.loop = self.loop
        layer_ind.bg_layer = self.bg_layer
        opacity_disp.opacity = self.opacity

        # Start a timer to update the local frame counter
        self._frame_timer = self.set_interval(0.1, self._tick_frame)

    def _tick_frame(self) -> None:
        if not self.playing:
            return
        advance = int(self.meta["fr"] * self.speed * 0.1)
        if advance < 1:
            advance = 1
        self.current_frame += advance
        if self.loop:
            self.current_frame %= self.total_frames
        elif self.current_frame >= self.total_frames:
            self.current_frame = self.total_frames - 1
            self.playing = False

        info = self.query_one("#info-bar", InfoBar)
        frame_ctr = info.query_one(FrameCounter)
        play_ind = info.query_one(PlayIndicator)
        progress = info.query_one(ProgressBar)
        frame_ctr.current_frame = self.current_frame
        progress.current_frame = self.current_frame
        play_ind.playing = self.playing

    def action_toggle_play(self) -> None:
        if self.playing:
            apc_pause()
            self.playing = False
        else:
            apc_play(self.speed, self.loop)
            self.playing = True

        info = self.query_one("#info-bar", InfoBar)
        play_ind = info.query_one(PlayIndicator)
        play_ind.playing = self.playing

    def action_seek_back(self) -> None:
        self.current_frame = max(0, self.current_frame - 5)
        apc_seek(self.current_frame)
        info = self.query_one("#info-bar", InfoBar)
        frame_ctr = info.query_one(FrameCounter)
        progress = info.query_one(ProgressBar)
        frame_ctr.current_frame = self.current_frame
        progress.current_frame = self.current_frame

    def action_seek_forward(self) -> None:
        self.current_frame = min(self.total_frames - 1, self.current_frame + 5)
        apc_seek(self.current_frame)
        info = self.query_one("#info-bar", InfoBar)
        frame_ctr = info.query_one(FrameCounter)
        progress = info.query_one(ProgressBar)
        frame_ctr.current_frame = self.current_frame
        progress.current_frame = self.current_frame

    def action_speed_up(self) -> None:
        self.speed = min(8.0, self.speed * 1.5)
        if self.playing:
            apc_play(self.speed, self.loop)
        info = self.query_one("#info-bar", InfoBar)
        speed_disp = info.query_one(SpeedDisplay)
        speed_disp.speed = self.speed

    def action_speed_down(self) -> None:
        self.speed = max(0.1, self.speed / 1.5)
        if self.playing:
            apc_play(self.speed, self.loop)
        info = self.query_one("#info-bar", InfoBar)
        speed_disp = info.query_one(SpeedDisplay)
        speed_disp.speed = self.speed

    def action_restart(self) -> None:
        apc_stop()
        apc_play(self.speed, self.loop)
        self.playing = True
        self.current_frame = 0
        info = self.query_one("#info-bar", InfoBar)
        play_ind = info.query_one(PlayIndicator)
        frame_ctr = info.query_one(FrameCounter)
        progress = info.query_one(ProgressBar)
        play_ind.playing = True
        frame_ctr.current_frame = 0
        progress.current_frame = 0

    def action_toggle_loop(self) -> None:
        self.loop = not self.loop
        if self.playing:
            apc_play(self.speed, self.loop)
        info = self.query_one("#info-bar", InfoBar)
        loop_ind = info.query_one(LoopIndicator)
        loop_ind.loop = self.loop

    def action_toggle_layer(self) -> None:
        self.bg_layer = not self.bg_layer
        self.layer_str = "background" if self.bg_layer else "foreground"
        self._replace_placement()
        info = self.query_one("#info-bar", InfoBar)
        layer_ind = info.query_one(LayerIndicator)
        layer_ind.bg_layer = self.bg_layer

    def action_opacity_down(self) -> None:
        self.opacity = max(0.1, self.opacity - 0.1)
        self._replace_placement()
        info = self.query_one("#info-bar", InfoBar)
        opacity_disp = info.query_one(OpacityDisplay)
        opacity_disp.opacity = self.opacity

    def action_opacity_up(self) -> None:
        self.opacity = min(1.0, self.opacity + 0.1)
        self._replace_placement()
        info = self.query_one("#info-bar", InfoBar)
        opacity_disp = info.query_one(OpacityDisplay)
        opacity_disp.opacity = self.opacity

    def on_resize(self, event) -> None:
        self.set_timer(0.05, self._recenter_animation)

    def on_unmount(self) -> None:
        # Clean up animation and restore terminal
        apc_delete()
        tty_write(b"\x1b[?25h")  # show cursor
        tty_write(b"\x1b[2J\x1b[H")  # clear screen
        tty_write(b"\x1b[?1049l")  # exit alt screen
