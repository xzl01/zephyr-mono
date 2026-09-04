#!/usr/bin/env python3
"""Fail when an ESP32-S3 Zephyr link map exceeds the internal DRAM budget."""

from __future__ import annotations

import argparse
import pathlib
import re


REGION_RE = re.compile(
    r"^\s*dram0_0_seg\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+",
    re.MULTILINE,
)
END_RE = re.compile(
    r"^\s*(0x[0-9a-fA-F]+)\s+_end\s+=\s+\.\s*$", re.MULTILINE
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "map_file",
        type=pathlib.Path,
        help="path to build/zephyr/zephyr.map",
    )
    parser.add_argument(
        "--max-percent",
        type=float,
        default=85.0,
        help="maximum permitted internal DRAM use (default: 85.0)",
    )
    args = parser.parse_args()

    if not 0.0 < args.max_percent < 100.0:
        parser.error("--max-percent must be between 0 and 100")

    text = args.map_file.read_text(encoding="utf-8", errors="replace")
    region = REGION_RE.search(text)
    end = END_RE.search(text)
    if region is None or end is None:
        raise SystemExit(
            f"cannot locate dram0_0_seg and _end in {args.map_file}"
        )

    origin = int(region.group(1), 16)
    capacity = int(region.group(2), 16)
    image_end = int(end.group(1), 16)
    used = image_end - origin
    if used < 0 or used > capacity:
        raise SystemExit(
            f"invalid internal DRAM range: origin=0x{origin:x}, "
            f"end=0x{image_end:x}, capacity={capacity}"
        )

    percent = used * 100.0 / capacity
    print(
        f"internal DRAM: {used:,} / {capacity:,} bytes "
        f"({percent:.2f}%, limit {args.max_percent:.2f}%)"
    )
    if percent > args.max_percent:
        print("ERROR: internal DRAM budget exceeded")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

