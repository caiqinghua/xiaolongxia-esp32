#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-voice-cloud.XXXXXX")"
trap 'rm -rf "$build"' EXIT
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
cjson="${CJSON_SOURCE_DIR:-$idf/components/json/cJSON}"
parser="${HTTP_PARSER_SOURCE_DIR:-$idf/components/http_parser}"
# Build only desktop tests, never ESP-IDF firmware or sdkconfig.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$cjson" -c "$cjson/cJSON.c" -o "$build/cJSON.o"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$parser" -c "$parser/http_parser.c" -o "$build/http_parser.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$idf/components/esp_http_client/include" \
  -I "$root/main" -I "$root/tests/host/voice_cloud_stubs" -I "$cjson" -I "$parser" \
  "$root/main/watch/watch_voice_cloud.cc" "$root/main/watch/watch_voice_cloud_client.cc" \
  "$root/tests/host/watch_voice_cloud_test.cc" "$build/cJSON.o" "$build/http_parser.o" \
  -o "$build/watch_voice_cloud_test"
"$build/watch_voice_cloud_test" "$@"
for protected in 0 1; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DCONFIG_NVS_ENCRYPTION="$protected" -DCONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC="$protected" \
    -I "$idf/components/nvs_flash/include" \
    -I "$root/main" -I "$root/tests/host/voice_cloud_stubs" -I "$cjson" -I "$parser" \
    "$root/main/watch/watch_voice_cloud.cc" "$root/main/watch/watch_voice_binding.cc" \
    "$root/tests/host/watch_voice_binding_test.cc" "$build/cJSON.o" "$build/http_parser.o" \
    -o "$build/watch_voice_binding_test"
  "$build/watch_voice_binding_test" write
  "$build/watch_voice_binding_test" commit
  "$build/watch_voice_binding_test" receipt-write
  "$build/watch_voice_binding_test" receipt-commit
  if [[ "$protected" == 1 ]]; then
    "$build/watch_voice_binding_test" receipt-export "$build/settings-receipt.bin"
    "$build/watch_voice_binding_test" receipt-reboot "$build/settings-receipt.bin"
  fi
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
  "$root/main/watch/watch_voice_task.cc" "$root/tests/host/watch_voice_task_test.cc" \
  -o "$build/watch_voice_task_test"
"$build/watch_voice_task_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DCONFIG_AUDREAM_WATCH_UI=1 -DCONFIG_USE_AUDIO_PROCESSOR=1 \
  '-DCONFIG_AUDREAM_VOICE_API_ORIGIN="https://audream-api.tulingbc.com"' \
  '-DCONFIG_AUDREAM_VOICE_WEBSOCKET_URL="wss://voice.audream.ai/v1/device"' \
  -I "$root/tests/host/voice_application_stubs" -I "$root/tests/host/voice_cloud_stubs" \
  -I "$root/main" -I "$cjson" -I "$parser" \
  "$root/main/watch/watch_voice_application.cc" "$root/main/watch/watch_voice_task.cc" \
  "$root/main/watch/watch_network_application.cc" "$root/main/watch/watch_station_policy.cc" \
  "$root/main/watch/watch_station_recovery.cc" \
  "$root/main/watch/watch_voice_cloud.cc" "$root/main/watch/watch_wifi_gate.cc" \
  "$root/main/audio/audio_session_coordinator.cc" "$root/tests/host/watch_voice_application_test.cc" \
  "$build/cJSON.o" "$build/http_parser.o" -o "$build/watch_voice_application_test"
"$build/watch_voice_application_test"
