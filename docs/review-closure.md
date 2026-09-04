# 代码 Review 闭环记录

更新日期：2026-08-31

本文记录 PaperMono C153 代码 Review 的处理结果。构建和主机测试不等于
真实硬件验收；2026-08-27 基线 Review 当时没有重新写入设备，2026-08-31
触摸可靠性追加闭环已构建并写入当前默认镜像。所有未闭环项目继续以
[验收记录](acceptance.md) 为准。

## 2026-08-31 触摸可靠性追加闭环

| 问题 | 处理结果 |
| --- | --- |
| USB 日志采集后只显示旧墨水屏页面 | 找到采集器旧 `--reset` 序列实际进入 `boot:0x21 (DOWNLOAD)`；改为 DTR 始终释放的应用复位，默认使用无控制线动作的被动采集 |
| 触摸偶发完全无响应 | FT6336 从 20 ms system workqueue 改为独立 4 KiB、8 ms 预抢占线程；同步 input 回调不再占用 1 KiB system workqueue 栈 |
| 刷新边界丢手势 | 修复回调等待锁期间刷新已解除后仍无条件返回的竞态；移除会与普通入队竞争的整队列 purge |
| 刷新期间连点穿透 | 每次刷新只保留第一个完整手势，后续完整手势合并，不绕过输入守卫 |
| 系统刷新事件重复 | 移除旧 purge 配套的 `QUEUED -> PENDING` 回退，避免未被删除的事件再次入队 |
| 控制器离线后触点可能锁住 | 在线失败和离线重探阶段都执行 100 ms 陈旧触点释放 |
| USB-only 诊断不足 | 状态 API 增加按下/释放次数和最大轮询间隔；USB 过滤器保留 FT6336、touch 和 UI 证据 |

追加验证：默认镜像构建通过，十八组主机测试全部通过；内部 DRAM
320,640 / 399,108 bytes（80.34%，低于 85% 水位）。最终镜像
`16084c2b...582575` 已写入并通过 hash 校验。前序候选已在实机完整捕获
FT6336 按下/释放、UI 入队/处理、刷新期间手势保留以及 SSD1677 局刷；最终
边界修正版仍需一次写入后的物理页面切换复验，不能只凭构建或静态墨水屏判定。

## 2026-08-31 全项目 Review 与仓库整理

| Review 项 | 处理结果 |
| --- | --- |
| Git 基线 | 在项目根目录初始化 `main` 分支；本轮不擅自暂存或提交 |
| 内容目录扫描遗留 | 删除已被线性 FatFs 遍历替代的 `fat_modified_time()`；补齐 host FatFs 桩，并让 `f_readdir()` I/O 错误返回 `-EIO`，不再把截断索引误报为成功 |
| 无效接口与状态 | 删除无人调用的显示刷新原子状态、ZIP 按序号取条目接口、时间同步状态 getter；路由器当前应用查询收为实现私有函数 |
| 无效配置/脚本 | 删除没有任何门控效果的 `PAPERMONO_INFO_CARDS` 开关、危险且硬编码本机路径的 `build-review.sh`，以及 GBK 生成器不可达的 non-BMP sentinel 分支 |
| 网页跨 origin 接力 | 修复 `sessionStorage` 不能跨 SoftAP/LAN origin 的问题；认证会话先申请 60 秒一次性 ticket，只把 ticket 放入 fragment，目标页立即清除后通过 POST 兑换；长期 token 不进入 URL |
| 配对协议和文档 | 网页与文档统一为八位 Crockford Base32 配对码、首次 ticket 兑换和 `/api/v1/session` 双向 ticket 接口 |
| 构建可复现性 | 将外部 Zephyr/hal_espressif 中已验证的 MWDT 启动顺序修复收录为两个锁定版本补丁，并补充 README 应用命令 |
| 许可证与 HIL 工具 | 为 PDM binding/生成字体补 SPDX；USB HIL 采集必须显式提供设备序列号，不再内置单台设备身份 |

验证结果：十八组 host 测试全部通过；内容中心脚本通过 JavaScript 语法与 ticket
静态合约检查。默认固件重新构建成功，FLASH 2,207,748 bytes（13.16%）、内部
DRAM 320,648 / 399,108 bytes（80.34%），审查镜像 SHA-256 为
`744a934a...228a1acc`。该镜像只归档、未写入设备；构建不会替代 TF、SoftAP
浏览器接力、BLE 和触摸的实机复验。

## 已处理问题

| Review 项 | 处理结果 |
| --- | --- |
| clean workspace 缺模块 | `west.yml` allowlist 补齐 FatFS、Mbed TLS、TF-PSA-Crypto 和 MCUboot；干净 workspace 的 `west update`、默认构建和 sysbuild 已通过 |
| `papermono_board.c` 死代码 | 删除重复且从未进入链接的板级初始化文件，M5PM1 初始化职责只保留在驱动中 |
| RX8130 分钟档不可达 | 使用 64-bit 毫秒阈值修正秒/分钟/小时分频选择 |
| RTC RAM 失败恢复 | 初始读取失败立即退出；只有成功保存原值后才写回，并保留第一个测试错误 |
| selftest 依赖不完整 | Kconfig 补齐显示、输入、存储、Wi-Fi、LoRa、NFC、麦克风、充电计等依赖，非法组合在配置期被拒绝 |
| 内部 DRAM 水位偏高 | 显示与 LVGL 大缓冲已迁入 PSRAM；新增 `scripts/check_memory_budget.py`，默认上限 85% |
| display readiness 顺序 | `papermono_power_prepare_peripherals()` 在调用显示 API 前检查设备 readiness |
| M5PM1 wake 冲突位 | 对照官方 M5PM1 说明确认 GPIO0/GPIO2、GPIO3/GPIO4 是两组互斥唤醒源，并在代码中注明硬件约束 |
| SSD1677 CS/DC 错误被吞 | transaction begin/end 改为返回 GPIO 错误；结束阶段仍保证尝试释放 CS，并返回首个失败 |
| 文档 API 名不一致 | 统一为 `papermono_ssd1677_refresh_gray4()` |
| README 构建上下文 | 明确 workspace 父目录、SDK 检查、Espressif blob、Zephyr C17 patch、默认构建和 sysbuild 命令 |
| 仓库卫生 | 新增 `.gitignore`，删除重复源码和临时构建目录；仅在忽略的 `artifacts/` 保留最终可刷写镜像及校验值 |
| HIL 覆盖描述超前 | 驱动矩阵区分自动 selftest、手工破坏性 HIL 和尚未完成的实机门槛 |
| 负数传感器日志 | 修复 `val1 == 0 && val2 < 0` 时丢失负号的问题 |
| NFC/PDM 公共 API 前置检查 | 在解引用设备私有数据和加锁之前统一检查空指针、ISR 上下文与 readiness |

MCUboot 额外使用 `sysbuild/mcuboot.overlay` 禁用应用级 M5PM1/M5IOE1
节点，避免 bootloader 链接并不需要的 I2C GPIO 扩展器驱动；应用镜像的 DTS
不受影响。

## 2026-08-27 基线验证结果

- 主机测试：`tests/host/run.sh` 九组全部通过，包括 21,419 个实际字形和完整
  基础 CJK 覆盖。
- 默认完整构建：通过；FLASH 13.44%，内部 DRAM 80.77%，PSRAM 54.30%。
- `driver_selftest.conf` 完整构建：通过；内部 DRAM 76.69%。
- 干净 workspace：清单按锁定 revision 解析成功，Espressif blob 就绪后默认
  完整构建通过。
- 干净 sysbuild：MCUboot 与主应用均配置、编译和链接通过；主应用内部 DRAM
  81.90%，低于 85% 工程水位。
- 没有执行 flash，因此这些结果不改变触摸唤醒、TF、SoftAP、BLE 和并行压力
  的真实硬件验收状态。
