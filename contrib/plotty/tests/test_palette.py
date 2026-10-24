"""Tests for plotty.palette — the Dracula Theme palette and its use in the CSS.

Verifies that the shipped palette matches the official Dracula specification
and that every ``{{Token}}`` placeholder in the TUI resolves to a real token.
"""

from __future__ import annotations

import os
import re

from plotty.palette import DRACULA, dracula

SRC_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "src", "plotty"
)
TUI_PATH = os.path.join(SRC_DIR, "tui.py")

PLACEHOLDER = re.compile(r"\{\{([A-Za-z][A-Za-z ]*)\}\}")


def test_palette_matches_dracula_spec():
    """Spot-check the palette against draculatheme.com/spec (Dracula Classic)."""
    expected = {
        "Background": "#282A36",
        "Current Line": "#6272A4",
        "Selection": "#44475A",
        "Foreground": "#F8F8F2",
        "Comment": "#6272A4",
        "Red": "#FF5555",
        "Orange": "#FFB86C",
        "Yellow": "#F1FA8C",
        "Green": "#50FA7B",
        "Cyan": "#8BE9FD",
        "Purple": "#BD93F9",
        "Pink": "#FF79C6",
        "Background Dark": "#21222C",
    }
    for token, value in expected.items():
        assert DRACULA.get(token) == value, f"{token} != {value}"


def test_palette_values_are_hex_colors():
    """Every palette entry is a #RRGGBB color."""
    for token, value in DRACULA.items():
        assert re.fullmatch(r"#[0-9A-Fa-f]{6}", value), f"{token}={value}"


def test_dracula_substitutes_known_tokens():
    assert dracula("color: {{Green}};") == "color: #50FA7B;"
    assert dracula("background: {{Background Dark}};") == "background: #21222C;"


def test_dracula_leaves_unknown_tokens_alone():
    assert dracula("color: {{Nope}};") == "color: {{Nope}};"


def test_tui_placeholders_resolve():
    """Every palette placeholder used in the TUI CSS is a known token."""
    with open(TUI_PATH, encoding="utf-8") as f:
        source = f.read()

    used = set(PLACEHOLDER.findall(source))
    assert used, "no palette placeholders found in tui.py"

    unknown = sorted(token for token in used if token not in DRACULA)
    assert not unknown, f"unknown palette tokens: {unknown}"
