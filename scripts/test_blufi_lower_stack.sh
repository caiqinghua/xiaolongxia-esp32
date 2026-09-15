#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-blufi-lower-stack.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# Desktop source test only. Never build an ESP image or modify the SDK.
cmake -S "$root/tests/host/blufi_lower_stack" -B "$build" -DIDF_BT_DIR="$idf/components/bt" -DREPO_ROOT="$root"
cmake --build "$build" --target lower_stack_test lower_stack_patched_test -j 2
"$build/lower_stack_test"
"$build/lower_stack_patched_test"
