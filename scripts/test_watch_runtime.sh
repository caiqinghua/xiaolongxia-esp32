#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-watch-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
: "${SODIUM_SOURCE_DIR:?Run scripts/prepare_host_pairing_crypto.sh and set SODIUM_SOURCE_DIR for the native security suites}"

# Host-only tests: do not invoke ESP-IDF, modify sdkconfig, or create firmware.
bash "$root/scripts/test_recording_reader.sh"
bash "$root/scripts/test_ota_upgrade.sh"
bash "$root/scripts/test_recording_playback.sh"
bash "$root/scripts/test_watch_opus_writer.sh"
bash "$root/scripts/test_watch_stream_encoder.sh"
bash "$root/scripts/test_watch_alert_sound.sh"
bash "$root/scripts/test_watch_notifications.sh"
bash "$root/scripts/test_control_bond.sh"
bash "$root/scripts/test_control_bond_native.sh"
bash "$root/scripts/test_blufi_control.sh"
bash "$root/scripts/test_blufi_startup.sh"
bash "$root/scripts/test_blufi_overlay.sh"
bash "$root/scripts/test_watch_wrist.sh"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/storage/recording_file_deletion.cc" "$root/tests/host/recording_file_deletion_test.cc" \
    -o "$build_dir/recording_file_deletion_test"
"$build_dir/recording_file_deletion_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_activity_record.cc" \
    "$root/tests/host/watch_activity_test.cc" -o "$build_dir/watch_activity_test"
"$build_dir/watch_activity_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_activity_record.cc" \
    "$root/main/watch/watch_motion_task.cc" "$root/tests/host/watch_motion_task_test.cc" \
    -o "$build_dir/watch_motion_task_test"
"$build_dir/watch_motion_task_test"
bash "$root/scripts/test_voice_cloud.sh"
bash "$root/scripts/test_audio_processors.sh"
bash "$root/scripts/test_wake_word.sh"
bash "$root/scripts/test_ap_transfer.sh"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/watch_level_stubs" -I "$root/tests/host/audio_processor_stubs" \
    -I "$root/tests/host/transport_stubs" -I "$root/main" \
    "$root/main/watch/watch_level_settings.cc" "$root/main/watch/watch_voice_settings.cc" \
    "$root/main/watch/watch_notification_settings.cc" \
    "$root/main/watch/watch_wifi_settings.cc" "$root/main/audio/audio_codec.cc" \
    "$root/tests/host/watch_level_settings_test.cc" \
    -o "$build_dir/watch_level_settings_test"
"$build_dir/watch_level_settings_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_wifi_gate.cc" "$root/tests/host/watch_wifi_gate_test.cc" \
    -o "$build_dir/watch_wifi_gate_test"
"$build_dir/watch_wifi_gate_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_station_policy.cc" "$root/tests/host/watch_station_policy_test.cc" \
    -o "$build_dir/watch_station_policy_test"
"$build_dir/watch_station_policy_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_station_recovery.cc" "$root/main/watch/watch_wifi_gate.cc" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/tests/host/watch_station_recovery_test.cc" \
    -o "$build_dir/watch_station_recovery_test"
"$build_dir/watch_station_recovery_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_push_to_talk.cc" "$root/tests/host/watch_push_to_talk_test.cc" \
    -o "$build_dir/watch_push_to_talk_test"
"$build_dir/watch_push_to_talk_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/tests/host/watch_recording_test.cc" -o "$build_dir/watch_recording_test"
"$build_dir/watch_recording_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_assistant_state.cc" "$root/main/audio/audio_session_coordinator.cc" \
    "$root/tests/host/watch_assistant_test.cc" -o "$build_dir/watch_assistant_test"
"$build_dir/watch_assistant_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main/boards/common" \
    "$root/tests/host/watch_runtime_test.cc" -o "$build_dir/watch_runtime_test"
"$build_dir/watch_runtime_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/recording_stubs" -I "$root/main" \
    "$root/main/device_state_machine.cc" "$root/tests/host/device_startup_test.cc" \
    -o "$build_dir/device_startup_test"
"$build_dir/device_startup_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/panel_stubs" -I "$root/main" \
    "$root/main/display/sh8601_panel_factory.cc" "$root/tests/host/panel_factory_test.cc" \
    -o "$build_dir/panel_factory_test"
"$build_dir/panel_factory_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main/audio" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/tests/host/audio_session_test.cc" -o "$build_dir/audio_session_test"
"$build_dir/audio_session_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/assistant_session_lifecycle.cc" \
    "$root/tests/host/assistant_session_test.cc" -o "$build_dir/assistant_session_test"
"$build_dir/assistant_session_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/main/protocols/assistant_transport_handoff.cc" \
    "$root/tests/host/assistant_transport_handoff_test.cc" -o "$build_dir/assistant_transport_handoff_test"
"$build_dir/assistant_transport_handoff_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/recording_stubs" -I "$root/main" \
    "$root/main/audio/audio_session_coordinator.cc" \
    "$root/main/audio/local_recording_service.cc" \
    "$root/main/audio/assistant_session_lifecycle.cc" \
    "$root/main/audio/ogg_opus_writer.cc" \
    "$root/tests/host/local_recording_test.cc" -o "$build_dir/local_recording_test"
"$build_dir/local_recording_test" "$build_dir/recordings"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main/watch" "$root/main/watch/watch_time_tools.cc" \
    "$root/tests/host/watch_time_test.cc" -o "$build_dir/watch_time_test"
"$build_dir/watch_time_test"

cjson_dir="${CJSON_SOURCE_DIR:-${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}/components/json/cJSON}"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
    printf '%s\n' "Set CJSON_SOURCE_DIR to the cJSON source directory for watch persistence tests." >&2
    exit 1
fi
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"
http_parser_dir="${HTTP_PARSER_SOURCE_DIR:-${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}/components/http_parser}"
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$http_parser_dir" -c "$http_parser_dir/http_parser.c" -o "$build_dir/http_parser.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" -I "$cjson_dir" \
    "$root/main/watch/watch_recording_library.cc" "$root/tests/host/watch_recording_library_test.cc" \
    "$build_dir/cJSON.o" -o "$build_dir/watch_recording_library_test"
"$build_dir/watch_recording_library_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" -I "$cjson_dir" \
    "$root/main/protocols/assistant_wire_format.cc" "$root/tests/host/assistant_wire_test.cc" \
    "$build_dir/cJSON.o" -o "$build_dir/assistant_wire_test"
"$build_dir/assistant_wire_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main" -I "$cjson_dir" \
    "$root/main/protocols/assistant_wire_format.cc" "$root/main/protocols/assistant_receive_queue.cc" \
    "$root/tests/host/assistant_receive_queue_test.cc" "$build_dir/cJSON.o" \
    -o "$build_dir/assistant_receive_queue_test"
"$build_dir/assistant_receive_queue_test"
for watch in 0 1; do
    "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -DCONFIG_AUDREAM_WATCH_UI="$watch" \
        -I "$root/tests/host/protocol_stubs" -I "$root/main" -I "$cjson_dir" -I "$http_parser_dir" \
        "$root/main/protocols/assistant_wire_format.cc" "$root/main/protocols/assistant_receive_queue.cc" \
        "$root/main/protocols/protocol.cc" \
        "$root/main/watch/watch_voice_cloud.cc" \
        "$root/main/audio/audio_session_coordinator.cc" "$root/main/audio/assistant_session_lifecycle.cc" \
        "$root/main/protocols/websocket_protocol.cc" "$root/tests/host/websocket_protocol_test.cc" \
        "$build_dir/cJSON.o" "$build_dir/http_parser.o" -o "$build_dir/websocket_protocol_test"
    "$build_dir/websocket_protocol_test"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/main/watch" -I "$cjson_dir" \
    "$root/main/watch/watch_time_tools.cc" "$root/main/watch/watch_time_state.cc" \
    "$root/main/watch/watch_calendar.cc" "$root/main/watch/watch_time_service.cc" \
    "$root/tests/host/watch_time_service_test.cc" "$build_dir/cJSON.o" \
    -o "$build_dir/watch_time_service_test"
"$build_dir/watch_time_service_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" -I "$cjson_dir" \
    "$root/main/watch/watch_time_tools.cc" "$root/main/watch/watch_time_state.cc" \
    "$root/main/watch/watch_calendar.cc" "$root/main/watch/watch_time_service.cc" \
    "$root/main/watch/watch_time_mcp.cc" "$root/tests/host/watch_time_mcp_test.cc" \
    "$build_dir/cJSON.o" -o "$build_dir/watch_time_mcp_test"
"$build_dir/watch_time_mcp_test"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DCONFIG_PM_ENABLE=1 \
    -I "$root/tests/host/power_stubs" -I "$root/main" -I "$cjson_dir" \
    "$root/main/audio/audio_session_coordinator.cc" "$root/main/system/audream_power_manager.cc" \
    "$root/main/watch/watch_time_tools.cc" "$root/main/watch/watch_time_state.cc" \
    "$root/main/watch/watch_calendar.cc" "$root/main/watch/watch_time_service.cc" \
    "$root/tests/host/watch_sleep_test.cc" "$build_dir/cJSON.o" \
    -o "$build_dir/watch_sleep_test"
"$build_dir/watch_sleep_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/tests/host/watch_runtime_stubs" -I "$root/tests/host/recording_stubs" \
    -I "$root/main" -I "$cjson_dir" \
    "$root/main/watch/watch_time_tools.cc" "$root/main/watch/watch_time_state.cc" \
    "$root/main/watch/watch_calendar.cc" "$root/main/watch/watch_time_service.cc" \
    "$root/main/watch/watch_time_runtime.cc" "$root/tests/host/watch_nvs_test.cc" "$build_dir/cJSON.o" \
    -o "$build_dir/watch_nvs_test"
for scenario in restore read_failures save_failures; do
    "$build_dir/watch_nvs_test" "$scenario"
done

bash "$root/scripts/test_transport_lifecycle.sh"
