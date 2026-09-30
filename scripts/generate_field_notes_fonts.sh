#!/bin/sh
# SPDX-License-Identifier: MIT
# Run from repository root; generated C files are already checked in.
set -eu
task_fonts_dir="$PWD/.cache/field-notes-fonts"
mkdir -p "$task_fonts_dir"
task_serif_font="$task_fonts_dir/SourceHanSerifSC-Regular.otf"
if [ ! -f "$task_serif_font" ]; then
    curl -fsSL https://raw.githubusercontent.com/adobe-fonts/source-han-serif/release/OTF/SimplifiedChinese/SourceHanSerifSC-Regular.otf -o "$task_serif_font"
fi
python3 - "$task_serif_font" <<'PY'
import hashlib, sys
from pathlib import Path
assert hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() == \
    '78aa7a328fd974df2d688c8a9fd74a33d8334dfa84ab24d9d11efb2ffc464117', \
    'Source Han Serif source differs from the reviewed font'
PY
export npm_config_cache="${npm_config_cache:-$PWD/.cache/font-converter}"
npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 1 --size 148 \
    --font assets/fonts/LibreBodoni.ttf -r 0x2D,0x30-0x3A --format lvgl \
    -o src/generated/papermono_clock_148.c --force-fast-kern-format --lv-include lvgl.h
npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 1 --size 48 \
    --font "$task_serif_font" -r 0x20-0x7e --symbols '应用书库微信读日历待办便签内容中心遥控器信息卡片硬件工具设置电源系统状态阅读菜单目录连接设备确认休眠关机重新配网清除蓝牙对恢复默认？' --format lvgl \
    -o src/generated/papermono_heading_48.c --force-fast-kern-format --lv-include lvgl.h
npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --no-kerning --bpp 1 --size 24 \
    --font "$task_serif_font" -r 0x20-0x7e,0x3000-0x301f,0x4e00-0x9fff,0xff00-0xffef \
    --symbols '·—…°℃' --format lvgl -o src/generated/papermono_reading_24.c --lv-include lvgl.h
npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --no-kerning --bpp 1 --size 40 \
    --font "$task_serif_font" --symbols '本地书架不可用没有可显示的书籍暂无日程待办便签尚未配置数据源书签' --format lvgl \
    -o src/generated/papermono_state_40.c --lv-include lvgl.h
python3 scripts/normalize_field_notes_fonts.py
