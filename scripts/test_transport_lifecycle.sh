#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audream-transport-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT

# Exercise the same nested-worktree layout used by an in-repository IDF build.
git -C "$build_dir" init --quiet
for enabled in ON OFF ON; do
    cmake -S "$root/tests/host/transport_overlay_project" -B "$build_dir/$enabled" \
        -DREPO_ROOT="$root" -DAUDREAM_TEST_ENABLE="$enabled" \
        -DCMAKE_CXX_COMPILER="${CXX:-c++}" >/dev/null
done
printf '%s\n' "PASS: 2 transport overlay configuration test groups"

overlay="$build_dir/ON/audream_components/esp-ml307"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Dsocket=host_socket -Dconnect=host_connect -Dclose=host_close \
    -Dshutdown=host_shutdown -Drecv=host_recv -Dsend=host_send -Dgethostbyname=host_gethostbyname \
    -I "$root/tests/host/transport_stubs" -I "$overlay/include" -I "$overlay/src/esp" \
    "$overlay/src/esp/esp_tcp.cc" "$overlay/src/esp/esp_ssl.cc" "$overlay/src/web_socket.cc" \
    "$root/tests/host/transport_lifecycle_test.cc" -o "$build_dir/transport_lifecycle_test"
"$build_dir/transport_lifecycle_test"
