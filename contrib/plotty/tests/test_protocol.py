"""Tests for plotty.protocol — validates APC protocol output.

Captures the APC escape sequences that plotty would emit and verifies
they follow the coffer Lottie protocol spec.
"""

from __future__ import annotations

import base64
import json
import os

import plotty.protocol as proto

DEMO_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "demo-lottie.json"
)


class FakeStdout:
    """Captures APC output via the protocol module's _apc_output_hook."""

    def __init__(self):
        self.data = bytearray()

    def getvalue(self):
        return bytes(self.data)

    def clear(self):
        self.data = bytearray()


def _with_fake_stdout(fn, *args, **kwargs):
    """Run a protocol function, capturing APC output via the hook."""
    fake = FakeStdout()

    def hook(b64_str):
        fake.data.extend(b"\x1b_" + b64_str.encode() + b"\x1b\\")

    orig_hook = proto._apc_output_hook
    proto._apc_output_hook = hook
    try:
        fn(*args, **kwargs)
        return fake.getvalue()
    finally:
        proto._apc_output_hook = orig_hook


def extract_apc_sequences(data: bytes) -> list[dict]:
    """Extract and decode all APC sequences from raw byte output."""
    results = []
    i = 0
    while i < len(data):
        if data[i] == 0x1B and i + 1 < len(data) and data[i + 1] == 0x5F:
            start = i + 2
            end = start
            while end < len(data) - 1:
                if data[end] == 0x1B and data[end + 1] == 0x5C:
                    break
                end += 1

            b64_payload = data[start:end].decode("ascii")
            try:
                json_str = base64.b64decode(b64_payload).decode("utf-8")
                cmd = json.loads(json_str)
                results.append(cmd)
            except Exception as e:
                results.append({"_error": str(e), "_raw_b64": b64_payload[:80]})

            i = end + 2
        else:
            i += 1

    return results


# ---- Tests ----


def test_demo_lottie_json():
    with open(DEMO_PATH) as f:
        lottie = json.load(f)

    assert "v" in lottie
    assert lottie["fr"] == 30
    assert lottie["ip"] == 0
    assert lottie["op"] == 60
    assert lottie["w"] == 200
    assert lottie["h"] == 200
    assert len(lottie["layers"]) >= 1
    assert lottie["layers"][0]["ty"] == 4
    assert len(lottie["layers"][0]["shapes"]) >= 1
    ks = lottie["layers"][0]["ks"]
    assert "p" in ks
    assert ks["p"]["a"] == 1
    print("  ✓ demo-lottie.json is valid Lottie with bouncing ball animation")


def test_apc_pause():
    data = _with_fake_stdout(proto.apc_pause)
    cmds = extract_apc_sequences(data)
    pause_cmd = next((c for c in cmds if c.get("cmd") == "pause"), None)
    assert pause_cmd is not None, f"No pause command found in: {cmds}"
    assert pause_cmd["id"] == 1
    print("  ✓ apc_pause emits correct APC sequence")


def test_apc_play():
    data = _with_fake_stdout(proto.apc_play, 2.0, False)
    cmds = extract_apc_sequences(data)
    play_cmd = next((c for c in cmds if c.get("cmd") == "play"), None)
    assert play_cmd is not None, f"No play command found in: {cmds}"
    assert play_cmd["id"] == 1
    assert play_cmd["speed"] == 2.0
    assert play_cmd["loop"] is False
    print("  ✓ apc_play emits correct APC sequence with speed and loop")


def test_apc_seek():
    data = _with_fake_stdout(proto.apc_seek, 42)
    cmds = extract_apc_sequences(data)
    seek_cmd = next((c for c in cmds if c.get("cmd") == "seek"), None)
    assert seek_cmd is not None, f"No seek command found in: {cmds}"
    assert seek_cmd["id"] == 1
    assert seek_cmd["frame"] == 42
    print("  ✓ apc_seek emits correct APC sequence")


def test_apc_stop():
    data = _with_fake_stdout(proto.apc_stop)
    cmds = extract_apc_sequences(data)
    stop_cmd = next((c for c in cmds if c.get("cmd") == "stop"), None)
    assert stop_cmd is not None, f"No stop command found in: {cmds}"
    assert stop_cmd["id"] == 1
    print("  ✓ apc_stop emits correct APC sequence")


def test_apc_delete():
    data = _with_fake_stdout(proto.apc_delete)
    cmds = extract_apc_sequences(data)
    delete_cmd = next((c for c in cmds if c.get("cmd") == "delete"), None)
    assert delete_cmd is not None, f"No delete command found in: {cmds}"
    assert delete_cmd["id"] == 1
    print("  ✓ apc_delete emits correct APC sequence")


def test_apc_load_small():
    small_lottie = json.dumps(
        {"v": "5.6.0", "fr": 30, "ip": 0, "op": 60, "w": 40, "h": 24, "layers": []}
    )
    data = _with_fake_stdout(
        proto.apc_load,
        small_lottie,
        5,
        10,
        "foreground",
        0.85,
        1.0,
        True,
        max_cols=8,
        max_rows=4,
    )
    cmds = extract_apc_sequences(data)
    load_cmd = next((c for c in cmds if c.get("cmd") == "load"), None)
    assert load_cmd is not None, f"No load command found in: {cmds}"
    assert load_cmd["id"] == 1
    assert load_cmd["placement"]["row"] == 5
    assert load_cmd["placement"]["col"] == 10
    assert load_cmd["max_cols"] == 8
    assert load_cmd["max_rows"] == 4
    assert load_cmd["layer"] == "foreground"
    assert load_cmd["opacity"] == 0.85
    assert load_cmd["play"]["speed"] == 1.0
    assert load_cmd["play"]["loop"] is True
    assert load_cmd["play"]["autostart"] is True
    assert load_cmd["lottie"]["v"] == "5.6.0"
    print("  ✓ apc_load (small) emits correct single load command")


def test_apc_load_chunked():
    big_lottie = json.dumps(
        {
            "v": "5.6.0",
            "fr": 30,
            "ip": 0,
            "op": 60,
            "w": 400,
            "h": 400,
            "layers": [],
            "padding": "x" * 50000,
        }
    )
    data = _with_fake_stdout(
        proto.apc_load,
        big_lottie,
        1,
        1,
        "background",
        0.5,
        2.0,
        False,
        max_cols=10,
        max_rows=10,
    )
    cmds = extract_apc_sequences(data)
    chunk_cmds = [c for c in cmds if c.get("cmd") == "load-chunk"]
    place_cmds = [c for c in cmds if c.get("cmd") == "place"]
    play_cmds = [c for c in cmds if c.get("cmd") == "play"]

    assert (
        len(chunk_cmds) > 1
    ), f"Expected multiple load-chunk commands, got {len(chunk_cmds)}"
    assert all(c["id"] == 1 for c in chunk_cmds), "All chunks should have id=1"
    assert chunk_cmds[0]["seq"] == 0, "First chunk should have seq=0"
    assert (
        chunk_cmds[-1]["seq"] == chunk_cmds[-1]["total"] - 1
    ), "Last chunk seq should be total-1"
    assert len(place_cmds) >= 1, "Expected at least one place command"
    assert len(play_cmds) >= 1, "Expected at least one play command"
    print(f"  ✓ apc_load (chunked) emits {len(chunk_cmds)} chunks + place + play")


def test_apc_place():
    data = _with_fake_stdout(
        proto.apc_place, 3, 5, "background", 0.7, max_cols=12, max_rows=8
    )
    cmds = extract_apc_sequences(data)
    place_cmd = next((c for c in cmds if c.get("cmd") == "place"), None)
    assert place_cmd is not None, f"No place command found in: {cmds}"
    assert place_cmd["id"] == 1
    assert place_cmd["placement"]["row"] == 3
    assert place_cmd["placement"]["col"] == 5
    assert place_cmd["max_cols"] == 12
    assert place_cmd["max_rows"] == 8
    assert place_cmd["layer"] == "background"
    assert place_cmd["opacity"] == 0.7
    print("  ✓ apc_place emits correct APC sequence")


def test_placement_computation():
    row, col, rows, cols = proto.compute_placement(200, 200, 24, 80)
    assert (
        rows > 0 and cols > 0
    ), f"Expected positive placement, got rows={rows}, cols={cols}"
    assert (
        row >= 1 and col >= 1
    ), f"Expected positive position, got row={row}, col={col}"
    print(
        f"  ✓ compute_placement(200,200,24,80) → row={row}, col={col}, rows={rows}, cols={cols}"
    )


def test_parse_lottie_meta():
    lottie = {"v": "5.7.4", "fr": 60, "ip": 10, "op": 120, "w": 300, "h": 150}
    meta = proto.parse_lottie_meta(lottie)
    assert meta["w"] == 300
    assert meta["h"] == 150
    assert meta["fr"] == 60
    assert meta["ip"] == 10
    assert meta["op"] == 120
    print("  ✓ parse_lottie_meta extracts w, h, fr, ip, op correctly")


def test_apc_wire_format():
    """Verify the raw wire format matches the coffer spec."""
    data = _with_fake_stdout(proto.apc_pause)
    # Should start with ESC _
    assert data[0:2] == b"\x1b_", f"APC should start with ESC _, got {data[0:2]!r}"
    # Should end with ESC \\
    assert data[-2:] == b"\x1b\\", f"APC should end with ESC \\, got {data[-2:]!r}"
    # Middle should be valid base64
    b64_payload = data[2:-2].decode("ascii")
    json_str = base64.b64decode(b64_payload).decode("utf-8")
    cmd = json.loads(json_str)
    assert cmd["cmd"] == "pause"
    print("  ✓ APC wire format matches spec: ESC _ <b64> ESC \\\\")


def main():
    print("Testing plotty.protocol APC output\n")

    tests = [
        test_demo_lottie_json,
        test_apc_pause,
        test_apc_play,
        test_apc_seek,
        test_apc_stop,
        test_apc_delete,
        test_apc_load_small,
        test_apc_load_chunked,
        test_apc_place,
        test_placement_computation,
        test_parse_lottie_meta,
        test_apc_wire_format,
    ]

    passed = 0
    failed = 0

    for test in tests:
        name = test.__name__
        try:
            test()
            passed += 1
        except Exception as e:
            print(f"  ✗ {name}: {e}")
            import traceback

            traceback.print_exc()
            failed += 1

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
