#!/usr/bin/env python3
"""Original geometric Field Notes icons; MIT. No raster reference is traced.

Run from the repository root. These deterministic 96px, binary-opacity assets keep
the launcher inexpensive: one LVGL image instead of many objects per icon.
"""
import argparse
from pathlib import Path
import re
from PIL import Image, ImageDraw



def icon(kind):
    image = Image.new("RGBA", (96, 96), (255, 255, 255, 0))
    d = ImageDraw.Draw(image)
    ink = (0, 0, 0, 255)

    def line(points, width=3):
        d.line(points, fill=ink, width=width)

    def box(bounds, width=3):
        d.rectangle(bounds, outline=ink, width=width)

    if kind == "library":
        box((14, 24, 30, 80)); box((36, 14, 56, 80)); box((62, 22, 80, 80))
        line([(17, 37), (27, 37)], 2); line([(39, 65), (53, 65)], 2)
        line([(65, 36), (77, 36)], 2)
        line([(10, 85), (86, 85)])
    elif kind == "weread":
        line([(12, 22), (28, 16), (48, 24), (68, 16), (84, 22),
              (84, 76), (68, 72), (48, 80), (28, 72), (12, 76), (12, 22)])
        line([(48, 24), (48, 80)])
        for y in (36, 49, 62):
            line([(22, y), (28, y-2), (38, y+2)], 2)
            line([(58, y+2), (68, y-2), (74, y)], 2)
    elif kind == "calendar":
        box((15, 20, 81, 84)); line([(15, 37), (81, 37)])
        for x in (29, 48, 67):
            d.rounded_rectangle((x-4, 10, x+4, 29), radius=4, outline=ink, width=3)
    elif kind == "tasks":
        for y in (19, 45, 71):
            box((12, y, 30, y+18)); line([(40, y+9), (84, y+9)], 2)
        line([(17, 27), (21, 31), (26, 23)])
    elif kind == "notes":
        line([(18, 12), (62, 12), (80, 30), (80, 84), (18, 84), (18, 12)])
        line([(62, 12), (62, 30), (80, 30)])
        for y in (44, 56, 68):
            line([(29, y), (68, y)], 2)
    elif kind == "remote":
        d.rounded_rectangle((29, 7, 67, 89), radius=6, outline=ink, width=3)
        d.ellipse((37, 17, 59, 39), outline=ink, width=3)
        for y in (52, 65, 78):
            d.ellipse((46, y-2, 50, y+2), fill=ink)
    elif kind == "content":
        # Local import/export tray; no cloud storage is implied.
        line([(14, 58), (14, 82), (82, 82), (82, 58),
              (66, 58), (60, 68), (36, 68), (30, 58), (14, 58)])
        line([(34, 16), (34, 48)])
        line([(24, 38), (34, 48), (44, 38)])
        line([(64, 48), (64, 16)])
        line([(54, 26), (64, 16), (74, 26)])
    elif kind == "info":
        line([(24, 14), (84, 14), (84, 72)])
        box((12, 24, 74, 84))
        d.ellipse((24, 36, 42, 54), outline=ink, width=2)
        d.rectangle((32, 40, 34, 42), fill=ink)
        line([(33, 46), (33, 50)], 2)
        for y in (40, 52):
            line([(50, y), (64, y)], 2)
        for y in (66, 76):
            line([(24, y), (64, y)], 2)
    elif kind == "tools":
        line([(61, 13), (73, 11), (69, 23), (77, 31), (87, 27),
              (85, 39), (77, 47), (65, 47), (31, 81), (24, 84),
              (13, 73), (16, 66), (50, 32), (50, 23), (61, 13)])
        d.ellipse((20, 68, 29, 77), outline=ink, width=2)
    elif kind == "settings":
        for y, x in ((23, 34), (48, 65), (73, 42)):
            line([(12, y), (84, y)])
            d.ellipse((x-7, y-7, x+7, y+7), fill=(255, 255, 255, 255), outline=ink, width=3)
    elif kind == "power":
        d.arc((18, 18, 78, 80), 305, 235, fill=ink, width=4)
        line([(48, 8), (48, 44)], 4)
    elif kind == "sleep":
        d.ellipse((12, 12, 84, 84), outline=ink, width=3)
        d.ellipse((40, 0, 100, 62), fill=(255, 255, 255, 0), outline=ink, width=3)
        # Keep only the two arcs forming the crescent silhouette.
        for y in range(96):
            for x in range(96):
                if (x-48)**2 + (y-48)**2 > 36**2:
                    image.putpixel((x,y), (255,255,255,0))
    elif kind == "bookmark":
        line([(24, 12), (72, 12), (72, 84), (48, 64), (24, 84), (24, 12)])
    elif kind == "document":
        line([(20, 12), (58, 12), (80, 34), (80, 84), (20, 84), (20, 12)])
        line([(58, 12), (58, 34), (80, 34)])
        for y in (48, 60, 72): line([(30, y), (69, y)], 2)
    elif kind == "wifi":
        for radius in (37, 26, 15):
            d.arc((48-radius, 72-radius, 48+radius, 72+radius), 215, 325, fill=ink, width=4)
        d.ellipse((44, 69, 52, 77), fill=ink)
    elif kind == "bluetooth":
        line([(42, 10), (67, 32), (26, 66), (42, 80), (42, 10), (67, 57), (26, 25)], 4)
    elif kind == "restore":
        d.arc((15, 15, 81, 81), 35, 320, fill=ink, width=4)
        line([(66, 12), (77, 27), (58, 28)], 4)
    elif kind == "book":
        box((22, 12, 76, 84)); line([(34, 13), (34, 83)], 2)
        line([(35, 76), (74, 76)], 2)
    elif kind == "headphones":
        d.arc((16, 12, 80, 80), 180, 360, fill=ink, width=4)
        box((14, 44, 28, 76)); box((68, 44, 82, 76))
    elif kind == "sun":
        d.ellipse((30, 30, 66, 66), outline=ink, width=3)
        for a, b in [((48,10),(48,22)),((48,74),(48,86)),((10,48),(22,48)),((74,48),(86,48)),
                     ((21,21),(29,29)),((67,67),(75,75)),((21,75),(29,67)),((67,29),(75,21))]:
            line([a,b], 3)
    elif kind == "toc":
        for y in (24,48,72):
            box((13,y-5,23,y+5),2); line([(34,y),(84,y)],3)
    elif kind == "return":
        line([(77,48),(19,48),(40,27)],4); line([(19,48),(40,69)],4)
    else:
        raise ValueError(f"Unknown icon: {kind}")
    return image


def generate(output_dir, check=False):
    base = ("library", "weread", "calendar", "tasks", "notes", "remote",
            "content", "info", "tools", "settings", "power", "sleep", "bookmark", "document",
            "wifi", "bluetooth", "restore")
    small = ("book", "headphones", "bookmark", "settings", "sun", "toc", "return", "library", "document", "info")
    focal = ("library", "weread", "calendar", "tasks", "document", "bookmark", "wifi")
    variants = [(kind, "", 96) for kind in base] + [(kind, "_small", 48) for kind in small] + [(kind, "_focal", 176) for kind in focal]
    mismatches = []
    for kind, suffix, size in variants:
        name = "papermono_icon_" + kind + suffix
        path = output_dir / (name + ".c")
        existing = path.read_text(encoding="utf-8") if path.exists() else None
        # Existing assets retain their license. Wi-Fi focal was introduced under
        # GPL; the other original geometric icons are MIT. Never relicense them.
        license_id = "GPL-3.0-only" if name == "papermono_icon_wifi_focal" else "MIT"
        if existing is not None:
            match = re.match(r"/\* SPDX-License-Identifier: ([^*\n]+) \*/\n", existing)
            if not match:
                raise ValueError(f"Missing SPDX license header: {path}")
            license_id = match.group(1)
        # A8 is supported directly by LVGL's monochrome software renderer.
        # All values are binary (0 or 255); no gray pixels or decoder buffers.
        image = icon(kind).resize((size, size), Image.Resampling.NEAREST)
        pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
        data = bytes(255 if a >= 128 and r < 128 else 0
                     for r, g, b, a in pixels)
        rows = ["    " + " ".join(f"0x{v:02x}," for v in data[i:i+16])
                for i in range(0, len(data), 16)]
        source = (
            f"/* SPDX-License-Identifier: {license_id} */\n"
            "/* Generated by scripts/generate_field_notes_assets.py. Original geometry.\n"
            " * A8 storage uses binary opacity only; the display remains one-bit. */\n"
            "#include <lvgl.h>\n"
            "static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST\n"
            f"uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            "    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,\n"
            f"                .w = {size}, .h = {size}, .stride = {size} }},\n"
            f"    .data_size = sizeof({name}_map), .data = {name}_map,\n"
            "};\n")
        if existing != source:
            if check:
                mismatches.append(str(path))
            else:
                output_dir.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")
    if mismatches:
        raise SystemExit("Generated icons differ: " + ", ".join(mismatches))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/generated")
    parser.add_argument("--check", action="store_true", help="verify without writing files")
    args = parser.parse_args()
    generate(args.output_dir, args.check)
