#!/usr/bin/env python3
"""Prepare per-watch factory identity inputs without flashing or contacting an API."""

import argparse
import base64
import csv
import io
import json
import os
from pathlib import Path
import re
import stat
from uuid import UUID

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def exclusive_write(path: Path, data: bytes):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def public_bytes(seed: bytes) -> bytes:
    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def prepare(directory: Path, hardware_id: str):
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", hardware_id):
        raise ValueError("Invalid hardware ID")
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    seed = os.urandom(32)
    public = public_bytes(seed)
    exclusive_write(directory / "pairing-seed.bin", seed)
    exclusive_write(directory / "pairing-public-key.base64", base64.b64encode(public) + b"\n")
    manifest = {"format": "audream-identity-v1", "hardware_id": hardware_id,
                "public_key": base64.b64encode(public).decode("ascii")}
    exclusive_write(directory / "public-manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())


def finalize(directory: Path, enrollment: Path):
    """Use the administrator's exact enrollment response, not a guessed device UUID."""
    seed_path = directory / "pairing-seed.bin"
    metadata = seed_path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or stat.S_IMODE(metadata.st_mode) & 0o077:
        raise ValueError("Pairing seed must be an owner-only regular file")
    seed = seed_path.read_bytes()
    if len(seed) != 32 or seed == bytes(32):
        raise ValueError("Invalid pairing seed")
    manifest = json.loads((directory / "public-manifest.json").read_text())
    registered = json.loads(enrollment.read_text())
    hardware = manifest["hardware_id"]
    if (manifest.get("format") != "audream-identity-v1" or
            not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", hardware) or
            registered.get("hardware_id") != hardware or registered.get("status") != "unbound"):
        raise ValueError("Enrollment does not match the prepared identity")
    public = public_bytes(seed)
    if (base64.b64decode(manifest["public_key"], validate=True) != public or
            base64.b64decode((directory / "pairing-public-key.base64").read_bytes().strip(), validate=True) != public or
            registered.get("public_key") != manifest["public_key"]):
        raise ValueError("Enrollment public key does not match the private seed")
    device = registered["device_id"]
    if str(UUID(device)) != device:
        raise ValueError("Expected a canonical device UUID")
    hardware_bytes = hardware.encode("ascii")
    record = b"AID1" + device.encode("ascii") + bytes([len(hardware_bytes)]) + hardware_bytes.ljust(128, b"\0") + public + seed
    assert len(record) == 233
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(["key", "type", "encoding", "value"])
    writer.writerow(["audream_ident", "namespace", "", ""])
    writer.writerow(["identity_v1", "data", "hex2bin", record.hex()])
    exclusive_write(directory / "factory-identity.nvs.csv", output.getvalue().encode("ascii"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prepare_parser = actions.add_parser("prepare")
    prepare_parser.add_argument("--directory", type=Path, required=True)
    prepare_parser.add_argument("--hardware-id", required=True)
    final_parser = actions.add_parser("finalize")
    final_parser.add_argument("--directory", type=Path, required=True)
    final_parser.add_argument("--enrollment", type=Path, required=True)
    options = parser.parse_args()
    try:
        if options.action == "prepare":
            prepare(options.directory, options.hardware_id)
        else:
            finalize(options.directory, options.enrollment)
    except (OSError, ValueError, KeyError, TypeError):
        parser.exit(1, "Factory identity operation failed; check inputs and protected output files.\n")
    print("Factory identity files prepared. No firmware, device or server was modified.")


if __name__ == "__main__":
    main()
