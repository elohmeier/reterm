#!/usr/bin/env python3
"""Fail when site/src/lib/devices.ts diverges from the measured E1004 profile.

The editor's E1004 pigment palette is a set of literals derived from
profiles/e1004-IMG_5327/profile.json with linear-light black-point
compensation (the same algorithm as tools/send-image.py:load_profile_colors).
A recalibration that regenerates the profile without updating devices.ts
would silently desynchronize the wire palette; this stdlib-only check runs in
CI to catch that.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PROFILE = REPO / "profiles" / "e1004-IMG_5327" / "profile.json"
DEVICES_TS = REPO / "site" / "src" / "lib" / "devices.ts"


def profile_colors(path: Path) -> list[tuple[int, int, int]]:
    profile = json.loads(path.read_text())
    entries = sorted(profile["palette"], key=lambda entry: entry["index"])
    if [entry["index"] for entry in entries] != list(range(6)):
        raise SystemExit("profile does not contain palette indices 0 through 5")

    measured = [entry["measured"]["linear_srgb"] for entry in entries]
    black, white = measured[0], measured[1]
    scales = [white[channel] - black[channel] for channel in range(3)]
    if any(scale <= 0 for scale in scales):
        raise SystemExit("profile has invalid black/white endpoints")

    def encode(linear: float) -> int:
        linear = max(0.0, min(1.0, linear))
        encoded = (12.92 * linear if linear <= 0.0031308
                   else 1.055 * linear ** (1 / 2.4) - 0.055)
        return round(encoded * 255)

    return [
        tuple(encode((color[channel] - black[channel]) / scales[channel])
              for channel in range(3))
        for color in measured
    ]


def editor_colors(path: Path) -> list[tuple[int, int, int]]:
    source = path.read_text()
    match = re.search(r"const E1004[^=]*=.*?pigments:\s*\[(.*?)\],\s*color:",
                      source, re.S)
    if not match:
        raise SystemExit("could not locate the E1004 pigments block in devices.ts")
    pigments = re.findall(
        r"rgb:\s*\[(\d+),\s*(\d+),\s*(\d+)\],\s*hex:\s*'#([0-9a-fA-F]{6})'",
        match.group(1))
    if len(pigments) != 6:
        raise SystemExit(
            f"expected 6 E1004 pigments in devices.ts, found {len(pigments)}")
    colors = []
    for r, g, b, hexvalue in pigments:
        rgb = (int(r), int(g), int(b))
        from_hex = tuple(int(hexvalue[i:i + 2], 16) for i in (0, 2, 4))
        if rgb != from_hex:
            raise SystemExit(
                f"devices.ts pigment rgb {rgb} does not match hex #{hexvalue}")
        colors.append(rgb)
    return colors


def main() -> int:
    expected = profile_colors(PROFILE)
    actual = editor_colors(DEVICES_TS)
    if expected != actual:
        print("E1004 palette mismatch between profile and editor:", file=sys.stderr)
        for index, (want, have) in enumerate(zip(expected, actual)):
            marker = "  " if want == have else "->"
            print(f"{marker} index {index}: profile {want} devices.ts {have}",
                  file=sys.stderr)
        print(f"regenerate the literals in {DEVICES_TS} from {PROFILE}",
              file=sys.stderr)
        return 1
    print(f"palette in sync: {len(expected)} pigments match {PROFILE.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
