# 应用图标检查与重绘

检查两页全部 11 枚图标。重绘微信读书、便签、内容中心、信息卡片、硬件工具，微调书库与待办；日历、遥控器、设置、电源的资源逐字节保留。7 枚原图同步生成对应 48/176 px 变体，合计更新 12 份 C 资源，另外 22 份资源保持不变。所有既有 SPDX 文件头保留。

`icon-comparison.png` 在 96 px 原尺寸对照全部入口。`derived-icons.png` 在原尺寸检查 5 枚派生图标；`overview.png` 是当前两页应用的原尺寸拼接。目录内 12 张单页 PNG 是生产 UI C 源码与真实 LVGL 软件渲染器的 480×800 I1 输出；网络、硬件和内容均为合成 fixture。PNG 只增加来源文字元数据，没有改变原生页面像素；`SOURCES.txt` 保存构建输入哈希。

验证通过：

- `python3 tests/host/test_field_notes_assets.py`：34 份二值透明度 A8、重复生成一致、只读检查与资源篡改检测、混合 SPDX 许可保留。
- `bash tests/host/run.sh`：完整主机回归通过。
- `OUT=tmp/icon-redraw-20261026 bash tests/preview/build.sh launcher-1 launcher-2 library library-populated weread tasks tasks-populated notes notes-populated info content-center tools`：12 个相关原生场景通过；保留上一轮其余场景的基线，不将本次检查表述为重新运行全部场景。
- `west build -d build-style-refresh`：默认带 MIC 固件构建通过。`scripts/check_memory_budget.py` 报告内部 DRAM 338,520 / 399,108 字节（84.82%，上限 85%），与上一版相同；两页应用主机 LVGL 已用内存分别为 13,400 与 8,776 字节。
- 全部 15 张 PNG 有 `impeccable:prompt` 来源记录；`git diff --check` 通过。图标几何没有增加运行时对象，A8 尺寸和字节数保持相同。

构建固件 SHA-256、逐文件变更范围及原生堆统计见 `metrics.json`。独立终审结果见 `review.md`。

本次未刷机；构建和主机预览不代表实机显示、残影、触摸或运行时余量已经验收。既有页面布局、日历日号叠加和命中区域保持原实现。
