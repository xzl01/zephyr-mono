#!/usr/bin/env python3
"""Catch image formats that vanish on the one-bit software display path."""
from pathlib import Path
import re

assets = sorted(Path("src/generated").glob("papermono_icon_*.c"))
assert len(assets) >= 33
for path in assets:
    name = path.stem
    source = path.read_text()
    assert ".cf = LV_COLOR_FORMAT_A8" in source, name
    dimensions = re.search(r"\.w = (\d+), \.h = (\d+), \.stride = (\d+)", source)
    size, height, stride = map(int, dimensions.groups())
    assert size in (48, 96, 176) and size == height == stride, name
    array = source.split("_map[] = {", 1)[1].split("};", 1)[0]
    pixels = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", array)]
    assert len(pixels) == size * size, name
    assert set(pixels) == {0, 255}, name
    assert 0.01 < pixels.count(255) / len(pixels) < 0.5, name
print(f"Field Notes icons: {len(assets)} binary-opacity A8 resources, no palette or gray: PASS")
