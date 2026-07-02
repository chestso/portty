"""portty_lottie_protocol — APC escape sequence protocol for portty Lottie.

Emits Lottie animation control sequences following the coffer / portty
Lottie protocol spec. All payloads are JSON objects, base64-encoded, carried in
APC (ESC _ ... ESC \\) sequences.

On Windows ConPTY, uses OSC 5555 instead (ESC ] 5555 ; ... BEL).
"""

from __future__ import annotations

import base64
import json
import os
import sys

ANIM_ID = 1
DEFAULT_CELL_W_PX = 10
DEFAULT_CELL_H_PX = 20
MAX_APC_PAYLOAD = 65536
BASE64_CHUNK_SIZE = 4096

# Cache the TTY fd at module import time, before any TUI framework
# takes over stdout. Textual intercepts sys.stdout.buffer, so APC
# sequences must go directly to the terminal device.
_tty_fd: int | None = None


def _init_tty_fd() -> None:
    global _tty_fd
    if _tty_fd is not None:
        return
    try:
        _tty_fd = os.open("/dev/tty", os.O_WRONLY | os.O_NOCTTY)
    except OSError:
        try:
            _tty_fd = sys.stdout.fileno()
        except (AttributeError, OSError):
            _tty_fd = -1  # unusable, but won't crash


def _get_tty_fd() -> int:
    _init_tty_fd()
    return _tty_fd if _tty_fd is not None and _tty_fd >= 0 else -1


def tty_write(data: bytes) -> None:
    """Write raw bytes directly to the TTY, bypassing TUI framework interception."""
    fd = _get_tty_fd()
    if fd >= 0:
        os.write(fd, data)


# For testing: an optional callable that intercepts apc() output.
# When set, apc() calls this instead of writing to the TTY.
_apc_output_hook = None


def apc(json_str: str) -> None:
    """Emit an APC sequence: ESC _ <base64-json> ESC \\\\
    On Windows ConPTY, use OSC 5555 instead.
    Writes directly to the TTY to bypass TUI framework stdout interception."""
    b64 = base64.b64encode(json_str.encode()).decode()
    if _apc_output_hook is not None:
        _apc_output_hook(b64)
        return
    fd = _get_tty_fd()
    if fd < 0:
        return
    if sys.platform == "win32":
        os.write(fd, b"\x1b]5555;" + b64.encode() + b"\x07")
    else:
        os.write(fd, b"\x1b_" + b64.encode() + b"\x1b\\")


def apc_load(
    lottie_json: str,
    row: int,
    col: int,
    rows: int,
    cols: int,
    layer: str = "foreground",
    opacity: float = 1.0,
    speed: float = 1.0,
    loop: bool = True,
    autostart: bool = True,
) -> None:
    """Load a Lottie animation via APC. Uses chunked upload for large payloads."""
    b64_len = len(base64.b64encode(lottie_json.encode()))

    if b64_len < MAX_APC_PAYLOAD:
        cmd = json.dumps(
            {
                "cmd": "load",
                "id": ANIM_ID,
                "lottie": json.loads(lottie_json),
                "placement": {
                    "row": row,
                    "col": col,
                    "rows": rows,
                    "cols": cols,
                },
                "layer": layer,
                "opacity": round(opacity, 2),
                "play": {
                    "speed": round(speed, 2),
                    "loop": loop,
                    "autostart": autostart,
                },
            },
            separators=(",", ":"),
        )
        apc(cmd)
        return

    # Chunked upload for large files
    b64 = base64.b64encode(lottie_json.encode()).decode()
    total_chunks = (len(b64) // BASE64_CHUNK_SIZE) + 1

    for seq in range(total_chunks):
        offset = seq * BASE64_CHUNK_SIZE
        chunk = b64[offset : offset + BASE64_CHUNK_SIZE]
        cmd = json.dumps(
            {
                "cmd": "load-chunk",
                "id": ANIM_ID,
                "seq": seq,
                "total": total_chunks,
                "data": chunk,
            },
            separators=(",", ":"),
        )
        apc(cmd)

    # Place the animation after chunks are sent
    apc_place(row, col, rows, cols, layer, opacity)

    # Start playback
    apc_play(speed, loop)


def apc_pause() -> None:
    cmd = json.dumps({"cmd": "pause", "id": ANIM_ID}, separators=(",", ":"))
    apc(cmd)


def apc_play(speed: float = 1.0, loop: bool = True) -> None:
    cmd = json.dumps(
        {"cmd": "play", "id": ANIM_ID, "speed": round(speed, 2), "loop": loop},
        separators=(",", ":"),
    )
    apc(cmd)


def apc_seek(frame: int) -> None:
    cmd = json.dumps(
        {"cmd": "seek", "id": ANIM_ID, "frame": frame}, separators=(",", ":")
    )
    apc(cmd)


def apc_stop() -> None:
    cmd = json.dumps({"cmd": "stop", "id": ANIM_ID}, separators=(",", ":"))
    apc(cmd)


def apc_delete() -> None:
    cmd = json.dumps({"cmd": "delete", "id": ANIM_ID}, separators=(",", ":"))
    apc(cmd)


def apc_place(
    row: int,
    col: int,
    rows: int,
    cols: int,
    layer: str = "foreground",
    opacity: float = 1.0,
) -> None:
    cmd = json.dumps(
        {
            "cmd": "place",
            "id": ANIM_ID,
            "placement": {"row": row, "col": col, "rows": rows, "cols": cols},
            "layer": layer,
            "opacity": round(opacity, 2),
        },
        separators=(",", ":"),
    )
    apc(cmd)


def parse_lottie_meta(lottie: dict) -> dict:
    """Extract w, h, fr, ip, op from Lottie JSON."""
    return {
        "w": lottie.get("w", 100),
        "h": lottie.get("h", 100),
        "fr": lottie.get("fr", 30),
        "ip": lottie.get("ip", 0),
        "op": lottie.get("op", 60),
    }


def compute_placement(
    canvas_w: int,
    canvas_h: int,
    term_rows: int,
    term_cols: int,
    *,
    chrome_rows: int = 6,
    border_cols: int = 2,
) -> tuple[int, int, int, int]:
    """Compute (row, col, rows, cols) for the animation placement, centered.

    chrome_rows: rows consumed by title bar + info bar + borders.
    border_cols: columns consumed by left/right borders.
    """
    avail_rows = term_rows - chrome_rows
    avail_cols = term_cols - border_cols

    cols = (canvas_w + DEFAULT_CELL_W_PX - 1) // DEFAULT_CELL_W_PX
    rows = (canvas_h + DEFAULT_CELL_H_PX - 1) // DEFAULT_CELL_H_PX

    if cols > avail_cols or rows > avail_rows:
        scale_w = avail_cols / cols
        scale_h = avail_rows / rows
        scale = min(scale_w, scale_h)
        cols = max(1, int(cols * scale))
        rows = max(1, int(rows * scale))

    start_row = chrome_rows // 2 + 1
    start_col = border_cols // 2 + 1
    place_row = start_row + (avail_rows - rows) // 2
    place_col = start_col + (avail_cols - cols) // 2

    return max(1, place_row), max(1, place_col), rows, cols
