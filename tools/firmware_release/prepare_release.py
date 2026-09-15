#!/usr/bin/env python3
"""Validate a watch release pair and render public production trust configuration."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

FIRMWARE_DOMAIN = b"audream-firmware-release-v1\n"
ASSET_DOMAIN = b"audream-asset-release-v1\n"
TOKEN = re.compile(r"[A-Za-z0-9_.+-]+", re.ASCII)
POLICY_FIELDS = {
    "schema", "model", "source_firmware_sequence", "firmware_sequence",
    "firmware_public_key", "asset_public_key", "firmware_api_origin",
    "asset_manifest_url", "sr_abi", "wake_model",
    "factory_asset_firmware_min", "factory_asset_firmware_max",
}


def _strict_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def _json(text: str) -> object:
    return json.loads(text, object_pairs_hook=_strict_object)


def _integer(value: object, name: str, minimum: int = 0) -> int:
    if type(value) is not int or not minimum <= value <= 2147483647:
        raise ValueError(f"{name} must be an integer between {minimum} and 2147483647")
    return value


def _token(value: object, name: str, maximum: int) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum or not TOKEN.fullmatch(value):
        raise ValueError(f"Invalid {name}")
    return value


def _key(value: object, name: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or value != value.lower():
        raise ValueError(f"Invalid {name}")
    try:
        raw = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"Invalid {name}") from error
    if not any(raw):
        raise ValueError(f"Invalid {name}")
    return value


def _https(value: object, name: str, origin: bool = False) -> str:
    if not isinstance(value, str) or len(value) > 512 or not value.startswith("https://"):
        raise ValueError(f"Invalid {name}")
    parsed = urlsplit(value)
    if (not parsed.hostname or parsed.username is not None or parsed.password is not None or
            parsed.query or parsed.fragment or parsed.port == 0 or
            any(ord(ch) <= 32 or ord(ch) >= 127 or ch in '\\"' for ch in value)):
        raise ValueError(f"Invalid {name}")
    if origin and (parsed.path not in ("", "/") or value.endswith("/")):
        raise ValueError(f"{name} must be an HTTPS origin without a path or trailing slash")
    return value


def load_policy(path: Path) -> dict[str, object]:
    policy = _json(path.read_text(encoding="utf-8"))
    if not isinstance(policy, dict) or set(policy) != POLICY_FIELDS or policy.get("schema") != "audream-watch-release-trust-v1":
        raise ValueError("Invalid release trust policy schema or fields")
    _token(policy["model"], "model", 95)
    _token(policy["sr_abi"], "sr_abi", 63)
    _token(policy["wake_model"], "wake_model", 31)
    source = _integer(policy["source_firmware_sequence"], "source_firmware_sequence", 1)
    target = _integer(policy["firmware_sequence"], "firmware_sequence", 1)
    if target <= source:
        raise ValueError("firmware_sequence must be newer than source_firmware_sequence")
    minimum = _integer(policy["factory_asset_firmware_min"], "factory_asset_firmware_min")
    maximum = _integer(policy["factory_asset_firmware_max"], "factory_asset_firmware_max")
    if minimum > maximum:
        raise ValueError("Factory asset firmware range is reversed")
    firmware_key = _key(policy["firmware_public_key"], "firmware_public_key")
    asset_key = _key(policy["asset_public_key"], "asset_public_key")
    if firmware_key == asset_key:
        raise ValueError("Firmware and asset release keys must be independent")
    _https(policy["firmware_api_origin"], "firmware_api_origin", origin=True)
    _https(policy["asset_manifest_url"], "asset_manifest_url")
    return policy


def _envelope(path: Path, public_key_hex: str, domain: bytes) -> dict[str, str]:
    envelope = _json(path.read_text(encoding="utf-8"))
    if not isinstance(envelope, dict) or set(envelope) != {"payload", "signature"}:
        raise ValueError(f"Invalid signed envelope: {path}")
    payload = envelope["payload"]
    signature = envelope["signature"]
    if (not isinstance(payload, str) or not isinstance(signature, str) or len(signature) != 128 or
            signature != signature.lower()):
        raise ValueError(f"Invalid signed envelope fields: {path}")
    try:
        Ed25519PublicKey.from_public_bytes(bytes.fromhex(public_key_hex)).verify(
            bytes.fromhex(signature), domain + payload.encode("utf-8"))
    except (InvalidSignature, ValueError, TypeError) as error:
        raise ValueError(f"Signature verification failed: {path}") from error
    decoded = _json(payload)
    if not isinstance(decoded, dict) or not all(isinstance(key, str) and isinstance(value, str) for key, value in decoded.items()):
        raise ValueError(f"Invalid signed payload: {path}")
    return decoded


def validate_pair(policy: dict[str, object], firmware_path: Path, asset_path: Path) -> None:
    firmware = _envelope(firmware_path, str(policy["firmware_public_key"]), FIRMWARE_DOMAIN)
    asset = _envelope(asset_path, str(policy["asset_public_key"]), ASSET_DOMAIN)
    if set(firmware) != {"schema", "model", "version", "sequence", "size", "sha256", "url"}:
        raise ValueError("Unexpected firmware manifest fields")
    if firmware["schema"] != "audream-firmware-v1" or firmware["model"] != policy["model"]:
        raise ValueError("Firmware manifest identity does not match the trust policy")
    firmware_sequence = _manifest_uint(firmware["sequence"], "firmware sequence")
    _manifest_uint(firmware["size"], "firmware size", 396, 32 * 1024 * 1024)
    _digest(firmware["sha256"], "firmware sha256")
    _https(firmware["url"], "firmware URL")
    _token(firmware["version"], "firmware version", 31)
    if firmware_sequence != policy["firmware_sequence"]:
        raise ValueError("Firmware manifest sequence does not match the trust policy")
    expected_asset_fields = {"schema", "format", "model", "version", "sequence", "size", "sha256", "url",
                             "firmware_min", "firmware_max", "sr_abi", "wake_model"}
    if set(asset) != expected_asset_fields or asset["schema"] != "audream-assets-v1" or asset["format"] != "mmap-v1":
        raise ValueError("Unexpected asset manifest fields")
    if (asset["model"] != policy["model"] or asset["sr_abi"] != policy["sr_abi"] or
            asset["wake_model"] != policy["wake_model"]):
        raise ValueError("Asset manifest identity does not match the trust policy")
    _token(asset["version"], "asset version", 31)
    _manifest_uint(asset["sequence"], "asset sequence", 1)
    _manifest_uint(asset["size"], "asset size", 58, 16 * 1024 * 1024)
    _digest(asset["sha256"], "asset sha256")
    _https(asset["url"], "asset URL")
    minimum = _manifest_uint(asset["firmware_min"], "asset firmware_min", 1)
    maximum = _manifest_uint(asset["firmware_max"], "asset firmware_max", 1)
    if minimum > maximum:
        raise ValueError("Asset firmware range is reversed")
    source = int(policy["source_firmware_sequence"])
    target = int(policy["firmware_sequence"])
    if not minimum <= source <= maximum or not minimum <= target <= maximum:
        raise ValueError("Asset release must support both source and target firmware sequences")


def _manifest_uint(value: str, name: str, minimum: int = 1, maximum: int = 2147483647) -> int:
    if not re.fullmatch(r"[1-9][0-9]{0,9}", value):
        raise ValueError(f"Invalid {name}")
    parsed = _integer(int(value), name, minimum)
    if parsed > maximum:
        raise ValueError(f"{name} exceeds {maximum}")
    return parsed


def _digest(value: str, name: str) -> None:
    if len(value) != 64 or value != value.lower() or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"Invalid {name}")


def render_sdkconfig(policy: dict[str, object]) -> str:
    values = (
        ("AUDREAM_OTA_PUBLIC_KEY_HEX", policy["firmware_public_key"]),
        ("AUDREAM_OTA_API_ORIGIN", policy["firmware_api_origin"]),
        ("AUDREAM_ASSET_PUBLIC_KEY_HEX", policy["asset_public_key"]),
        ("AUDREAM_ASSET_SR_ABI", policy["sr_abi"]),
        ("AUDREAM_ASSET_MANIFEST_URL", policy["asset_manifest_url"]),
    )
    lines = [f'CONFIG_{name}="{value}"' for name, value in values]
    lines.extend((
        f'CONFIG_AUDREAM_FACTORY_ASSET_FIRMWARE_MIN={policy["factory_asset_firmware_min"]}',
        f'CONFIG_AUDREAM_FACTORY_ASSET_FIRMWARE_MAX={policy["factory_asset_firmware_max"]}',
        f'CONFIG_AUDREAM_OTA_RELEASE_SEQUENCE={policy["firmware_sequence"]}',
    ))
    return "\n".join(lines) + "\n"


def _create(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        stream.write(content)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--firmware-manifest", type=Path)
    parser.add_argument("--asset-manifest", type=Path)
    parser.add_argument("--sdkconfig-output", type=Path, required=True)
    parser.add_argument("--server-public-keys-output", type=Path)
    args = parser.parse_args()
    try:
        if bool(args.firmware_manifest) != bool(args.asset_manifest):
            raise ValueError("Firmware and asset manifests must be supplied together")
        outputs = [path for path in (args.sdkconfig_output, args.server_public_keys_output) if path]
        if len(set(outputs)) != len(outputs) or any(path.exists() for path in outputs):
            raise ValueError("Output paths must be distinct and must not already exist")
        policy = load_policy(args.policy)
        if args.firmware_manifest:
            validate_pair(policy, args.firmware_manifest, args.asset_manifest)
        _create(args.sdkconfig_output, render_sdkconfig(policy))
        if args.server_public_keys_output:
            keys = {str(policy["model"]): str(policy["firmware_public_key"])}
            _create(args.server_public_keys_output, json.dumps(keys, sort_keys=True, separators=(",", ":")) + "\n")
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
        parser.exit(1, f"Release preparation failed: {error}\n")
    print(f"Release trust configuration created: {args.sdkconfig_output}")


if __name__ == "__main__":
    main()
