# Repository instructions

## Device-hosted web assets

- The E1004 serves an HTML shell but loads `device-uploader.js` and
  `device-uploader.css` from GitHub Pages. GitHub Pages may cache these assets
  for ten minutes, including in iOS Safari after a tab is closed.
- Whenever either asset changes in a way that accompanies firmware behavior or
  an HTTP API/protocol change, increment the `?v=` cache-busting value for both
  asset URLs in `firmware/e1004/src/main.cpp`.
- Preserve or deliberately update the inline compatibility shim in the device
  HTML shell when changing uploader endpoints; it protects iOS clients that
  execute an older cached Pages script.
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
