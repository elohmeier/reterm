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
  -d mqtt_host=192.168.20.5 -d mqtt_port=1883 \
  -d mqtt_user=reterm -d mqtt_password=... \
  -d wake_interval_min=60
```

In the hf40 network the frames live on the IoT VLAN (Skynet) and use the
broker's direct listener on `192.168.20.5` — same-segment L2, no firewall in
the path. `homehub.hf40.de` (192.168.10.5) also works from any VLAN but
crosses OPNsense and depends on homehub's source policy routing.

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

- A power-on or reset boot (including the reboot after a firmware update)
  checks in right away instead of waiting a full interval.
- Wakes from deep sleep skip the three-second UART command window; only a
  power-on or reset keeps it for the workbench tools.
- Wi-Fi reconnects skip the scan: the last association's channel and BSSID
  survive deep sleep in RTC memory. If that access point does not answer
  within 4 s the device forgets it and scans as after a power-on.
- Latency marks (milliseconds since boot) are kept in RTC memory across
  wakes without a check-in (view flips, menus) and published at the next
  check-in as `{"event":"timing","marks":[["button",0,12],["flip",412,12],…]}`
  — name, milliseconds, boot number; each boot starts with a mark named
  after its wake reason.
- Check-ins no longer wait a fixed three seconds for retained messages: the
  device publishes to its own `reterm/<id>/sync` after subscribing and
  continues once that message arrives behind the retained ones.
- Every check-in syncs the clock over SNTP from the DHCP gateway (3 s at
  most). The RTC keeps the time across deep sleep, so a missed sync only adds
  drift (the internal RC oscillator can drift a few percent, i.e. minutes
  over a long night).

## Wake schedule and out-of-date badge

A server may publish a retained plan on `reterm/<id>/schedule`. The device
reads it on every check-in, persists it, and never clears it:

```json
{"wake": [1792209720, 1792211520],
 "valid_until": 1792210320,
 "sha256": "<sha256 of the image the server means to show>",
 "badge": [8, 452, 198, 24],
 "tz": "CET-1CEST,M3.5.0,M10.5.0/3"}
```

- `wake`: epochs (ascending, at most 96). The device sleeps until the first
  one at least 20 s ahead and at most 24 h away. When the list runs out, or
  the clock is unknown, it falls back to the wake interval; interval `0`
  still disables timer wakes. Quiet hours are simply gaps in the list.
- `valid_until` and `sha256`: an image delivered by an MQTT `image` command
  counts as out of date once `valid_until` passed, or when the plan names a
  different image than the panel shows (e.g. the fetch failed). Manually
  uploaded images never do. Publishing a fresh plan on every server run
  keeps the image current; a dead server or a broken network lets it expire.
- `badge`: where the board draws the out-of-date badge ("STAND FR 20:30",
  the image's `issued` time in `tz`), as a windowed partial refresh over the
  saved image. It is removed the same way once the image is current again,
  and never drawn over menus, games, or wall modes. The E1004 does not draw
  badges yet. Without a `badge` box the state still reports `stale`.
- `tz`: POSIX TZ string for badge times (default Europe/Berlin).

The state topic adds `next_wake` (epoch), `next_wake_iso`, `clock`, `stale`,
and `badge`; discovery adds a "Next wake" timestamp sensor and an "Image
out of date" problem binary sensor, and stretches `expire_after` over the
longest planned gap so quiet hours do not mark the device unavailable.

## MQTT surface

Device id is `<hostname>-<mac6>`, e.g. `reterm-e1001-a1b2c3` (same suffix as
the provisioning AP name).

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `homeassistant/device/<id>/config` | device → HA | yes | device-based discovery: battery %, voltage, RSSI, wake reason, wake-interval number |
| `reterm/<id>/state` | device → HA | yes | the `/api/status` JSON (geometry + fw, battery_mv, battery_pct, rssi, wake, wake_interval_min) |
| `reterm/<id>/cmd` | HA → device | yes | command JSON; with an `id` it stays retained (the stored id drops redeliveries), without one, or carrying a Wi-Fi password, it is cleared after processing |
| `reterm/<id>/ota` | server → device | yes | firmware rollout `{id, url, sha256, version}`, independent of `cmd` |
| `reterm/<id>/set/wake-interval` | HA → device | yes | minutes, written by the HA number entity (`retain: true`), cleared after apply |
| `reterm/<id>/schedule` | server → device | yes | the wake plan above; read every check-in, never cleared |
| `reterm/<id>/event` | device → HA | no | acks: `{"event":"image","ok":…,"id":…}`, `{"event":"session","url":…}`, errors |

Sensors carry `expire_after` (longest planned gap + interval + 5 min) so the device shows
unavailable after two missed check-ins. Discovery is republished on every
check-in, so a wiped broker heals itself.

## Commands

Commands carry an optional `id`; the device persists the last processed id
and silently drops retained duplicates. Commands with an id are left
retained rather than cleared: a blind clear after a long command (a pages
fetch) would wipe whatever a server published meanwhile. Commands without
an id are cleared, and so is the `wifi` command, so its password does not
stay on the broker. `tools/ha-publish-image.py` appends a
timestamp so identical images can be re-sent deliberately.

- `{"action":"image","id":"…","url":"http://…","sha256":"…"}` — the device
  GETs the URL (exact packed wire format, same bytes as `POST /api/image`),
  streams it to the panel and the SPIFFS pending/current/backup sequence, and
  refreshes only after the optional sha256 verifies. Only `http://` URLs.
- `{"action":"session","id":"…"}` — the device starts a normal QR upload
  session and first publishes the tokenized URL to `reterm/<id>/event`; an HA
  automation can forward it as a phone notification, replacing a walk to the
  device. The token grants uploads for the session window: treat broker read
  access to `reterm/#` as trusted. After a button session the command is left
  retained (one session per wake); the next timer wake runs it.
- `{"action":"wifi","id":"…","ssid":"…","password":"…"}` — roams the device
  to another network by rewriting the saved `wificaptive` credentials; they
  take effect on the next connection, so expect one more check-in from the
  old network first. Wrong credentials strand the device until someone runs
  the physical captive-portal flow — check the `wifi` ack event and the `ip`
  in the following state. The password sits retained on the broker until the
  next wake consumes it.

- `{"action":"ota","id":"…","url":"http://…","sha256":"…","version":"…"}`
  — streams a firmware image into the inactive OTA slot, checks the
  mandatory sha256 before the slot becomes bootable, acks with
  `{"event":"ota","ok":…,"detail":…}`, and reboots. The command id is
  stored before installing, so a redelivery can never re-flash. An optional `version` equal to the running
  `RETERM_FW_VERSION` is acked without reinstalling. The new firmware boots
  in "pending verify" and confirms itself only after its first successful
  check-in (event `"detail":"confirmed <version>"`); if it crashes or cannot
  check in during that first boot, the bootloader returns to the previous
  firmware. Without a configured broker a boot confirms immediately.
- The same rollout can come on its own retained topic,
  `reterm/<id>/ota` (`{"id","url","sha256","version"}`), so a content
  server publishing to `cmd` and a firmware rollout never replace each
  other. The device checks it at every check-in, before commands; a
  `version` already running is ignored without an event, and each `id`
  gets exactly one attempt (stored in NVS), so a bad image cannot loop.
  Clear the retained message once the new version is confirmed.
- `image` commands may carry `"issued": <epoch>`, the content time the
  out-of-date badge shows.
- `{"action":"pages","id":"…","issued":…,"pages":[{"name":"woche","title":"Diese Woche","url":"http://…","sha256":"…"},…]}`
  — up to eight named views (names `a-z0-9-`, at most 12 characters, sha256
  mandatory). The device fetches only pages whose sha256 changed, deletes
  pages no longer listed, and shows the first (the home view) when it is new
  — unless someone flipped to another view in the last 15 minutes. The home
  view also becomes the saved image, so the schedule's `sha256` and the
  badge refer to it. Ack: `{"event":"pages","ok":…,"detail":"fetched 1 of 3, home shown"}`.
  The last manually uploaded photo is kept as a local "Foto" page. A timer
  wake brings the home view back once another view has been up for 15
  minutes.

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
subscribe access to `reterm/<id>/cmd`, `reterm/<id>/set/wake-interval`, and
`reterm/<id>/schedule`. HA
must have the MQTT integration with discovery enabled (default prefix
`homeassistant`).
