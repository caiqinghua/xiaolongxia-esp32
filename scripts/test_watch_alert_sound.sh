#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-alert-sound.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Host audio-output tests only. No ESP-IDF or firmware build.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/watch/watch_alert_sound.cc" \
    "$root/tests/host/watch_alert_sound_test.cc" -o "$build/alert_test"
"$build/alert_test"
