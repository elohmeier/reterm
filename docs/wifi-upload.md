# E1004 wireless photo upload

## User flow

1. Press the E1004 button while the device is sleeping.
2. On first use, scan the `SET UP WI-FI` QR. It joins a temporary,
   password-protected device network and opens a captive portal. The submitted
   credentials are stored only in the ESP32 `wificaptive` NVS namespace.
3. Once connected to the home Wi-Fi, the E1004 generates a random 128-bit
   session token and displays a second QR.
4. The QR opens `https://elohmeier.github.io/reterm/`. Its URL fragment carries
   the local device address and token; URL fragments are not sent to GitHub.
5. The user chooses a JPEG, PNG, WebP, HEIC, or other browser-decodable image,
   then adjusts rotation, fill/contain mode, zoom, and position.
6. A Web Worker resizes the image to 1200x1600, applies Floyd-Steinberg
   dithering to black, white, green, blue, red, and yellow, and packs two
   pixels into each byte.
7. The browser sends the 960,000-byte framebuffer to the E1004. The firmware
   copies each completed 600-byte row directly into PSRAM, acknowledges the
   upload, refreshes the panel, invalidates the token, disables Wi-Fi, and
   returns to deep sleep.

The upload API remains available for 60 seconds after the Pages QR has
finished refreshing. First-run provisioning remains available for three
minutes. Pressing the button during either window cancels it.

## HTTP API

The API listens on port 80 of the address encoded in the QR.

### `OPTIONS /api/status`, `/api/image`, or `/api/cancel`

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

### `POST /api/image`

```http
Content-Type: application/octet-stream
X-Upload-Token: <32 lowercase hexadecimal characters>
Content-Length: 960000
```

The 4-bit source palette is the GxEPD2 encoding already used by the custom
driver: 0 black, 1 white, 2 green, 3 blue, 4 red, and 5 yellow. The first pixel
is in the high nibble. Values 6 and 7 are not accepted from the browser
quantizer.

### `POST /api/cancel`

Requires the session token and closes the session without changing the panel.

## Security properties

- Tokens contain 128 bits from `esp_fill_random()`.
- Production tokens exist only in RAM, are never written to NVS or UART logs,
  and are compared without data-dependent early exit.
- A token is valid for one short session and is cleared after upload, cancel,
  or timeout.
- The token and device address use the QR URL fragment, so GitHub Pages and
  referrer headers never receive them.
- CORS permits only `https://elohmeier.github.io`; wildcard origins are not
  used.
- The API requires an exact 960,000-byte body and does not allocate a second
  full framebuffer.
- First-run access point passwords are generated randomly for each attempt.

The local API uses HTTP. The token protects authorization, but image bytes can
still be observed by another party on the same Wi-Fi. Supporting trusted HTTPS
directly on an unprovisioned local device would require a separate certificate
ownership and trust design.

## Browser compatibility

The Pages site requests the emerging Local Network Access capability by
constructing requests with `targetAddressSpace: "local"`. Current Chromium
and Edge builds can prompt the user for permission and allow an HTTPS page to
reach an HTTP private address. Browser support is not uniform, particularly
on older Safari and Firefox releases. The USB/UART `tools/send-image.sh` path
remains the reliable fallback.

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
