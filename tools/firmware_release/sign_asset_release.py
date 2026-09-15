#!/usr/bin/env python3
"""Validate and sign an existing mmap model asset bundle; never build or flash it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import struct
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from sign_release import TOKEN, validate_url

DOMAIN = b"audream-asset-release-v1\n"
MAX_BYTES = 16 * 1024 * 1024


def token(value: str, maximum: int) -> bool:
    return isinstance(value, str) and 0 < len(value) <= maximum and TOKEN.fullmatch(value) is not None


def name(data: bytes) -> str:
    if len(data) != 32 or b"\0" not in data:
        raise ValueError("Asset name must be null terminated")
    value = data.split(b"\0", 1)[0].decode("ascii")
    if not token(value, 31):
        raise ValueError("Invalid asset name")
    return value


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result or "\0" in key:
            raise ValueError("Duplicate or null JSON key")
        result[key] = value
    return result


def parse_index(raw: bytes) -> dict:
    if not raw or len(raw) > 65536 or b"\0" in raw or b"\\u0000" in raw:
        raise ValueError("Invalid asset index")
    depth, quoted, escaped = 0, False, False
    for ch in raw.decode("utf-8"):
        if quoted:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif ch in "[{":
            depth += 1
            if depth > 16:
                raise ValueError("Asset index nesting exceeds the device limit")
        elif ch in "]}":
            depth -= 1
            if depth < 0:
                raise ValueError("Invalid asset index nesting")
    if depth or quoted:
        raise ValueError("Unterminated asset index")
    index = json.loads(raw, object_pairs_hook=unique_object)
    if (not isinstance(index, dict) or type(index.get("version")) not in (int, float)
            or index["version"] != 1 or not token(index.get("srmodels"), 31)):
        raise ValueError("Unsupported asset index version or model reference")
    return index


def validate_bundle(data: bytes, wake_model: str) -> None:
    if not token(wake_model, 31) or not 12 <= len(data) <= MAX_BYTES:
        raise ValueError("Invalid bundle size or wake model")
    count, checksum, length = struct.unpack_from("<III", data)
    if (not 1 <= count <= 1024 or length != len(data) - 12 or count * 44 > length
            or sum(data[12:]) & 0xFFFF != checksum):
        raise ValueError("Invalid mmap header or checksum")
    payload_start = 12 + count * 44
    assets = {}
    for index in range(count):
        entry = 12 + index * 44
        asset_name = name(data[entry:entry + 32])
        size, offset = struct.unpack_from("<II", data, entry + 32)
        start = payload_start + offset
        if (asset_name in assets or start + 2 + size > len(data)
                or data[start:start + 2] != b"ZZ"):
            raise ValueError("Duplicate asset or invalid payload bounds")
        assets[asset_name] = data[start + 2:start + 2 + size]
    index = parse_index(assets.get("index.json", b""))
    models = assets.get(index["srmodels"], b"")
    if len(models) < 4:
        raise ValueError("Missing model bundle")
    model_count, = struct.unpack_from("<I", models)
    if not 1 <= model_count <= 32:
        raise ValueError("Invalid model count")
    cursor, first_payload = 4, len(models)
    names = set()
    for _ in range(model_count):
        if len(models) - cursor < 36:
            raise ValueError("Truncated model header")
        model_name = name(models[cursor:cursor + 32])
        if model_name in names:
            raise ValueError("Duplicate model name")
        names.add(model_name)
        files, = struct.unpack_from("<I", models, cursor + 32)
        cursor += 36
        if not 1 <= files <= 128 or files * 40 > len(models) - cursor:
            raise ValueError("Invalid model file table")
        file_names = set()
        for _ in range(files):
            file_name = name(models[cursor:cursor + 32])
            offset, size = struct.unpack_from("<II", models, cursor + 32)
            if file_name in file_names or not 1 <= size <= 2147483647 or offset + size > len(models):
                raise ValueError("Invalid model file bounds or duplicate name")
            file_names.add(file_name)
            first_payload = min(first_payload, offset)
            cursor += 40
    if first_payload < cursor or wake_model not in names:
        raise ValueError("Model overlaps metadata or required wake model is missing")


def sign_asset_release(image: Path, key_file: Path, model: str, version: str, sequence: int,
                       url: str, firmware_min: int, firmware_max: int,
                       sr_abi: str, wake_model: str) -> dict[str, str]:
    if (not token(model, 95) or not model[0].isalnum() or not token(version, 31)
            or not token(sr_abi, 63) or not token(wake_model, 31)):
        raise ValueError("Invalid model, version or model ABI")
    if (any(type(n) is not int or not 1 <= n <= 2147483647 for n in (sequence, firmware_min, firmware_max))
            or firmware_min > firmware_max):
        raise ValueError("Invalid sequence or firmware compatibility range")
    validate_url(url)
    descriptor = os.open(image, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or not 12 <= info.st_size <= MAX_BYTES:
            raise ValueError("Asset image must be a bounded regular file")
        data = stream.read(MAX_BYTES + 1)
    validate_bundle(data, wake_model)
    if key_file.stat().st_mode & 0o077:
        raise ValueError("Signing private key must not be accessible to group or other users")
    key = serialization.load_pem_private_key(key_file.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("Asset signing requires an Ed25519 private key")
    payload = json.dumps({
        "schema": "audream-assets-v1", "format": "mmap-v1", "model": model, "version": version,
        "sequence": str(sequence), "size": str(len(data)), "sha256": hashlib.sha256(data).hexdigest(),
        "url": url, "firmware_min": str(firmware_min), "firmware_max": str(firmware_max),
        "sr_abi": sr_abi, "wake_model": wake_model,
    }, ensure_ascii=True, separators=(",", ":"))
    return {"payload": payload, "signature": key.sign(DOMAIN + payload.encode()).hex()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("image", "key", "output"):
        parser.add_argument(f"--{flag}", type=Path, required=True)
    for flag in ("model", "version", "url", "sr-abi", "wake-model"):
        parser.add_argument(f"--{flag}", required=True)
    for flag in ("sequence", "firmware-min", "firmware-max"):
        parser.add_argument(f"--{flag}", type=int, required=True)
    args = vars(parser.parse_args())
    output = args.pop("output")
    args["key_file"] = args.pop("key")
    try:
        envelope = sign_asset_release(**args)
        with output.open("x", encoding="utf-8") as stream:
            json.dump(envelope, stream, ensure_ascii=True, separators=(",", ":"))
            stream.write("\n")
    except (OSError, ValueError, TypeError, RecursionError) as error:
        parser.exit(1, f"Asset signing failed: {error}\n")
    print(f"Signed asset manifest created: {output}")


if __name__ == "__main__":
    main()
