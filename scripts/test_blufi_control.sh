#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-blufi-control.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Host-only application admission tests; no firmware build or Bluetooth access.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/main" \
    "$root/tests/host/blufi_control_session_test.cc" -o "$build/control_test"
"$build/control_test"
