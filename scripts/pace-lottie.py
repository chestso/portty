#!/usr/bin/env python3
"""Generate a frame-pacing test animation and a portty -S script that plays it.

Frame i of the animation is a flat colour that encodes i (red = i % 16, green
= i // 16 % 16, in steps of 16), so a screendump identifies the frame that was
on screen as well as the pace trace does.

  scripts/pace-lottie.py --fps 30 --seconds 10 --script /tmp/pace.txt
  (also writes /tmp/pace.txt.apc, the escape sequence the script cats)
  PORTTY_PACE_TRACE=/tmp/pace.trace build/src/portty -S /tmp/pace.txt
  scripts/pace-report.sh /tmp/pace.trace 16666667
"""

import argparse
import base64
import json


def colour(i):
    r = (i % 16) * 16 + 8
    g = ((i // 16) % 16) * 16 + 8
    return [r / 255.0, g / 255.0, 0.5, 1.0]


def animation(fps, frames):
    keys = [{"t": i, "s": colour(i), "h": 1} for i in range(frames)]
    return {
        "v": "5.6.0",
        "fr": fps,
        "ip": 0,
        "op": frames,
        "w": 200,
        "h": 200,
        "nm": "pace",
        "ddd": 0,
        "assets": [],
        "layers": [
            {
                "ddd": 0,
                "ind": 1,
                "ty": 4,
                "nm": "bg",
                "sr": 1,
                "ks": {
                    "o": {"a": 0, "k": 100},
                    "r": {"a": 0, "k": 0},
                    "p": {"a": 0, "k": [100, 100, 0]},
                    "a": {"a": 0, "k": [0, 0, 0]},
                    "s": {"a": 0, "k": [100, 100, 100]},
                },
                "ao": 0,
                "shapes": [
                    {
                        "ty": "gr",
                        "it": [
                            {
                                "ty": "rc",
                                "d": 1,
                                "s": {"a": 0, "k": [200, 200]},
                                "p": {"a": 0, "k": [0, 0]},
                                "r": {"a": 0, "k": 0},
                            },
                            {
                                "ty": "fl",
                                "c": {"a": 1, "k": keys},
                                "o": {"a": 0, "k": 100},
                                "r": 1,
                            },
                            {
                                "ty": "tr",
                                "p": {"a": 0, "k": [0, 0]},
                                "a": {"a": 0, "k": [0, 0]},
                                "s": {"a": 0, "k": [100, 100]},
                                "r": {"a": 0, "k": 0},
                                "o": {"a": 0, "k": 100},
                            },
                        ],
                    }
                ],
                "ip": 0,
                "op": frames,
                "st": 0,
                "bm": 0,
            }
        ],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--loop-seconds", type=float, default=2,
                    help="length of one loop of the animation")
    ap.add_argument("--seconds", type=float, default=10,
                    help="how long the script plays it")
    ap.add_argument("--script", required=True, help="portty -S script to write")
    ap.add_argument("--json", help="also write the Lottie JSON here")
    ap.add_argument("--cols", type=int, default=20)
    ap.add_argument("--rows", type=int, default=10)
    args = ap.parse_args()

    frames = max(2, round(args.fps * args.loop_seconds))
    lottie = animation(args.fps, frames)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(lottie, f)

    cmd = {
        "cmd": "load",
        "id": 1,
        "lottie": lottie,
        "fit": "contain",
        "placement": {"row": 2, "col": 2, "cols": args.cols, "rows": args.rows},
        "play": {"speed": 1.0, "loop": True, "autostart": True},
    }
    b64 = base64.b64encode(json.dumps(cmd, separators=(",", ":")).encode()).decode()
    if len(b64) > 60000:
        raise SystemExit("animation too large for one APC (%d bytes)" % len(b64))

    # The script parser caps line length, so the APC goes into a file that
    # the shell cats to the terminal.
    apc_path = args.script + ".apc"
    with open(apc_path, "w") as f:
        f.write("\x1b_%s\x1b\\" % b64)

    with open(args.script, "w") as f:
        f.write("wait 0.5\n")
        f.write("sendln clear; cat %s; echo ZZLOA''DED\n" % apc_path)
        f.write("wait-for ZZLOADED 20\n")
        f.write("wait %g\n" % args.seconds)
        f.write("quit\n")


if __name__ == "__main__":
    main()
