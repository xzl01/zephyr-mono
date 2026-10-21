#!/usr/bin/env python3
"""Original geometric Field Notes icons; MIT. No raster reference is traced.

Run from the repository root. These deterministic 96px, binary-opacity assets keep
the launcher inexpensive: one LVGL image instead of many objects per icon.
"""
from pathlib import Path
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
        box((10, 18, 25, 83)); box((29, 8, 49, 83)); box((53, 26, 66, 83))
        d.polygon([(70, 32), (80, 29), (94, 78), (84, 81)], outline=ink, width=3)
        line([(12, 34), (23, 34)]); line([(32, 69), (46, 69)])
    elif kind == "weread":
        d.polygon([(8, 22), (27, 18), (45, 24), (45, 77), (27, 71), (8, 75)], fill=ink)
        d.polygon([(51, 24), (69, 18), (88, 22), (88, 75), (69, 71), (51, 77)], fill=ink)
        line([(8, 81), (28, 77), (48, 83), (69, 77), (88, 81)])
    elif kind == "calendar":
        box((15, 20, 81, 84)); line([(15, 37), (81, 37)])
        for x in (29, 48, 67):
            d.rounded_rectangle((x-4, 10, x+4, 29), radius=4, outline=ink, width=3)
    elif kind == "tasks":
        for y in (19, 45, 71):
            box((10, y, 26, y+16), 2); line([(37, y+8), (86, y+8)], 2)
        line([(14, 25), (18, 29), (31, 15)], 3)
    elif kind == "notes":
        for y in (20, 32, 44, 56, 68):
            line([(13, y), (83, y)], 2)
        line([(13, 80), (66, 80)], 2)
        d.polygon([(72, 73), (85, 73), (72, 86)], fill=ink)
    elif kind == "remote":
        d.rounded_rectangle((29, 7, 67, 89), radius=6, outline=ink, width=3)
        d.ellipse((37, 17, 59, 39), outline=ink, width=3)
        for y in (52, 65, 78):
            d.ellipse((46, y-2, 50, y+2), fill=ink)
    elif kind == "content":
        # Local transfer symbol; a cloud would falsely imply cloud storage.
        box((13, 17, 83, 78)); line([(13, 67), (83, 67)], 2)
        line([(48, 58), (48, 30)]); line([(34, 44), (48, 30), (62, 44)])
        line([(34, 85), (62, 85)]); line([(48, 78), (48, 85)])
    elif kind == "info":
        box((21, 14, 87, 72)); box((9, 26, 75, 85))
        d.ellipse((19, 40, 31, 52), fill=ink)
        d.rectangle((16, 57, 34, 67), fill=ink)
        for y in (44, 56, 67):
            line([(42, y), (65, y)], 2)
    elif kind == "tools":
        d.polygon([(11, 12), (24, 16), (34, 29), (30, 34), (80, 81),
                   (85, 76), (35, 29), (29, 17), (16, 8)], outline=ink, width=3)
        d.polygon([(67, 10), (57, 20), (56, 33), (10, 79), (17, 86),
                   (63, 40), (77, 39), (88, 27), (77, 31), (67, 21)],
                  outline=ink, width=3)
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
    return image


if __name__ == "__main__":
    base = ("library", "weread", "calendar", "tasks", "notes", "remote",
            "content", "info", "tools", "settings", "power", "sleep", "bookmark", "document",
            "wifi", "bluetooth", "restore")
    small = ("book", "headphones", "bookmark", "settings", "sun", "toc", "return", "library", "document", "info")
    focal = ("library", "weread", "calendar", "tasks", "document", "bookmark", "wifi")
    variants = [(kind, "", 96) for kind in base] + [(kind, "_small", 48) for kind in small] + [(kind, "_focal", 176) for kind in focal]
    for kind, suffix, size in variants:
        name = "papermono_icon_" + kind + suffix
        # A8 is supported directly by LVGL's monochrome software renderer.
        # All values are binary (0 or 255); no gray pixels or decoder buffers.
        image = icon(kind).resize((size, size), Image.Resampling.NEAREST)
        pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
        data = bytes(255 if a >= 128 and r < 128 else 0
                     for r, g, b, a in pixels)
        rows = ["    " + " ".join(f"0x{v:02x}," for v in data[i:i+16])
                for i in range(0, len(data), 16)]
        Path("src/generated", name + ".c").write_text(
            "/* SPDX-License-Identifier: GPL-3.0-only */\n"
            "/* Generated by scripts/generate_field_notes_assets.py. Original geometry.\n"
            " * A8 storage uses binary opacity only; the display remains one-bit. */\n"
            "#include <lvgl.h>\n"
            "static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST\n"
            f"uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            "    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,\n"
            f"                .w = {size}, .h = {size}, .stride = {size} }},\n"
            f"    .data_size = sizeof({name}_map), .data = {name}_map,\n"
            "};\n", encoding="utf-8")
