#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${WATCH_PAIRING_BUILD_DIR:-${TMPDIR:-/tmp}/audream-watch-pairing-host}"
sodium="${SODIUM_SOURCE_DIR:?Set SODIUM_SOURCE_DIR to the configured native libsodium source directory}"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
cjson="$idf/components/json/cJSON"
parser="$idf/components/http_parser"
mkdir -p "$build"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -I "$cjson" -c "$cjson/cJSON.c" -o "$build/cJSON.o"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -I "$parser" -c "$parser/http_parser.c" -o "$build/http_parser.o"
# Native crypto and firmware domain code; no ESP-IDF configure/build or hardware access.
for protected in 0 1; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -DCONFIG_NVS_ENCRYPTION="$protected" -DCONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC="$protected" \
        -I "$root/tests/host/voice_cloud_stubs" -I "$idf/components/nvs_flash/include" \
        -I "$sodium/src/libsodium/include" -I "$root/main" -I "$cjson" -I "$parser" \
        "$root/main/watch/watch_voice_identity.cc" "$root/main/watch/watch_voice_pairing.cc" \
        "$root/main/watch/watch_voice_cloud.cc" "$root/tests/host/watch_voice_pairing_test.cc" \
        "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" \
        -o "$build/watch_voice_pairing_$protected"
    "$build/watch_voice_pairing_$protected"
done
printf 'Native API-contract signer: %s/watch_voice_pairing_1\n' "$build"
