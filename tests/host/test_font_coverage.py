#!/usr/bin/env python3
"""Verify the bundled 1-bit font covers every non-ASCII UI character."""

from pathlib import Path
import re
import sys


FONT_PATH = Path("src/generated/papermono_cjk_16.c")
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
    Path("src/generated/papermono_clock_148.c"),
    "papermono_clock_148",
    "-0123456789:",
)
clock_source = Path("src/generated/papermono_clock_148.c").read_text(encoding="utf-8")
clock_codepoints = re.findall(r"/\* U\+([0-9A-F]+) ", clock_source)
clock_table = clock_source.split("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[]", 1)[1].split("};", 1)[0]
advances = re.findall(r"\.adv_w = (\d+)", clock_table)[1:]
clock_widths = {chr(int(cp, 16)): int(width) / 16
                for cp, width in zip(clock_codepoints, advances)}
for hour in range(24):
    for minute in range(60):
        if sum(clock_widths[c] for c in f"{hour:02d}:{minute:02d}") > 440:
            fail("clock can wrap at a valid time")

reading_source = Path("src/generated/papermono_reading_24.c").read_text(encoding="utf-8")
reading_glyphs = glyph_set(reading_source)
if "const lv_font_t papermono_reading_24" not in reading_source or ".bpp = 1," not in reading_source:
    fail("Field Notes reading font must be one-bit")
if not re.search(r"\.line_height\s*=\s*30\s*,", reading_source):
    fail("reading font line box differs from pagination")
if ".fallback = &papermono_cjk_16," not in reading_source:
    fail("reading font must retain complete CJK fallback")
require_subset(Path("src/generated/papermono_heading_48.c"), "papermono_heading_48",
               "0123456789%应用书库微信读日历待办便签内容中心遥控器信息卡片硬件工具设置电源系统状态阅读菜单目录连接设备确认休眠关机重新配网清除蓝牙对恢复默认？")
# Validate the actual headings/confirmation copy, including newly added pages,
# instead of relying only on the original design's fixed character list.
heading_copy = []
for arguments in re.findall(r"\bmake_header\(screen,(.*?)\);", ui_source, re.S):
    heading_copy += re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', arguments)
confirmation_copy = re.search(r"const char \*title = confirmation ==.*?;", ui_source, re.S)
if confirmation_copy:
    heading_copy += re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', confirmation_copy.group())
heading_copy += re.findall(r'\bdetail_confirmation\(screen,\s*&\w+,\s*"([^"\\]*)"', ui_source)
require_subset(Path("src/generated/papermono_heading_48.c"), "papermono_heading_48",
               "".join(heading_copy))
require_subset(Path("src/generated/papermono_reading_24.c"), "papermono_reading_24", "°℃")
require_subset(Path("src/generated/papermono_state_40.c"), "papermono_state_40",
               "本地书架不可用没有可显示的书籍暂无日程待办便签尚未配置数据源书签")
state_copy = re.findall(r'\bdetail_state_title\(screen,\s*"([^"\\]*)"', ui_source)
require_subset(Path("src/generated/papermono_state_40.c"), "papermono_state_40",
               "".join(state_copy))
for codepoint in ui_codepoints:
    if codepoint not in reading_glyphs and codepoint not in glyphs:
        fail(f"reading/title font missing U+{codepoint:04X}")

print(f"font coverage PASS ({len(glyphs)} glyphs, full basic CJK and UI subsets)")
