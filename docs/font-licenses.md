# 字体与图标来源及许可证

仓库根目录的 MIT 许可证不会把下列第三方字体字形重新授权为 MIT。生成后的
LVGL 字体数据继续按原字体的 SIL Open Font License 1.1（OFL-1.1）分发；
Phosphor 图标及其 1-bit 派生图像按 Phosphor 的 MIT 许可证分发。

## 已检入的字体资源

| 生成文件 | 上游来源与字形范围 | 许可证 |
| --- | --- | --- |
| `src/generated/papermono_cjk_16.c` | Source Han Sans SC Normal 提供英文、符号、中文标点、基础 CJK 和全角字形；GNU Unifont 17.0.04 仅补齐 `U+9FF0-U+9FFF` 16 个字形。完整范围记录在文件头 | OFL-1.1；见 `licenses/SourceHanSansSC-OFL-1.1.txt`、`licenses/GNU-Unifont-OFL-1.1.txt` 及下方 Unifont 说明 |
| `src/generated/papermono_titles_28.c` | Source Han Sans SC Normal 的 28 px、1-bit 子集，包含 `U+0020-U+007E` 和首页、启动器所需中文标题 | OFL-1.1；见 `licenses/SourceHanSansSC-OFL-1.1.txt` |
| `src/generated/papermono_digits_58.c` | Montserrat Medium 的 58 px、1-bit 子集：`%`、连字符、数字 `0-9` 和冒号 | OFL-1.1；见 `licenses/Montserrat-OFL-1.1.txt` |
| `src/generated/papermono_digits_96.c` | Montserrat Medium 的 96 px、1-bit 子集：连字符、数字 `0-9` 和冒号 | OFL-1.1；见 `licenses/Montserrat-OFL-1.1.txt` |

Source Han Sans SC Copyright 2014-2021 Adobe，保留字体名 `Source`。本项目的
生成字体使用 `papermono_cjk_16` 和 `papermono_titles_28` 作为固件符号名；
上游来源为 [Adobe Source Han Sans](https://github.com/adobe-fonts/source-han-sans)。

GNU Unifont 17.0.04 以 OFL-1.1 或 GPL-2.0-or-later 加字体嵌入例外双重授权；
本项目对上述 16 个补充字形选择 OFL-1.1。Copyright 1998-2026 Roman
Czyborra, Paul Hardy, Qianqian Fang, Andrew Miller, Johnnie Weaver, David
Corbett, Ælla Chiana Moskopp, Rebecca Bettencourt, Ho-Seok Ee, et al.

Montserrat Copyright 2011 The Montserrat Project Authors。两个数字字体由 LVGL
仓库随附的 `scripts/built_in_font/Montserrat-Medium.ttf` 生成；上游项目为
[The Montserrat Font Project](https://github.com/JulietaUla/Montserrat)。

所有生成字体均为 1-bit 静态字形数据，文件头保留尺寸和字符范围；新加入的
28/58/96 px 子集还保留了 `lv_font_conv` 生成参数。
固件公开的 LVGL 符号分别为 `papermono_cjk_16`、`papermono_titles_28`、
`papermono_digits_58` 和 `papermono_digits_96`。旧的 LVGL 稀疏 Source Han
字库已关闭，避免重复占用 Flash。

## Phosphor cat 图标

- 原始矢量：`assets/phosphor/cat-fill.svg`
- 上游文件：[Phosphor Icons `cat-fill`](https://github.com/phosphor-icons/core/blob/main/assets/fill/cat-fill.svg)
- 固件派生资源：`src/generated/papermono_cat_64.c`，64 × 64、1-bit LVGL I1
  图像，公开符号为 `papermono_cat_64`
- 转换工具：`scripts/generate_ui_asset.py`
- 许可证：MIT，Copyright (c) 2023 Phosphor Icons；完整文本见
  `licenses/Phosphor-Icons-MIT.txt`

原始 SVG 和生成的 C 图像都保留 MIT 标识。固件构建只使用已经检入的 C 数据，
不会在构建阶段联网下载图标，也不会依赖 SVG/PNG 转换工具。

## 构建接入

上述四个字体文件和 `papermono_cat_64.c` 均在根目录 `CMakeLists.txt` 的
`target_sources(app PRIVATE ...)` 中显式列出。字体源文件和 SVG 只用于来源审计
与重新生成，不会作为运行时文件系统内容部署到设备。
