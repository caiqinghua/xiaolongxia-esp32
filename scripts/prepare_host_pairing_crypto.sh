#!/usr/bin/env bash
set -euo pipefail
# Prepare only a native test library in a dedicated cache. Never configure ESP-IDF.
cache="${WATCH_PAIRING_CRYPTO_CACHE:-$HOME/.cache/audream/host-pairing-crypto-1.0.22-1}"
archive="$cache/component.zip"
source="$cache/source/libsodium/libsodium"
expected=d8bbd5dddbc1744869ba1a5c7202cddc4204a6fd63cae6f59f7c3522ac8e9f27
mkdir -p "$cache"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --max-time 120 \
        'https://components.espressif.com/api/downloads/?object_id=bfecfe37-5cd3-4ddf-8f59-490e188b7cb7&object_type=component' \
        --output "$archive.partial"
    mv "$archive.partial" "$archive"
fi
actual="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
if [[ "$actual" != "$expected" ]]; then
    printf '%s\n' 'Pairing crypto archive checksum mismatch; cached source was not used.' >&2
    exit 1
fi
if [[ ! -d "$cache/source" ]]; then
    unzip -q "$archive" -d "$cache/source"
fi
if [[ ! -f "$source/src/libsodium/.libs/libsodium.a" ]]; then
    cd "$source"
    bash configure --disable-shared --enable-static --disable-dependency-tracking > "$cache/configure.log" 2>&1
    make -j4 > "$cache/build.log" 2>&1
fi
printf 'Native libsodium ready. Set SODIUM_SOURCE_DIR=%s\n' "$source"
