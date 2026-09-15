#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-ap-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
# Compile the actual service on the host with SDK boundary replacements.
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/ap_transfer_stubs" -I "$root/main" \
    "$root/main/storage/temporary_ap_transfer_service.cc" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/main/watch/watch_wifi_gate.cc" "$root/tests/host/ap_transfer_test.cc" \
    -o "$build_dir/ap_transfer_test"
"$build_dir/ap_transfer_test"
