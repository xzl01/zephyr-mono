#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host tests for the ESP image SHA-256 digest post-processing."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "append_esp_digest", ROOT / "scripts/append_esp_digest.py")
digest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(digest)


def make_image(segments=((0x3FC97820, b"\x11" * 64), (0x40374000, b"\x22" * 96))):
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = len(segments)
    body = bytearray()
    for addr, data in segments:
        body += struct.pack("<II", addr, len(data)) + data
    end = len(header) + len(body)
    checksum_offset = ((end + 1 + 15) & ~15) - 1
    image = header + body
    image += b"\x00" * (checksum_offset - len(image))
    check = 0xEF
    for byte in body:
        check ^= byte
    image.append(check)
    digest_offset = checksum_offset + 1
    image += struct.pack("<II", 0, 0x100)          # hidden padding segment
    image += b"\x00" * (0x400 - len(image))
    return bytes(image), digest_offset


class DigepstTests(unittest.TestCase):
    def test_digest_written_and_layout_preserved(self):
        image, offset = make_image()
        data = bytearray(image)
        self.assertTrue(digest.fix_image(data))
        self.assertEqual(len(data), len(image))
        self.assertEqual(data[0x17], 1)
        stored = bytes(data[offset:offset + 32])
        self.assertEqual(stored, hashlib.sha256(bytes(data[:offset])).digest())
        pad_addr, pad_len = struct.unpack_from("<II", data, offset + 32)
        self.assertEqual((pad_addr, pad_len), (0, 0x100 - 32))
        # The padding segment keeps its end offset: data after it is untouched.
        end = offset + 8 + 0x100
        self.assertEqual(bytes(data[end:]), image[end:])
        self.assertEqual(bytes(data[offset + 32:offset + 40]),
                         struct.pack("<II", 0, 0x100 - 32))
        self.assertEqual(data[offset + 40:end], b"\x00" * (end - offset - 40))

    def test_idempotent(self):
        data = bytearray(make_image()[0])
        self.assertTrue(digest.fix_image(data))
        self.assertFalse(digest.fix_image(data))

    def test_rejects_corrupt_digest_with_flag_set(self):
        image, offset = make_image()
        data = bytearray(image)
        self.assertTrue(digest.fix_image(data))
        data[offset] ^= 0xFF
        with self.assertRaises(ValueError):
            digest.fix_image(data)

    def test_rejects_non_image_and_unexpected_padding(self):
        with self.assertRaises(ValueError):
            digest.fix_image(bytearray(64))
        image, offset = make_image()
        data = bytearray(image)
        struct.pack_into("<II", data, offset, 0x1234, 0x100)
        with self.assertRaises(ValueError):
            digest.fix_image(data)

    def test_cli_writes_file(self):
        image, _ = make_image()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "zephyr.bin"
            path.write_bytes(image)
            self.assertEqual(digest.main(["x", str(path)]), 0)
            self.assertEqual(path.read_bytes()[0x17], 1)
            self.assertEqual(digest.main(["x", str(path)]), 0)


if __name__ == "__main__":
    unittest.main()
