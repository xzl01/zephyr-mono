#!/usr/bin/env python3
"""Check binary A8 assets and reproducible, license-preserving generation."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts/generate_field_notes_assets.py"
ASSET_DIR = ROOT / "src/generated"
LICENSE_LINE = re.compile(r"^/\* SPDX-License-Identifier: ([^*]+) \*/$", re.MULTILINE)


def assets_in(directory):
    assets = sorted(directory.glob("papermono_icon_*.c"))
    assert len(assets) == 34, (directory, len(assets))
    assert directory / "papermono_icon_wifi_focal.c" in assets
    return assets


def check_binary_a8(directory):
    for path in assets_in(directory):
        name = path.stem
        source = path.read_text()
        assert ".cf = LV_COLOR_FORMAT_A8" in source, name
        dimensions = re.search(r"\.w = (\d+), \.h = (\d+), \.stride = (\d+)", source)
        assert dimensions is not None, name
        size, height, stride = map(int, dimensions.groups())
        assert size in (48, 96, 176) and size == height == stride, name
        if name.endswith("_focal"):
            assert size == 176, name
        elif name.endswith("_small"):
            assert size == 48, name
        else:
            assert size == 96, name
        array = source.split("_map[] = {", 1)[1].split("};", 1)[0]
        pixels = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", array)]
        assert len(pixels) == size * size, name
        assert set(pixels) == {0, 255}, name
        assert 0.01 < pixels.count(255) / len(pixels) < 0.5, name


def asset_bytes(directory):
    return {path.name: path.read_bytes() for path in assets_in(directory)}


def snapshot(directory):
    # Include timestamps so rewriting identical bytes still fails the read-only check.
    return {
        path.name: (path.read_bytes(), path.stat().st_mtime_ns)
        for path in assets_in(directory)
    }


def run_generator(*args, success=True):
    result = subprocess.run(
        [sys.executable, str(GENERATOR), *map(str, args)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert (result.returncode == 0) == success, (
        result.args, result.returncode, result.stdout, result.stderr
    )


check_binary_a8(ASSET_DIR)
before_check = snapshot(ASSET_DIR)
run_generator("--check")
assert snapshot(ASSET_DIR) == before_check, "--check modified checked-in assets"

with tempfile.TemporaryDirectory(prefix="field-notes-assets-") as temporary:
    output_dir = Path(temporary) / "generated"
    run_generator("--output-dir", output_dir)
    check_binary_a8(output_dir)
    first_generation = asset_bytes(output_dir)

    for filename, source in first_generation.items():
        license_match = LICENSE_LINE.search(source.decode())
        assert license_match is not None, filename
        expected_license = (
            "GPL-3.0-only" if filename == "papermono_icon_wifi_focal.c" else "MIT"
        )
        assert license_match.group(1) == expected_license, filename

    run_generator("--output-dir", output_dir)
    assert asset_bytes(output_dir) == first_generation, "generation is not deterministic"

    customized_path = output_dir / "papermono_icon_library.c"
    customized_source, substitutions = LICENSE_LINE.subn(
        "/* SPDX-License-Identifier: BSD-2-Clause */",
        customized_path.read_text(),
        count=1,
    )
    assert substitutions == 1
    customized_path.write_text(customized_source)
    mixed_licenses = asset_bytes(output_dir)
    run_generator("--output-dir", output_dir)
    assert asset_bytes(output_dir) == mixed_licenses, "generation overwrote an existing SPDX license"

    before_check = snapshot(output_dir)
    run_generator("--check", "--output-dir", output_dir)
    assert snapshot(output_dir) == before_check, "--check modified temporary assets"

    # Flip one binary alpha value without depending on the icon's particular geometry.
    tampered_source, substitutions = re.subn(
        r"0x(00|ff),",
        lambda match: "0xff," if match.group(1) == "00" else "0x00,",
        customized_path.read_text(),
        count=1,
    )
    assert substitutions == 1
    customized_path.write_text(tampered_source)
    before_failed_check = snapshot(output_dir)
    run_generator("--check", "--output-dir", output_dir, success=False)
    assert snapshot(output_dir) == before_failed_check, "failed --check modified assets"

print("Field Notes icons: 34 binary A8 resources; reproducibility, read-only checks, and SPDX preservation: PASS")
