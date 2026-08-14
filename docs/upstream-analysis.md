# reTerminal E1001 upstream analysis

Analysis date: 2026-08-14.

## What was acquired

Seeed's public Firmware Hub is the authoritative open-source firmware source.
The vendored `upstream/seeed-trmnl-1.8.10` tree is the Hub's TRMNL snapshot:

- Hub repository: `Seeed-Projects/OSHW-reTerminal-Series-E-D`
- Hub tag/commit: `fw-2026.08.05.2`,
  `be1e32a6002fe8f7fe909dec7776545996396e2d`
- Embedded TRMNL upstream commit: `7abc83c`
- Firmware version: `1.8.10`
- License: GPL-3.0 (retained as `upstream/seeed-trmnl-1.8.10/LICENSE`)

The matching four release images and the ESP Web Tools manifest are in
`upstream/binaries/trmnl-1.8.10`. `SHA256SUMS` is the local integrity record.
The SenseCraft factory firmware is downloadable through Seeed's flasher, but
Seeed does not publish its corresponding application source. It is therefore
not a suitable source base for custom firmware.

## Hardware map

The board is an ESP32-S3R8 (dual Xtensa LX7, 8 MB octal PSRAM) with a discrete
32 MB SPI flash and a GDEY075T7/UC8179-class 800x480 monochrome e-paper panel.
The USB-C data path uses a CH341 UART bridge, hence Linux exposes `ttyUSB0` and
the firmware deliberately disables native USB CDC.

| Function | GPIO | Notes |
| --- | ---: | --- |
| e-paper SCK / MOSI | 7 / 9 | dedicated SPI in the samples |
| e-paper CS / DC / reset / busy | 10 / 11 / 12 / 13 | busy is active-low in common integrations |
| SD MISO / CS / detect / enable | 8 / 14 / 15 / 16 | shares SPI clock/data; avoid simultaneous uncoordinated access |
| buttons | 3 / 4 / 5 | GPIO3 is green wake/refresh button |
| status LED | 6 | board indicator |
| battery ADC / divider enable | 1 / 21 | wait about 10 ms after enabling before sampling |
| UART1 TX / RX | 17 / 18 | also present on expansion header |
| I2C SDA / SCL | 19 / 20 | expansion header; RTC/charger buses need source verification |
| microphone data / clock / enable | 41 / 42 / 38 | PDM microphone |
| buzzer enable | 45 | schematic net name `BUZZER_EN` |
| serial console | 43 / 44 | ESP32-S3 UART0 through CH341 |

The 8-pin expansion header provides 3.3 V, ground, GPIO46, GPIO2, UART1 TX/RX,
and I2C SCL/SDA. GPIO0 is the ESP boot strap and should not be repurposed.

## Binary layout

The official manifest flashes without erasing the whole chip:

| Offset | Image |
| ---: | --- |
| `0x0000` | bootloader |
| `0x8000` | partition table |
| `0xe000` | OTA data initialiser (`boot_app0.bin`) |
| `0x10000` | application |

Decoded partition table:

| Partition | Offset | Size |
| --- | ---: | ---: |
| nvs | `0x9000` | 20 KiB |
| otadata | `0xe000` | 8 KiB |
| app0 / app1 | `0x10000` / `0x1f0000` | 1920 KiB each |
| spiffs | `0x3d0000` | 128 KiB |
| coredump | `0x3f0000` | 64 KiB |

Both upstream bootloader and application headers declare 8 MB DIO flash at 80 MHz,
while the table ends at 4 MB. This is compatible with the physical 32 MB chip,
but wastes most of it. A custom layout may use more flash only after confirming
the detected chip capacity and deciding whether OTA compatibility matters.
The official application is 1,442,624 bytes and reports ESP-IDF 4.4.7 from its
Arduino core. Secure version is zero; the image itself gives no indication that
secure boot or flash encryption is required.

## Safe device workflow

1. Turn the rear power switch on and wake the unit with the green button. Deep
   sleep can otherwise make the serial bootloader handshake fail.
2. Identify the chip and flash before writing:

   ```sh
   uvx --from esptool esptool --port /dev/ttyUSB0 flash-id
   uvx --from esptool espefuse --port /dev/ttyUSB0 summary
   ```

3. Back up the full physical flash after `flash-id` confirms its size (expected
   32 MB). This reads but does not erase it:

   ```sh
   mkdir -p backups
   uvx --from esptool esptool --port /dev/ttyUSB0 read-flash \
     0 0x2000000 backups/factory-32MiB.bin
   sha256sum backups/factory-32MiB.bin > backups/factory-32MiB.bin.sha256
   ```

   Treat the backup as secret: NVS can contain Wi-Fi credentials and tokens;
   `backups/` is gitignored.

4. Build the custom bring-up firmware. Initially flash only the factory app0
   partition at **`0x90000`**, preserving the known-good bootloader, partition
   table, NVS, and empty OTA slot:
   preserving the known-good bootloader, partition table, NVS, and OTA slot:

   ```sh
   sh tools/build-container.sh
   uvx --from esptool esptool --port /dev/ttyUSB0 write-flash \
     0x90000 firmware/bringup/.pio/build/reterminal-e1001/firmware.bin
   ```

Do not use the public TRMNL offset (`0x10000`) with the factory partition table:
it would overwrite the large factory NVS partition. The initial app-only write
is intentionally conservative. Do not use
`erase-flash` until the backup has been verified. If the app does not boot,
restore the four known-good images at the manifest offsets above, or restore the
complete factory dump at offset zero.

## Current connected-device result

After the rear switch was turned on, the board identified as ESP32-S3 revision
0.2 with 8 MB embedded PSRAM and a 32 MB (`ef:4019`) 3.3 V quad-SPI flash. UART
download mode is enabled. eFuse inspection confirms that secure boot, flash
encryption, and anti-rollback are disabled.

A full 32 MiB backup was read and verified at
`backups/factory-32MiB.bin` (gitignored), SHA-256
`1cd1e5502e1544e0f395874cc8ae76abda37b98e3af31be8149ae27285d902bd`.
The factory table differs substantially from public TRMNL:

| Partition | Offset | Size |
| --- | ---: | ---: |
| nvs | `0x9000` | 500 KiB |
| otadata / phy_init | `0x86000` / `0x88000` | 8 KiB / 4 KiB |
| app0 / app1 | `0x90000` / `0xc90000` | 12 MiB each |
| spiffs | `0x1890000` | 6 MiB |
| coredump | `0x1e90000` | 64 KiB |

Only app0 originally contained an image; app1 was erased. Factory app0 declares
32 MB DIO flash at 80 MHz and uses the same Arduino/ESP-IDF 4.4.7 base visible
in the public image.

## Custom bring-up result

The pinned Debian/glibc Podman build completed successfully with PlatformIO
6.1.18 and Espressif32 platform 6.12.0. The resulting application is 298,016
bytes and has SHA-256
`a674afba36ab2d3d6be395741716bb5daf4319126cde68a80940b63c141697a2`.
Its header declares ESP32-S3, 32 MB DIO flash at 80 MHz. The generated partition
table was byte-for-byte identical to the factory partition table.

The application alone was flashed to factory app0 at `0x90000`; esptool's
post-write hash verification passed. Boot output confirmed normal SPI flash boot,
display power-on/full-update/power-off, and panel hibernation. The expected
"reterm custom firmware / E1001 display bring-up succeeded" text was visibly
confirmed on the physical display. The factory backup remains available for
recovery.

## Design direction

The small `firmware/bringup` program is the first custom image: it initializes
the validated panel class and pin map, renders a bordered success message, then
hibernates the panel. Once hardware bring-up succeeds, the next layer should add
button wake, coordinated SD access, battery measurement, networking, and deep
sleep one subsystem at a time. This keeps e-paper waveform/power issues separate
from application and network failures.
