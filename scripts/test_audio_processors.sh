#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-audio-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT

# Compile the actual processor implementations against host-only SDK boundaries.
for aec in 0 1; do
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_USE_DEVICE_AEC="$aec" \
    -I "$root/tests/host/audio_processor_stubs" -I "$root/tests/host/transport_stubs" \
    -I "$root/main" -I "$root/main/audio" \
    "$root/main/audio/processors/afe_audio_processor.cc" \
    "$root/main/audio/processors/no_audio_processor.cc" \
    "$root/tests/host/audio_processor_test.cc" -o "$build_dir/audio_processor_test"
"$build_dir/audio_processor_test"
done
