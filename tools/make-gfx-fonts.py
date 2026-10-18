#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["freetype-py>=2.5", "uharfbuzz>=0.45"]
# ///
"""Generate the E1001 menu's Adafruit GFX fonts from TrueType sources.

FreeType renders each glyph with its hinted monochrome rasterizer
(FT_LOAD_TARGET_MONO) and supplies the exact integer metrics (bitmap_left,
bitmap_top, hinted advance), as Adafruit's fontconvert does, so every glyph
sits exactly on the baseline. HarfBuzz picks the glyph for each character
under the font's variation settings (Fraunces needs its axis-dependent
substitutions, or b and h fall back to wonky forms) and measures kerning
pairs, which the firmware's text() applies; GFX fonts alone have none.

The range is Latin-1 (0x20-0xFF without the C1 controls); firmware strings
go through a UTF-8 to Latin-1 helper. Pixel sizes are chosen where hinting
keeps the x-height uniform (DejaVu splits at 17 and 19 px); the generator
refuses fonts whose x-height letters disagree on baseline or top.

  uv run tools/make-gfx-fonts.py   # writes firmware/e1001/src/fonts/*.h
"""

from pathlib import Path

import freetype
import uharfbuzz as hb

REPO = Path(__file__).resolve().parents[1]
SOURCES = REPO / "tools/fonts"
OUT = REPO / "firmware/e1001/src/fonts"
# (symbol, file, pixel size, variation overrides)
FONTS = [
    ("UiSmall", "DejaVuSansCondensed.ttf", 13, {}),
    ("UiBody", "DejaVuSansCondensed.ttf", 18, {}),
    ("UiBold", "DejaVuSansCondensed-Bold.ttf", 18, {}),
    # The default instance (optical size 9, Black, no softness) matches the
    # calendar; only the wonky alternates go.
    ("UiTitle", "Fraunces.ttf", 33, {"WONK": 0}),
]
FIRST, LAST = 0x20, 0xFF
# Characters kerning is measured between: what the UI's German text uses.
KERN_CHARS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
              "ÄÖÜäöüß0123456789.,:;-–'\"()/%°·")
X_HEIGHT = "acemnorsuvwxz"


class Font:
    def __init__(self, name: str, px: int, variations: dict[str, float]):
        path = str(SOURCES / name)
        self.px = px
        self.ft = freetype.Face(path)
        self.ft.set_pixel_sizes(0, px)
        blob = hb.Blob.from_file_path(path)
        self.hb_face = hb.Face(blob)
        self.hb = hb.Font(self.hb_face)
        self.upem = self.hb_face.upem
        if variations or self.ft.has_multiple_masters:
            axes = self.ft.get_variation_info().axes
            coords = [variations.get(a.tag, a.default) for a in axes]
            self.ft.set_var_design_coords(coords)
            self.hb.set_variations({a.tag: c for a, c in zip(axes, coords)})

    def shape(self, text: str) -> tuple[list[int], list[int]]:
        """Glyph ids and advances (font units) for `text`, kerning applied."""
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.hb, buf, {"kern": True, "liga": False})
        return ([i.codepoint for i in buf.glyph_infos],
                [p.x_advance for p in buf.glyph_positions])

    def glyph(self, char: str):
        """(bits, width, height, xAdvance, xOffset, yOffset) for one character."""
        gids, _ = self.shape(char)
        self.ft.load_glyph(gids[0], freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = self.ft.glyph
        bitmap = g.bitmap
        bits = []
        for row in range(bitmap.rows):
            line = bitmap.buffer[row * bitmap.pitch:(row + 1) * bitmap.pitch]
            bits += [(line[x >> 3] >> (7 - (x & 7))) & 1 for x in range(bitmap.width)]
        return bits, bitmap.width, bitmap.rows, (g.advance.x + 32) >> 6, g.bitmap_left, -g.bitmap_top

    def kerning(self) -> list[tuple[int, int, int]]:
        """(left, right, pixels) for pairs whose kerning rounds to a pixel."""
        pairs = []
        single = {c: self.shape(c)[1][0] for c in KERN_CHARS}
        for a in KERN_CHARS:
            for b in KERN_CHARS:
                _, advances = self.shape(a + b)
                delta = advances[0] - single[a]
                pixels = round(delta * self.px / self.upem)
                if pixels:
                    pairs.append((ord(a), ord(b), pixels))
        return sorted(pairs)

    def check(self, symbol: str) -> None:
        bottoms, tops = set(), set()
        for c in X_HEIGHT:
            _, _, h, _, _, y = self.glyph(c)
            bottoms.add(y + h)
            tops.add(y)
        if len(bottoms) != 1 or len(tops) != 1:
            raise SystemExit(f"{symbol}: x-height letters disagree (bottoms {bottoms}, "
                             f"tops {tops}); pick another pixel size")


def emit(symbol: str, font: Font) -> str:
    data, glyphs = bytearray(), []
    for code in range(FIRST, LAST + 1):
        if 0x7F <= code <= 0x9F:
            glyphs.append((len(data), 0, 0, 0, 0, 0))
            continue
        bits, w, h, adv, xo, yo = font.glyph(chr(code))
        offset = len(data)
        for i in range(0, len(bits), 8):
            chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
            data.append(sum(b << (7 - k) for k, b in enumerate(chunk)))
        glyphs.append((offset, w, h, adv, xo, yo))
    kern = [(a, b, k) for a, b, k in font.kerning() if a <= 0xFF and b <= 0xFF]
    y_advance = font.ft.size.height >> 6
    rows = ",\n".join("  " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
                      for i in range(0, len(data), 16))
    table = ",\n".join(f"  {{{o}, {w}, {h}, {a}, {x}, {y}}}" for o, w, h, a, x, y in glyphs)
    pairs = ",\n".join("  " + ", ".join(f"{{{a}, {b}, {k}}}" for a, b, k in kern[i:i + 6])
                       for i in range(0, len(kern), 6)) or "  {0, 0, 0}"
    return f"""// Generated by tools/make-gfx-fonts.py; do not edit.
#pragma once
#include "UiFont.h"

const uint8_t {symbol}Bitmaps[] PROGMEM = {{
{rows}
}};

const GFXglyph {symbol}Glyphs[] PROGMEM = {{
{table}
}};

const GFXfont {symbol}Gfx PROGMEM = {{(uint8_t *){symbol}Bitmaps, (GFXglyph *){symbol}Glyphs,
                                   0x{FIRST:02X}, 0x{LAST:02X}, {y_advance}}};

// Kerning pairs (Latin-1 left, right, pixels), sorted for binary search.
const UiKern {symbol}Kerning[] PROGMEM = {{
{pairs}
}};

const UiFont {symbol} = {{&{symbol}Gfx, {symbol}Kerning, {len(kern)}}};
"""


SHARED = """// Generated by tools/make-gfx-fonts.py; do not edit.
#pragma once
#include <Adafruit_GFX.h>

struct UiKern {
  uint8_t left;
  uint8_t right;
  int8_t pixels;
};

// A GFX font plus the kerning pairs GFX fonts cannot carry.
struct UiFont {
  const GFXfont *gfx;
  const UiKern *kerning;
  uint16_t kerningCount;
};
"""


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "UiFont.h").write_text(SHARED)
    for symbol, name, px, variations in FONTS:
        font = Font(name, px, variations)
        font.check(symbol)
        path = OUT / f"{symbol}.h"
        path.write_text(emit(symbol, font))
        print(f"{path.relative_to(REPO)}: {px} px, {len(font.kerning())} kerning pairs")


if __name__ == "__main__":
    main()
