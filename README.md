# PaperMono C153 Zephyr 个人信息终端

本仓库为 M5Stack PaperMono C153 提供 Zephyr 4.4.1 板级支持、外设驱动和
基于 LVGL 的 480 × 800 单色墨水屏个人信息终端。目标硬件为 ESP32-S3R8、
16 MB Flash 和 8 MB Octal PSRAM。

默认固件已从早期三页演示重构为应用路由框架，界面以简体中文为主。设备不
实现软键盘或输入法；书籍、日历、待办和便签由电脑或手机维护，设备负责浏览
以及翻页、书签、勾选和收藏等轻量操作。

## 已实现功能

- 系统底座：应用路由、页面生命周期、统一输入队列、刷新调度器、NVS 设置、
  全局返回/回首页手势、损坏设置回退、硬件 TRNG、可恢复的休眠/关机确认、
  五档前光亮度、集中式 RGB 状态指示、默认 `Asia/Shanghai` 和 24 小时制。
- 首页：时间、电量、Wi-Fi/BLE/TF 状态、下一日程和最近阅读入口。
- Field Notes 界面：纸白留白、衬线大时钟、横线分区和黑白图标；首页显示继续
  阅读、下一日程和待办，底部固定首页/应用/设置。启动器使用有间距的直角
  3×3 图标网格，11 个应用分两页显示，触控与绘制共用布局；阅读正文为
  24 px、1-bit 字体，菜单、书签和进度独立呈现。
- 阅读器：TF 书库、UTF-8/GBK TXT、文字型无 DRM EPUB 2/3、目录、进度、
  书签、最近阅读、阅读页精确恢复、内容变更自动重扫、三档阅读密度、中文标点
  禁则、可自动重建的 TF 页索引，以及覆盖完整基础 CJK 的 1-bit 字体。
- 微信读书实验书架：从内容中心原子导入无凭据的 `weread-v1.json`，在设备上
  分页查看书名、作者、类型、进度、阅读时长和更新时间；当前不登录账号、不调
  私有接口，也不下载微信读书正文。
- 内容中心：首次启动 SoftAP、本地中文网页、一次性配对码、版本化 HTTP API、
  单事务流式上传、CRC32 可选校验、临时文件/备份原子替换、并发请求隔离和断网
  SoftAP 退避恢复；浏览器会话可在刷新后恢复，并在配网成功后从 SoftAP 自动
  接力到局域网地址；固件会验证 AP 接口的 SSID/信道并在异常时重新启动。
- BLE 遥控：标准 HID over GATT，包含媒体、演示和相对鼠标复合 Report，
  passkey 配对、bond 保存、清除配对和自动重连。
- 个人数据：ICS 日历、`tasks-v1.json`、Markdown/TXT 便签，以及与源文件分离
  的勾选、收藏、已读 overlay 状态。
- 信息卡片：与供应商无关的 Provider 接口、统一卡片模型和 NVS 离线缓存。
- 扩展：经过认证的 BLE 小数据同步服务，以及不影响主线的硬件工具入口。

> 状态边界：本节“已实现”指源码、主机测试和固件构建状态。单色编辑式界面镜像
> 2026-09-30 已修复系统工作队列栈溢出并刷入，USB 独立复位后的启动、全刷、局刷
> 和触摸处理日志通过，实机左边缘返回链路也已捕获。像素观感、长时间残影、
> 50 次休眠唤醒和运行时 LVGL 堆余量仍需验收。详见
> [启动恢复记录](docs/boot-recovery-20260930.md)。
>
> 当前交付状态与剩余缺口见 [交付状态](docs/delivery-status.md)；
> 2026-10-20 排查过一个「设备不启动」的假故障，根因是复位序列把芯片留在下载模式，
> 不是硬件问题。详见 [复位序列陷阱](docs/boot-reset-pitfall-20261020.md)。
>
> 2026-10-20 TF 卡首次打通：板级 pinctrl 曾把 SD 的 CLK/CMD 接反，卡收不到时钟，
> 初始化以 `-134` 失败。修正后卡可正常识别，并在实机完成 FAT 挂载、写入、
> 回读校验；期间把原先的 GPT 卡按操作者确认格式化为 FAT。内容中心的 HTTP 上传
> 端到端验证仍待完成。详见 [TF 卡打通记录](docs/tf-card-20261020.md)。
> 电源键深睡唤醒已补上并完成单次实机验证，触摸唤醒与 SSD1677 完整恢复刷新
> 同样取得日志；已刷回日常交互固件并经 USB-JTAG 软件复位运行到首页，但最新
> 镜像仍出现 USB 复位早期启动异常，可靠启动尚未闭环。详见
> [电源键唤醒记录](docs/button-wake-20260930.md)。

2026-09-30 响应优化已将刷新后的输入稳定窗口降至 32 ms，允许触摸线程抢占
UI 布局，左边缘支持短距离斜向右滑返回，并优先保留刷新期间的返回动作。
构建、镜像和实机验证范围见 [响应优化记录](docs/interaction-response-20260930.md)。

详细页面和操作见 [应用说明](docs/application.md)，内容目录和 HTTP/BLE 协议
见 [内容与同步接口](docs/content-api.md)，软件/实机验收边界见
[验收记录](docs/acceptance.md) 和 [驱动覆盖矩阵](docs/driver-coverage.md)。
本轮代码审查处理结果见 [Review 闭环记录](docs/review-closure.md)，字体来源与
许可证见 [字体许可证](docs/font-licenses.md)，CrossMux/FreeInk 的借鉴项和不移植边界见
[参考边界](docs/crossmux-reference.md)。视觉规则见
[Field Notes 设计系统](DESIGN.md)。

## 数据边界

- TF 卡内容根目录是 `/papermono/`；Zephyr 挂载点写作 `/SD:`，因此固件内
  完整路径是 `/SD:/papermono/...`。
- 书籍存放在 `books/`，日历存放在 `calendar/`，待办固定为
  `tasks/tasks-v1.json`，便签存放在 `notes/`，微信读书书架摘要固定为
  `weread/weread-v1.json`。
- 可重建的阅读页索引位于 `.cache/reader/`；删除该目录不会丢失书籍或
  NVS 中的阅读进度。
- 内部 192 KiB `storage` 分区只保存设置、内容稳定 ID、阅读状态、overlay、
  bond 和信息卡片缓存，不保存整本书。
- 网络、BLE 和存储回调不直接调用 LVGL；后台工作通过消息队列通知 UI。

## 构建

先安装 Zephyr 支持的 SDK、CMake、Ninja、Python 虚拟环境、`west` 和 PySerial。
构建流程适用于 Zephyr 支持的主机；USB 日志采集脚本依赖 POSIX `termios`，当前
在 macOS 上使用 `/dev/cu.usbmodem*` 验证。以下命令
从一个新的父目录创建完整 workspace；不要在仓库内部再次执行 `west init`：

```sh
mkdir papermono-workspace
cd papermono-workspace
git clone https://github.com/xzl01/zephyr-mono.git zephyr-mono
west init -l zephyr-mono
west update
west blobs fetch hal_espressif
west sdk list
git -C zephyr apply ../zephyr-mono/patches/zephyr-4.4-esp32-gdma-c17.patch
git -C zephyr apply ../zephyr-mono/patches/zephyr-4.4-esp32-wifi-apsta-runtime.patch
git -C zephyr apply ../zephyr-mono/patches/zephyr-4.4-esp32s3-watchdog-order.patch
git -C modules/hal/espressif apply ../../../zephyr-mono/patches/hal-espressif-19f979cf-mwdt-disable.patch
west build -p always -b m5stack_papermono/esp32s3/procpu zephyr-mono
```

前两个补丁修复 Zephyr 4.4.1 的 ESP32 GDMA C17 编译和 APSTA 运行时问题；后
两个补丁成对修复数字核复位后 MWDT0 仍在运行所导致的 simple-boot 重启循环。
它们分别锁定 Zephyr `v4.4.1` 与 `hal_espressif` 的 `west.yml` revision，升级
上游版本时必须重新审查，不能静默跳过。

验证默认 MCUboot sysbuild（仍从 workspace 根目录执行）：

```sh
west build -p always --sysbuild -d build-sysbuild \
  -b m5stack_papermono/esp32s3/procpu zephyr-mono -- \
  -DBOARD_ROOT="$PWD/zephyr-mono" -DDTS_ROOT="$PWD/zephyr-mono"
```

构建后执行内部 DRAM 水位检查；超过 85% 会返回非零状态：

```sh
python3 zephyr-mono/scripts/check_memory_budget.py build/zephyr/zephyr.map
```

主机端解析/安全测试：

```sh
sh zephyr-mono/tests/host/run.sh
```

默认固件不会运行自检、发射 LoRa 或自动进入一次性 HIL 电源动作。专项配置：

| Profile | 用途 |
| --- | --- |
| `partial_demo.conf` | 有限次数局刷与维护全刷 |
| `driver_selftest.conf` | 非破坏性板级驱动自检 |
| `power_touch_hil.conf` | 单次触摸深睡/唤醒 |
| `power_sleep_button_hil.conf` | ESP32-S3 深睡/M5PM1 电源键 IRQ 唤醒（不同于关机重启） |
| `power_touch_endurance_hil.conf` | 50 次触摸唤醒与显示恢复耐久验收 |
| `power_timer_hil.conf` | ESP32-S3 定时深睡/唤醒 |
| `power_shutdown_button_hil.conf` | M5PM1 关机/电源键唤醒 |
| `power_shutdown_rtc_hil.conf` | M5PM1 关机/RX8130 定时唤醒 |
| `power_shutdown_imu_hil.conf` | M5PM1 关机/IMU 唤醒 |
| `lora_rx_hil.conf` / `lora_tx_hil.conf` | 有限 RX / 显式 RF TX |

例如：

```sh
west build -p always -d build-selftest \
  -b m5stack_papermono/esp32s3/procpu zephyr-mono -- \
  -DEXTRA_CONF_FILE=driver_selftest.conf
```

`power_*.conf` 只描述高风险电源动作，必须与 `driver_selftest.conf` 合并构建，
例如 `-DEXTRA_CONF_FILE='driver_selftest.conf;power_touch_hil.conf'`。新 workspace
应先执行 `west update`，确认 `lvgl` 和 `fatfs` 已出现在 `west list` 再构建。

## 写入与 USB 日志

C153 没有单独引出的串口，开发和日志都使用 ESP32-S3 原生 USB Serial/JTAG：

```sh
west flash -d build --esp-device /dev/cu.usbmodemXXXX
python3 zephyr-mono/scripts/papermono_usb_hil_capture.py \
  --serial <USB序列号> --timeout 60 --reconnect-settle 1
```

不要用会在打开/关闭端口时自动改变 DTR/RTS 的普通串口监视器；ESP32-S3
原生 USB 会把控制线解释为复位/下载请求。采集器默认只读且关闭 HUPCL；它的
可选 `--reset` 始终保持 DTR 释放，只脉冲 RTS 进入 Flash 应用。

深睡时 USB 会消失，触摸唤醒后重新枚举。不要把 USB 行状态复位留下的 ROM
输出或调试器暂停状态误判为应用崩溃；验收以完整启动日志、运行状态和物理屏幕
结果共同判断。

## 墨水屏与 RF 安全

显示调度器串行化刷新；高压刷新期间仍采样 FT6336，并在刷新结束后投递首个
完整触摸手势，实体键保持屏蔽。SSD1677 在建立全刷基线后使用 OTP 局刷波形，
连续 10 次局刷后自动维护全刷，局刷失败会退回全刷。不要加入未验证波形或
无限局刷循环。

LoRa 默认只配置不发射。只有在确认天线、频率、功率和当地法规后才使用 TX
profile。NFC、LoRa 等只有完成真实标签/对端验证后才应标记为正式可用功能。

## 参考实现

- [M5Stack C153 产品资料](https://docs.m5stack.com/en/products/sku/C153)
- [M5Stack PaperMono UserDemo](https://github.com/m5stack/M5PaperMono-UserDemo)
- [M5Stack PaperMono OTP Demo](https://github.com/m5stack/M5PaperMono-OTP-Demo)
- [CrossMux](https://github.com/0x1abin/crossmux)
- [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk)
- [Zephyr LVGL 集成](https://docs.zephyrproject.org/latest/services/gui/lvgl.html)

板级时序参考上述 MIT 许可的 M5Stack 项目。项目原创代码采用 MIT License；
第三方及上游派生文件继续保留 Apache-2.0、OFL-1.1 或各自的许可证和归属，详见
`licenses/` 与 [字体许可证](docs/font-licenses.md)。

## 许可证

本项目以 **MIT License** 发布，全文见 [LICENSE](LICENSE)。选择 MIT 的原因：
它是本仓库文件头使用最广的许可（M5Stack 参考实现同样为 MIT），条款简单、允许
商用和闭源衍生，且与仓库内其他许可可以共存。

| 内容 | 许可 |
| --- | --- |
| `src/`、`include/`、`scripts/`、`tests/` 等原创代码 | MIT（[LICENSE](LICENSE)） |
| `patches/zephyr/` 中针对 Zephyr 4.4.1 的补丁（上游为 Apache-2.0） | Apache-2.0（[licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)），补丁本身随上游条款 |
| 字体（思源、Montserrat、Libre Bodoni 等） | SIL OFL-1.1（[字体许可证](docs/font-licenses.md)） |
| Phosphor 图标 | MIT（[licenses/Phosphor-Icons-MIT.txt](licenses/Phosphor-Icons-MIT.txt)） |

仅在个人设备上刷写本固件不涉及再分发；若你将修改后的固件或源码对外发布，请保留
`LICENSE`、`licenses/` 与相应文件头中的版权与许可声明。

提交补丁前请阅读 [贡献指南](CONTRIBUTING.md)；安全问题请按
[安全策略](SECURITY.md) 私下报告，不要在公开 issue 中附带 Wi-Fi 凭据、会话
令牌、配对码或设备唯一标识。
