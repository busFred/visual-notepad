#!/usr/bin/env python3
"""Writes resources/fonts/VisualNotepadDepth.ttf, the font that gives a formula
its depth below the baseline (see src/markdown/MathObject.h).

Qt makes a line deeper below the baseline only through the font of the
characters in it: an inline object aligned to the baseline reaches its
character's font descent below it (QTextDocumentLayout::resizeInlineObject),
and the font's leading joins the line's. Text fonts are no measure for that:
Calibri and Cambria at most small sizes come from bitmaps measured as no
descent at all, their leading may be negative, and a larger size jumps by
pixels. This font has nothing to show and the same measure everywhere: no
ascent, a descent of one em, no leading. At a pixel size of N it reaches N
pixels below the baseline.

Needs fontTools (pip install fonttools). Run from the repository root:
  python3 scripts/make_depth_font.py
"""

import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

FAMILY = "Visual Notepad Depth"
EM = 1000


def main():
    builder = FontBuilder(EM, isTTF=True)
    glyphs = [".notdef", "space"]
    builder.setupGlyphOrder(glyphs)
    # The object replacement character, which Qt gives inline objects, and a
    # space; both empty.
    builder.setupCharacterMap({0x20: "space", 0xFFFC: "space"})
    empty = TTGlyphPen(None).glyph()
    builder.setupGlyf({name: empty for name in glyphs})
    builder.setupHorizontalMetrics({name: (0, 0) for name in glyphs})
    # The same measure in every table a system may read it from.
    builder.setupHorizontalHeader(ascent=0, descent=-EM, lineGap=0)
    builder.setupOS2(
        version=4,
        sTypoAscender=0,
        sTypoDescender=-EM,
        sTypoLineGap=0,
        usWinAscent=0,
        usWinDescent=EM,
        sxHeight=0,
        sCapHeight=0,
        fsSelection=0x40 | 0x80,  # REGULAR, USE_TYPO_METRICS
        achVendID="VNPD",
    )
    builder.setupNameTable({
        "familyName": FAMILY,
        "styleName": "Regular",
        "uniqueFontIdentifier": FAMILY + " Regular",
        "fullName": FAMILY,
        "psName": "VisualNotepadDepth-Regular",
        "version": "Version 1.000",
    })
    builder.setupPost()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "resources", "fonts", "VisualNotepadDepth.ttf")
    builder.save(path)
    print("wrote", path, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    main()
