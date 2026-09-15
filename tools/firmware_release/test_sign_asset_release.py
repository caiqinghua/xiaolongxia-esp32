from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from sign_asset_release import DOMAIN, parse_index, sign_asset_release


def bundle(wake: str = "wn9_xiaolongxiaolong_tts", index: bytes | None = None) -> bytes:
    model = (struct.pack("<I32sI32sII", 1, wake.encode(), 1, b"wn9_data", 80, 1) + b"*")
    if index is None:
        index = b'{"version":1,"srmodels":"srmodels.bin"}'
    table = (struct.pack("<32sIIHH", b"index.json", len(index), 0, 0, 0)
             + struct.pack("<32sIIHH", b"srmodels.bin", len(model), len(index) + 2, 0, 0))
    body = table + b"ZZ" + index + b"ZZ" + model
    return struct.pack("<III", 2, sum(body) & 0xFFFF, len(body)) + body


class AssetReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.image = self.root / "assets.bin"
        self.key_file = self.root / "test.pem"
        self.key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        self.key_file.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        self.key_file.chmod(0o600)
        self.image.write_bytes(bundle())

    def release(self, **overrides) -> dict[str, str]:
        args = dict(image=self.image, key_file=self.key_file, model="test-watch", version="1.0.0", sequence=8,
                    url="https://updates.example/assets.bin", firmware_min=6, firmware_max=9,
                    sr_abi="esp-sr-2.3.0-esp32s3", wake_model="wn9_xiaolongxiaolong_tts")
        args.update(overrides)
        return sign_asset_release(**args)

    def test_signature_and_compatibility_metadata(self) -> None:
        envelope = self.release()
        self.key.public_key().verify(bytes.fromhex(envelope["signature"]), DOMAIN + envelope["payload"].encode())
        metadata = json.loads(envelope["payload"])
        self.assertEqual(metadata["schema"], "audream-assets-v1")
        self.assertEqual(metadata["firmware_min"], "6")
        self.assertEqual(metadata["size"], str(len(bundle())))
        self.assertEqual(metadata["wake_model"], "wn9_xiaolongxiaolong_tts")

    def test_invalid_metadata(self) -> None:
        for field in ("sequence", "firmware_min", "firmware_max"):
            for value in (0, -1, 2147483648, True):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.release(**{field: value})
        for values in ({"firmware_min": 10}, {"model": "../other"}, {"sr_abi": ""},
                       {"version": "with space"}, {"url": "http://updates.example/a"},
                       {"url": "https://u@updates.example/a"}, {"wake_model": "wn9_other"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.release(**values)

    def test_invalid_bundle_and_permissions(self) -> None:
        data = bundle()
        for length in range(len(data)):
            self.image.write_bytes(data[:length])
            with self.assertRaises(ValueError):
                self.release()
        for data in (bundle("wn9_other"), bundle(index=b'{}'), bundle() + b"extra",
                     bundle(index=b'{"version":1,"srmodels":"missing.bin"}'),
                     bundle(index=b'{"version":1,"version":1,"srmodels":"srmodels.bin"}')):
            self.image.write_bytes(data)
            with self.assertRaises(ValueError):
                self.release()
        self.image.write_bytes(bundle())
        self.key_file.chmod(0o644)
        with self.assertRaises(ValueError):
            self.release()

    def test_cli_refuses_overwrite(self) -> None:
        output = self.root / "assets.json"
        command = [sys.executable, str(Path(__file__).with_name("sign_asset_release.py")),
                   "--image", str(self.image), "--key", str(self.key_file), "--model", "test-watch",
                   "--version", "1.0.0", "--sequence", "8", "--url", "https://updates.example/assets.bin",
                   "--firmware-min", "6", "--firmware-max", "9", "--sr-abi", "esp-sr-2.3.0-esp32s3",
                   "--wake-model", "wn9_xiaolongxiaolong_tts", "--output", str(output)]
        first = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        original = output.read_bytes()
        second = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(original, output.read_bytes())
        self.assertNotIn("Traceback", second.stderr)

    def test_index_depth_matches_device_limit(self) -> None:
        prefix = b'{"version":1,"srmodels":"srmodels.bin","nested":'
        self.assertEqual(parse_index(prefix + b"[" * 15 + b"0" + b"]" * 15 + b"}")["version"], 1)
        with self.assertRaises(ValueError):
            parse_index(prefix + b"[" * 16 + b"0" + b"]" * 16 + b"}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--export-fixture":
        case = AssetReleaseTests()
        case.setUp()
        try:
            destination = Path(sys.argv[2])
            (destination / "asset-release.json").write_text(json.dumps(case.release()), encoding="utf-8")
            (destination / "assets.bin").write_bytes(case.image.read_bytes())
        finally:
            case.doCleanups()
    else:
        unittest.main()
