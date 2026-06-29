"""plotty.cli — Command-line entry point for plotty."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .tui import LottiePlayerApp


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="plotty",
        description="Play Lottie animations in portty",
    )
    parser.add_argument(
        "file",
        help="Path to Lottie JSON file",
    )
    parser.add_argument(
        "-l",
        "--loop",
        action="store_true",
        default=True,
        help="Loop playback (default)",
    )
    parser.add_argument(
        "-L",
        "--no-loop",
        action="store_true",
        help="Play once and exit",
    )
    parser.add_argument(
        "-s",
        "--speed",
        type=float,
        default=1.0,
        help="Playback speed multiplier (default: 1.0)",
    )
    parser.add_argument(
        "--bg",
        action="store_true",
        help="Render as background layer",
    )
    parser.add_argument(
        "--opacity",
        type=float,
        default=1.0,
        help="Opacity 0.0-1.0 (default: 1.0)",
    )

    args = parser.parse_args()

    # Read the Lottie file
    path = Path(args.file)
    if not path.exists():
        print(f"ERROR: Cannot open '{args.file}': No such file", file=sys.stderr)
        sys.exit(1)

    lottie_json = path.read_text()

    # Validate it's valid JSON
    try:
        json.loads(lottie_json)
    except json.JSONDecodeError as e:
        print(f"ERROR: Invalid JSON in '{args.file}': {e}", file=sys.stderr)
        sys.exit(1)

    loop = not args.no_loop
    speed = max(0.1, args.speed)
    opacity = max(0.0, min(1.0, args.opacity))

    app = LottiePlayerApp(
        lottie_json=lottie_json,
        filepath=args.file,
        loop=loop,
        speed=speed,
        opacity=opacity,
        bg_layer=args.bg,
    )
    app.run()
