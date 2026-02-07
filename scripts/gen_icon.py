#!/usr/bin/env python3
"""Generate bloom-terminal app icons (>_ prompt, Charm/Bubbletea colors).

Requires: pip install Pillow

Usage:
    python3 scripts/gen_icon.py

Outputs:
    data/icons/hicolor/scalable/apps/bloom-terminal.svg
    data/icons/hicolor/symbolic/apps/bloom-terminal-symbolic.svg
    data/icons/hicolor/256x256/apps/bloom-terminal.png
"""

import os
from PIL import Image, ImageDraw, ImageFont

# Charmbracelet / Bubbletea brand colors
CHARM_PINK = "#FF5FAF"
CHARM_PURPLE = "#7571F9"
BG_COLOR_HEX = "#000000"
BG_COLOR_RGBA = (0x00, 0x00, 0x00, 255)
CHARM_PINK_RGBA = (0xFF, 0x5F, 0xAF, 255)
CHARM_PURPLE_RGBA = (0x75, 0x71, 0xF9, 255)

# Symbolic icon color (GNOME convention)
SYMBOLIC_COLOR = "#241f31"

NOTO_MONO = "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
DATA_DIR = os.path.join(PROJECT_DIR, "data", "icons", "hicolor")

# -- SVG icon dimensions (128x128 viewBox) --
# Rounded rect
SVG_MARGIN = 8
SVG_SIZE = 128
SVG_RECT_W = SVG_SIZE - 2 * SVG_MARGIN
SVG_CORNER = 16
# >_ geometry
SVG_CHEVRON_X = 28
SVG_CHEVRON_TOP = 42
SVG_CHEVRON_MID_X = 56
SVG_CHEVRON_MID_Y = 64
SVG_CHEVRON_BOT = 86
SVG_UNDERSCORE_X1 = 62
SVG_UNDERSCORE_X2 = 98
SVG_UNDERSCORE_Y = 86
SVG_STROKE_W = 9


def generate_scalable_svg():
    """Generate the scalable app icon SVG."""
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<svg width="{SVG_SIZE}" height="{SVG_SIZE}" viewBox="0 0 {SVG_SIZE} {SVG_SIZE}" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="text-gradient" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="108" y2="0">
      <stop offset="0" stop-color="{CHARM_PINK}"/>
      <stop offset="1" stop-color="{CHARM_PURPLE}"/>
    </linearGradient>
  </defs>
  <rect x="{SVG_MARGIN}" y="{SVG_MARGIN}" width="{SVG_RECT_W}" height="{SVG_RECT_W}" rx="{SVG_CORNER}" ry="{SVG_CORNER}" fill="{BG_COLOR_HEX}"/>
  <path d="M {SVG_CHEVRON_X} {SVG_CHEVRON_TOP} L {SVG_CHEVRON_MID_X} {SVG_CHEVRON_MID_Y} L {SVG_CHEVRON_X} {SVG_CHEVRON_BOT}" fill="none" stroke="url(#text-gradient)" stroke-width="{SVG_STROKE_W}" stroke-linecap="round" stroke-linejoin="round"/>
  <line x1="{SVG_UNDERSCORE_X1}" y1="{SVG_UNDERSCORE_Y}" x2="{SVG_UNDERSCORE_X2}" y2="{SVG_UNDERSCORE_Y}" stroke="url(#text-gradient)" stroke-width="{SVG_STROKE_W}" stroke-linecap="round"/>
</svg>
"""


def generate_symbolic_svg():
    """Generate the symbolic icon SVG (monochrome, 16x16)."""
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<svg width="16" height="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg">
  <g fill="{SYMBOLIC_COLOR}">
    <path d="M 2.2 1 C 1 1 0 2 0 3.2 V 12.8 C 0 14 1 15 2.2 15 H 13.8 C 15 15 16 14 16 12.8 V 3.2 C 16 2 15 1 13.8 1 Z M 2.2 3 H 13.8 C 13.9 3 14 3.1 14 3.2 V 12.8 C 14 12.9 13.9 13 13.8 13 H 2.2 C 2.1 13 2 12.9 2 12.8 V 3.2 C 2 3.1 2.1 3 2.2 3 Z"/>
    <path d="M 3.5 5.5 L 6 8 L 3.5 10.5" fill="none" stroke="{SYMBOLIC_COLOR}" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/>
    <line x1="7" y1="10.5" x2="12" y2="10.5" stroke="{SYMBOLIC_COLOR}" stroke-width="1.2" stroke-linecap="round"/>
  </g>
</svg>
"""


def load_font(size):
    """Load Noto Sans Mono Bold at the given size."""
    try:
        font = ImageFont.truetype(NOTO_MONO, size)
        font.set_variation_by_name("Bold")
        return font
    except (OSError, IOError, ValueError):
        pass
    # Fallback: try fontconfig
    import subprocess
    try:
        result = subprocess.run(
            ["fc-match", "-f", "%{file}", "Noto Sans Mono:style=Bold"],
            capture_output=True, text=True, timeout=5)
        if result.returncode == 0 and result.stdout.strip():
            return ImageFont.truetype(result.stdout.strip(), size)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return ImageFont.load_default()


def rounded_rectangle(draw, xy, radius, fill):
    """Draw a rounded rectangle."""
    x0, y0, x1, y1 = xy
    draw.rectangle([x0 + radius, y0, x1 - radius, y1], fill=fill)
    draw.rectangle([x0, y0 + radius, x1, y1 - radius], fill=fill)
    draw.pieslice([x0, y0, x0 + 2 * radius, y0 + 2 * radius], 180, 270, fill=fill)
    draw.pieslice([x1 - 2 * radius, y0, x1, y0 + 2 * radius], 270, 360, fill=fill)
    draw.pieslice([x0, y1 - 2 * radius, x0 + 2 * radius, y1], 90, 180, fill=fill)
    draw.pieslice([x1 - 2 * radius, y1 - 2 * radius, x1, y1], 0, 90, fill=fill)


def lerp_color(c1, c2, t):
    """Linearly interpolate between two RGBA colors."""
    return tuple(int(a + (b - a) * t) for a, b in zip(c1, c2))


def generate_png(render_size, output_size):
    """Generate PNG icon at render_size, then downscale to output_size."""
    img = Image.new("RGBA", (render_size, render_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Draw rounded rectangle terminal background
    margin = int(render_size * 0.06)
    radius = int(render_size * 0.12)
    rounded_rectangle(draw, (margin, margin, render_size - margin,
                             render_size - margin), radius, BG_COLOR_RGBA)

    # Draw >_ text
    text = ">_"
    font = load_font(int(render_size * 0.5))

    bbox = draw.textbbox((0, 0), text, font=font)
    text_w = bbox[2] - bbox[0]
    text_h = bbox[3] - bbox[1]
    x = (render_size - text_w) // 2 - bbox[0]
    y = (render_size - text_h) // 2 - bbox[1]

    # Render text as grayscale mask
    text_img = Image.new("L", (render_size, render_size), 0)
    text_draw = ImageDraw.Draw(text_img)
    text_draw.text((x, y), text, fill=255, font=font)

    # Apply horizontal gradient (pink -> purple) through the mask
    pixels = img.load()
    mask = text_img.load()
    for px in range(render_size):
        t = px / max(render_size - 1, 1)
        color = lerp_color(CHARM_PINK_RGBA, CHARM_PURPLE_RGBA, t)
        for py in range(render_size):
            a = mask[px, py]
            if a > 0:
                # Alpha-blend gradient text over the background
                bg = pixels[px, py]
                inv = 255 - a
                r = (color[0] * a + bg[0] * inv) // 255
                g = (color[1] * a + bg[1] * inv) // 255
                b = (color[2] * a + bg[2] * inv) // 255
                pixels[px, py] = (r, g, b, bg[3])

    # Downscale with high-quality resampling
    if render_size != output_size:
        img = img.resize((output_size, output_size), Image.LANCZOS)

    return img


def write_svg(path, content):
    """Write SVG content to file, creating directories as needed."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    print(f"Generated {path}")


def main():
    # Scalable SVG
    path = os.path.join(DATA_DIR, "scalable", "apps", "bloom-terminal.svg")
    write_svg(path, generate_scalable_svg())

    # Symbolic SVG
    path = os.path.join(DATA_DIR, "symbolic", "apps", "bloom-terminal-symbolic.svg")
    write_svg(path, generate_symbolic_svg())

    # 256x256 PNG (for SDL_SetWindowIcon fallback — SDL can't load SVG)
    out_dir = os.path.join(DATA_DIR, "256x256", "apps")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "bloom-terminal.png")
    img = generate_png(512, 256)
    img.save(out_path, "PNG")
    print(f"Generated {out_path} (256x256)")


if __name__ == "__main__":
    main()
