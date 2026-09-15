#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-watch-rtc.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
# Desktop source tests only; SDK I2C calls are fixture boundaries.
flags=(-std=c++17 -Wall -Wextra -Werror -pedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer)
"${CXX:-c++}" "${flags[@]}" -I "$root/main" \
    "$root/main/watch/watch_pcf85063.cc" "$root/tests/host/watch_pcf85063_test.cc" -o "$build_dir/rtc"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/rtc"
"${CXX:-c++}" "${flags[@]}" -I "$root/tests/host/rtc_stubs" -I "$root/tests/host/power_stubs" -I "$root/main" \
    "$root/main/watch/watch_pcf85063.cc" "$root/main/watch/watch_rtc_runtime.cc" \
    "$root/tests/host/watch_rtc_runtime_test.cc" -o "$build_dir/runtime"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/runtime"
