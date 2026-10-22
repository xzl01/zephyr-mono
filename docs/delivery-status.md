# 交付状态（2026-10-21 更新）

一页式交付状态：哪些已经实现、哪些已在实机验证、哪些还缺什么。所有结论都指向
仓库内的证据文件，避免「实现」与「验证」被混为一谈。

## 判定口径

- **已实现**：源码 + 主机测试 + 固件构建通过。
- **已实机验证**：在 C153 上取得串口/屏幕证据，日志可用
  `scripts/papermono_usb_hil_capture.py` 复现。
- **缺条件**：需要特定外部条件（人手触屏、手机连 SoftAP、断电重启等），
  不是代码问题。

## 矩阵

| 领域 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 构建与主机测试 | **已实现** | 默认镜像与 `driver_selftest.conf` 均 0 warning；主机测试 25 组全通过 |
| 显示（SSD1677） | **已实机验证** | 全刷 4312–4316 ms、局刷 738 ms、4 级灰阶；见 [SSD1677](ssd1677-hil.md) |
| 触摸与交互 | **已实机验证** | FT6336 就绪、边缘返回手势、输入串行化 |
| TF 卡与内容存储 | **已实机验证** | CLK/CMD 接反修复后：4-bit/20 MHz/HS、FAT 挂载、写入回读、导入事务、事务清理；见 [TF 卡记录](tf-card-20261020.md) |
| 设备内格式化入口 | **已实机走查** | 系统状态页底部「格式化 TF 卡」→ 确认页 → 取消返回 → 确认执行，实机日志 `TF card formatted from the device UI`；走查使用非破坏性 HIL 镜像（`PAPERMONO_HIL_FAKE_UNFORMATTED`，模拟格式化、卡片数据保留），真实 mkfs 已由 microSD FAT 自检与早前实卡格式化分别验证 |
| 书库 / 阅读器 | **已实现** | 分页、进度、缓存校验、残影维护；长读残影未做耐久 |
| 日历 / 待办 / 便签 | **已实现** | 组织器在实机加载 `events=0 tasks=0 notes=0 error=0` |
| 微信读书书架 | **已实现** | JSON 结构校验、相邻去重；真实书架文件待用户导入 |
| 内容中心 HTTP | **已实机验证（端到端）** | 手机热点 2.4 GHz 下完成：配对→会话→txt 25 B/10 KB 上传→**EPUB 上传**→空 body 411→未授权 401→列表→**DELETE 200 且条目消失**。过程中修复两个缺陷：PSRAM 读缓冲导致 ≥1 KB 上传失败、DELETE 被精确资源遮蔽返回 405（见 [上传缺陷](<docs/upload-psram-20261021.md>)） |
| 配网与 SoftAP | **回退已修复** | STA 异步断开（含 status=-1）会回退 SoftAP 并按 5→10→20 s 退避重连，实机日志验证；AP 启用后的 STA 重连仍未成功，待排查（见 [上传缺陷](<docs/upload-psram-20261021.md>)） |
| BLE HID / 同步 | **部分验证** | HID 与 sync 初始化就绪、ACL 缓冲已与控制器对齐；真实主机配对未做 |
| 遥控器语音 | **部分实机验证** | 2026-10-22 实机通过：启动、按住说话/松开、AUDIO_START/STOP、虚拟麦克风 `MiRemoteV 2ch` 链路、60 秒上限；修复 PDM 实际 8 kHz 与 I2S 阻塞溢出。语音期间自动暂停 Wi-Fi 后 16 kHz 送达率约 88%；目标 App/Windows/Linux 和长时间音质未验证。见 [实机验收记录](remote-mic-hil-20261022.md) |
| 电源与深睡 | **已实机验证** | 50 次深睡唤醒耐久 **PASS 50/50**（`Power HIL endurance PASS: 50/50 wake/display cycles verified`，约 35 分钟，含逐个唤醒的全刷/局刷与显示恢复）；过程有少量 M5IOE1 I2C 重试告警，恢复机制均成功 |
| 状态灯 / 蜂鸣器 / 前光 | **已实现** | 策略有主机测试；前光实际观感未验收 |
| 可靠启动 | **已修复** | 早期双异常根因是内核初始化前中断取到零 IRQ 栈，`__start` 提前屏蔽中断后整芯片冷复位 **10/10** 正常启动（见 [冷复位异常定位](<docs/boot-crash-20261021.md>)）；ROM SHA 警告已修复，构建自动追加镜像摘要（见 [镜像摘要](<docs/firmware-image-digest.md>)）。 |

## 当前未完成项

1. **内容中心 HTTP 端到端**：需要手机/电脑连 `PaperMono-XXXXXXXX`，输入屏幕上的
   配对码，上传一个 txt 与一个 epub。
2. **50 次深睡/唤醒耐久**：脚本与镜像已就绪（`power_timer_hil.conf` +
   `REPEAT_COUNT=50`），只需设备空闲约 35 分钟跑完。
3. **设备内「格式化 TF」UI 走查**：需要触屏操作。

最新组合恢复后 TF 挂载、UI 渲染、SoftAP 接口校验、BLE 初始化均完成；
但 ROM SHA warning 与 RTC 复位后早期异常仍需修复，不能据此宣称启动可靠性已交付。
正常镜像已恢复随机凭据，不含临时测试的路由器密码。

## 恢复/更换设备后的验收顺序

设备恢复或更换后，按下面顺序做，每步都有明确的成功标志；前一步不通过就不要往下做。

1. **启动**：`python3 scripts/papermono_usb_recover.py --verify-only`
   成功标志：出现 `m5pm1 ... ready`、`TF content store mounted at /SD:`、
   `PaperMono LVGL page rendered`。
2. **刷入当前固件**：`python3 scripts/papermono_usb_recover.py build/zephyr/zephyr.bin`
   成功标志：`[flash] verified` 且随后串口有启动日志；启动日志中
   `wrn`/`err` 计数应为 0，且**不再出现** `failed to unlink path`。
3. **TF 读写**：`driver_selftest.conf` 镜像，期望
   `HIL microSD FAT check passed` 与 `HIL content import check passed`。
4. **50 次深睡唤醒**：`driver_selftest.conf;power_timer_hil.conf` 加
   `CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT=50`，期望
   `Power HIL endurance PASS: 50/50`。
5. **内容中心 HTTP**（需手机）：连 `PaperMono-XXXXXXXX`，输入屏幕配对码，
   上传 txt 与 epub；期望设备日志出现导入与 generation 递增，网页列表可见条目。
6. **UI 走查**（需触屏）：系统状态页 → 未格式化 TF 时「格式化 TF 卡」→ 确认页。
7. **主机测试**：`sh tests/host/run.sh`，期望 25 组全 PASS。

## 一键验收

设备恢复/更换后，优先跑：

```sh
scripts/papermono_acceptance.sh build
```

它会依次执行：主机测试 → 固件构建（0 warning 校验）→ 设备串口探测 →
烧录校验 → RTC watchdog + Espressif JTAG 自动恢复 → 启动日志检查（TF/UI/BLE），
最后打印仍需人工完成的三项（50 次唤醒、HTTP 上传、格式化 UI 走查）。

## 复现命令

```sh
# 主机测试
sh tests/host/run.sh

# 构建
west build -p always -d build -b m5stack_papermono/esp32s3/procpu .

# 烧录（stub 校验失败时改用 ROM loader）
west flash -d build --esp-device /dev/cu.usbmodem21201
python3 scripts/papermono_usb_recover.py build/zephyr/zephyr.bin

# 串口证据采集
python3 scripts/papermono_usb_hil_capture.py --serial 28:84:85:44:6D:A4 \
    --mode boot --timeout 35 --reconnect-settle 0 --reset
```

## 2026-10-21 关于复位和供电的再次更正

过去的“供电不足／电池没电／必须手动电源循环”没有测量证据，撤回；
“所有静默都是 ROM 下载模式、usb-reset + hard-reset 总能恢复”也不成立。
本轮通过 JTAG 读到 Zephyr 早期双异常，另一次则确认旧 strap 留在下载模式。

现在默认使用 RTC 整芯片复位后接 JTAG reset run，恢复前先开串口采集，
正常固件连续 3/3、最终写入后自动恢复均已通过。它不需要用户手动开关，
但不等于 PMIC 掉电，也不表示早期异常已被底层修复。
详见 [USB 恢复记录](<docs/usb-reset-20261021.md>) 和
[更正后的旧排查记录](<docs/boot-reset-pitfall-20261020.md>)。

运行恢复/验收前，须设置 `PAPERMONO_OPENOCD` 为真正 Espressif OpenOCD；
普通 boot 采集须加 `--mode boot`。完整验收 shell 本轮未从头执行，
不把局部恢复测试当作整份产品验收。
