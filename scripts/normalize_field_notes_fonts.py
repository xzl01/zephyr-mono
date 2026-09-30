#!/usr/bin/env python3
"""Normalize converter output to the fixed, tested Field Notes line boxes."""
from pathlib import Path
import re

for name in ("clock_148", "heading_48", "reading_24", "state_40"):
    path = Path("src/generated/papermono_" + name + ".c")
    text = path.read_text(encoding="utf-8")
    # Converter headers must stay portable and must not publish local paths.
    text = re.sub(r"(--font )\S*[/\\]SourceHanSerifSC-Regular\.otf",
                  r"\1.cache/field-notes-fonts/SourceHanSerifSC-Regular.otf", text)
    if not text.startswith("/* SPDX-License-Identifier:"):
        text = "/* SPDX-License-Identifier: OFL-1.1 */\n" + text
    if name == "reading_24":
        # Prevent tall Japanese vertical-repeat marks from defining the line
        # box: the generation range intentionally ends at U+301F.
        metrics = re.findall(r"\.box_h = (\d+), .ofs_x = -?\d+, .ofs_y = (-?\d+)", text)
        for height, offset in metrics:
            assert int(offset) >= -8
            assert int(height) + int(offset) <= 22
        text = re.sub(r"\.line_height = \d+,", ".line_height = 30,", text)
        text = re.sub(r"\.base_line = \d+,", ".base_line = 8,", text)
        text = text.replace(".fallback = NULL,", ".fallback = &papermono_cjk_16,")
        if "LV_FONT_DECLARE(papermono_cjk_16);" not in text:
            text = text.replace("#ifndef PAPERMONO_READING_24",
                                "LV_FONT_DECLARE(papermono_cjk_16);\n\n#ifndef PAPERMONO_READING_24")
    if name == "state_40":
        text = re.sub(r"\.line_height = \d+,", ".line_height = 50,", text)
        text = re.sub(r"\.base_line = \d+,", ".base_line = 10,", text)
    path.write_text(text.rstrip() + "\n", encoding="utf-8")
