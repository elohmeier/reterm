#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11"]
# ///

"""Generate the indexed E1004 target used for camera color calibration."""

from __future__ import annotations

import argparse
import json
from itertools import combinations
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH = 1200
HEIGHT = 1600
TARGET_ID = "E1004-COLOR-V1"

# These RGB values identify the six panel indices used by send-image.py. The
# calibration measures their actual appearance; the values are not claims
# about the pigments' colorimetry.
COLORS = [
    ("black", (0, 0, 0)),
    ("white", (255, 255, 255)),
    ("green", (0, 145, 70)),
    ("blue", (0, 75, 190)),
    ("red", (210, 30, 40)),
    ("yellow", (245, 205, 30)),
]

# An 8x8 Bayer matrix gives every 1/8 step exactly eight pixels of the second
# ink per 64-pixel tile. Averaging a photographed patch therefore measures a
# known physical mixture without relying on the host's image quantizer.
BAYER_8 = (
    (0, 48, 12, 60, 3, 51, 15, 63),
    (32, 16, 44, 28, 35, 19, 47, 31),
    (8, 56, 4, 52, 11, 59, 7, 55),
    (40, 24, 36, 20, 43, 27, 39, 23),
    (2, 50, 14, 62, 1, 49, 13, 61),
    (34, 18, 46, 30, 33, 17, 45, 29),
    (10, 58, 6, 54, 9, 57, 5, 53),
    (42, 26, 38, 22, 41, 25, 37, 21),
)


def font(size: int, *, bold: bool = False) -> ImageFont.ImageFont:
    family = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    try:
        return ImageFont.truetype(family, size)
    except OSError:
        return ImageFont.load_default()


def centered_text(
    draw: ImageDraw.ImageDraw,
    bounds: tuple[int, int, int, int],
    text: str,
    text_font: ImageFont.ImageFont,
    fill: int = 0,
) -> None:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=text_font)
    width = right - left
    height = bottom - top
    x = bounds[0] + (bounds[2] - bounds[0] - width) // 2 - left
    y = bounds[1] + (bounds[3] - bounds[1] - height) // 2 - top
    draw.text((x, y), text, font=text_font, fill=fill)


def fill_mixture(
    pixels: Image.PixelAccess,
    bounds: tuple[int, int, int, int],
    first: int,
    second: int,
    second_eighths: int,
) -> None:
    x0, y0, x1, y1 = bounds
    threshold = second_eighths * 8
    for y in range(y0, y1):
        row = BAYER_8[y & 7]
        for x in range(x0, x1):
            pixels[x, y] = second if row[x & 7] < threshold else first


def add_patch(
    image: Image.Image,
    draw: ImageDraw.ImageDraw,
    bounds: tuple[int, int, int, int],
    first: int,
    second: int,
    second_eighths: int,
) -> dict[str, object]:
    x0, y0, x1, y1 = bounds
    draw.rectangle((x0, y0, x1 - 1, y1 - 1), fill=0)
    inner = (x0 + 3, y0 + 3, x1 - 3, y1 - 3)
    fill_mixture(image.load(), inner, first, second, second_eighths)
    sample = (x0 + 11, y0 + 11, x1 - 11, y1 - 11)
    return {
        "rect": [x0, y0, x1 - x0, y1 - y0],
        "sample_rect": [
            sample[0], sample[1], sample[2] - sample[0], sample[3] - sample[1]
        ],
        "first_index": first,
        "second_index": second,
        "second_fraction": second_eighths / 8,
    }


def draw_fiducial(
    draw: ImageDraw.ImageDraw, center: tuple[int, int], corner: str
) -> dict[str, object]:
    x, y = center
    # Three nested squares give a crisp, color-independent center after a
    # perspective warp. The top-left marker has an extra black center so a
    # 180-degree rotation can be resolved without recognizing any text/color.
    draw.rectangle((x - 36, y - 36, x + 35, y + 35), fill=0)
    draw.rectangle((x - 25, y - 25, x + 24, y + 24), fill=1)
    draw.rectangle((x - 12, y - 12, x + 11, y + 11), fill=0)
    if corner != "top_left":
        draw.rectangle((x - 5, y - 5, x + 4, y + 4), fill=1)
    return {"corner": corner, "center": [x, y], "outer_size": 72}


def generate() -> tuple[Image.Image, dict[str, object]]:
    palette = Image.new("P", (WIDTH, HEIGHT), color=1)
    flat = [component for _, rgb in COLORS for component in rgb]
    palette.putpalette(flat + [0] * (768 - len(flat)))
    draw = ImageDraw.Draw(palette)

    # A narrow physical border helps screen-edge detection while leaving a
    # broad white quiet area around every measurement patch.
    draw.rectangle((18, 18, WIDTH - 19, HEIGHT - 19), outline=0, width=5)
    fiducials = [
        draw_fiducial(draw, (62, 62), "top_left"),
        draw_fiducial(draw, (WIDTH - 63, 62), "top_right"),
        draw_fiducial(draw, (WIDTH - 63, HEIGHT - 63), "bottom_right"),
        draw_fiducial(draw, (62, HEIGHT - 63), "bottom_left"),
    ]

    centered_text(draw, (130, 28, WIDTH - 130, 76),
                  "E1004 CAMERA COLOR TARGET", font(31, bold=True))
    centered_text(draw, (130, 72, WIDTH - 130, 104),
                  f"{TARGET_ID}  |  native inks + ordered mixtures", font(18))

    solid_patches: list[dict[str, object]] = []
    solid_x = 103
    solid_y = 120
    solid_width = 155
    solid_gap = 17
    for index, (name, _) in enumerate(COLORS):
        x = solid_x + index * (solid_width + solid_gap)
        patch = add_patch(
            palette, draw, (x, solid_y, x + solid_width, solid_y + 104),
            index, index, 0
        )
        patch.update({"kind": "solid", "index": index, "name": name})
        solid_patches.append(patch)
        centered_text(draw, (x, solid_y + 106, x + solid_width, solid_y + 139),
                      name.upper(), font(17, bold=True))

    draw.text((48, 284), "NEUTRAL: BLACK -> WHITE", font=font(18, bold=True), fill=0)
    neutral_patches: list[dict[str, object]] = []
    neutral_x = 145
    neutral_y = 318
    neutral_width = 94
    neutral_gap = 12
    for step in range(9):
        x = neutral_x + step * (neutral_width + neutral_gap)
        patch = add_patch(
            palette, draw, (x, neutral_y, x + neutral_width, neutral_y + 88),
            0, 1, step
        )
        patch.update({"kind": "neutral", "step": step})
        neutral_patches.append(patch)
        centered_text(draw, (x, neutral_y + 90, x + neutral_width, neutral_y + 119),
                      f"{step}/8", font(15))

    matrix_y = 486
    row_height = 58
    row_gap = 4
    matrix_x = 150
    patch_width = 102
    patch_gap = 7
    draw.text((43, matrix_y - 35), "PAIR", font=font(16, bold=True), fill=0)
    for step in range(9):
        x = matrix_x + step * (patch_width + patch_gap)
        centered_text(draw, (x, matrix_y - 38, x + patch_width, matrix_y - 5),
                      f"{step}/8", font(15, bold=True))

    mixture_patches: list[dict[str, object]] = []
    for row, (first, second) in enumerate(combinations(range(len(COLORS)), 2)):
        y = matrix_y + row * (row_height + row_gap)
        codes = ("K", "W", "G", "B", "R", "Y")
        pair_label = f"{codes[first]}-{codes[second]}"
        centered_text(draw, (44, y, 137, y + row_height), pair_label,
                      font(17, bold=True))
        for step in range(9):
            x = matrix_x + step * (patch_width + patch_gap)
            patch = add_patch(
                palette, draw, (x, y, x + patch_width, y + row_height),
                first, second, step
            )
            patch.update({
                "kind": "pairwise",
                "pair": [COLORS[first][0], COLORS[second][0]],
                "step": step,
            })
            mixture_patches.append(patch)

    footer_y = matrix_y + 15 * (row_height + row_gap) + 8
    centered_text(draw, (130, footer_y, WIDTH - 130, footer_y + 34),
                  "Photograph the complete border and all four corner markers",
                  font(17, bold=True))

    manifest: dict[str, object] = {
        "schema": "reterm.e1004.color-target.v1",
        "target_id": TARGET_ID,
        "size": [WIDTH, HEIGHT],
        "palette": [
            {"index": index, "name": name, "source_rgb": list(rgb)}
            for index, (name, rgb) in enumerate(COLORS)
        ],
        "fiducials": fiducials,
        "patches": solid_patches + neutral_patches + mixture_patches,
        "dither": {
            "type": "bayer-8x8",
            "matrix": [list(row) for row in BAYER_8],
        },
    }
    return palette, manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=Path("/root/.cache/e1004-color-target.png"))
    parser.add_argument("--manifest", type=Path,
                        default=Path("/root/.cache/e1004-color-target.json"))
    args = parser.parse_args()

    image, manifest = generate()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output, optimize=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"target: {args.output}")
    print(f"manifest: {args.manifest}")
    print(f"patches: {len(manifest['patches'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
