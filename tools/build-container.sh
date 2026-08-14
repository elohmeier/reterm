#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cache_dir=/root/.cache/reterm-platformio
project_dir=${1:-firmware/bringup}

mkdir -p "$cache_dir"
podman build --runtime runc -t reterm-platformio -f "$repo_dir/containers/Containerfile" "$repo_dir"
podman run --rm --runtime runc \
  --volume "$repo_dir:/workspace" \
  --volume "$cache_dir:/root/.platformio" \
  reterm-platformio run -d "$project_dir"
