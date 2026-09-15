#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-watch-motion.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT

# Desktop tests only. Never invoke ESP-IDF or modify the firmware configuration.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_activity_record.cc" \
    "$root/tests/host/watch_activity_test.cc" \
    -o "$build_dir/watch_activity_test"
"$build_dir/watch_activity_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_activity_record.cc" \
    "$root/main/watch/watch_motion_task.cc" "$root/tests/host/watch_motion_task_test.cc" \
    -o "$build_dir/watch_motion_task_test"
"$build_dir/watch_motion_task_test"

idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$idf/components/nvs_flash/include" -I "$root/tests/host/motion_storage_stubs" \
    -I "$root/tests/host/voice_cloud_stubs" -I "$root/main" \
    "$root/main/watch/watch_activity.cc" "$root/main/watch/watch_wrist_control.cc" \
    "$root/main/watch/watch_activity_record.cc" "$root/main/watch/watch_motion_storage.cc" \
    "$root/tests/host/watch_motion_storage_test.cc" -o "$build_dir/watch_motion_storage_test"
"$build_dir/watch_motion_storage_test"

sensorlib="${SENSORLIB_SOURCE_DIR:?Set SENSORLIB_SOURCE_DIR to the pinned SensorLib checkout}"
revision=f3cc656a71ea9500f12ecbd4c4078ce9bd0282ea
if [[ "$(git -C "$sensorlib" rev-parse HEAD)" != "$revision" ]] || \
    [[ -n "$(git -C "$sensorlib" status --porcelain)" ]]; then
    printf '%s\n' "SensorLib must be an unmodified checkout of $revision" >&2
    exit 1
fi
cmake -DSENSORLIB="$sensorlib" -DOUTPUT="$build_dir/sensorlib" \
    -P "$root/tests/host/motion_overlay_test.cmake"
cmake -S "$root/tests/host/motion_overlay" -B "$build_dir/overlay-config" -DSENSORLIB="$sensorlib"
sensorlib="$build_dir/sensorlib"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -Wno-unused-parameter -Wno-gnu-zero-variadic-macro-arguments \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DINPUT=0 -I "$root/main" -isystem "$sensorlib/src" \
    "$sensorlib/src/sensor/imu/qmi8658/SensorQMI8658.cpp" \
    "$root/main/watch/watch_qmi8658.cc" "$root/tests/host/watch_qmi8658_test.cc" \
    -o "$build_dir/watch_qmi8658_test"
"$build_dir/watch_qmi8658_test"
