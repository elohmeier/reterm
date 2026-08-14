# reTerminal E1001 firmware workbench

This repository is a reproducible starting point for custom firmware on the
Seeed Studio reTerminal E1001. It contains:

- `firmware/bringup`: a small custom, offline display bring-up image;
- `upstream/seeed-trmnl-1.8.10`: the official open-source firmware snapshot;
- `upstream/binaries/trmnl-1.8.10`: matching official release binaries; and
- `docs/upstream-analysis.md`: hardware, firmware, recovery, and flashing notes.

The connected board currently appears as `/dev/ttyUSB0` through a CH341 UART
bridge (`1a86:7523`). Do not erase it before making a full flash backup.

## Build the custom bring-up firmware

```sh
sh tools/build-container.sh
```

The result is `firmware/bringup/.pio/build/reterminal-e1001/firmware.bin`.
See the analysis notes before attempting to flash it.

The initial bring-up image has been built, flashed at factory app0 offset
`0x90000`, and visually verified on the connected E1001.

The pinned build container supplies the glibc environment required by
PlatformIO's aarch64 Espressif compiler. On this Alpine host Podman also needs
cgroup v2 mounted (`rc-service cgroups start`).
