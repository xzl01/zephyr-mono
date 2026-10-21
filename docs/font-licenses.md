# 字体与图标来源及许可证

仓库根目录的 GPL-3.0 许可证不会把下列第三方字体字形重新授权为 GPL-3.0。生成后的
LVGL 字体数据继续按原字体的 SIL Open Font License 1.1（OFL-1.1）分发；
`src/generated/` 下的图标位图保留 MIT 声明，字体位图保留 OFL-1.1；它们作为独立素材计入 GPL-3.0 工程的聚合分发。已弃用的 Phosphor 猫图标
不再分发任何派生产物，仅保留其许可证文本用于来源审计。

## Field Notes 当前字体

| 生成文件 | 上游来源与字形范围 | 许可证 |
| --- | --- | --- |
| `src/generated/papermono_clock_148.c` | Libre Bodoni 148 px、1 bpp；`U+002D`、`U+0030-U+003A`，用于首页时钟 | OFL-1.1；`licenses/LibreBodoni-OFL-1.1.txt` |
| `src/generated/papermono_reading_24.c` | Source Han Serif SC Regular 24 px、1 bpp；ASCII、`U+3000-U+301F`、完整基础 CJK、全角范围及 `·—…°℃`，用于阅读、内容标题和主要操作文字 | OFL-1.1；`licenses/SourceHanSerifSC-OFL-1.1.txt` |
| `src/generated/papermono_heading_48.c` | Source Han Serif SC Regular 48 px、1 bpp；ASCII 和当前全部页头/确认标题中文子集，含中文问号。准确字符列表见生成脚本的 `--symbols` | OFL-1.1；`licenses/SourceHanSerifSC-OFL-1.1.txt` |
| `src/generated/papermono_state_40.c` | Source Han Serif SC Regular 40 px、1 bpp；当前书库、微信读书、日历、待办、便签、信息和书签空状态标题所用的 28 个中文字符 | OFL-1.1；`licenses/SourceHanSerifSC-OFL-1.1.txt` |

Libre Bodoni Copyright 2012 The Libre Bodoni Project Authors；源文件保存在
`assets/fonts/LibreBodoni.ttf`，上游为 [Libre Bodoni](https://github.com/googlefonts/Libre-Bodoni)。
Source Han Serif Copyright 2017-2022 Adobe，保留字体名 `Source`；上游为
[Adobe Source Han Serif](https://github.com/adobe-fonts/source-han-serif)。

`scripts/generate_field_notes_fonts.sh` 使用固定 `lv_font_conv@1.5.3` 生成以上四份
字体。Source Han Serif 源文件下载到 `.cache/field-notes-fonts`，并校验脚本中固定的
SHA-256；Libre Bodoni 使用检入的源文件。生成时全部采用 `--bpp 1`、不压缩。
`scripts/normalize_field_notes_fonts.py` 保留 OFL 标识，将阅读字体行框固定为
30 px、基线 8 px，并设置 `papermono_cjk_16` 缺字回退；状态标题行框固定为
50 px、基线 10 px。扩展后的 48 px 标题实际行框为 55 px、基线 14 px，
148 px 时钟实际行框为 117 px、基线 5 px；字号不等于排版标签高度。
新标题必须扩展相应固定子集，并重新执行字体覆盖检查。

## 保留的字体资源

| 生成文件 | 上游来源与字形范围 | 许可证 |
| --- | --- | --- |
| `src/generated/papermono_cjk_16.c` | Source Han Sans SC Normal 提供英文、符号、中文标点、基础 CJK 和全角字形；GNU Unifont 17.0.04 仅补齐 `U+9FF0-U+9FFF` 16 个字形。完整范围记录在文件头 | OFL-1.1；见 `licenses/SourceHanSansSC-OFL-1.1.txt`、`licenses/GNU-Unifont-OFL-1.1.txt` 及下方 Unifont 说明 |

Source Han Sans SC Copyright 2014-2021 Adobe，保留字体名 `Source`。本项目的
生成字体使用 `papermono_cjk_16` 作为唯一仍在构建中的 Sans 子集符号名；
上游来源为 [Adobe Source Han Sans](https://github.com/adobe-fonts/source-han-sans)。

GNU Unifont 17.0.04 以 OFL-1.1 或 GPL-2.0-or-later 加字体嵌入例外双重授权；
本项目对上述 16 个补充字形选择 OFL-1.1。Copyright 1998-2026 Roman
Czyborra, Paul Hardy, Qianqian Fang, Andrew Miller, Johnnie Weaver, David
Corbett, Ælla Chiana Moskopp, Rebecca Bettencourt, Ho-Seok Ee, et al.

Montserrat Copyright 2011 The Montserrat Project Authors。首页字标使用 LVGL
随附的内置 `lv_font_montserrat_18`（未校时提示为 48），来自 LVGL 仓库的
`scripts/built_in_font/Montserrat-Medium.ttf`；上游项目为
[The Montserrat Font Project](https://github.com/JulietaUla/Montserrat)，
许可证见 `licenses/Montserrat-OFL-1.1.txt`。

上述字体均为 1-bit 静态字形数据，文件头保留尺寸和字符范围。曾经的
`papermono_titles_28`、`papermono_digits_58` 和 `papermono_digits_96` 三个
自建子集已删除：Field Notes 层级不再引用它们，而它们仍占用约 132 KiB Flash。
保留旧的 LVGL 稀疏 Source Han 字库仍是关闭状态。

## Field Notes 原创几何图标

`scripts/generate_field_notes_assets.py` 使用 Pillow 的确定性几何绘图生成 33 个
图标，所有公开符号均以 `papermono_icon_` 开头，输出路径为
`src/generated/papermono_icon_*.c`：

| 尺寸与用途 | 符号后缀 |
| --- | --- |
| 96 × 96，应用、电源选项与确认动作（17 个） | `library`、`weread`、`calendar`、`tasks`、`notes`、`remote`、`content`、`info`、`tools`、`settings`、`power`、`sleep`、`bookmark`、`document`、`wifi`、`bluetooth`、`restore` |
| 48 × 48，内容类型与操作图标通道（10 个） | `book_small`、`headphones_small`、`bookmark_small`、`settings_small`、`sun_small`、`toc_small`、`return_small`、`library_small`、`document_small`、`info_small` |
| 176 × 176，空状态焦点符号（6 个） | `library_focal`、`weread_focal`、`calendar_focal`、`tasks_focal`、`document_focal`、`bookmark_focal` |

各尺寸从同一原创几何图形以最近邻方式生成。图标不描摹参考图中的栅格图像；
脚本和生成 C 文件使用 MIT 标识，适用仓库根目录 `LICENSE`。

图像格式为 LVGL A8，stride 等于各图标宽度，每个像素仅有 0 或 255 透明度，以支持实际
单色软件渲染路径；这不是 1 bpp 存储，也不是灰阶素材。运行时重着色为黑色，
固件屏幕仍为 I1。重新生成需要 Pillow，普通固件构建使用检入的 C 数据。

## Phosphor cat 图标（已弃用）

Field Notes 方向用原创几何图标取代了首页猫形标记，因此派生产物已删除，仓库中
不再分发任何 Phosphor 图像数据：

- 已删除的原始矢量：`assets/phosphor/cat-fill.svg`
- 已删除的固件派生资源：`src/generated/papermono_cat_64.c`，64 × 64、1-bit
  LVGL I1 图像，公开符号为 `papermono_cat_64`
- 上游参考：[Phosphor Icons `cat-fill`](https://github.com/phosphor-icons/core/blob/main/assets/fill/cat-fill.svg)
- 许可证文本保留在 `licenses/Phosphor-Icons-MIT.txt`（MIT，
  Copyright (c) 2023 Phosphor Icons），用于来源审计

删除原因包含一项实测缺陷：该资源声明为 `LV_COLOR_FORMAT_I1`，而 LVGL 9.5 的
软件渲染器 `lv_draw_sw_img.c` 没有 I1 源图像路径，`lv_image_set_src()` 对它
静默不绘制。若将来重新引入单色位图，必须改走 A8（见上节 33 个图标），或在
vendor 树中补齐 I1 源图像路径。删除后
`scripts/generate_ui_asset.py` 成为不产出构建输入的通用 SVG→1 bpp 转换工具，
保留供后续使用。

## 构建接入

上述字体文件（`papermono_cjk_16`、`papermono_clock_148`、`papermono_reading_24`、
`papermono_heading_48`、`papermono_state_40` 以及 GBK 码表）和 33 个 Field Notes 图标
均在根目录 `CMakeLists.txt` 的
`target_sources(app PRIVATE ...)` 中显式列出。字体源文件和 SVG 只用于来源审计
与重新生成，不会作为运行时文件系统内容部署到设备。
