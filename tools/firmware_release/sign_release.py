#!/usr/bin/env python3
"""Sign an existing firmware image's release metadata; never build or flash it."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

DOMAIN = b"audream-firmware-release-v1\n"
TOKEN = re.compile(r"[A-Za-z0-9_.+-]+", re.ASCII)


def validate_url(url: str) -> None:
    if not url or len(url) > 512 or any(ord(ch) <= 32 or ord(ch) >= 127 or ch == "\\" for ch in url):
        raise ValueError("Firmware URL must be printable ASCII HTTPS")
    parsed = urlsplit(url)
    if (not url.startswith("https://") or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or "?" in url or "#" in url or parsed.port == 0):
        raise ValueError("Firmware URL must use HTTPS without user information, query or fragment")


def sign_release(image: Path, key_file: Path, model: str, sequence: int, url: str) -> dict[str, str]:
    if not TOKEN.fullmatch(model) or len(model) > 95 or not model[0].isalnum():
        raise ValueError("Invalid firmware model")
    if type(sequence) is not int or not 1 <= sequence <= 2147483647:
        raise ValueError("Release sequence must be between 1 and 2147483647")
    validate_url(url)
    if key_file.stat().st_mode & 0o077:
        raise ValueError("Signing private key must not be accessible to group or other users")
    key = serialization.load_pem_private_key(key_file.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("Release signing requires an Ed25519 private key")
    digest = hashlib.sha256()
    size = 0
    with image.open("rb") as stream:
        prefix = stream.read(396)
        if len(prefix) != 396 or prefix[0] != 0xE9 or struct.unpack_from("<I", prefix, 32)[0] != 0xABCD5432:
            raise ValueError("Firmware is missing the ESP application descriptor")
        magic, image_sequence, raw_model = struct.unpack_from("<8sI96s", prefix, 288)
        if (magic != b"AUDOTA1\0" or image_sequence != sequence or b"\0" not in raw_model
                or raw_model.split(b"\0", 1)[0].decode("ascii") != model):
            raise ValueError("Release model or sequence does not match the embedded image identity")
        raw_version = prefix[48:80]
        if b"\0" not in raw_version:
            raise ValueError("Firmware version is not terminated")
        version = raw_version.split(b"\0", 1)[0].decode("ascii")
        if not TOKEN.fullmatch(version) or len(version) > 31:
            raise ValueError("Invalid firmware version")
        digest.update(prefix)
        size += len(prefix)
        for chunk in iter(lambda: stream.read(65536), b""):
            size += len(chunk)
            if size > 32 * 1024 * 1024:
                raise ValueError("Firmware exceeds maximum release size")
            digest.update(chunk)
    payload = json.dumps({
        "schema": "audream-firmware-v1", "model": model, "version": version,
        "sequence": str(sequence), "size": str(size), "sha256": digest.hexdigest(), "url": url,
    }, ensure_ascii=True, separators=(",", ":"))
    return {"payload": payload, "signature": key.sign(DOMAIN + payload.encode("utf-8")).hex()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--sequence", type=int, required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        envelope = sign_release(args.image, args.key, args.model, args.sequence, args.url)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(envelope, stream, ensure_ascii=True, separators=(",", ":"))
            stream.write("\n")
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f"Release signing failed: {error}\n")
    print(f"Signed release manifest created: {args.output}")


if __name__ == "__main__":
    main()
