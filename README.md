# reTerminal E1001 and E1004 firmware workbench

This repository is a reproducible starting point for custom firmware on the
Seeed Studio reTerminal E1001 and E1004. It contains:

- `firmware/lib/reterm`: the shared photo-frame runtime (UART protocol,
  QR-driven Wi-Fi upload session, provisioning, persistence, deep sleep);
- `firmware/e1001`: board support for the 7.5-inch monochrome E1001;
- `firmware/e1004`: board support for the 13.3-inch six-color E1004;
- `site`: the static Vite + Svelte photo editor, Wi-Fi upload app, and the
  browser firmware flasher (`flash.html`, ESP Web Tools over Web Serial);
- `profiles`: measured E1004 pigment data and synthetic reconstructions;
- `upstream/seeed-trmnl-1.8.10`: the official open-source firmware snapshot;
- `upstream/seeed-trmnl-e1004-1.8.10`: the E1004 firmware snapshot;
- `upstream/binaries/trmnl-1.8.10`: matching official release binaries; and
- `docs/upstream-analysis.md`: hardware, firmware, recovery, and flashing notes.

The boards appear as serial ports through CH341 UART bridges (E1001
`1a86:7523`, E1004 `1a86:7522`), typically `/dev/ttyUSB0`. Do not erase a
device before making a full flash backup.

## Build the firmware

```sh
sh tools/build-container.sh firmware/e1001
sh tools/build-container.sh firmware/e1004
```

Results land in `firmware/<device>/.pio/build/reterminal-<device>/firmware.bin`.
On hosts with a working native toolchain, `pio run -d firmware/<device>` works
too. See the analysis notes before attempting to flash.

## Flash from the browser

Every Pages deploy builds both firmwares and publishes them with ESP Web
Tools manifests, so a desktop Chrome or Edge can flash a USB-connected frame
directly at <https://elohmeier.github.io/reterm/flash.html> — no local
toolchain required. The page ships from the same commit as the editor, so the
flashed firmware always matches the deployed web assets.

## Send an image over USB

The image sender supports HEIC, JPEG, PNG, and other Pillow formats. It applies
EXIF orientation, automatically chooses the best 90-degree orientation,
center-crops and resizes to the panel, and Floyd-Steinberg dithers into the
panel's pigments — six inks at 1200x1600 on the E1004, black and white at
800x480 on the E1001.

```sh
sh tools/send-image.sh /path/to/photo.heic                    # E1004 (default)
sh tools/send-image.sh /path/to/photo.heic --device e1001     # E1001
```

The default `--fit cover` fills the screen. Use `--fit contain` to preserve the
entire image with white letterboxing. After the receiver firmware has been
installed once, skip rebuilding and reflashing it:

```sh
sh tools/send-image.sh /path/to/photo.jpg --no-flash
```

A dithered PNG preview is written to
`/root/.cache/reterm-<device>-preview.png` before the image is sent.
The E1004's default pigment mapping is derived from an iPhone ProRAW capture with
linear-light black-point compensation. See
[docs/color-calibration.md](docs/color-calibration.md) for the repeatable
target/capture workflow and the important distinction between reflective
measurements and input-space dithering colors.

## Wireless photo upload

Press a button on the sleeping frame (the touch controls on the E1004, the
green button on the E1001) to open a five-minute wireless upload session. On
first use, scan the displayed Wi-Fi QR and use the captive portal to save a
home network. The device then displays a one-time QR whose link carries the
session token and device model; the editor dithers for that panel and streams
the same packed framebuffer used by the UART tool — 960,000 bytes on the
E1004, 48,000 bytes on the E1001.

See [docs/wifi-upload.md](docs/wifi-upload.md) for the state machine, API,
security model, GitHub Pages deployment, and browser compatibility notes.

To change networks later, hold the E1001's green button for five seconds
through a wake, or call the authenticated `POST /api/wifi/forget` endpoint
during an open session; the next wake shows the captive portal again.

## E1001 arcade

The arcade menu is always one gesture away: wake the E1001 with either white
top button, or hold the green button for about a second — from sleep, from a
game, or while a QR upload session or the provisioning portal is on screen
(the session closes cleanly and the menu opens instead). A green tap still
starts the photo session directly, and holding green for five seconds
through a wake still forgets the saved Wi-Fi. The menu (left/right moves the
selection, green chooses) offers rock paper scissors against the hardware
RNG, Luna — a slow unicorn tamagotchi — a "send a photo" entry that starts
an upload session, and a shortcut back to the last photo. Luna ages with the Home Assistant timer wakes (one interval
per check-in), hatches after eight hours, and grows through foal, unicorn,
and celestial stages; the white buttons feed her and play with her, and two
days of starvation send her off in a whirlwind until a new egg is adopted.
Leaving her screen idle keeps her on the display, where each check-in
repaints her current state — uploading a new photo always takes the wall
back. Game scores and pet state persist in NVS across firmware updates.

The menu's BAD APPLE entry replays the last video delivered over MQTT. The
E1001 plays RTV1 streams produced by `tools/encode-video.py`: 1bpp
delta-encoded frames driven with encoder-supplied UC8179 waveform scripts
(so refresh speed, ghost-cleanup policy, and SPI clock are tuned by
re-encoding, not reflashing), plus a monophonic buzzer note track extracted
from a MIDI file. Publish `{"action":"video","url":"http://..."}` on the
frame's command topic; playback stats come back on the event topic, and
holding green aborts playback. Video and MIDI sources stay out of this
repository.

Pixel art: the unicorn, horse, and tornado come from Clint Bellanger's
[Tiny Creatures](https://opengameart.org/content/tiny-creatures) (CC0); the
heart, apple, sparkle, and frame icons from Kenney's
[1-Bit Pack](https://kenney.nl/assets/1-bit-pack) (CC0). All are converted
to 1-bit string art in `firmware/e1001/src/main.cpp`.

## Update firmware over Wi-Fi

An open upload session also accepts a firmware image on
`POST /api/firmware/<token>`. The device writes it into the inactive OTA
slot, validates it, and reboots while keeping the displayed photo:

```sh
sh tools/ota-update.sh --device e1004 --address 192.168.1.50 --token <from QR>
```

Without `--address`, the tool starts a deterministic test session over USB,
which requires firmware built with `-DRETERM_UPLOAD_FIXTURE` (test builds
only; `send-image.sh --transport http` installs one automatically).

The pinned build container supplies the glibc environment required by
PlatformIO's aarch64 Espressif compiler; on glibc x86_64 hosts a native
`platformio` works directly.
