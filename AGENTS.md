# Repository instructions

## Device-hosted web assets

- The E1004 serves an HTML shell that loads the stable `device-boot.js` from
  GitHub Pages. That loader is regenerated on every site build (see
  `site/vite.config.ts`) and injects the current hashed editor bundle, which
  runs in the device page's origin so uploads stay same-origin on iOS Safari.
  GitHub Pages may cache `device-boot.js` for ten minutes, including in iOS
  Safari after a tab is closed; each deploy replaces the hashed chunks, so a
  stale cached loader can 404 for up to ten minutes after a deploy.
- Deploy the site to Pages before flashing firmware that references a new
  `?v=` value, and increment the `?v=` on the `device-boot.js` URL in
  `firmware/e1004/src/main.cpp` whenever shell or protocol behavior changes
  together with firmware.
- The legacy `device-uploader.js`/`device-uploader.css` classic app and the
  inline XHR compatibility shim were removed with the `?v=4` shell; firmware
  older than that must be reflashed, since its Pages assets no longer exist.
- Preserve the initial-page token handshake and same-client fallback unless a
  replacement has been tested on iOS Safari with stale external assets.
- Build and test both firmware and site for paired protocol changes. Do not
  report an upload fix as verified until an authenticated 960,000-byte HTTP
  upload returns success and the serial log reaches `HTTP image displayed`.
  Browser-path HTTP tests must include the real same-origin `Origin` header;
  command-line clients that omit it do not exercise Safari's origin checks.
- Changes to session timeout or framebuffer persistence must preserve the
  pending/current/backup SPIFFS sequence and verify that an unused QR session
  restores the last complete image before deep sleep.

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
