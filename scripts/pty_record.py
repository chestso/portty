#!/usr/bin/env python3
"""
Record what a child writes to a PTY, with optional auto-replies to
common terminal queries (DA1, DA2, XTVERSION). Useful for diagnosing
what a TUI emits before any real terminal renders it — feature
detection, capability probes, escape-sequence quirks.

Originally written to track down why Claude Code did not push the
kitty keyboard protocol stack on bloom-terminal: the answer turned out
to be a hardcoded `TERM_PROGRAM` allowlist, and this script is what
made it observable in minutes instead of guesswork.

Usage:
    scripts/pty_record.py [opts] -- CMD [ARGS...]

Common recipes:
    # See what claude probes on startup with a custom TERM:
    TERM=foo scripts/pty_record.py --timeout=4 -- claude

    # Pretend to be ghostty for a TUI:
    TERM_PROGRAM=ghostty scripts/pty_record.py -- some-tui

    # Inject keystrokes after the child settles:
    scripts/pty_record.py --send='\\x1b[13;2u' --send-after=1.0 -- some-tui

    # Auto-respond to XTVERSION queries:
    scripts/pty_record.py --xtversion-reply='\\x1bP>|kitty 0\\x1b\\\\' \\
        -- some-tui

Output (stderr):
    raw hex dump, escape-sanitised printable form, and a count of
    kitty keyboard protocol push/pop/set/query sequences.
"""

import argparse
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def decode_escape(s: str) -> bytes:
    return s.encode().decode("unicode_escape").encode("latin-1")


def render(b: bytes) -> str:
    out = []
    for c in b:
        if c == 0x1B:
            out.append("\\x1b")
        elif c == 0x0D:
            out.append("\\r")
        elif c == 0x0A:
            out.append("\\n")
        elif c == 0x09:
            out.append("\\t")
        elif 0x20 <= c < 0x7F:
            out.append(chr(c))
        else:
            out.append(f"\\x{c:02x}")
    return "".join(out)


def count_kitty(buf: bytes) -> dict:
    """Count kitty-keyboard CSI sequences (push/pop/set/query)."""
    counts = {">u": 0, "<u": 0, "=u": 0, "?u": 0}
    finals = b"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
    i = 0
    while i < len(buf) - 2:
        if buf[i] == 0x1B and buf[i + 1] == ord("["):
            j = i + 2
            if j < len(buf) and buf[j] in b"><=?":
                marker = chr(buf[j])
                k = j + 1
                while k < len(buf) and buf[k] not in finals:
                    k += 1
                if k < len(buf) and chr(buf[k]) == "u":
                    counts[marker + "u"] += 1
                i = k + 1
                continue
        i += 1
    return counts


def auto_reply_scan(accum: bytearray, scan: int, fd: int,
                    xtversion_reply, da1_reply, da2_reply) -> int:
    """Scan newly-arrived bytes for ESC [ ... <final> queries and
    write canned replies back into the PTY. Returns the new scan
    cursor."""
    i = max(0, scan - 4)
    while i < len(accum) - 1:
        if accum[i] == 0x1B and accum[i + 1] == ord("["):
            j = i + 2
            inter = b""
            if j < len(accum) and accum[j] in b"><=?":
                inter = bytes([accum[j]])
                j += 1
            while j < len(accum) and 0x30 <= accum[j] <= 0x3F:
                j += 1
            if j < len(accum):
                final = accum[j]
                seq = bytes(accum[i:j + 1])
                if final == ord("q") and inter == b">" and xtversion_reply:
                    os.write(fd, xtversion_reply)
                    sys.stderr.write(f"[fake] saw {seq!r}, replied {xtversion_reply!r}\n")
                elif final == ord("c") and inter == b"" and da1_reply:
                    os.write(fd, da1_reply)
                elif final == ord("c") and inter == b">" and da2_reply:
                    os.write(fd, da2_reply)
                i = j + 1
                continue
        i += 1
    return len(accum)


def main():
    ap = argparse.ArgumentParser(
        description="Record PTY traffic from a child process; useful for "
                    "diagnosing terminal feature-detect probes.")
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="Seconds to record before killing the child (default 3).")
    ap.add_argument("--rows", type=int, default=40)
    ap.add_argument("--cols", type=int, default=120)
    ap.add_argument("--send", default="",
                    help="Bytes to send to the child after --send-after seconds. "
                         r"Escape sequences like \x1b are decoded.")
    ap.add_argument("--send-after", type=float, default=0.5,
                    help="Delay before sending --send (default 0.5s).")
    ap.add_argument("--xtversion-reply", default="",
                    help=r"If non-empty, reply to CSI > q with this DCS string.")
    ap.add_argument("--da1-reply", default=r"\x1b[?62;22c",
                    help="DA1 reply for ESC [ c (set empty to disable).")
    ap.add_argument("--da2-reply", default=r"\x1b[>1;0;0c",
                    help="DA2 reply for ESC [ > c (set empty to disable).")
    ap.add_argument("cmd", nargs="+", help="Command and arguments to spawn.")
    args = ap.parse_args()

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(args.cmd[0], args.cmd)

    set_winsize(fd, args.rows, args.cols)

    xtversion_reply = decode_escape(args.xtversion_reply) if args.xtversion_reply else None
    da1_reply = decode_escape(args.da1_reply) if args.da1_reply else None
    da2_reply = decode_escape(args.da2_reply) if args.da2_reply else None
    inject = decode_escape(args.send) if args.send else None
    inject_at = time.time() + args.send_after if inject else None

    deadline = time.time() + args.timeout
    accum = bytearray()
    scan = 0

    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], max(0.01, deadline - time.time()))
        if fd in r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            accum.extend(data)
            scan = auto_reply_scan(accum, scan, fd,
                                   xtversion_reply, da1_reply, da2_reply)
        if inject and inject_at is not None and time.time() >= inject_at:
            os.write(fd, inject)
            inject_at = None

    try:
        os.kill(pid, 15)
        os.waitpid(pid, 0)
    except OSError:
        pass

    sys.stderr.write(f"\n--- captured {len(accum)} bytes ---\nHEX:\n")
    for i in range(0, len(accum), 32):
        chunk = accum[i:i + 32]
        sys.stderr.write(" ".join(f"{b:02x}" for b in chunk) + "\n")
    sys.stderr.write("\nRENDERED:\n")
    sys.stderr.write(render(bytes(accum)) + "\n")
    sys.stderr.write("\nKITTY MARKERS:\n")
    for k, v in count_kitty(bytes(accum)).items():
        sys.stderr.write(f"  CSI {k}: {v}\n")


if __name__ == "__main__":
    main()
