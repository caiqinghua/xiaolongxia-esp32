#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/audream-recording-reader.XXXXXX")"
trap 'rm -rf "$build"' EXIT
ogg="$root/main/audio/demuxer/libogg"
(cd "$ogg" && shasum -a 256 -c SHA256SUMS)

# Desktop-only: compile the same framing implementation used by the watch.
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I "$ogg/include" -c "$ogg/src/framing.c" -o "$build/framing.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I "$root/main" -I "$ogg/include" -I "$root/tests/host/recording_stubs" \
    "$root/main/audio/recording_packet_reader.cc" "$root/main/audio/ogg_opus_writer.cc" \
    "$root/tests/host/recording_packet_reader_test.cc" "$build/framing.o" -o "$build/reader_test"
"$build/reader_test" "$build"
