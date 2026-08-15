#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cache_dir=/root/.cache/reterm-platformio
project_dir=${1:-firmware/bringup}

mkdir -p "$cache_dir"
podman build --runtime runc -t reterm-platformio -f "$repo_dir/containers/Containerfile" "$repo_dir"
# PLATFORMIO_BUILD_FLAGS passes extra defines through to pio, e.g.
# -DRETERM_UPLOAD_FIXTURE for the deterministic HTTP test session.
podman run --rm --runtime runc \
  --volume "$repo_dir:/workspace" \
  --volume "$cache_dir:/root/.platformio" \
  ${PLATFORMIO_BUILD_FLAGS:+--env PLATFORMIO_BUILD_FLAGS="$PLATFORMIO_BUILD_FLAGS"} \
  reterm-platformio run -d "$project_dir"
