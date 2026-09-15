#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-watch-opus.XXXXXX")"
trap 'rm -rf "$build"' EXIT
ogg="$root/main/audio/demuxer/libogg"
codec="$root/managed_components/espressif__esp_audio_codec"
opus_prefix="${OPUS_PREFIX:-/opt/homebrew/opt/opus}"
if [[ -d "$opus_prefix/include/opus" ]]; then
    opus_flags=("-I$opus_prefix/include/opus" "-L$opus_prefix/lib" -lopus)
else
    read -r -a opus_flags <<< "$(pkg-config --cflags --libs opus)"
fi
# Desktop sanitizer tests only; no ESP-IDF build or firmware generation.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$ogg/include" -c "$ogg/src/framing.c" -o "$build/framing.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host/recording_stubs" -I "$ogg/include" \
    -I "$codec/include" -I "$codec/include/decoder" -I "$codec/include/decoder/impl" \
    "$root/main/audio/watch_opus_writer.cc" "$root/main/audio/recording_packet_reader.cc" \
    "$root/main/audio/recording_playback_source.cc" "$root/main/audio/recording_playback_decoder.cc" \
    "$root/tests/host/playback_codec_adapter.cc" "$root/tests/host/watch_opus_writer_test.cc" \
    "$build/framing.o" "${opus_flags[@]}" -o "$build/writer_test"
recordings="${WATCH_OPUS_ARTIFACTS:-$build/recordings}"
"$build/writer_test" "$recordings"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_AUDREAM_WATCH_UI=1 \
    -I "$root/tests/host/recording_stubs" -I "$root/main" -I "$ogg/include" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/local_recording_service.cc" \
    "$root/main/audio/assistant_session_lifecycle.cc" \
    "$root/main/audio/watch_opus_writer.cc" "$root/tests/host/local_recording_test.cc" \
    "$build/framing.o" "${opus_flags[@]}" -o "$build/recording_test"
"$build/recording_test" "$build/lifecycle"

if command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null; then
    for rate in 8000 12000 16000 24000 48000; do
        duration="$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$recordings/$rate.opus")"
        [[ "$duration" == "0.150000" ]]
        ffmpeg -v error -c:a libopus -i "$recordings/$rate.opus" -ar "$rate" -ac 1 -f s16le "$build/$rate.pcm" 2> "$build/decode.log"
        if [[ -s "$build/decode.log" ]]; then cat "$build/decode.log"; exit 1; fi
        [[ "$(wc -c < "$build/$rate.pcm" | tr -d ' ')" == "$((rate * 150 / 1000 * 2))" ]]
    done
    ffmpeg -v error -i "$recordings/16000.opus" -ar 16000 -ac 1 -f s16le "$build/native.pcm" 2> "$build/native.log"
    if [[ -s "$build/native.log" ]]; then cat "$build/native.log"; exit 1; fi
    [[ "$(wc -c < "$build/native.pcm" | tr -d ' ')" == 4800 ]]
    printf '%s\n' 'PASS: independent FFmpeg duration and decoded PCM lengths at all five Opus rates'
fi
