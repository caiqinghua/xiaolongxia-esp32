#!/usr/bin/env python3
"""Tests for production release trust rendering and pair validation."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from prepare_release import ASSET_DOMAIN, FIRMWARE_DOMAIN, load_policy, render_sdkconfig, validate_pair


def key_pair() -> tuple[Ed25519PrivateKey, str]:
    private = Ed25519PrivateKey.generate()
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    return private, public


def envelope(private: Ed25519PrivateKey, domain: bytes, payload: dict[str, str]) -> dict[str, str]:
    encoded = json.dumps(payload, separators=(",", ":"))
    return {"payload": encoded, "signature": private.sign(domain + encoded.encode()).hex()}


class PrepareReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.firmware_private, firmware_public = key_pair()
        self.asset_private, asset_public = key_pair()
        self.policy = {
            "schema": "audream-watch-release-trust-v1",
            "model": "esp32-s3-touch-amoled-2.06",
            "source_firmware_sequence": 7,
            "firmware_sequence": 8,
            "firmware_public_key": firmware_public,
            "asset_public_key": asset_public,
            "firmware_api_origin": "https://api.example.com",
            "asset_manifest_url": "https://updates.example.com/watch/models.json",
            "sr_abi": "esp-sr-2.3.0-esp32s3",
            "wake_model": "wn9_xiaolongxiaolong_tts",
            "factory_asset_firmware_min": 1,
            "factory_asset_firmware_max": 7,
        }
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(self.policy), encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def manifests(self, minimum: str = "7", maximum: str = "8") -> tuple[Path, Path]:
        firmware = {"schema": "audream-firmware-v1", "model": self.policy["model"], "version": "2.3.0",
                    "sequence": "8", "size": "396", "sha256": "11" * 32,
                    "url": "https://updates.example.com/firmware.bin"}
        asset = {"schema": "audream-assets-v1", "format": "mmap-v1", "model": self.policy["model"],
                 "version": "3.0.0", "sequence": "12", "size": "200", "sha256": "22" * 32,
                 "url": "https://updates.example.com/assets.bin", "firmware_min": minimum,
                 "firmware_max": maximum, "sr_abi": self.policy["sr_abi"], "wake_model": self.policy["wake_model"]}
        firmware_path, asset_path = self.root / "firmware.json", self.root / "asset.json"
        firmware_path.write_text(json.dumps(envelope(self.firmware_private, FIRMWARE_DOMAIN, firmware)), encoding="utf-8")
        asset_path.write_text(json.dumps(envelope(self.asset_private, ASSET_DOMAIN, asset)), encoding="utf-8")
        return firmware_path, asset_path

    def test_valid_pair_and_public_config(self) -> None:
        policy = load_policy(self.policy_path)
        firmware, asset = self.manifests()
        validate_pair(policy, firmware, asset)
        config = render_sdkconfig(policy)
        self.assertIn('CONFIG_AUDREAM_OTA_RELEASE_SEQUENCE=8', config)
        self.assertIn('CONFIG_AUDREAM_FACTORY_ASSET_FIRMWARE_MAX=7', config)
        self.assertNotIn("PRIVATE", config)

    def test_pair_must_bridge_source_and_target(self) -> None:
        firmware, asset = self.manifests("8", "8")
        with self.assertRaisesRegex(ValueError, "both source and target"):
            validate_pair(load_policy(self.policy_path), firmware, asset)

    def test_independent_keys_and_strict_policy_are_required(self) -> None:
        self.policy["asset_public_key"] = self.policy["firmware_public_key"]
        self.policy_path.write_text(json.dumps(self.policy), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "independent"):
            load_policy(self.policy_path)


if __name__ == "__main__":
    unittest.main()
