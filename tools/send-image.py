#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11", "pillow-heif>=0.22", "pyserial>=3.5"]
# ///

"""Prepare, dither, and send a full-screen image to a reTerminal E1001 or E1004."""

from __future__ import annotations

import argparse
import http.client
import json
import math
import subprocess
import sys
import time
from pathlib import Path

import serial
from PIL import Image, ImageOps
from pillow_heif import register_heif_opener

BAUD = 921600
MAGIC = b"E1IMG001"
WEB_MAGIC = b"E1WEB001"
TEST_TOKEN = "0123456789abcdef0123456789abcdef"
REPO = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = REPO / "profiles" / "e1004-IMG_5327" / "profile.json"
DEFAULT_TONE_GAMMA = 1.0

# Per-device geometry and wire format. "4bpp" packs two palette indices per
# byte (first pixel in the high nibble); "1bpp" packs eight pixels per byte
# (bit set = white, MSB is the leftmost pixel).
DEVICES = {
    "e1004": {
        "ready": "READY E1004",
        "width": 1200,
        "height": 1600,
        "packing": "4bpp",
        "project": "firmware/e1004",
        "firmware": "firmware/e1004/.pio/build/reterminal-e1004/firmware.bin",
    },
    "e1001": {
        "ready": "READY E1001",
        "width": 800,
        "height": 480,
        "packing": "1bpp",
        "project": "firmware/e1001",
        "firmware": "firmware/e1001/.pio/build/reterminal-e1001/firmware.bin",
    },
}

# Nominal colors remain useful for index-exact calibration assets. Ordinary
# photos use the measured profile selected in main(). Order is the GxEPD2
# palette encoding consumed by the custom panel driver.
NOMINAL_COLORS = [
    (0, 0, 0),        # black
    (255, 255, 255),  # white
    (0, 145, 70),     # green
    (0, 75, 190),     # blue
    (210, 30, 40),    # red
    (245, 205, 30),   # yellow
]

BW_COLORS = [(0, 0, 0), (255, 255, 255)]


def orientation_error(size: tuple[int, int], width: int, height: int) -> float:
    return abs(math.log((size[0] / size[1]) / (width / height)))


def load_profile_colors(path: Path) -> list[tuple[int, int, int]]:
    profile = json.loads(path.read_text())
    entries = sorted(profile["palette"], key=lambda entry: entry["index"])
    if [entry["index"] for entry in entries] != list(range(6)):
        raise RuntimeError("color profile does not contain palette indices 0 through 5")

    # The raw measurement includes the photographed paper's elevated black
    # point. Using it directly as an input-space palette collapses most shadow
    # and midtone pixels into physical black. Normalize in linear light between
    # measured paper black and white, retaining the pigments' measured hue.
    measured = [entry["measured"]["linear_srgb"] for entry in entries]
    black, white = measured[0], measured[1]
    if any(len(color) != 3 for color in measured):
        raise RuntimeError("color profile contains an invalid linear sRGB palette")
    scales = [white[channel] - black[channel] for channel in range(3)]
    if any(scale <= 0 for scale in scales):
        raise RuntimeError("color profile has invalid black/white endpoints")

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


def prepare(
    path: Path,
    fit: str,
    colors: list[tuple[int, int, int]],
    tone_gamma: float,
    width: int,
    height: int,
    packing: str,
) -> tuple[bytes, Image.Image, bool]:
    register_heif_opener()
    with Image.open(path) as opened:
        image = ImageOps.exif_transpose(opened).convert("RGB")

    rotated = (orientation_error((image.height, image.width), width, height)
               < orientation_error(image.size, width, height))
    if rotated:
        image = image.transpose(Image.Transpose.ROTATE_90)

    method = ImageOps.fit if fit == "cover" else ImageOps.contain
    if fit == "cover":
        image = method(image, (width, height), Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    else:
        contained = method(image, (width, height), Image.Resampling.LANCZOS)
        image = Image.new("RGB", (width, height), "white")
        image.paste(contained, ((width - contained.width) // 2,
                                (height - contained.height) // 2))

    # Spectra photos tend to read darker than their on-screen source. Lift
    # shadows and midtones without moving black or white; values above one
    # brighten because the transfer exponent is the reciprocal of gamma.
    if tone_gamma != 1:
        exponent = 1 / tone_gamma
        channel_lut = [round(255 * (value / 255) ** exponent) for value in range(256)]
        image = image.point(channel_lut * 3)

    palette = Image.new("P", (1, 1))
    flat = [component for color in colors for component in color]
    # Pillow requires 256 palette entries and may select the padding during
    # quantization. Make every unused slot a duplicate of physical black, then
    # collapse those duplicate indices back to wire value zero below.
    padding = list(colors[0]) * (256 - len(colors))
    full_palette = flat + padding
    palette.putpalette(full_palette)
    indexed = image.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    quantized = indexed.tobytes()
    pixels = bytes(value if value < len(colors) else 0 for value in quantized)
    if pixels != quantized:
        indexed = Image.frombytes("P", indexed.size, pixels)
        indexed.putpalette(full_palette)
    if packing == "1bpp":
        packed = bytearray(width * height // 8)
        for pos, value in enumerate(pixels):
            if value == 1:  # palette index 1 is white; GxEPD2 uses 1 = white
                packed[pos // 8] |= 0x80 >> (pos & 7)
    else:
        packed = bytearray(width * height // 2)
        for pos in range(0, len(pixels), 2):
            packed[pos // 2] = (pixels[pos] << 4) | pixels[pos + 1]
    return bytes(packed), indexed.convert("RGB"), rotated


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=REPO, check=True)


def wait_for_ready(port: str, ready_prefix: str) -> serial.Serial:
    # First boot of persistence-enabled firmware may format the factory's
    # previously unused SPIFFS partition before announcing READY.
    deadline = time.monotonic() + 120
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
            if text.startswith(ready_prefix):
                return device
            if text.startswith("READY E1"):
                device.close()
                raise RuntimeError(
                    f"connected device announced {text.split()[1]}; "
                    "pass the matching --device")
    device.close()
    raise RuntimeError("device did not announce image receiver")


def send_uart(port: str, packed: bytes, ready_prefix: str) -> None:
    device = wait_for_ready(port, ready_prefix)
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


def device_line(device: serial.Serial) -> str | None:
    line = device.readline()
    if not line:
        return None
    text = line.decode("utf-8", "replace").strip()
    print(f"device: {text}")
    return text


def send_http(port: str, packed: bytes, ready_prefix: str) -> None:
    """Start a deterministic test session and upload over checksummed TCP."""
    device = wait_for_ready(port, ready_prefix)
    try:
        handshake = WEB_MAGIC + TEST_TOKEN.encode("ascii")
        if device.write(handshake) != len(handshake):
            raise RuntimeError("short write while starting HTTP test session")
        device.flush()

        address: str | None = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = device_line(device)
            if text and text.startswith("UPLOAD API http://"):
                address = text.removeprefix("UPLOAD API http://")
                break
        if address is None:
            raise RuntimeError("device did not announce the HTTP upload API")

        origin = f"http://{address}"
        path = f"/api/image/{TEST_TOKEN}"
        print(f"uploading {len(packed):,} bytes to {origin}{path}")
        connection = http.client.HTTPConnection(address, 80, timeout=120)
        try:
            connection.request(
                "POST",
                path,
                body=packed,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Origin": origin,
                },
            )
            response = connection.getresponse()
            body = response.read().decode("utf-8", "replace")
            print(f"HTTP {response.status}: {body}")
            if response.status != 202:
                raise RuntimeError(f"HTTP upload failed with status {response.status}")
        finally:
            connection.close()

        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            text = device_line(device)
            if text == "HTTP image displayed":
                return
        raise RuntimeError("timed out waiting for panel refresh")
    finally:
        device.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--device", choices=sorted(DEVICES), default="e1004",
                        help="target frame model (default: %(default)s)")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--fit", choices=("cover", "contain"), default="cover")
    parser.add_argument("--transport", choices=("uart", "http"), default="uart",
                        help="transfer via direct UART or a Wi-Fi test session")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE,
                        help="measured panel profile used for color matching")
    parser.add_argument("--nominal-palette", action="store_true",
                        help="disable calibration for index-exact test assets")
    parser.add_argument("--tone-gamma", type=float, default=DEFAULT_TONE_GAMMA,
                        help="photo tone lift; 1 disables it (default: %(default)s)")
    parser.add_argument("--preview", type=Path,
                        help="dithered preview path "
                             "(default: /root/.cache/reterm-<device>-preview.png)")
    parser.add_argument("--no-flash", action="store_true",
                        help="reuse image-receiver firmware already on the device")
    parser.add_argument("--prepare-only", action="store_true",
                        help="write the dithered preview without contacting the device")
    args = parser.parse_args()

    if args.tone_gamma <= 0:
        parser.error("--tone-gamma must be greater than zero")
    device = DEVICES[args.device]
    if args.device == "e1001":
        # The measured pigment profiles are E1004-specific; the monochrome
        # panel dithers against plain black and white.
        colors = BW_COLORS
        color_matching = "black/white"
        tone_gamma = args.tone_gamma
    else:
        colors = NOMINAL_COLORS if args.nominal_palette else load_profile_colors(args.profile)
        color_matching = ("nominal/index-exact" if args.nominal_palette
                          else str(args.profile))
        tone_gamma = 1.0 if args.nominal_palette else args.tone_gamma
    preview_path = args.preview or Path(
        f"/root/.cache/reterm-{args.device}-preview.png")
    packed, preview, rotated = prepare(
        args.image, args.fit, colors, tone_gamma,
        device["width"], device["height"], device["packing"])
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    preview.save(preview_path)
    print(f"prepared {args.image}: {device['width']}x{device['height']}, "
          f"fit={args.fit}, auto-rotated={'yes' if rotated else 'no'}")
    print(f"dithered preview: {preview_path}")
    print(f"color matching: {color_matching}")
    print(f"photo tone gamma: {tone_gamma:.2f}" +
          (" (disabled for index-exact asset)" if args.nominal_palette else ""))

    if args.prepare_only:
        return 0

    if not args.no_flash:
        run(["sh", "tools/build-container.sh", device["project"]])
        run(["uvx", "--from", "esptool", "esptool", "--chip", "esp32s3",
             "--port", args.port, "--baud", "460800", "write-flash", "0x90000",
             device["firmware"]])
    if args.transport == "http":
        send_http(args.port, packed, device["ready"])
    else:
        send_uart(args.port, packed, device["ready"])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
