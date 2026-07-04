"""plotty.palette — CharmTone color palette."""

from __future__ import annotations

import json
from pathlib import Path

_PALETTE_PATH = Path(__file__).parent / "assets" / "charmtones.json"

with _PALETTE_PATH.open() as _f:
    CHARMTONES: dict[str, str] = json.load(_f)


def charm(css: str) -> str:
    """Replace ``{{Name}}`` placeholders in *css* with CharmTone hex values."""
    import re

    return re.sub(
        r"\{\{(\w+)\}\}",
        lambda m: CHARMTONES.get(m.group(1), m.group(0)),
        css,
    )
