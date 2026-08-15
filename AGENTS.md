# Repository instructions

## Firmware layout

- `firmware/lib/reterm` is the shared runtime (UART protocol, upload session,
  HTTP API and token security model, Wi-Fi provisioning, SPIFFS persistence,
  deep-sleep orchestration) used by both `firmware/e1001` and
  `firmware/e1004`. Behavior changes there affect BOTH devices — build both
  projects after touching it.
- Per-device wire formats: E1004 is 1200×1600 4bpp (960,000 bytes, 600-byte
  rows); E1001 is 800×480 1bpp (48,000 bytes, 100-byte rows, bit set = white,
  MSB is the leftmost pixel). The palette/bit order IS the wire format on both
  the site and firmware sides.

## Device-hosted web assets

- Both devices serve an HTML shell that loads the stable `device-boot.js`
  from GitHub Pages. That loader is regenerated on every site build (see
  `site/vite.config.ts`) and injects the current hashed editor bundle, which
  runs in the device page's origin so uploads stay same-origin on iOS Safari.
  GitHub Pages may cache `device-boot.js` for ten minutes, including in iOS
  Safari after a tab is closed; each deploy replaces the hashed chunks, so a
  stale cached loader can 404 for up to ten minutes after a deploy.
- Deploy the site to Pages before flashing firmware that references a new
  `?v=` value, and increment the `?v=` on the `device-boot.js` URL in
  `firmware/lib/reterm/src/reterm.cpp` whenever shell or protocol behavior
  changes together with firmware. The QR/session URL carries
  `model=<reterminal-e1001|reterminal-e1004>`; the editor resolves its device
  profile from it and re-checks against `/api/status`.
- The legacy `device-uploader.js`/`device-uploader.css` classic app and the
  inline XHR compatibility shim were removed with the `?v=4` shell; firmware
  older than that must be reflashed, since its Pages assets no longer exist.
- Preserve the initial-page token handshake and same-client fallback unless a
  replacement has been tested on iOS Safari with stale external assets.
- Build and test both firmware and site for paired protocol changes. Do not
  report an upload fix as verified until an authenticated full-size HTTP
  upload (960,000 bytes on E1004, 48,000 bytes on E1001) returns success and
  the serial log reaches `HTTP image displayed`. Browser-path HTTP tests must
  include the real same-origin `Origin` header; command-line clients that
  omit it do not exercise Safari's origin checks.
- The deterministic UART test session only exists in firmware built with
  `-DRETERM_UPLOAD_FIXTURE`. `tools/send-image.py --transport http` sets the
  flag itself when it builds and flashes; with `--no-flash` the device must
  already run a fixture build or the tool aborts on the firmware's
  `UART fixture disabled` line. Never add the flag to release or CI builds.
- Flash offsets come from `firmware/*/partitions.csv`: app at `0x90000`,
  `otadata` at `0x86000` — NOT Seeed's stock `0x10000` layout. The web-flash
  manifests (`tools/package-web-flash.sh`) and `tools/send-image.py` must
  stay in agreement with the partition tables. USB flashing erases `otadata`
  first so it overrides any prior `/api/firmware` OTA that booted from
  `app1`.
- `tools/check-palette-sync.py` (run in CI) fails when the E1004 pigment
  literals in `site/src/lib/devices.ts` diverge from the measured profile;
  after a recalibration regenerate those literals, do not delete the check.
- Changes to session timeout or framebuffer persistence must preserve the
  pending/current/backup SPIFFS sequence and verify that an unused QR session
  restores the last complete image before deep sleep.
- The Pages deploy also builds both firmwares and publishes ESP Web Tools
  manifests under `flash/<model>/` for the browser flasher at `flash.html`
  (see `tools/package-web-flash.sh`), so a deploy ships editor, loader, and
  firmware from the same commit.

## E1004 color calibration

- Treat the ProRAW profile's measured sRGB values as a reflective appearance
  record, not as the input-space dithering palette. The photographed paper
  black has an elevated value; using it directly crushes shadows into physical
  black and produces a dark, contrasty image.
- Derive working pigment colors by mapping measured paper black to 0 and white
  to 1 per channel in linear sRGB, then encode back to sRGB. Keep the raw
  measured values in the profile for analysis and preview reconstruction.
- Send indexed calibration targets with `--nominal-palette`; tone adjustment or
  profile remapping invalidates their known wire indices.
- Never add the source DNG or private rectified photo diagnostics to the repo.
  Commit only the numeric profile, synthetic reconstruction, tools, and notes.
