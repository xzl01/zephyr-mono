#!/usr/bin/env python3
"""Verify the bundled 1-bit font covers every non-ASCII UI character."""

from pathlib import Path
import re
import sys


FONT_PATH = Path("src/generated/papermono_cjk_16.c")
TITLE_FONT_PATH = Path("src/generated/papermono_titles_28.c")
DIGITS_58_FONT_PATH = Path("src/generated/papermono_digits_58.c")
DIGITS_96_FONT_PATH = Path("src/generated/papermono_digits_96.c")
UI_PATH = Path("src/papermono_ui.c")


def fail(message: str) -> None:
    print(f"font coverage FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


font_source = FONT_PATH.read_text(encoding="utf-8")
ui_source = UI_PATH.read_text(encoding="utf-8")


def glyph_set(source: str) -> set[int]:
    return {
        int(value, 16)
        for value in re.findall(r"/\* U\+([0-9A-Fa-f]{4,6}) ", source)
    }


def require_subset(path: Path, descriptor: str, required: str) -> None:
    source = path.read_text(encoding="utf-8")
    if f"const lv_font_t {descriptor}" not in source:
        fail(f"{descriptor} descriptor is missing")
    if not re.search(r"\.bpp\s*=\s*1\s*,", source):
        fail(f"{descriptor} is not encoded at 1 bit per pixel")
    available = glyph_set(source)
    missing = sorted({ord(character) for character in required} - available)
    if missing:
        fail(f"{descriptor} is missing " +
             ", ".join(f"U+{value:04X}" for value in missing))

if "const lv_font_t papermono_cjk_16" not in font_source:
    fail("papermono_cjk_16 descriptor is missing")
if not re.search(r"\.bpp\s*=\s*1\s*,", font_source):
    fail("font is not encoded at 1 bit per pixel")

glyphs = glyph_set(font_source)
missing_cjk = [codepoint for codepoint in range(0x4E00, 0xA000)
               if codepoint not in glyphs]
if missing_cjk:
    fail(f"basic CJK range is incomplete at U+{missing_cjk[0]:04X}")

required_punctuation = {
    0x00B7,  # middle dot used by status rows
    0x00D7,  # multiplication sign used by dimensions
    0x2014,  # em dash used by book text
    0x2026,  # ellipsis
    0x2502,  # vertical separator used by the UI
    0x3001, 0x3002, 0x300A, 0x300B,
    0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F,
}
missing_punctuation = sorted(required_punctuation - glyphs)
if missing_punctuation:
    fail("common punctuation is incomplete at " +
         ", ".join(f"U+{value:04X}" for value in missing_punctuation))

ui_codepoints = {ord(character) for character in ui_source if ord(character) > 0x7F}
missing_ui = sorted(ui_codepoints - glyphs)
if missing_ui:
    fail("UI source contains uncovered characters: " +
         ", ".join(f"U+{value:04X}" for value in missing_ui))

require_subset(
    TITLE_FONT_PATH,
    "papermono_titles_28",
    "今日继续阅读下一日程待办应用全部系统书库微信遥控器日历便签内容中心"
    "信息卡片硬件工具设置电源状态休眠传输最近",
)
require_subset(
    DIGITS_58_FONT_PATH,
    "papermono_digits_58",
    "%-0123456789:",
)
require_subset(
    DIGITS_96_FONT_PATH,
    "papermono_digits_96",
    "-0123456789:",
)

print(f"font coverage PASS ({len(glyphs)} glyphs, full basic CJK and UI subsets)")
