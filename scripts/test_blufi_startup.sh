#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-blufi-startup.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Compile the actual lifecycle methods, replacing only platform dependencies.
# Select the first definition because the source also contains a NimBLE variant.
for signature in 'esp_err_t Blufi::init(BlufiRole role) {' \
    'esp_err_t Blufi::_host_init() {' 'esp_err_t Blufi::_host_and_cb_init() {' \
    'esp_err_t Blufi::_host_deinit() {' 'esp_err_t Blufi::_controller_deinit() {' \
    'esp_err_t Blufi::AbortSecureHostStart() {' 'esp_err_t Blufi::DeinitLocked() {' \
    'void Blufi::HandleProfileInitialized(bool success) {' 'bool Blufi::IsControlReady() const {' \
    'void Blufi::start_wifi_scan() {' \
    'esp_err_t Blufi::deinit() {' \
    'uint64_t Blufi::AcquireControlLease() {' 'bool Blufi::ReleaseControlLease(uint64_t lease) {'; do
    awk -v signature="$signature" '
        $0 == signature {
            found = 1
            guarded = signature == "esp_err_t Blufi::AbortSecureHostStart() {"
            if (guarded) print "#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING"
        }
        found { print }
        found && $0 == "}" { if (guarded) print "#endif"; exit }
        END { if (!found) exit 1 }
    ' "$root/main/boards/common/blufi.cpp" >> "$build/methods.inc"
done
for secure in 0 1; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -DCONFIG_AUDREAM_WATCH_DEVICE_PAIRING="$secure" \
        -DCONFIG_AUDREAM_WATCH_ANCS=1 -DCONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS=1 \
        -DCONFIG_BT_CONTROLLER_ENABLED=1 -DCONFIG_BT_NIMBLE_ENABLED=0 \
        -I "$root/main" -I "$build" -I "$root/tests/host/blufi_startup_stubs" \
        -I "$root/tests/host/ancs_stubs" \
        -isystem "$idf/components/bt/include/esp32c3/include" \
        -isystem "$idf/components/bt/host/bluedroid/api/include/api" \
        -isystem "$idf/components/esp_common/include" \
        "$root/tests/host/blufi_startup_test.cc" -o "$build/test-$secure"
    "$build/test-$secure"
done
