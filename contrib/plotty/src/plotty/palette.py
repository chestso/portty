"""plotty.palette — Dracula Theme color palette.

Token names and values follow the official Dracula specification
(Dracula Classic) at https://draculatheme.com/spec.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

_PALETTE_PATH = Path(__file__).parent / "assets" / "dracula.json"

with _PALETTE_PATH.open() as _f:
    DRACULA: dict[str, str] = json.load(_f)

# Token names may contain spaces ("Current Line", "Background Dark").
_PLACEHOLDER = re.compile(r"\{\{([A-Za-z][A-Za-z ]*)\}\}")


def dracula(css: str) -> str:
    """Replace ``{{Token}}`` placeholders in *css* with Dracula hex values.

    Unknown tokens are left untouched so they show up verbatim in the CSS.
    """
    return _PLACEHOLDER.sub(
        lambda m: DRACULA.get(m.group(1), m.group(0)),
        css,
    )
