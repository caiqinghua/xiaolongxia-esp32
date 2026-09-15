#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-recording-playback.XXXXXX")"
trap 'rm -rf "$build"' EXIT
ogg="$root/main/audio/demuxer/libogg"
codec="${ESP_AUDIO_CODEC_SOURCE_DIR:-$root/managed_components/espressif__esp_audio_codec}"
opus_prefix="${OPUS_PREFIX:-}"
if [[ -z "$opus_prefix" && -d /opt/homebrew/opt/opus/include ]]; then
    opus_prefix=/opt/homebrew/opt/opus
fi
if [[ -n "$opus_prefix" ]]; then
    opus_cflags=("-I$opus_prefix/include/opus")
    opus_libs=("-L$opus_prefix/lib" -lopus)
else
    cflags="$(pkg-config --cflags opus)"
    libraries="$(pkg-config --libs opus)"
    read -r -a opus_cflags <<< "$cflags"
    read -r -a opus_libs <<< "$libraries"
fi

# Host-only API adapter: no ESP-IDF invocation or ESP binary linking.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$ogg/include" -c "$ogg/src/framing.c" -o "$build/framing.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host/recording_stubs" -I "$ogg/include" \
    -I "$codec/include" -I "$codec/include/decoder" -I "$codec/include/decoder/impl" \
    "${opus_cflags[@]}" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/recording_packet_reader.cc" \
    "$root/main/audio/recording_playback_decoder.cc" "$root/main/audio/recording_playback_task.cc" \
    "$root/main/audio/recording_playback_source.cc" \
    "$root/main/audio/ogg_opus_writer.cc" "$root/tests/host/playback_codec_adapter.cc" \
    "$root/tests/host/recording_playback_test.cc" "$build/framing.o" "${opus_libs[@]}" -o "$build/playback_test"
"$build/playback_test" "$build"

# Compile actual codec declarations and the production output implementation.
# Driver calls and unrelated constructor/legacy methods are host boundaries.
device="$root/managed_components/espressif__esp_codec_dev"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/playback_output_stubs" -I "$device/include" \
    -c "$device/esp_codec_dev_vol.c" -o "$build/volume.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_CODEC_ES7210_SUPPORT=1 \
    -I "$root/tests/host/playback_output_stubs" -I "$root/tests/host/watch_level_stubs" \
    -I "$root/tests/host/audio_processor_stubs" -I "$root/tests/host/transport_stubs" \
    -I "$root/main" -I "$root/main/audio" -I "$device/include" -I "$device/interface" \
    -I "$device/device/include" \
    "$root/main/audio/codecs/box_playback_output.cc" "$root/tests/host/box_playback_output_test.cc" \
    "$build/volume.o" -o "$build/output_test"
"$build/output_test"

cjson="${CJSON_SOURCE_DIR:-${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}/components/json/cJSON}"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/playback_output_stubs" -I "$root/tests/host/watch_level_stubs" \
    -I "$root/tests/host/audio_processor_stubs" -I "$root/tests/host/transport_stubs" \
    -I "$root/main" -I "$root/main/audio" -I "$root/main/protocols" -I "$root/main/audio/demuxer" \
    -I "$codec/include" -I "$codec/include/decoder" -I "$codec/include/decoder/impl" \
    -I "$codec/include/encoder" -I "$codec/include/encoder/impl" \
    -I "$root/managed_components/espressif__esp_audio_effects/include" -I "$cjson" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/local_playback_output.cc" \
    "$root/tests/host/local_playback_output_test.cc" -o "$build/service_output_test"
"$build/service_output_test"
