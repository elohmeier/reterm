#!/bin/sh
# Assembles ESP Web Tools manifests and binaries for the browser flasher at
# site/flash.html. Expects both firmware projects to be built already
# (pio run -d firmware/e1001 and -d firmware/e1004). Offsets follow this
# repo's firmware/*/partitions.csv, NOT Seeed's stock layout: bootloader 0x0,
# partition table 0x8000, OTA data initialiser 0x86000 (otadata), application
# 0x90000 (app0) — the same offset tools/send-image.py flashes over USB.
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir=${1:?usage: package-web-flash.sh <output-dir> [version-label]}
version=${2:-dev}

boot_app0="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
[ -f "$boot_app0" ] || { echo "error: $boot_app0 missing; build the firmware first" >&2; exit 1; }

for model in reterminal-e1001 reterminal-e1004; do
  case $model in
    reterminal-e1001) project=firmware/e1001 ;;
    reterminal-e1004) project=firmware/e1004 ;;
  esac
  build="$repo_dir/$project/.pio/build/$model"
  for part in bootloader.bin partitions.bin firmware.bin; do
    [ -f "$build/$part" ] || { echo "error: $build/$part missing; run pio run -d $project" >&2; exit 1; }
  done
  dest="$out_dir/$model"
  mkdir -p "$dest"
  cp "$build/bootloader.bin" "$build/partitions.bin" "$build/firmware.bin" "$dest/"
  cp "$boot_app0" "$dest/boot_app0.bin"
  cat > "$dest/manifest.json" <<EOF
{
  "name": "reterm $model",
  "version": "$version",
  "flashSize": "keep",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 549888 },
        { "path": "firmware.bin", "offset": 589824 }
      ]
    }
  ]
}
EOF
  echo "packaged $model -> $dest"
done
