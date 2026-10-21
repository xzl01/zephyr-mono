#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Convert a binary PBM (P4) from the preview harness into a PNG and report
the black-pixel fraction.  Uses Pillow only."""

import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: pbm_to_png.py <in.pbm> <out.png> <expected-page-name>",
              file=sys.stderr)
        return 2
    source, destination, name = sys.argv[1], sys.argv[2], sys.argv[3]
    image = Image.open(source)
    if image.size != (480, 800):
        print(f"FAIL {name}: expected 480x800, got {image.size[0]}x{image.size[1]}",
              file=sys.stderr)
        return 1
    gray = image.convert("L")
    histogram = gray.histogram()
    total = image.size[0] * image.size[1]
    # PBM "1" bits are black; after convert("L") they are 0.
    black = sum(histogram[:128])
    fraction = black / total
    gray.convert("1").save(destination, optimize=True)
    print(f"OK {name}: 480x800 black={fraction * 100:.3f}% ({black}/{total})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
