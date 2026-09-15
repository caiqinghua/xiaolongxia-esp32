#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-wake-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
codec="$root/managed_components/espressif__esp_audio_codec/include"
# Real wake-word implementation and Opus ABI; only SDK execution is simulated.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/wake_word_stubs" -I "$root/tests/host/audio_processor_stubs" \
    -I "$root/tests/host/transport_stubs" -I "$root/main" -I "$root/main/audio" \
    -I "$codec" -I "$codec/encoder" -I "$codec/encoder/impl" \
    "$root/main/audio/wake_words/afe_wake_word.cc" \
    "$root/tests/host/afe_wake_word_test.cc" -o "$build_dir/afe_wake_word_test"
"$build_dir/afe_wake_word_test"

cjson="${CJSON_SOURCE_DIR:-${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}/components/json/cJSON}"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/playback_output_stubs" -I "$root/tests/host/watch_level_stubs" \
    -I "$root/tests/host/audio_processor_stubs" -I "$root/tests/host/transport_stubs" \
    -I "$root/main" -I "$root/main/audio" -I "$root/main/protocols" -I "$root/main/audio/demuxer" \
    -I "$codec" -I "$codec/decoder" -I "$codec/decoder/impl" \
    -I "$codec/encoder" -I "$codec/encoder/impl" \
    -I "$root/managed_components/espressif__esp_audio_effects/include" -I "$cjson" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/assistant_audio_handoff.cc" \
    "$root/main/audio/model_validation_audio.cc" \
    "$root/main/audio/assistant_speech_monitor.cc" \
    "$root/tests/host/assistant_audio_handoff_test.cc" -o "$build_dir/handoff_test"
"$build_dir/handoff_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_voice_capture.cc" "$root/tests/host/watch_voice_capture_test.cc" \
    -o "$build_dir/capture_test"
"$build_dir/capture_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_speech_interrupt.cc" "$root/tests/host/watch_speech_interrupt_test.cc" \
    -o "$build_dir/interrupt_test"
"$build_dir/interrupt_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/speech_interrupt_stubs" -I "$root/main" \
    "$root/main/watch/watch_speech_interrupt.cc" "$root/main/watch/watch_speech_interrupt_application.cc" \
    "$root/main/watch/watch_voice_capture.cc" \
    "$root/tests/host/watch_speech_interrupt_application_test.cc" -o "$build_dir/interrupt_application_test"
"$build_dir/interrupt_application_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/tests/host/watch_wake_lease_test.cc" -o "$build_dir/lease_test"
"$build_dir/lease_test"
