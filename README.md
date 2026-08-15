# reTerminal E1001 and E1004 firmware workbench

This repository is a reproducible starting point for custom firmware on the
Seeed Studio reTerminal E1001 and E1004. It contains:

- `firmware/bringup`: a small custom, offline display bring-up image;
- `firmware/e1004`: a custom color display image for the 13.3-inch E1004;
- `site`: the static SvelteKit six-ink photo editor and Wi-Fi upload app;
- `profiles`: measured E1004 pigment data and synthetic reconstructions;
- `upstream/seeed-trmnl-1.8.10`: the official open-source firmware snapshot;
- `upstream/seeed-trmnl-e1004-1.8.10`: the E1004 firmware snapshot;
- `upstream/binaries/trmnl-1.8.10`: matching official release binaries; and
- `docs/upstream-analysis.md`: hardware, firmware, recovery, and flashing notes.

The connected board currently appears as `/dev/ttyUSB0` through a CH341 UART
bridge (`1a86:7523`). Do not erase it before making a full flash backup.

## Build the custom bring-up firmware

```sh
sh tools/build-container.sh
sh tools/build-container.sh firmware/e1004
```

The result is `firmware/bringup/.pio/build/reterminal-e1001/firmware.bin`.
The E1004 result is `firmware/e1004/.pio/build/reterminal-e1004/firmware.bin`.
See the analysis notes before attempting to flash it.

## Send an image to the E1004

The image sender supports HEIC, JPEG, PNG, and other Pillow formats. It applies
EXIF orientation, automatically chooses the best 90-degree orientation,
center-crops and resizes to 1200x1600, and Floyd-Steinberg dithers into the
panel's black, white, green, blue, red, and yellow pigments.

```sh
sh tools/send-image.sh /path/to/photo.heic
```

The default `--fit cover` fills the screen. Use `--fit contain` to preserve the
entire image with white letterboxing. After the receiver firmware has been
installed once, skip rebuilding and reflashing it:

```sh
sh tools/send-image.sh /path/to/photo.jpg --no-flash
```

A dithered PNG preview is written to
`/root/.cache/reterm-e1004-preview.png` before the image is sent.
The default pigment mapping is derived from an iPhone ProRAW capture with
linear-light black-point compensation. See
[docs/color-calibration.md](docs/color-calibration.md) for the repeatable
target/capture workflow and the important distinction between reflective
measurements and input-space dithering colors.

## Wireless photo upload

Press the E1004 button while it sleeps to open a five-minute wireless upload
session. On first use, scan the displayed Wi-Fi QR and use the captive portal
to save a home network. The device then displays a one-time QR for the GitHub
Pages uploader. The browser performs all resizing and six-pigment dithering,
then streams the same 960,000-byte packed framebuffer used by the UART tool.

See [docs/wifi-upload.md](docs/wifi-upload.md) for the state machine, API,
security model, GitHub Pages deployment, and browser compatibility notes.

The initial bring-up image has been built, flashed at factory app0 offset
`0x90000`, and visually verified on the connected E1001.

The pinned build container supplies the glibc environment required by
PlatformIO's aarch64 Espressif compiler. On this Alpine host Podman also needs
cgroup v2 mounted (`rc-service cgroups start`).
