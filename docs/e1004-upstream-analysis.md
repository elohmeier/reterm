# reTerminal E1004 firmware analysis

## Device and recovery baseline

The connected reTerminal E1004 uses an ESP32-S3 revision 0.2 with 8 MiB of
embedded PSRAM and 32 MiB of external flash. Its CH341 serial bridge appears as
USB `1a86:7522` and `/dev/ttyUSB0` on this host. Flash encryption, secure boot,
anti-rollback, and restricted UART download mode are all disabled.

Before modifying the device, its complete flash was saved to the ignored file
`backups/e1004/factory-32MiB.bin`. Its SHA-256 is:

```
83b46f8b51aefa444ab3f343a0e3ca75a61dc1459d51da14501f4320374bf5da
```

The factory partition table at `0x8000` is:

| Name | Offset | Size |
| --- | ---: | ---: |
| nvs | `0x9000` | `0x7d000` |
| otadata | `0x86000` | `0x2000` |
| phy_init | `0x88000` | `0x1000` |
| app0 | `0x90000` | `0xc00000` |
| app1 | `0xc90000` | `0xc00000` |
| spiffs | `0x1890000` | `0x600000` |
| coredump | `0x1e90000` | `0x10000` |

`app0` contained the factory firmware and `app1` was erased. The custom
partition binary is byte-for-byte identical to the factory partition table.

## Upstream snapshot

`upstream/seeed-trmnl-e1004-1.8.10` is the E1004 source distributed by Seeed's
firmware hub at tag `fw-2026.08.05.2` (hub commit
`be1e32a6002fe8f7fe909dec7776545996396e2d`). It identifies the underlying
TRMNL firmware snapshot as commit `64ae0ac` and version 1.8.10.

Matching release artifacts are under
`upstream/binaries/trmnl-e1004-1.8.10`; `SHA256SUMS` records their integrity.
The public release image uses a separate 16 MiB OTA layout, with its application
at `0x20000`. That layout must not be mixed with this unit's 32 MiB factory
layout. The public app image is 1,531,120 bytes, targets ESP32-S3 DIO flash at
80 MHz, and was compiled against ESP-IDF 5.5.2.

## Display hardware

The 13.3-inch 1200-by-1600 Spectra 6 panel is a T133A01 with two display
controllers, each responsible for 600 columns. The native palette is black,
white, yellow, red, blue, and green. A full packed framebuffer requires 960,000
bytes in PSRAM.

The upstream base driver uses these signals:

| Signal | GPIO |
| --- | ---: |
| SPI SCK | 7 |
| SPI MISO | 8 |
| SPI MOSI | 9 |
| controller 0 CS | 10 |
| data/command | 11 |
| controller 1 CS | 2 |
| reset | 38 |
| busy | 13 |
| panel enable | 12 |

The custom driver in `firmware/e1004` was derived from Seeed's E1004 base
example. It sends 480,000 bytes to each controller and uses a 10 MHz SPI clock.

## Custom build and verified flash

Build with:

```sh
sh tools/build-container.sh firmware/e1004
```

The resulting `firmware.bin` is 298,064 bytes and has SHA-256:

```
7ab5d8ae6c62a167e969bff204ae12dbf8fefcab8dec1d052a707179008b36cd
```

Its header targets ESP32-S3, 32 MiB DIO flash at 80 MHz. Only this application
was written to the factory `app0` offset `0x90000`; no bootloader, partition
table, settings, filesystem, or recovery slot was modified. Esptool verified
the written hash.

The first boot completed successfully over UART: PSRAM framebuffer allocation
and dual-controller transfer succeeded, the two halves each received 480,000
bytes, full refresh completed in 27.81 seconds, and the display then entered
hibernate.
