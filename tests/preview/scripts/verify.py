#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify the preview PNGs: exact size, non-blank, and print black coverage."""

import os
import sys

from PIL import Image

# A page with less ink than this did not render.
MIN_BLACK = 0.005
# A page that is essentially solid ink also means something went wrong.
MAX_BLACK = 0.95


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: verify.py <out-dir> <page> [page ...]", file=sys.stderr)
        return 2
    out_dir, pages = sys.argv[1], sys.argv[2:]
    failures = []
    print(f"{'page':<20} {'size':<10} {'black%':>8}  verdict")
    for page in pages:
        path = os.path.join(out_dir, f"{page}.png")
        if not os.path.exists(path):
            print(f"{page:<20} {'-':<10} {'-':>8}  MISSING")
            failures.append(f"{page}: no PNG")
            continue
        with Image.open(path) as image:
            size = image.size
            if image.mode != "1":
                failures.append(f"{page}: expected 1-bit PNG, got {image.mode}")
            gray = image.convert("L")
            histogram = gray.histogram()
            total = size[0] * size[1]
            black = sum(histogram[:128])
            fraction = black / total
        verdict = "ok"
        if size != (480, 800):
            verdict = f"BAD SIZE {size}"
            failures.append(f"{page}: size {size}")
        elif fraction < MIN_BLACK:
            verdict = "BLANK (render failed)"
            failures.append(f"{page}: only {fraction * 100:.3f}% ink")
        elif fraction > MAX_BLACK:
            verdict = "SOLID INK"
            failures.append(f"{page}: {fraction * 100:.3f}% ink")
        print(f"{page:<20} {size[0]}x{size[1]:<6} {fraction * 100:7.3f}%  {verdict}")

    if failures:
        print("\nVERIFICATION FAILED:", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print("\nall pages rendered with content")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
