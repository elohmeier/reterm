# Home Assistant integration

Both devices can check in with an MQTT broker on a periodic timer wake,
report their stats with Home Assistant MQTT discovery, and consume retained
commands. HA can never reach a deep-sleeping device, so everything is
device-initiated: whatever HA last left on the retained topics is applied at
the next wake.

## Enabling it

The integration is off until a broker host is configured. During any upload
session (the QR session), authenticate with the session token and POST the
settings:

```sh
curl -X POST "http://<device-ip>/api/config" \
  -H "X-Upload-Token: <token from the QR URL>" \
  -d mqtt_host=homehub.hf40.de -d mqtt_port=1883 \
  -d mqtt_user=reterm -d mqtt_password=... \
  -d wake_interval_min=60
```

Only supplied fields change; `GET /api/config` returns the current values
(password masked). Setting `mqtt_host` without a stored interval enables the
default 60-minute check-in; `wake_interval_min=0` disables timer wakes while
keeping MQTT reporting after button sessions; an empty `mqtt_host` disables
the integration. Settings live in the `reterm-ha` NVS namespace, next to the
`wificaptive` credentials, and survive OTA updates.

## Wake and check-in behavior

- `goToSleep()` arms `esp_sleep_enable_timer_wakeup` alongside the button
  wake whenever a broker and a non-zero interval are configured.
- On a timer wake the device reads the battery (before Wi-Fi, divider gated
  by GPIO21, ADC on GPIO1 ×2), connects, publishes discovery + retained
  state, processes retained commands, and sleeps. The panel is untouched
  unless a command replaced the image.
- After every button/UART upload session the device also publishes a state
  update (Wi-Fi is already up, so this is nearly free). Session commands are
  refused there: one session per wake.
- Battery percent is a LiPo open-circuit voltage curve — there is no fuel
  gauge on these boards. The E1004 battery ADC wiring matches upstream's
  `config.h` but is unverified on hardware (one upstream table marks the pin
  absent); sanity-check its first reported voltages.

## MQTT surface

Device id is `<hostname>-<mac6>`, e.g. `reterm-e1001-a1b2c3` (same suffix as
the provisioning AP name).

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `homeassistant/device/<id>/config` | device → HA | yes | device-based discovery: battery %, voltage, RSSI, wake reason, wake-interval number |
| `reterm/<id>/state` | device → HA | yes | the `/api/status` JSON (geometry + fw, battery_mv, battery_pct, rssi, wake, wake_interval_min) |
| `reterm/<id>/cmd` | HA → device | yes | command JSON, cleared by the device after processing |
| `reterm/<id>/set/wake-interval` | HA → device | yes | minutes, written by the HA number entity (`retain: true`), cleared after apply |
| `reterm/<id>/event` | device → HA | no | acks: `{"event":"image","ok":…,"id":…}`, `{"event":"session","url":…}`, errors |

Sensors carry `expire_after` (2× interval + 5 min) so the device shows
unavailable after two missed check-ins. Discovery is republished on every
check-in, so a wiped broker heals itself.

## Commands

Commands carry an optional `id`; the device persists the last processed id
and silently drops retained duplicates. `tools/ha-publish-image.py` appends a
timestamp so identical images can be re-sent deliberately.

- `{"action":"image","id":"…","url":"http://…","sha256":"…"}` — the device
  GETs the URL (exact packed wire format, same bytes as `POST /api/image`),
  streams it to the panel and the SPIFFS pending/current/backup sequence, and
  refreshes only after the optional sha256 verifies. Only `http://` URLs.
- `{"action":"session","id":"…"}` — the device starts a normal QR upload
  session and first publishes the tokenized URL to `reterm/<id>/event`; an HA
  automation can forward it as a phone notification, replacing a walk to the
  device. The token grants uploads for the session window: treat broker read
  access to `reterm/#` as trusted.

## Sending an image from the HA side

```sh
./tools/ha-publish-image.py photo.heic --device e1004 \
  --device-id reterm-e1004-a1b2c3 \
  --copy-to root@homehub.hf40.de:/var/lib/homeassistant/www/reterm/frame.bin \
  --url http://homehub.hf40.de:8123/local/reterm/frame.bin \
  --broker homehub.hf40.de --username reterm --password ...
```

Anything that can produce the packed format and publish MQTT works the same
way from an HA automation.

## Versioning

`/api/status`, the state topic, and discovery report `RETERM_FW_VERSION`.
Local builds say `dev`; the Pages workflow injects the same
`YYYY.MM.DD-<sha7>` label used for the web-flash manifests, so a device
reports exactly the deploy it runs.

## Broker expectations

The device speaks plain MQTT 3.1.1 (no TLS) with username/password. It needs
publish access to `reterm/<id>/…` and `homeassistant/device/<id>/config`, and
subscribe access to `reterm/<id>/cmd` and `reterm/<id>/set/wake-interval`. HA
must have the MQTT integration with discovery enabled (default prefix
`homeassistant`).
