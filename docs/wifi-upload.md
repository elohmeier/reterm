# E1004 wireless photo upload

## User flow

1. Tap any of the three E1004 capacitive buttons while the device is sleeping.
2. On first use, scan the `SET UP WI-FI` QR. It joins a temporary,
   password-protected device network and opens a captive portal. The submitted
   credentials are stored only in the ESP32 `wificaptive` NVS namespace.
3. Once connected to the home Wi-Fi, the E1004 generates a random 128-bit
   session token and displays a second QR.
4. The QR opens a compact uploader served directly by the E1004. This keeps
   the upload same-origin on iOS Safari, which blocks an HTTPS GitHub Pages
   page from posting to a local plain-HTTP device. The full GitHub Pages editor
   remains available at `https://elohmeier.github.io/reterm/` for compatible
   browsers. That editor composes the photo with stickers, editable text, and
   free-hand drawing on a fabric.js canvas, applies optional photo looks, and
   offers five dithering styles with a live six-ink proof. It sends the same
   authenticated 15-second `/api/status` heartbeat as the local uploader so
   long editing sessions do not hit the inactivity timeout, and it adopts a
   fresh `#device`/`token` hash without a reload if a new QR is scanned into
   an already-open tab.
5. The user chooses a JPEG, PNG, WebP, HEIC, or other browser-decodable image,
   then adjusts rotation, fill/contain mode, zoom, and position.
6. A Web Worker resizes the image to 1200x1600, applies Floyd-Steinberg
   dithering to black, white, green, blue, red, and yellow, and packs two
   pixels into each byte.
7. The browser sends the 960,000-byte framebuffer to the E1004. The firmware
   copies each completed 600-byte row directly into PSRAM while persisting the
   packed framebuffer in SPIFFS, acknowledges the upload, refreshes the panel,
   invalidates the token, disables Wi-Fi, and returns to deep sleep.

The HTTP session remains alive until the successful image request's `202`
response has been sent. Receiving the final framebuffer byte alone does not
stop the server, avoiding a race that left browsers waiting at 100% upload.
After queuing that response, firmware keeps the HTTP task alive for a one-second
TCP drain grace period before stopping Wi-Fi, preventing a late connection
reset before Safari receives the acknowledgement.

The upload API has a five-minute inactivity timeout after the upload QR has
finished refreshing. An authenticated heartbeat from the open local uploader
keeps the session active while the user chooses and edits a photo. An absolute
30-minute cap prevents an abandoned browser tab from holding Wi-Fi awake
forever. First-run provisioning remains available for three minutes. Tapping
another button does not cancel an active session; it ends only after a
successful upload or timeout.

The 6 MB SPIFFS partition retains the most recently completed framebuffer
across deep sleep. Writes use a pending file plus a recoverable rename, so an
interrupted upload does not replace the last good image. If a QR session times
out without an upload—or Wi-Fi cannot be restored after drawing the QR—the
firmware reloads that framebuffer, performs one normal color refresh, and then
sleeps. A device with no saved framebuffer yet leaves the QR visible.
The first persistence-enabled boot may take longer while firmware formats the
previously unused factory SPIFFS partition; the UART image tool allows up to
two minutes for this one-time initialization.
Persistence is performed on the flow-controlled HTTP upload path. UART image
reception remains timing-sensitive and does not write SPIFFS concurrently;
doing so can stall long enough to overrun the serial stream.

The device-hosted editor also performs dithering in a Web Worker. This keeps
its 15-second heartbeat and UI progress active while an iPhone processes all
1.92 million pixels, preventing CPU-bound image work from accidentally
expiring the device session before the HTTP upload starts.

On a button wake, firmware starts Wi-Fi and uses its assigned numeric address
in the QR. This avoids relying on multicast DNS, which is often unreliable on
guest or client-isolated WLANs. Button wakes bypass the UART recovery grace
period; that three-second window remains available after reset for host image
tools.
The HTTP server starts before the slow color-panel refresh begins. Its task
runs away from the Wi-Fi/system core and yields between raw request chunks;
otherwise draining a 960 KB body can starve the ESP32 idle task and trigger a
watchdog reboot just after browser progress reaches 100%. Image uploads are
accepted after the QR refresh completes so display writes cannot overlap.
Upload mode also disables Wi-Fi modem sleep and reconnects the station if panel
activity disrupts its association.
The QR renderer only visits modules intersecting the current 40-line display
page; avoiding repeatedly clipped QR geometry reduced measured time from an
upload command to physical panel refresh from about 24 seconds to 3.3 seconds.

## HTTP API

The API listens on port 80 of the address encoded in the QR.

### `OPTIONS /api/status` or `/api/image`

Successful preflights from the production Pages origin return:

```http
Access-Control-Allow-Origin: https://elohmeier.github.io
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Upload-Token
Access-Control-Allow-Private-Network: true
Access-Control-Max-Age: 600
Vary: Origin
```

### `GET /api/status`

Requires `X-Upload-Token`. It reports the model, dimensions, packed length,
format, and ordered palette.

### `POST /api/image/:token`

```http
Content-Type: application/octet-stream
Content-Length: 960000
```

The browser includes the session token in the exact per-session upload path.
The API also continues to accept `POST /api/image` with the token in the
`X-Upload-Token` header. The path form avoids an iOS Safari failure where the
custom header could be absent after a large XHR body had been transmitted.
Only the exact generated path is registered for the session, so successful
route dispatch is the bearer-token check; legacy `/api/image` requests still
use constant-time header comparison.

The 4-bit source palette is the GxEPD2 encoding already used by the custom
driver: 0 black, 1 white, 2 green, 3 blue, 4 red, and 5 yellow. The first pixel
is in the high nibble. Values 6 and 7 are not accepted from the browser
quantizer.

## Security properties

- Tokens contain 128 bits from `esp_fill_random()`.
- Production tokens exist only in RAM, are never written to NVS or UART logs,
  and are compared without data-dependent early exit.
- A token is valid for one short session and is cleared after upload or
  timeout.
- The local uploader's token is sent only to the E1004. GitHub Pages never
  receives it.
- CORS permits only `https://elohmeier.github.io`; wildcard origins are not
  used.
- The device's own numeric HTTP origin is accepted for its same-origin Safari
  uploader. It does not receive a CORS header because none is needed. The only
  accepted cross-origin origin remains `https://elohmeier.github.io`.
- The API requires an exact 960,000-byte body and does not allocate a second
  full framebuffer.
- Only a complete, validated upload replaces the persisted framebuffer; a
  backup filename protects the last good image during the final rename.
- First-run access point passwords are generated randomly for each attempt.

The local API uses HTTP. The token protects authorization, but image bytes can
still be observed by another party on the same Wi-Fi. Supporting trusted HTTPS
directly on an unprovisioned local device would require a separate certificate
ownership and trust design.

The device-served page is deliberately only a small HTML shell. It loads the
fixed `device-uploader.js` and `device-uploader.css` assets from GitHub Pages;
the classic script executes in the local page's origin, so its API upload is
same-origin while editor updates can be deployed without reflashing firmware.
Asset URLs carry a protocol-version query so Safari does not reuse an older
ten-minute GitHub Pages cache entry after firmware and uploader changes.
The shell also rewrites the legacy `/api/image` XHR target to the current
tokenized path, protecting active devices from older uploader JavaScript that
iOS may retain despite cache busting. Uploads started during the QR refresh
wait for the display bus instead of failing with a transient `503`.
As a final compatibility path, a valid token on the initial local page URL
binds that short-lived session to the browser's LAN address. A legacy
`/api/image` request from the same address is then accepted even if Safari
removes its custom header and ignores the inline endpoint rewrite.
Authentication failures include non-secret diagnostics in the JSON error:
route type, header length, peer and bound LAN addresses, origin state, and
display readiness. The uploader displays this detail directly for field
diagnosis without exposing the session token.

## Browser compatibility

The Pages site requests the emerging Local Network Access capability with
`targetAddressSpace: "local"` on Chromium and Edge. WebKit receives the same
CORS request without that Chromium-specific hint because affected Safari
versions fail the request when it is present. Browser support is not uniform,
particularly on older Safari and Firefox releases. The USB/UART
`tools/send-image.sh` path remains the reliable fallback.

The phone and E1004 must also be allowed to communicate with each other on the
local WLAN. Guest networks and access points with client/AP isolation enabled
usually block this traffic even when both devices receive addresses in the
same subnet; use a trusted home SSID without client isolation in that case.

## Build and deploy

Build the site locally with the same Node release as CI:

```sh
podman run --rm --runtime runc \
  -v "$PWD/site:/app" -w /app docker.io/library/node:22-alpine npm ci
podman run --rm --runtime runc \
  -v "$PWD/site:/app" -w /app docker.io/library/node:22-alpine npm run check
podman run --rm --runtime runc \
  -v "$PWD/site:/app" -w /app -e NODE_ENV=production \
  docker.io/library/node:22-alpine npm run build
```

`.github/workflows/pages.yml` checks and builds the static SvelteKit project,
then deploys `site/build`. Configure the repository's Pages source as GitHub
Actions before the first deployment.

Build the firmware with:

```sh
sh tools/build-container.sh firmware/e1004
```

Only flash `firmware.bin` to the factory application offset `0x90000`, as
described in the E1004 upstream analysis.
