#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-control-bond.XXXXXX")"
trap 'rm -rf "$build"' EXIT
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
sodium="${SODIUM_SOURCE_DIR:?Set SODIUM_SOURCE_DIR to the configured native libsodium source directory}"
# Native domain code with real SDK public types; no firmware build or radio access.
for protected in 0 1; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -DCONFIG_NVS_ENCRYPTION="$protected" -DCONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC="$protected" \
        -I "$root/tests/host/control_bond_stubs" -I "$root/tests/host/ancs_stubs" \
        -isystem "$idf/components/bt/host/bluedroid/api/include/api" \
        -isystem "$idf/components/esp_common/include" -isystem "$idf/components/esp_hw_support/include" \
        -isystem "$idf/components/nvs_flash/include" -I "$sodium/src/libsodium/include" -I "$root/main" \
        "$root/main/watch/watch_control_bond.cc" "$root/tests/host/watch_control_bond_test.cc" \
        "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/test_$protected"
    "$build/test_$protected"
done
"$build/test_1" set
"$build/test_1" commit
"$build/test_1" set-revoke
"$build/test_1" commit-revoke
