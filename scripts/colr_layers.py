#!/usr/bin/env python3
"""Export individual COLR v1 layers as PNG files."""

import sys
from fontTools.ttLib import TTFont
from fontTools.misc.arrayTools import scaleRect, intRect
from blackrenderer.font import BlackRendererFont
from blackrenderer.backends import getSurfaceClass

FONT_PATH = "/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"
FONT_SIZE = 128


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} TEXT OUTPUT_PREFIX")
        sys.exit(1)

    text = sys.argv[1]
    prefix = sys.argv[2]

    # Get surface class (prefer skia, fall back to cairo)
    SurfaceClass = getSurfaceClass("skia", ".png")
    if SurfaceClass is None:
        SurfaceClass = getSurfaceClass("cairo", ".png")
    if SurfaceClass is None:
        print("Error: No suitable backend (skia or cairo) available")
        sys.exit(1)

    # Load font
    font = BlackRendererFont(FONT_PATH)
    ttfont = TTFont(FONT_PATH)
    cmap = ttfont.getBestCmap()
    colr = ttfont["COLR"]

    for char in text:
        cp = ord(char)
        glyph_name = cmap.get(cp)
        if not glyph_name:
            print(f"No glyph for U+{cp:04X}")
            continue

        # Find base glyph paint record
        found = False
        for rec in colr.table.BaseGlyphList.BaseGlyphPaintRecord:
            if rec.BaseGlyph == glyph_name:
                paint = rec.Paint
                if paint.Format == 1:  # PaintColrLayers
                    export_layers(font, ttfont, colr, paint, prefix, glyph_name, SurfaceClass)
                else:
                    print(f"Glyph {glyph_name} uses paint format {paint.Format}, not PaintColrLayers")
                found = True
                break
        if not found:
            print(f"No COLR entry for glyph {glyph_name}")


def export_layers(font, ttfont, colr, paint, prefix, glyph_name, SurfaceClass):
    """Export each layer of a PaintColrLayers glyph."""
    layer_list = colr.table.LayerList.Paint
    first_idx = paint.FirstLayerIndex
    num_layers = paint.NumLayers

    print(f"Glyph {glyph_name}: {num_layers} layers (starting at index {first_idx})")

    # Get glyph bounds
    scale_factor = FONT_SIZE / font.unitsPerEm
    bounds = font.getGlyphBounds(glyph_name)
    if bounds is None:
        bounds = (0, 0, font.unitsPerEm, font.unitsPerEm)

    # Scale and add margin
    margin = 10
    bounds = scaleRect(bounds, scale_factor, scale_factor)
    bounds = (bounds[0] - margin, bounds[1] - margin, bounds[2] + margin, bounds[3] + margin)
    bounds = intRect(bounds)

    # Setup font rendering state (mimics drawGlyph initialization)
    font.currentPalette = font.getPalette(0)
    font.textColor = (0, 0, 0, 1)
    font._recursionCheck = set()
    font.currentPath = None

    try:
        from fontTools.misc.transform import Identity
        font.currentTransform = Identity
    except ImportError:
        from fontTools.pens.t2CharStringPen import t2CharString
        font.currentTransform = (1, 0, 0, 1, 0, 0)

    for i in range(num_layers):
        # 1. Individual layer (just layer i alone)
        single_path = f"{prefix}_single{i:02d}.png"
        surface = SurfaceClass()
        with surface.canvas(bounds) as canvas:
            canvas.scale(scale_factor)
            lp = layer_list[first_idx + i]
            font._drawPaint(lp, canvas)
        surface.saveImage(single_path)
        print(f"  Layer {i} single: saved to {single_path}")

        # 2. Cumulative layers (layers 0 through i)
        accum_path = f"{prefix}_accum{i:02d}.png"
        surface = SurfaceClass()
        with surface.canvas(bounds) as canvas:
            canvas.scale(scale_factor)
            for j in range(i + 1):
                lp = layer_list[first_idx + j]
                font._drawPaint(lp, canvas)
        surface.saveImage(accum_path)
        print(f"  Layer {i} accum:  saved to {accum_path}")


if __name__ == "__main__":
    main()
