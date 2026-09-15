#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-ble-link.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Desktop tests only. SDK public types are real; Bluetooth I/O is controlled.
for setting in \
    CONFIG_BT_CTRL_MODEM_SLEEP=y \
    CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y \
    CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y \
    CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y; do
    grep -Fq "\"$setting\"" \
        "$root/main/boards/waveshare/esp32-s3-touch-amoled-2.06/config.json"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_ble_advertising_policy.cc" \
    "$root/tests/host/watch_ble_advertising_policy_test.cc" -o "$build/advertising-policy"
"$build/advertising-policy"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_ble_link_policy.cc" "$root/tests/host/watch_ble_link_policy_test.cc" \
    -o "$build/policy"
"$build/policy"
awk '
    $0 == "esp_err_t Blufi::_gap_register_callback() {" { found = 1 }
    found { print }
    found && $0 == "}" { exit }
    END { if (!found) exit 1 }
' "$root/main/boards/common/blufi.cpp" > "$build/gap_method.inc"
for ancs in 0 1; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
        -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_AUDREAM_WATCH_ANCS="$ancs" \
        -DCONFIG_AUDREAM_WATCH_UI=1 \
        -I "$root/tests/host/ble_link_stubs" -I "$root/tests/host/ancs_stubs" -I "$root/main" -I "$build" \
        -isystem "$idf/components/bt/host/bluedroid/api/include/api" \
        -isystem "$idf/components/bt/common/api/include/api" \
        -isystem "$idf/components/esp_common/include" \
        -isystem "$idf/components/esp_wifi/include" -isystem "$idf/components/esp_event/include" \
        -isystem "$idf/components/esp_hw_support/include" \
        "$root/main/watch/watch_ble_advertising_policy.cc" "$root/main/watch/watch_ble_advertising_blufi.cc" \
        "$root/main/watch/watch_ble_link_policy.cc" "$root/main/watch/watch_ble_link_blufi.cc" \
        "$root/tests/host/watch_ble_link_blufi_test.cc" -o "$build/adapter-$ancs"
    "$build/adapter-$ancs"
done
