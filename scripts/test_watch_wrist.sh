#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-watch-wrist.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_wrist_control.cc" \
    "$root/main/watch/watch_activity_record.cc" \
    "$root/tests/host/watch_wrist_control_test.cc" -o "$build_dir/watch_wrist_control_test"
"$build_dir/watch_wrist_control_test"
