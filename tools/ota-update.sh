#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec uv run "$repo_dir/tools/ota-update.py" "$@"
