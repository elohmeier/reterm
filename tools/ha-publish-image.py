#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11", "pillow-heif>=0.22", "pyserial>=3.5", "paho-mqtt>=2"]
# ///

"""Queue an image for a reTerminal frame through Home Assistant's MQTT broker.

Packs the image into the device wire format (reusing send-image.py's pipeline),
writes it where an HTTP server can hand it out, and publishes a retained
command to reterm/<device-id>/cmd. The deep-sleeping device fetches and
displays the image on its next timer wake and verifies the sha256 before
refreshing the panel.

Typical use against Home Assistant's /local (www) directory:

  ./tools/ha-publish-image.py photo.heic --device e1004 \\
      --device-id reterm-e1004-a1b2c3 \\
      --copy-to root@homehub.hf40.de:/var/lib/homeassistant/www/reterm/frame.bin \\
      --url http://homehub.hf40.de:8123/local/reterm/frame.bin \\
      --broker homehub.hf40.de --username reterm --password ...
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def load_send_image_module():
    path = Path(__file__).resolve().parent / "send-image.py"
    spec = importlib.util.spec_from_file_location("send_image", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--device", choices=("e1001", "e1004"), default="e1004",
                        help="target frame model (default: %(default)s)")
    parser.add_argument("--device-id", required=True,
                        help="MQTT device id, e.g. reterm-e1004-a1b2c3 "
                             "(shown in /api/status QR host name and HA)")
    parser.add_argument("--url", required=True,
                        help="http:// URL where the device will fetch the "
                             "packed image on its next wake")
    parser.add_argument("--output", type=Path,
                        help="write the packed framebuffer here "
                             "(default: temp file, useful with --copy-to)")
    parser.add_argument("--copy-to",
                        help="scp destination for the packed file, e.g. "
                             "root@homehub:/var/lib/homeassistant/www/reterm/frame.bin")
    parser.add_argument("--broker", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--fit", choices=("cover", "contain"), default="cover")
    parser.add_argument("--tone-gamma", type=float, default=1.0)
    parser.add_argument("--nominal-palette", action="store_true",
                        help="disable calibration for index-exact test assets")
    args = parser.parse_args()

    send_image = load_send_image_module()
    device = send_image.DEVICES[args.device]
    if args.device == "e1001":
        colors = send_image.BW_COLORS
        tone_gamma = args.tone_gamma
    else:
        colors = (send_image.NOMINAL_COLORS if args.nominal_palette
                  else send_image.load_profile_colors(send_image.DEFAULT_PROFILE))
        tone_gamma = 1.0 if args.nominal_palette else args.tone_gamma

    packed, _preview, rotated = send_image.prepare(
        args.image, args.fit, colors, tone_gamma,
        device["width"], device["height"], device["packing"])
    digest = hashlib.sha256(packed).hexdigest()
    print(f"packed {args.image}: {len(packed)} bytes, sha256={digest[:16]}…, "
          f"auto-rotated={'yes' if rotated else 'no'}")

    output = args.output or Path(tempfile.gettempdir()) / f"reterm-{args.device}-frame.bin"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(packed)
    print(f"wrote {output}")

    if args.copy_to:
        # scp needs the remote directory to exist; mkdir is the caller's job.
        subprocess.run(["scp", "-q", str(output), args.copy_to], check=True)
        print(f"copied to {args.copy_to}")

    command = {
        "action": "image",
        # The device skips a retained command whose id it already ran; the
        # timestamp suffix lets the same image be re-sent deliberately.
        "id": f"{digest[:12]}-{int(time.time())}",
        "url": args.url,
        "sha256": digest,
    }
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.connect(args.broker, args.mqtt_port, keepalive=15)
    info = client.publish(f"reterm/{args.device_id}/cmd",
                          json.dumps(command), qos=1, retain=True)
    info.wait_for_publish(timeout=10)
    client.disconnect()
    print(f"queued retained command on reterm/{args.device_id}/cmd; "
          "the frame updates on its next wake")
    return 0


if __name__ == "__main__":
    sys.exit(main())
