# 2026-09-30 电源键深睡唤醒修复

## 确认的问题与修改

原来的日常深睡只启用触摸 GPIO4 的 EXT0；左右翻页键 GPIO2/3 和电源芯片
M5PM1 的 IRQ GPIO1 均未配置为 ESP 深睡唤醒输入，界面却提示电源键可唤醒。
M5PM1 按键中断屏蔽也未显式配置，不能依赖前一套固件留下的寄存器状态。

- 保留 EXT0 触摸唤醒，新增 GPIO1/2/3 的 EXT1 ANY_LOW；全部使用独立 RTC
  上拉，并由编译断言检查 RTC 引脚和 DT 标志。
- 深睡前显式解除 M5PM1 单击/唤醒中断屏蔽，禁用单击复位；保留其他按键
  时序、双击关机、长按与下载恢复配置。清除陈旧 IRQ 后等待所有输入释放。
- 输入就绪检查只读，不修改 FT6336 或 gpio-keys 运行态中断；失败继续走原有
  前光、状态灯和页面恢复路径。
- EXT1 来源区分 `esp_button` 与 `pmic_irq`，日志包含物理 GPIO 掩码。
  `pmic_irq` 不等同于 PMIC 关机后重新上电的 `pmic_button`。
- 新增显式 opt-in 的 `power_sleep_button_hil.conf`，要求 PMIC IRQ 唤醒且
  SSD1677 完整恢复刷新成功后才报告通过。默认镜像不自动运行 HIL。

M5PM1 寄存器依据：[官方驱动定义](https://github.com/m5stack/M5PM1/blob/main/src/M5PM1.h)
（IRQ_STATUS3=0x42、IRQ_MASK3=0x45、BTN_CFG1=0x49）。

## 软件验证

使用固定源码快照，避免工作区同时生成 UI/字体资源改变验证对象。
主机测试 24 组通过，新增 EXT1 空/未知/同时触发/高位掩码与来源标签用例。
默认、partial demo、driver selftest、power-button HIL 四配置构建通过，
内部 DRAM 分别为 324608、305136、307784、312056 bytes，均低于 85%。
另有 timer HIL 构建通过，尚未执行。`west manifest --validate` 通过。

## 实机状态

电源键 HIL 镜像已通过原生 USB 写入，Flash hash 校验成功。显式 USB 应用复位
后，完成 LVGL 绘制与外设检查，随后进入深睡并从主机 USB 列表消失。
TF 初始化仍报 -134，自检因此失败；电源 HIL 按现有策略继续隔离验证，
不能把本次休眠视为 TF 读写通过。

实机采集包含两个唤醒周期：

- 第一次通过触摸重新枚举，报告 `wake source 1 (touch)`；SSD1677 完整刷新
  4314 ms、`baseline ready`，LVGL 绘制完成。该来源不匹配电源键专用 HIL，
  因此按设计重新进入深睡，没有误记为电源键通过。
- 随后真实短按电源键，重新枚举后报告 `EXT1 wake GPIO mask 0x2 (pmic_irq)`
  和 `wake source 8 (pmic_irq)`，证明 MCU 通过 GPIO1 的 EXT1 唤醒，而不是
  PMIC 关机后冷启动。SSD1677 完整刷新 4314 ms、`baseline ready`，随后
  `Power HIL wake verified; the power action will not repeat`。用户确认画面刷新。

电源键 HIL 镜像 4187044 bytes，SHA-256：
`f7e3b34bc008b7e67e3c1bdb40171e4058e2ac3b9d33f1197e7a8da327a3991b`。
唤醒后约 10 秒开始全刷，约 14.35 秒完成恢复；此时间含启动观察延迟、TF
初始化失败等待和 4.3 秒墨水屏物理全刷，不代表触摸事件队列延迟。

本轮只完成单次触摸和单次电源键唤醒/显示恢复，不宣称 50 次耐久、左右
翻页键实机唤醒或日常交互页面的深睡恢复已经通过。测试后恢复日常镜像。

日常镜像已重新写入，4273396 bytes，SHA-256：
`ba3837ef238b7c0af52f47ed3fa669a4cbe3d1ba81f6ec067e5e75b0dffadf92`。
Flash hash 校验成功；`.config` 确认交互 UI 开启，自检、partial demo 和自动
power HIL 均关闭。没有擦除 storage/NVS 或 TF 内容。

恢复日常镜像时，USB/WDT 复位后观察到早期 `_DoubleExceptionVector`，
`EXCCAUSE=28`、`EXCVADDR=0x2c`；当时内核 current 尚未初始化，不能直接归因
于此前已修复的系统工作队列栈溢出。通过单核 USB-JTAG 的软件复位恢复启动，
随后读取到应用路由为 `PAPERMONO_APP_HOME`、14 个应用、SSD1677
`baseline_ready=true`、全刷 2 次、局刷 13 次；最近全刷 4315 ms、局刷 737 ms，
内核运行在 idle 线程。检查后显式 resume/detach 并退出 OpenOCD。

这证明日常镜像完成了首页与显示刷新，但本轮没有完成独立冷启动、重复 USB
复位或日常交互深睡恢复验收。USB 复位的早期启动异常仍是未闭环问题，不能
将调试器辅助恢复描述为可靠启动已修复。另一次移除 Debug 构建类型的构建
通过，但未刷入，不能用其镜像哈希替代上面已写入镜像的哈希。

## 后续：短按休眠路径（同日）

用户反馈再次短按不能休眠。源码确认 M5PM1 的中断工作项读取、清除了按钮
状态并缓存到 `pending_irq`，但日常 UI 没有消费者，也没有 `INPUT_KEY_POWER`
输入路径；此前的 EXT1 修改只完成了唤醒方向。

新增运行态单击配置和 Zephyr Input 上报。工作项在成功清除 IRQ、释放 I²C
锁后报告按下/释放；UI 回调只入队，UI 线程从任意应用执行原有深睡路径。
刷新中保留单个休眠请求，优先于返回/首页；内容写入事务中取消该次请求。
屏幕菜单仍保留确认页，物理短按直接请求休眠。

中断位依据同一官方头文件：IRQ_STATUS3 bit 0 为单击、bit 1 为唤醒、bit 2
为双击。仅单击转换为休眠，保留双击/长按的 PMIC 行为。新增主机测试覆盖
全部 256 个状态值、唤醒/双击组合及刷新期间电源键优先级。

首次短按实现镜像（4273588 bytes，SHA-256
`ab02104eb70da6b3e1946ac0bbbccf2724fab11d800b5df251435b4fde5021eb`）
已刷入并通过写入 hash 校验，仍需 USB-JTAG 软件复位恢复初次启动。
运行时捕获 `Short power click IRQ3=0x01`、`UI power click queued (0)` 和
`UI power click entering deep sleep from app=1`，随后 USB 断开。一次再次
启动在 UI 循环开始时立即执行了先前排队的休眠请求，说明只过滤 IRQ 唤醒位
不足以隔离启动期间延迟到达的单击。

追加启动门控：UI 事件循环完全启动之前的电源键事件不入队，启动完成后
才接收下一次单击。最终候选 4273668 bytes，SHA-256
`4e62692acbd74d00eb5a12d11223c5fb33779e78e6531f47ee7343f87b87ca75`。
25 组主机测试及默认、partial、selftest、power-button HIL 四配置构建通过；
内部 DRAM 为 324608、305136、307784、312056 bytes，均低于 85%。
截至此记录更新，设备 USB 未枚举，最终候选尚未刷入，完整
“短按休眠→短按唤醒→再次短按休眠”仍待实机闭环，不能以首个短按日志代替。
