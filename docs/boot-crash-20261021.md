# 2026-10-21 冷复位后早期双异常：阶段性定位

## 现象

对已连接的 C153（ESP32-S3 rev v0.2）执行 **整芯片复位**（RTC 看门狗）后，
应用在**内核初始化之前**进入 `_DoubleExceptionVector`，PRO CPU 停在
`0x403743c0`。串口无输出、屏幕保持旧画面——这就是此前反复出现的“设备静默”状态。

JTAG halt 读到的固定签名：

| 寄存器 | 值 | 解析 |
| --- | --- | --- |
| PC | `0x403743c0` | `_DoubleExceptionVector` 自旋 |
| EPC1 | `0x40385dfd` | `rtc_clk_cpu_freq_set_config` 的 `retw.n` |
| DEPC | `0x403740d8` | `_WindowUnderflow8` 最后一条 `l32e a7, a7, -20` |
| EXCCAUSE | `0` | 双异常不更新该寄存器 |
| `_kernel` / `z_idle_threads` | 全 0 | 尚未进入内核初始化 |
| `a1` | `0x3fceb4f0` | 仍在 ROM 启动栈内 |

即：`hardware_init()` → `bootloader_clock_configure()` →
`rtc_clk_cpu_freq_set_config()` 返回时触发窗口下溢，下溢处理程序自身再次异常，
形成双异常。

## 关键区分

| 复位方式 | 结果 |
| --- | --- |
| RTC 看门狗整芯片复位（esptool 或 JATG 触发） | **复现崩溃**：r8-b1 12/12、r5-b5 8/8、最小探针镜像 5/6 |
| Espressif JTAG `reset run`（只复位数字域，RTC 保留） | 每次都正常启动到 TF/UI/BLE |
| 用户真实电源键开关（PMIC 断电再上电） | 正常启动 |

因此：**不是镜像回归**（昨天的 r5-b5 同样复现，最小探针镜像也复现），
也**不是应用代码问题**（崩溃发生在内核初始化之前）。

## 已排除或需要的假设

- **Erratum 572（零开销循环缓冲）**：把 `hardware_init` 写入的 MEMCTL 改为 0
  禁用该缓冲后仍 5/10 崩溃 → 不是（或不只是）该勘误。
- **本地 `hw_init.c` 补丁**（把 `config_wdt()` 提前）：去掉后崩溃率从 12/12 降到
  4/8，说明有放大作用但不是唯一原因。
- **窗口继承（ESP-IDF #4bf8d49）**：同类崩溃在 IDF 中通过启动时
  `xthal_window_spill()` 修复；Zephyr 无 `xthal_window_spill`，
  需要在 `__start` 用等价汇编实现。尚未验证。
- **ROM soft-reset / run**：只会停在 ROM 下载模式，不能作为启动路径。

## 2026-10-21 后续复测

- 修复镜像 SHA 摘要后（见 [镜像摘要](<docs/firmware-image-digest.md>)），
  冷复位崩溃仍存在：新构建 3/8 落在 `_DoubleExceptionVector`。
- 精确到中断入口：`_handle_excint` 在 `0x4037b283` 执行
  `l32i a2, a2, 0x2c`，此时 `a2 = *(a1) = 0`；`0x2c` 正是 BSA 的 scratch
  偏移，`EXCCAUSE=0x1c`（load prohibited）、`EXCVADDR=0x2c`，与寄存器读数一致。
- 已排除：镜像摘要缺失（已修复但崩溃仍在）、bootloader 时钟重配
  （跳过仍崩）、运行时 240 MHz 切换（跳过仍崩）、MEMCTL/Erratum 572、
  本地 `config_wdt()` 提前补丁、`wdt_hal_disable` 补丁。
- 仍成立的相关性：只有“整芯片复位”（RTC 看门狗，无论由 esptool 还是
  OpenOCD 触发）后出现；JTAG `reset run`（数字域复位）和真实电源循环后正常。

## 根因与修复

崩溃点在中继入口 `_handle_excint`：它从 IRQ 栈帧首字段取 `ptr_to_bsa`，
再读 BSA 里的 C handler 指针。内核初始化前 `_kernel.cpus[0].irq_stack`
仍是 0（BSS 零值），此时若有中断被取走，`ptr_to_bsa` 即为 0，
读取 `0x2c` 触发 `EXCCAUSE=0x1c`（load prohibited，`EXCVADDR=0x2c`）；
由于 `EXCM=1`，直接进入双异常向量自旋。这与实测寄存器
（`DEPC=0x4037b283`、`a1[0]=0`、`_kernel` 全零）完全吻合。

整芯片复位（RTC 看门狗）后部分中断源可能已挂起，启动早期就会命中；
JTAG `reset run` 只复位数字域，会清掉这些挂起状态，所以此前表现成
“JTAG 能救、真实断电也行”。既不是镜像问题，也不是电量或应用代码问题。

**修复**：`__start` 在调用 `hardware_init()` 前把 PS.INTLEVEL 设为
`XCHAL_NUM_INTLEVELS`，屏蔽所有中断级别，直到内核 `z_init_cpu()`
安装好每 CPU 的 IRQ 栈、并由线程上下文恢复合适的 PS。
改动位于 `soc/espressif/common/loader.c`。

**验证**：

- 整芯片冷复位连续 **10/10** 落在 `arch_cpu_idle`/`busy_wait`，不再进异常向量。
- 完整启动日志 9399 字节：TF 挂载 6.255 s、UI 渲染 10.927 s、
  SoftAP 校验 12.303 s、BLE HID/同步 12.364 s；
  `<wrn>`/`<err>` 计数 **0**，`SHA-256 comparison failed` 计数 **0**。

## 对交付的影响与当前处置

- 早期双异常��定位并修复（见上节），整芯片冷复位 10/10 正常启动。
- 恢复工具已能自动把设备救回（`scripts/papermono_usb_recover.py`，
  RTC 看门狗 + JTAG reset run），无需人工开关，实测恢复正常固件 3/3。
- 复现脚本：`/Volumes/extend/tmpwork/papermono-reset-tools/repro_boot.py`
  （每次整芯片复位后用 JTAG 读 PC 与异常寄存器）。

## 后续

- 该修复值得提交上游（Zephyr ESP32 `__start` 早期中断屏蔽 / 提前安装 IRQ 栈）。
- 继续跑剩余产品验收：内容中心 HTTP 端到端、50 次深睡唤醒、格式化 UI 走查。
