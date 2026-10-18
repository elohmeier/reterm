#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11"]
# ///
"""Render text with the generated E1001 UI fonts, the way the firmware does.

Reads the C headers in firmware/e1001/src/fonts/ (bitmaps, glyph table,
kerning pairs) rather than re-rendering the TrueType sources, so the PNG
shows exactly what the panel will: Adafruit GFX glyph placement plus the
UI's kerning. Also checks that every sample's lowercase x-height letters
share one baseline.

  uv run tools/preview-gfx-fonts.py /tmp/fonts.png
"""

import re
import sys
from pathlib import Path

from PIL import Image

FONTS = Path(__file__).resolve().parents[1] / "firmware/e1001/src/fonts"
SAMPLES = {
    "UiTitle": ["Menü", "Einstellungen", "Status", "Ansicht", "Spiele"],
    "UiBold": ["Jetzt aktualisieren", "Bildschirm auffrischen", "Töne", "Status-LED"],
    "UiBody": ["Foto senden", "Neu starten", "WLAN vergessen", "Nächstes Aufwachen",
               "Tagliatelle · Vorwärts · Wochenübersicht", "hf40-iot  ·  -28 dBm  ·  192.168.20.38"],
    "UiSmall": ["zuletzt heute 19:30", "Grün: öffnen  ·  Grün halten: zurück", "Akku 47 %  ·  3,80 V"],
}


def load(symbol: str):
    src = (FONTS / f"{symbol}.h").read_text()
    bitmaps = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-F]{2})",
                    src.split("Bitmaps[] PROGMEM = {")[1].split("};")[0]))
    glyphs = [tuple(map(int, g)) for g in re.findall(
        r"\{(\d+), (\d+), (\d+), (\d+), (-?\d+), (-?\d+)\}",
        src.split("Glyphs[] PROGMEM = {")[1].split("};")[0])]
    kern = {(int(a), int(b)): int(k) for a, b, k in re.findall(
        r"\{(\d+), (\d+), (-?\d+)\}", src.split("Kerning[] PROGMEM = {")[1].split("};")[0])}
    first = int(re.search(r"0x([0-9A-F]{2}), 0x([0-9A-F]{2}), (\d+)\}", src).group(1), 16)
    y_advance = int(re.search(r"0x[0-9A-F]{2}, 0x[0-9A-F]{2}, (\d+)\}", src).group(1))
    return bitmaps, glyphs, kern, first, y_advance


def draw(img, font, x, baseline, text, marks=None):
    """Adafruit_GFX::drawChar for custom fonts, plus the UI's kerning."""
    bitmaps, glyphs, kern, first, _ = font
    prev = None
    for ch in text.encode("latin-1", "replace"):
        if prev is not None:
            x += kern.get((prev, ch), 0)
        offset, w, h, adv, xo, yo = glyphs[ch - first]
        bit = 0
        for yy in range(h):
            for xx in range(w):
                if bitmaps[offset + (bit >> 3)] & (0x80 >> (bit & 7)):
                    img.putpixel((x + xo + xx, baseline + yo + yy), 0)
                bit += 1
        if marks is not None and chr(ch) in "acemnorsuvwxz" and h:
            marks.add(yo + h)
        x += adv
        prev = ch
    return x


def main() -> None:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/ui-fonts.png")
    img = Image.new("1", (800, 600), 1)
    y, failures = 8, []
    for symbol, lines in SAMPLES.items():
        font = load(symbol)
        for text in lines:
            y += font[4]
            marks = set()
            draw(img, font, 16, y, text, marks)
            if len(marks) > 1:
                failures.append(f"{symbol} {text!r}: baselines {sorted(marks)}")
        y += 6
    img.crop((0, 0, 800, y + 8)).save(out)
    print(out)
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    main()
