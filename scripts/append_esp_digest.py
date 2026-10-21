#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Append the ESP image SHA-256 digest to a Zephyr simple-boot image.

Zephyr builds simple-boot images with esptool ``--ram-only-header``; esptool
forces ``--dont-append-digest`` in that mode, so the ESP32-S3 ROM compares the
32-byte digest slot against padding (the next segment header) and prints
"SHA-256 comparison failed ... Attempting to boot anyway".

This rewrites the image in place without changing its size or any segment
offset: the digest takes 32 bytes of existing zero padding, and the hidden
padding segment header that lived there is moved 32 bytes later with its length
reduced. Header byte 0x17 (hash_appended) is set before hashing, because it is
part of the hashed range.
"""

import hashlib
import struct
import sys

HASH_LEN = 32
ALIGN = 16
HEADER_LEN = 24
HASH_APPENDED_OFFSET = 0x17


def fix_image(data):
    if len(data) < HEADER_LEN or data[0] != 0xE9:
        raise ValueError("not an ESP image")
    segment_count = data[1]
    offset = HEADER_LEN
    for _ in range(segment_count):
        if offset + 8 > len(data):
            raise ValueError("truncated segment table")
        _addr, length = struct.unpack_from("<II", data, offset)
        offset += 8 + length
    checksum_offset = ((offset + 1 + ALIGN - 1) & ~(ALIGN - 1)) - 1
    digest_offset = checksum_offset + 1
    if digest_offset + HASH_LEN > len(data):
        raise ValueError("digest slot outside image")

    if data[HASH_APPENDED_OFFSET] == 1:
        stored = bytes(data[digest_offset:digest_offset + HASH_LEN])
        if hashlib.sha256(bytes(data[:digest_offset])).digest() == stored:
            return False  # already fixed (idempotent)
        raise ValueError("hash_appended is set but the digest does not match")

    pad_addr, pad_len = struct.unpack_from("<II", data, digest_offset)
    if pad_addr != 0 or pad_len < HASH_LEN:
        raise ValueError("unexpected segment header in digest slot: "
                         f"addr={pad_addr:#x} len={pad_len:#x}")

    data[HASH_APPENDED_OFFSET] = 1
    digest = hashlib.sha256(bytes(data[:digest_offset])).digest()
    data[digest_offset:digest_offset + HASH_LEN] = digest
    struct.pack_into("<II", data, digest_offset + HASH_LEN, 0, pad_len - HASH_LEN)
    return True


def main(argv):
    if len(argv) != 2:
        print("usage: append_esp_digest.py <zephyr.bin>", file=sys.stderr)
        return 2
    path = argv[1]
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    try:
        changed = fix_image(data)
    except ValueError as error:
        print(f"append_esp_digest: {error}", file=sys.stderr)
        return 1
    if changed:
        with open(path, "wb") as handle:
            handle.write(data)
        print(f"append_esp_digest: digest written to {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
