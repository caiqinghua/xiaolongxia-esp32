#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-ota-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
: "${SODIUM_SOURCE_DIR:?Set SODIUM_SOURCE_DIR to the configured native libsodium source}"
sodium="$SODIUM_SOURCE_DIR"
cjson="$idf/components/json/cJSON"
parser="$idf/components/http_parser"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$cjson" -c "$cjson/cJSON.c" -o "$build/cJSON.o"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$parser" -c "$parser/http_parser.c" -o "$build/http_parser.o"
# Real Ota::Upgrade and HTTP interface; native image headers, simulated I/O.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/tests/host/ota_stubs" -I "$root/main" \
    -I "$root/managed_components/78__esp-ml307/include" \
    -isystem "$idf/components/esp_common/include" \
    -isystem "$idf/components/esp_app_format/include" \
    -isystem "$idf/components/bootloader_support/include" \
    -I "$sodium/src/libsodium/include" \
    "$root/main/ota_upgrade.cc" "$root/tests/host/ota_upgrade_test.cc" \
    "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/test"
"$build/test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/tests/host/ota_stubs" -I "$root/main" \
    -I "$root/managed_components/78__esp-ml307/include" \
    -isystem "$idf/components/esp_common/include" \
    -isystem "$idf/components/esp_app_format/include" \
    "$root/main/ota_boot.cc" "$root/tests/host/ota_boot_test.cc" -o "$build/boot-test"
"$build/boot-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/main" -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/firmware_release.cc" "$root/tests/host/firmware_release_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/release-test"
"$build/release-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host" -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/asset_release.cc" "$root/main/firmware_release.cc" "$root/tests/host/asset_release_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/asset-test"
"$build/asset-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host" -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/asset_storage.cc" "$root/main/asset_release.cc" "$root/main/firmware_release.cc" \
    "$root/tests/host/asset_storage_test.cc" "$build/cJSON.o" "$build/http_parser.o" \
    "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/asset-storage-test"
"$build/asset-storage-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host" -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    -isystem "$idf/components/esp_partition/include" -isystem "$idf/components/nvs_flash/include" \
    -isystem "$idf/components/esp_common/include" -I "$root/tests/host/ota_stubs" \
    "$root/main/asset_mapping.cc" "$root/main/asset_partition.cc" "$root/main/asset_storage.cc" "$root/main/asset_release.cc" \
    "$root/main/firmware_release.cc" "$root/main/asset_update_client.cc" \
    "$root/main/watch/watch_firmware_update.cc" -I "$root/managed_components/78__esp-ml307/include" \
    "$root/tests/host/asset_partition_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/asset-partition-test"
"$build/asset-partition-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$root/main" -I "$root/tests/host" -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/firmware_release.cc" "$root/main/watch/watch_firmware_update.cc" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/tests/host/watch_firmware_update_test.cc" "$build/cJSON.o" "$build/http_parser.o" \
    "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/update-test"
"$build/update-test"
# Only a known public test key is compiled here; production defaults remain empty.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DCONFIG_AUDREAM_WATCH_UI=1 -DCONFIG_AUDREAM_OTA_RELEASE_SEQUENCE=7 '-DBOARD_TYPE="test-watch"' \
    '-DCONFIG_AUDREAM_OTA_API_ORIGIN="https://updates.example"' \
    '-DCONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX="03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"' \
    -I "$root/tests/host/ota_stubs" -I "$root/main" \
    -I "$root/managed_components/78__esp-ml307/include" \
    -isystem "$idf/components/esp_common/include" -isystem "$idf/components/esp_app_format/include" \
    -isystem "$idf/components/bootloader_support/include" \
    -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/ota_upgrade.cc" "$root/main/firmware_release.cc" "$root/main/firmware_release_identity.cc" \
    "$root/main/watch/watch_firmware_update.cc" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/tests/host/ota_signed_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/signed-test"
"$build/signed-test"
# Actual Application update/network methods with controlled board, power and OTA I/O.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DWATCH_FIRMWARE_APPLICATION_TEST=1 -DCONFIG_AUDREAM_WATCH_UI=1 \
    -DCONFIG_AUDREAM_OTA_RELEASE_SEQUENCE=7 '-DBOARD_TYPE="test-watch"' \
    '-DCONFIG_AUDREAM_ASSET_MANIFEST_URL="https://updates.example/models/latest"' \
    '-DCONFIG_AUDREAM_OTA_API_ORIGIN="https://updates.example"' \
    '-DCONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX="03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"' \
    -I "$root/tests/host/firmware_application_stubs" -I "$root/tests/host/voice_application_stubs" \
    -I "$root/tests/host/voice_cloud_stubs" -I "$root/main" -I "$root/tests/host" \
    -I "$sodium/src/libsodium/include" -I "$cjson" -I "$parser" \
    "$root/main/watch/watch_firmware_application.cc" "$root/main/watch/watch_firmware_update.cc" "$root/main/asset_release.cc" \
    "$root/main/watch/watch_network_application.cc" "$root/main/watch/watch_station_policy.cc" \
    "$root/main/watch/watch_station_recovery.cc" "$root/main/watch/watch_wifi_gate.cc" \
    "$root/main/watch/watch_voice_task.cc" "$root/main/audio/audio_session_coordinator.cc" \
    "$root/main/watch/watch_voice_cloud.cc" \
    "$root/main/firmware_release.cc" "$root/main/firmware_release_identity.cc" \
    "$root/tests/host/watch_firmware_application_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/application-test"
"$build/application-test"
if [[ -n "${RELEASE_DISCOVERY_FIXTURE_DIR:-}" ]]; then
    "$build/release-test" "$RELEASE_DISCOVERY_FIXTURE_DIR/api-release.json"
    "$build/signed-test" "$RELEASE_DISCOVERY_FIXTURE_DIR/api-release.json" "$RELEASE_DISCOVERY_FIXTURE_DIR/api-firmware.bin"
fi
if [[ -n "${PYTHON_BIN:-}" ]]; then
    "$PYTHON_BIN" "$root/tools/firmware_release/test_prepare_release.py"
    "$PYTHON_BIN" "$root/tools/firmware_release/test_sign_asset_release.py"
    "$PYTHON_BIN" "$root/tools/firmware_release/test_sign_asset_release.py" --export-fixture "$build"
    "$build/asset-test" "$build/asset-release.json" "$build/assets.bin"
    "$PYTHON_BIN" "$root/tools/firmware_release/test_sign_release.py"
    "$PYTHON_BIN" "$root/tools/firmware_release/test_sign_release.py" --export-fixture "$build"
    "$build/release-test" "$build/release.json"
    "$build/signed-test" "$build/release.json" "$build/firmware.bin"
else
    printf '%s\n' 'Python signer interoperability not run; set PYTHON_BIN to a cryptography-enabled uv environment.'
fi
