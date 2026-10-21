# 2026-10-20 启动排查记录：2026-10-21 更正

## 撤回过强结论

旧版曾断言“硬件没有故障”“所有静默都是 DTR/RTS 把芯片留在下载模式”以及
“`usb_reset + hard_reset` 总能启动应用”。这些判断超出了证据，现撤回。
之前要求充电、反复拔插或手动电源循环，也没有得到电压、电流或电池状态证据支持。

一次成功启动不能证明每次静默根因相同；一次 IDF hello_world 启动也不等于验证了
Wi-Fi 连接、全部外设或整板电源。电子纸本来就能保留旧画面，不能用“屏幕未熄灭”
判断电源状态。

## 2026-10-21 的实测区分

- 原生 USB-Serial/JTAG 的 RTS 复位不是通过外部 EN 引脚断电重启，也不重新采样
  boot straps；普通 `hard-reset` 可能仍留在下载模式。
- 单连接清除 `FORCE_DOWNLOAD_BOOT` 并触发 RTC watchdog，可以执行整芯片复位，
  本轮观察到 USB 重枚举、锁存 strap 从 `0x21` 变成 `0x2b`。
- 但 RTC 复位后，当前镜像曾进入 Zephyr `_DoubleExceptionVector`，不是“没有执行应用”。
  JTAG 读到 `PC=0x403743c0`、`EPC1=0x40385dfd`、`DEPC=0x403740d5`；
  对应早期时钟配置返回和寄存器窗口 underflow handler。异常机制尚未闭环。
- 同一份未改写镜像再由真正的 Espressif OpenOCD 执行 `reset run` 后，
  抓到 TF 挂载、电子纸刷新、LVGL 渲染、BLE 就绪的完整链路。
- 刷写后只做 JTAG `reset run` 的另一次试验明确返回下载模式 `boot:0x23`，
  因此单独 JTAG 也不是通用解法。
- 最终组合为 **RTC 整芯片复位 → 重新枚举 → 提前打开 CDC → JTAG reset run →
  验证本次启动**，正常固件连续三次通过；未请求用户按键或拔插。

详细命令、日志和边界见 [2026-10-21 USB 恢复记录](<docs/usb-reset-20261021.md>)。

## 观察与证据边界

| 观察 | 能说明什么／不能说明什么 |
| --- | --- |
| 串口 0 字节 | 可能漏采、通道未初始化、下载模式、早期异常或应用空闲；不能推断没电 |
| 不见 SoftAP | 也可能处于 STA 配网或 Wi-Fi 失败；不能单独定位启动阶段 |
| 旧电子纸画面 | 画面会保留；既不能证明主控停机，也不能判断电源开关状态 |
| `esptool --before no-reset` 成功 | 只说明本次打开后的下载协议可达；普通 pyserial open 仍可能修改控制线 |
| 镜像写入 hash 通过 | 只证明被写区域与待写数据校验一致，不等于启动成功 |
| OpenOCD attach/halt 失败 | 需先确认是否真正 Espressif 版本、接口配置、核数；不是硬件故障证据 |
| USB 重新枚举 | 是 USB/复位层证据，仍需要本次应用就绪标志 |
| `cu` 报 lock 目录权限不足 | 不等于已有进程占用串口；应单独核对进程持有情况 |

## 脚本修正

恢复与采集共用 [USB 复位模块](<scripts/papermono_usb_reset.py>)，默认 `--method full`。
按 VID/PID 和完整序列号选设备；复位前监听、重连不清输入缓冲；保存原始日志；
检查子进程退出码；普通启动和电源 HIL 分开验收。没有证据时返回未验证，
不再宣称“软件恢复已经穷尽”或要求按键才算有效。

这条路径是 SoC/数字外设复位，不是 PMIC 自身断电。未枚举、USB 被禁用、
安全下载模式限制、物理 strap 持续拉低等仍有硬边界。ROM SHA comparison warning
和 RTC 复位后的早期异常仍单独保留，不能描述为“启动全程零警告”或产品已验收。

来源：[Espressif 复位范围说明](<https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html#reset-modes>)。
