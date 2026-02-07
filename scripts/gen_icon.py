#!/usr/bin/env python3
"""Generate bloom-terminal app icons (>_ prompt, Charm/Bubbletea colors).

Requires: pip install Pillow

Usage:
    python3 scripts/gen_icon.py

Outputs:
    data/icons/hicolor/256x256/apps/bloom-terminal.png
    data/icons/hicolor/48x48/apps/bloom-terminal.png
"""

import os
from PIL import Image, ImageDraw, ImageFont

# Charmbracelet / Bubbletea brand colors
CHARM_PINK = (0xFF, 0x5F, 0xAF, 255)     # #FF5FAF
CHARM_PURPLE = (0x75, 0x71, 0xF9, 255)   # #7571F9
BG_COLOR = (0x00, 0x00, 0x00, 255)       # Pitch black

NOTO_MONO = "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)


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


def generate_icon(render_size, output_size):
    """Generate icon at render_size, then downscale to output_size."""
    img = Image.new("RGBA", (render_size, render_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Draw rounded rectangle terminal background
    margin = int(render_size * 0.06)
    radius = int(render_size * 0.12)
    rounded_rectangle(draw, (margin, margin, render_size - margin,
                             render_size - margin), radius, BG_COLOR)

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
        color = lerp_color(CHARM_PINK, CHARM_PURPLE, t)
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


def main():
    # Render at 512px for quality, downscale to target sizes
    # Only 256x256 PNG needed (for SDL_SetWindowIcon fallback).
    # Compositor/desktop uses the SVG in scalable/ instead.
    sizes = {256: "256x256"}

    for size, dirname in sizes.items():
        out_dir = os.path.join(PROJECT_DIR, "data", "icons", "hicolor",
                               dirname, "apps")
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, "bloom-terminal.png")

        img = generate_icon(512, size)
        img.save(out_path, "PNG")
        print(f"Generated {out_path} ({size}x{size})")


if __name__ == "__main__":
    main()
