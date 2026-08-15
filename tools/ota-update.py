#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///

"""Push a firmware image to a reTerminal E1001 or E1004 over Wi-Fi.

Two modes:

- Session mode (no serial access needed): pass --address and --token copied
  from a live upload session's QR URL. Works against release firmware.
- Fixture mode (default): drives the UART test-session handshake, which only
  release-excluded RETERM_UPLOAD_FIXTURE builds accept. Used for end-to-end
  testing from the workbench.

The firmware streams the image into the inactive OTA slot, validates it,
flips otadata, and reboots. USB flashing via tools/send-image.py erases
otadata first, so it always wins over a previous OTA.
"""

from __future__ import annotations

import argparse
import http.client
import sys
import time
from pathlib import Path

BAUD = 921600
WEB_MAGIC = b"E1WEB001"
TEST_TOKEN = "0123456789abcdef0123456789abcdef"
REPO = Path(__file__).resolve().parents[1]

FIRMWARES = {
    "e1004": REPO / "firmware/e1004/.pio/build/reterminal-e1004/firmware.bin",
    "e1001": REPO / "firmware/e1001/.pio/build/reterminal-e1001/firmware.bin",
}


def post_firmware(address: str, token: str, firmware: bytes) -> None:
    origin = f"http://{address}"
    path = f"/api/firmware/{token}"
    print(f"uploading {len(firmware):,} firmware bytes to {origin}{path}")
    connection = http.client.HTTPConnection(address, 80, timeout=300)
    try:
        connection.request(
            "POST",
            path,
            body=firmware,
            headers={
                "Content-Type": "application/octet-stream",
                "Origin": origin,
            },
        )
        response = connection.getresponse()
        body = response.read().decode("utf-8", "replace")
        print(f"HTTP {response.status}: {body}")
        if response.status != 202:
            raise RuntimeError(f"firmware upload failed with status {response.status}")
    finally:
        connection.close()


def fixture_session(port: str, firmware: bytes) -> None:
    import serial

    deadline = time.monotonic() + 120
    device = serial.Serial(port, BAUD, timeout=0.25, write_timeout=10)
    try:
        # CH341 DTR/RTS wiring: pulse reset so the receiver starts fresh.
        device.dtr = False
        device.rts = True
        time.sleep(0.1)
        device.rts = False
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            try:
                text = line.decode("ascii").strip()
            except UnicodeDecodeError:
                continue  # ROM boot output uses a different baud rate.
            print(f"device: {text}")
            if text.startswith("READY E1"):
                break
        else:
            raise RuntimeError("device did not announce image receiver")

        handshake = WEB_MAGIC + TEST_TOKEN.encode("ascii")
        if device.write(handshake) != len(handshake):
            raise RuntimeError("short write while starting HTTP test session")
        device.flush()

        address: str | None = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            text = line.decode("utf-8", "replace").strip()
            print(f"device: {text}")
            if text.startswith("UART fixture disabled"):
                raise RuntimeError(
                    "device firmware was built without RETERM_UPLOAD_FIXTURE; "
                    "use --address/--token from a live session QR instead")
            if text.startswith("UPLOAD API http://"):
                address = text.removeprefix("UPLOAD API http://")
                break
        if address is None:
            raise RuntimeError("device did not announce the HTTP upload API")

        post_firmware(address, TEST_TOKEN, firmware)

        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            text = line.decode("utf-8", "replace").strip()
            print(f"device: {text}")
            if text.startswith("READY E1"):
                print("new firmware booted")
                return
        raise RuntimeError("timed out waiting for the updated firmware to boot")
    finally:
        device.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, nargs="?",
                        help="firmware.bin (default: the device's pio build output)")
    parser.add_argument("--device", choices=sorted(FIRMWARES), default="e1004",
                        help="target frame model (default: %(default)s)")
    parser.add_argument("--port", default="/dev/ttyUSB0",
                        help="serial port for fixture mode")
    parser.add_argument("--address", help="device IP from a live session QR")
    parser.add_argument("--token", help="session token from a live session QR")
    args = parser.parse_args()

    if bool(args.address) != bool(args.token):
        parser.error("--address and --token must be used together")

    firmware_path = args.firmware or FIRMWARES[args.device]
    if not firmware_path.is_file():
        raise RuntimeError(f"{firmware_path} not found; build the firmware first")
    firmware = firmware_path.read_bytes()
    print(f"firmware image: {firmware_path} ({len(firmware):,} bytes)")

    if args.address:
        post_firmware(args.address, args.token, firmware)
        print("device accepted the update and is rebooting")
    else:
        fixture_session(args.port, firmware)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
