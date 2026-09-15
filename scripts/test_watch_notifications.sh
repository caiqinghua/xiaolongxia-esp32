#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-notifications.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Host protocol tests only. Do not build firmware or access Bluetooth hardware.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_ancs_session.cc" "$root/tests/host/watch_ancs_session_test.cc" \
    -o "$build/notifications_test"
"$build/notifications_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_android_notifications.cc" "$root/tests/host/watch_android_notifications_test.cc" \
    -o "$build/android_notifications_test"
if [[ -n "${AUDREAM_NOTIFICATION_FIXTURE:-}" ]]; then
    "$build/android_notifications_test" "$AUDREAM_NOTIFICATION_FIXTURE"
else
    "$build/android_notifications_test"
fi
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
bt="$idf/components/bt/host/bluedroid/api/include/api"
if [[ ! -f "$bt/esp_gattc_api.h" ]]; then
    printf '%s\n' "Set IDF_PATH to ESP-IDF v5.5.4 for native ANCS API header tests." >&2
    exit 1
fi
# Compile the real client and real SDK public types; replace only native I/O
# and the monotonic clock. This does not build ESP-IDF or a firmware image.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/ancs_stubs" -isystem "$bt" -isystem "$idf/components/esp_common/include" \
    -isystem "$idf/components/esp_hw_support/include" -I "$root/main" \
    "$root/main/watch/watch_ancs_session.cc" "$root/main/watch/watch_ancs_client.cc" \
    "$root/main/watch/watch_android_notifications.cc" "$root/main/watch/watch_android_gatt_server.cc" \
    "$root/main/watch/watch_gatt_dispatcher.cc" \
    "$root/main/watch/watch_pairing_gatt_server.cc" \
    "$root/main/watch/watch_blufi_security.cc" \
    "$root/tests/host/watch_ancs_client_test.cc" -o "$build/ancs_client_test"
"$build/ancs_client_test"
if [[ -n "${AUDREAM_PAIRING_GATT_CLIENT:-}" ]]; then
    "$AUDREAM_PAIRING_GATT_CLIENT" "$build/ancs_client_test"
fi
