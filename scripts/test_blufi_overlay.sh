#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
source_file="$idf/components/bt/common/btc/profile/esp/blufi/bluedroid_host/esp_blufi.c"
sources=("$source_file" "$idf/components/bt/common/api/esp_blufi_api.c"
    "$idf/components/bt/common/btc/profile/esp/blufi/blufi_prf.c"
    "$idf/components/bt/common/btc/profile/esp/blufi/blufi_protocol.c"
    "$idf/components/bt/host/bluedroid/bta/gatt/bta_gatts_api.c"
    "$idf/components/bt/host/bluedroid/bta/gatt/bta_gatts_act.c"
    "$idf/components/bt/host/bluedroid/stack/l2cap/l2c_link.c"
    "$idf/components/bt/host/bluedroid/bta/gatt/include/bta_gatts_int.h")
source_hash="$(shasum -a 256 "${sources[@]}")"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-blufi-overlay.XXXXXX")"
trap 'rm -rf "$build"' EXIT
for enabled in OFF ON; do
    cmake -S "$root/tests/host/blufi_overlay" -B "$build/$enabled" \
        -DREPO_ROOT="$root" -DIDF_BT_DIR="$idf/components/bt" -DAUDREAM_TEST_ENABLE="$enabled"
done
cmake --build "$build/ON" --target sdk_test -j 4
"$build/ON/sdk_test"
cmake --build "$build/ON" --target queue_test -j 4
"$build/ON/queue_test"
cmake --build "$build/ON" --target receive_test -j 4
"$build/ON/receive_test"
cmake --build "$build/ON" --target scope_test -j 4
"$build/ON/scope_test"
cmake --build "$build/ON" --target bta_test -j 4
"$build/ON/bta_test"
for rejected in TAMPER HEADER LOWER_STACK MISSING SMP VERSION; do
    extra=(-D"AUDREAM_TEST_$rejected"=ON)
    if [[ "$rejected" == SMP ]]; then extra=(-DCONFIG_BT_BLUFI_BLE_SMP_ENABLE=ON); fi
    if cmake -S "$root/tests/host/blufi_overlay" -B "$build/$rejected" \
        -DREPO_ROOT="$root" -DIDF_BT_DIR="$idf/components/bt" -DAUDREAM_TEST_ENABLE=ON \
        "${extra[@]}" > "$build/$rejected.log" 2>&1; then
        printf 'FAIL: unsafe BLUFI overlay configuration accepted: %s\n' "$rejected" >&2
        exit 1
    fi
    case "$rejected" in
        TAMPER) reason='requires the reviewed ESP-IDF' ;;
        HEADER) reason='requires the reviewed BTA argument layout' ;;
        LOWER_STACK) reason='requires the reviewed ESP-IDF' ;;
        MISSING) reason='Expected each reviewed BLUFI source exactly once' ;;
        SMP) reason='requires the shared native security owner' ;;
        VERSION) reason='requires ESP-IDF version 5.5.4' ;;
    esac
    if ! grep -q "$reason" "$build/$rejected.log"; then
        cat "$build/$rejected.log"
        exit 1
    fi
done
[[ "$source_hash" == "$(shasum -a 256 "${sources[@]}")" ]]
printf '%s\n' 'PASS: SDK source unchanged; incompatible version, source, target and automatic SMP configurations rejected'
