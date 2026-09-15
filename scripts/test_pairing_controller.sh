#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-pairing-controller.XXXXXX")"
trap 'rm -rf "$build"' EXIT
sodium="${SODIUM_SOURCE_DIR:?Set SODIUM_SOURCE_DIR to the configured native libsodium source directory}"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
cjson="$idf/components/json/cJSON"
parser="$idf/components/http_parser"
bt="$idf/components/bt/host/bluedroid/api/include/api"
# Native controller, signer, binding, HTTP client and pthreads. No firmware build,
# radio, network, factory provisioning, or production storage is accessed.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -I "$cjson" -c "$cjson/cJSON.c" -o "$build/cJSON.o"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -I "$parser" -c "$parser/http_parser.c" -o "$build/http_parser.o"
for kind in controller application; do
  extra=(-DCONFIG_AUDREAM_WATCH_DEVICE_PAIRING=0)
  if [[ "$kind" == application ]]; then
    extra=(-DCONFIG_AUDREAM_WATCH_DEVICE_PAIRING=1
      -I "$root/tests/host/pairing_application_stubs" -I "$root/tests/host/voice_application_stubs"
      "$root/main/watch/watch_pairing_application.cc" "$root/main/audio/audio_session_coordinator.cc"
      "$root/main/protocols/assistant_transport_handoff.cc" "$root/main/watch/watch_wifi_gate.cc")
  fi
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread "${extra[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DCONFIG_NVS_ENCRYPTION=1 -DCONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=1 \
    -I "$root/tests/host/voice_cloud_stubs" -I "$root/tests/host/ancs_stubs" \
    -isystem "$bt" -isystem "$idf/components/esp_common/include" \
    -isystem "$idf/components/esp_hw_support/include" \
    -I "$idf/components/nvs_flash/include" -I "$idf/components/esp_http_client/include" \
    -I "$sodium/src/libsodium/include" -I "$root/main" -I "$cjson" -I "$parser" \
    "$root/main/watch/watch_pairing_controller.cc" "$root/main/watch/watch_voice_task.cc" \
    "$root/main/watch/watch_voice_identity.cc" "$root/main/watch/watch_voice_pairing.cc" \
    "$root/main/watch/watch_voice_binding.cc" "$root/main/watch/watch_voice_cloud.cc" \
    "$root/main/watch/watch_voice_cloud_client.cc" "$root/main/watch/watch_ancs_session.cc" \
    "$root/tests/host/watch_pairing_${kind}_test.cc" \
    "$build/cJSON.o" "$build/http_parser.o" "$sodium/src/libsodium/.libs/libsodium.a" \
    -o "$build/${kind}_test"
done
for scenario in success approval invalid-challenge clock identity unprotected open-failed cancel-prepare \
    cancel-http disconnect-http recording-http cancel-before-write cancel-during-write disconnect-during-write \
    stop-during-write wrong-account unauthorized tls-clock wrong-wire write-failed commit-failed \
    cancel-commit-failed stop-commit-failed cancel-acquire expired-window acquire-failed destructor-join \
    generation-conflict stop-completed-write control-success control-reject control-open-failed \
    control-cancel control-cancel-completed control-timeout control-stop \
    remove-success remove-absent remove-unprotected remove-reject remove-timeout remove-clock-rollback \
    remove-cancel-read remove-cancel-before-write remove-cancel-write remove-stop-write remove-stop-completed \
    remove-generation-conflict remove-write-failed remove-commit-failed remove-cancel-commit-failed \
    forget-success forget-reject forget-cancel-before forget-expired forget-clock-rollback forget-admission-failed \
    forget-storage-failed forget-cancel-failed forget-pending-io forget-cancel-active forget-stop-active forget-stop-completed \
    saved-success saved-empty saved-read-failed saved-cancel-read saved-stop-read saved-stale saved-expired \
    saved-clock-rollback saved-reject saved-cancel-active saved-pending-io saved-storage-failed saved-selected-stale \
    saved-all-success saved-all-reject saved-all-second-failed; do
    "$build/controller_test" "$scenario"
done
for scenario in success offline provisioning wake-running busy storage-failure network-lease-failure \
    ble-lease-failure cancel-queued-open stale-cancel reject-after-approval stale-rejection \
    cancel-during-write cancel-commit-failed wifi-off recording-handoff handoff-timeout \
    local-offline local-cancel local-recording local-reconnect native-not-ready profile-not-ready \
    remove-success remove-enabled remove-busy remove-storage-failure remove-protocol-busy \
    remove-queued-cancel remove-enable-cancel remove-reject remove-cancel-write \
    forget-success forget-enabled forget-busy forget-starting forget-storage-lease forget-ble-lease forget-queued-cancel \
    forget-reject forget-enable-cancel forget-cancel-active forget-recording-handoff forget-failed \
    saved-success saved-enabled saved-busy saved-native-busy saved-storage-lease saved-ble-lease \
    saved-reject saved-enable-cancel saved-cancel-active saved-queued-cancel saved-recording-handoff \
    saved-all-success saved-all-reject saved-all-second-failed; do
    "$build/application_test" "$scenario"
done
