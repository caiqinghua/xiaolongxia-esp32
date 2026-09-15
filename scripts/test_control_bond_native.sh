#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-control-bond-native.XXXXXX")"
trap 'rm -rf "$build"' EXIT
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
sodium="${SODIUM_SOURCE_DIR:?Set SODIUM_SOURCE_DIR to the configured native libsodium source directory}"
# Actual native owner and storage; only radio, clock and NVS I/O are substituted.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DCONFIG_AUDREAM_WATCH_DEVICE_PAIRING=1 -DCONFIG_NVS_ENCRYPTION=1 \
    -DCONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=1 \
    -I "$root/tests/host/control_bond_stubs" -I "$root/tests/host/ancs_stubs" \
    -isystem "$idf/components/bt/host/bluedroid/api/include/api" \
    -isystem "$idf/components/esp_common/include" -isystem "$idf/components/esp_hw_support/include" \
    -isystem "$idf/components/nvs_flash/include" -I "$sodium/src/libsodium/include" -I "$root/main" \
    "$root/main/watch/watch_ancs_session.cc" "$root/main/watch/watch_ancs_client.cc" \
    "$root/main/watch/watch_control_bond.cc" "$root/tests/host/control_bond_fixture.cc" \
    "$root/main/watch/watch_android_notifications.cc" "$root/main/watch/watch_android_gatt_server.cc" \
    "$root/main/watch/watch_gatt_dispatcher.cc" "$root/main/watch/watch_pairing_gatt_server.cc" \
    "$root/main/watch/watch_blufi_security.cc" "$root/tests/host/watch_ancs_client_test.cc" \
    "$sodium/src/libsodium/.libs/libsodium.a" -o "$build/test"
"$build/test" bonds
"$build/test" bonds-clear-failure
for scenario in success unapproved provisional storage-failure api-failure changed-bond absent \
    host-before-poll host-during-write reconnect-during-write timeout clock-rollback wrong-completion \
    callback-failure still-stored other-bond list-race; do
    "$build/test" "forget-$scenario"
done
for scenario in success empty unapproved malformed list-race lifecycle-race poll-race inspection-busy refresh expired \
    clock-rollback reconnect host-stop notifications changed-bond storage-failure; do
    "$build/test" "saved-$scenario"
done
if [[ -n "${AUDREAM_PAIRING_GATT_CLIENT:-}" ]]; then
    "$AUDREAM_PAIRING_GATT_CLIENT" "$build/test"
fi
