#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${WATCH_UI_BUILD_DIR:-${TMPDIR:-/tmp}/audream-watch-ui-host}"
artifacts="${1:-${TMPDIR:-/tmp}/audream-watch-ui-shots}"
mkdir -p "$build_dir" "$artifacts"

# This is a desktop LVGL build, not an ESP-IDF firmware build.
if ! cmake -S "$root/tests/host/watch_ui" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
    -DWATCH_TEST_LANGUAGE="${WATCH_UI_LANGUAGE:-en-US}" > "$build_dir/configure.log" 2>&1; then
    cat "$build_dir/configure.log"
    exit 1
fi
if ! cmake --build "$build_dir" -j 4 > "$build_dir/build.log" 2>&1; then
    tail -100 "$build_dir/build.log"
    exit 1
fi
UBSAN_OPTIONS=halt_on_error=1 "$build_dir/watch_ui_test" "$artifacts"
if command -v sips >/dev/null 2>&1; then
    for source in "$artifacts"/*.ppm; do
        sips -s format png "$source" --out "${source%.ppm}.png" >/dev/null
    done
fi
printf 'Watch UI rendering artifacts: %s\n' "$artifacts"
