#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-stream-encoder.XXXXXX")"
trap 'rm -rf "$build"' EXIT
codec="$root/managed_components/espressif__esp_audio_codec"
cjson="${CJSON_SOURCE_DIR:-${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}/components/json/cJSON}"
prefix="${OPUS_PREFIX:-}"
if [[ -z "$prefix" && -d /opt/homebrew/opt/opus/include ]]; then
    prefix=/opt/homebrew/opt/opus
fi
if [[ -n "$prefix" ]]; then
    opus_cflags=("-I$prefix/include/opus")
    opus_libs=("-L$prefix/lib" -lopus)
else
    read -r -a opus_cflags <<< "$(pkg-config --cflags opus)"
    read -r -a opus_libs <<< "$(pkg-config --libs opus)"
fi
# The production worker and queue methods run on a host thread. Capture,
# service startup, hardware and decoder calls are explicit test boundaries.
for watch in 0 1; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
        -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_AUDREAM_WATCH_UI="$watch" \
        -I "$root/tests/host/playback_output_stubs" -I "$root/tests/host/watch_level_stubs" \
        -I "$root/tests/host/audio_processor_stubs" -I "$root/tests/host/transport_stubs" \
        -I "$root/main" -I "$root/main/audio" -I "$root/main/protocols" -I "$root/main/audio/demuxer" \
        -I "$codec/include" -I "$codec/include/decoder" -I "$codec/include/decoder/impl" \
        -I "$codec/include/encoder" -I "$codec/include/encoder/impl" \
        -I "$root/managed_components/espressif__esp_audio_effects/include" -I "$cjson" \
        "${opus_cflags[@]}" "$root/main/audio/opus_codec_task.cc" \
        "$root/main/audio/watch_stream_encoder.cc" "$root/tests/host/watch_stream_encoder_test.cc" \
        "${opus_libs[@]}" -o "$build/encoder-$watch"
    "$build/encoder-$watch"
done
