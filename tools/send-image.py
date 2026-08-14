#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11", "pillow-heif>=0.22", "pyserial>=3.5"]
# ///

"""Prepare, dither, and send a full-screen image to a reTerminal E1004."""

from __future__ import annotations

import argparse
import math
import subprocess
import sys
import time
from pathlib import Path

import serial
from PIL import Image, ImageOps
from pillow_heif import register_heif_opener

WIDTH = 1200
HEIGHT = 1600
BAUD = 921600
MAGIC = b"E1IMG001"
REPO = Path(__file__).resolve().parents[1]

# Order is the 3-bit GxEPD2 encoding consumed by the custom panel driver.
COLORS = [
    (0, 0, 0),        # black
    (255, 255, 255),  # white
    (0, 145, 70),     # green
    (0, 75, 190),     # blue
    (210, 30, 40),    # red
    (245, 205, 30),   # yellow
]


def orientation_error(size: tuple[int, int]) -> float:
    return abs(math.log((size[0] / size[1]) / (WIDTH / HEIGHT)))


def prepare(path: Path, fit: str) -> tuple[bytes, Image.Image, bool]:
    register_heif_opener()
    with Image.open(path) as opened:
        image = ImageOps.exif_transpose(opened).convert("RGB")

    rotated = orientation_error((image.height, image.width)) < orientation_error(image.size)
    if rotated:
        image = image.transpose(Image.Transpose.ROTATE_90)

    method = ImageOps.fit if fit == "cover" else ImageOps.contain
    if fit == "cover":
        image = method(image, (WIDTH, HEIGHT), Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    else:
        contained = method(image, (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        image = Image.new("RGB", (WIDTH, HEIGHT), "white")
        image.paste(contained, ((WIDTH - contained.width) // 2,
                                (HEIGHT - contained.height) // 2))

    palette = Image.new("P", (1, 1))
    flat = [component for color in COLORS for component in color]
    palette.putpalette(flat + [0] * (768 - len(flat)))
    indexed = image.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    pixels = indexed.tobytes()
    if max(pixels) >= len(COLORS):
        raise RuntimeError("quantizer emitted an unexpected palette index")
    packed = bytearray(WIDTH * HEIGHT // 2)
    for pos in range(0, len(pixels), 2):
        packed[pos // 2] = (pixels[pos] << 4) | pixels[pos + 1]
    return bytes(packed), indexed.convert("RGB"), rotated


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=REPO, check=True)


def wait_for_ready(port: str) -> serial.Serial:
    deadline = time.monotonic() + 20
    device = serial.Serial(port, BAUD, timeout=0.25, write_timeout=10)
    # CH341 DTR/RTS wiring: pulse reset so --no-flash starts a fresh receiver.
    device.dtr = False
    device.rts = True
    time.sleep(0.1)
    device.rts = False
    while time.monotonic() < deadline:
        line = device.readline()
        if line:
            try:
                text = line.decode("ascii").strip()
            except UnicodeDecodeError:
                continue  # ROM boot output uses a different baud rate.
            print(f"device: {text}")
            if text.startswith("READY E1004"):
                return device
    device.close()
    raise RuntimeError("device did not announce image receiver")


def send(port: str, packed: bytes) -> None:
    device = wait_for_ready(port)
    try:
        device.write(MAGIC)
        for offset in range(0, len(packed), 16384):
            device.write(packed[offset:offset + 16384])
        device.flush()
        print(f"sent {len(packed):,} packed image bytes")

        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            text = line.decode("utf-8", "replace").strip()
            print(f"device: {text}")
            if text == "IMAGE DISPLAYED":
                return
            if text.startswith("IMAGE ERROR"):
                raise RuntimeError(text)
        raise RuntimeError("timed out waiting for panel refresh")
    finally:
        device.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--fit", choices=("cover", "contain"), default="cover")
    parser.add_argument("--preview", type=Path,
                        default=Path("/root/.cache/reterm-e1004-preview.png"))
    parser.add_argument("--no-flash", action="store_true",
                        help="reuse image-receiver firmware already on the device")
    parser.add_argument("--prepare-only", action="store_true",
                        help="write the dithered preview without contacting the device")
    args = parser.parse_args()

    packed, preview, rotated = prepare(args.image, args.fit)
    args.preview.parent.mkdir(parents=True, exist_ok=True)
    preview.save(args.preview)
    print(f"prepared {args.image}: 1200x1600, fit={args.fit}, "
          f"auto-rotated={'yes' if rotated else 'no'}")
    print(f"dithered preview: {args.preview}")

    if args.prepare_only:
        return 0

    if not args.no_flash:
        run(["sh", "tools/build-container.sh", "firmware/e1004"])
        run(["uvx", "--from", "esptool", "esptool", "--chip", "esp32s3",
             "--port", args.port, "--baud", "460800", "write-flash", "0x90000",
             "firmware/e1004/.pio/build/reterminal-e1004/firmware.bin"])
    send(args.port, packed)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
