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

from sign_release import DOMAIN, sign_release


class ReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.image = root / "firmware.bin"
        self.key_file = root / "release.pem"
        self.key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        self.key_file.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        self.key_file.chmod(0o600)
        self.body = bytearray(5000)
        self.body[0] = 0xE9
        self.body[1] = 1
        struct.pack_into("<I", self.body, 32, 0xABCD5432)
        self.body[48:53] = b"3.0.0"
        struct.pack_into("<8sI96s", self.body, 288, b"AUDOTA1\0", 8, b"test-watch")
        self.image.write_bytes(self.body)

    def release(self, **overrides):
        args = dict(image=self.image, key_file=self.key_file, model="test-watch", sequence=8,
                    url="https://updates.example/firmware.bin")
        args.update(overrides)
        return sign_release(**args)

    def test_metadata_and_signature(self) -> None:
        envelope = self.release()
        self.key.public_key().verify(bytes.fromhex(envelope["signature"]), DOMAIN + envelope["payload"].encode())
        payload = json.loads(envelope["payload"])
        self.assertEqual(payload["size"], "5000")
        self.assertEqual(payload["version"], "3.0.0")
        self.assertEqual(payload["sequence"], "8")

    def test_invalid_metadata(self) -> None:
        for bad in (0, -1, 2147483648, True):
            with self.assertRaises(ValueError):
                self.release(sequence=bad)
        for bad in ("", "bad model", "x" * 97):
            with self.assertRaises(ValueError):
                self.release(model=bad)
        for bad in ("http://updates.example/file", "https://a@b/file", "https://a/file?x=1", "https://a/#x",
                    "https://a:0/file", "https://a/\\file", "https://a/\nfile"):
            with self.assertRaises(ValueError):
                self.release(url=bad)

    def test_descriptor_and_key_permissions(self) -> None:
        self.key_file.chmod(0o644)
        with self.assertRaises(ValueError):
            self.release()
        self.key_file.chmod(0o600)
        for offset in (0, 32, 288, 296, 300):
            body = bytearray(self.body)
            body[offset] = 0
            self.image.write_bytes(body)
            with self.assertRaises(ValueError):
                self.release()
        self.image.write_bytes(self.body[:287])
        with self.assertRaises(ValueError):
            self.release()

    def test_cli_refuses_existing_output(self) -> None:
        output = Path(self.directory.name) / "release.json"
        command = [sys.executable, str(Path(__file__).with_name("sign_release.py")),
                   "--image", str(self.image), "--key", str(self.key_file), "--model", "test-watch",
                   "--sequence", "8", "--url", "https://updates.example/firmware.bin", "--output", str(output)]
        first = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        original = output.read_bytes()
        second = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(output.read_bytes(), original)
        self.assertNotIn("Traceback", second.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--export-fixture":
        case = ReleaseTests()
        case.setUp()
        try:
            destination = Path(sys.argv[2])
            (destination / "release.json").write_text(json.dumps(case.release()), encoding="utf-8")
            (destination / "firmware.bin").write_bytes(case.image.read_bytes())
        finally:
            case.doCleanups()
    else:
        unittest.main()
