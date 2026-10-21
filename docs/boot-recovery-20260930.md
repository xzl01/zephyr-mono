# 2026-09-30 整机卡死修复记录

## 根因与修复

旧镜像停在 `_DoubleExceptionVector`，`EXCCAUSE=28`、`EXCVADDR=0x2c`，
线程保存的栈上下文已损坏。单核 USB-JTAG 数据观察点捕获到写入
`sys_work_q_stack[0]` 时栈指针已经到达 1024-byte 工作队列栈的下界。
调用链为：

```text
status_led_worker -> status_led_apply_locked(CONNECTING)
 -> papermono_rgb_set_brightness -> m5pm1_set_status_led
 -> I2C transmit -> k_sem_take -> scheduler -> z_add_timeout
```

这不是单纯的触控命中问题：状态灯在系统工作队列中执行阻塞 I²C 操作，栈溢出
破坏上下文，使整个应用停止，墨水屏则保留旧内容。

`prj.conf` 将 `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` 从默认 1024 增至 4096，
开启 `INIT_STACKS` 和 `STACK_SENTINEL`。状态灯模块添加 `BUILD_ASSERT`，禁止
profile 将工作队列缩回不足 4096 bytes。没有清除 NVS 或用户内容。

修复版运行后读到栈哨兵 `0xf0f0f0f0` 完整，哨兵后的 4092 bytes 中仍有
2924 bytes 保持填充值，相当于已使用约 1168 bytes，超过旧栈容量。
这是本轮观察水位，不代表所有网络、BLE 和电源组合下的最坏上界。

## 验证结果

- `sh tests/host/run.sh`：22 组通过。
- 默认、`partial_demo.conf`、`driver_selftest.conf`、
  `driver_selftest.conf;power_touch_hil.conf` 四种构建通过。
- 四种配置的内部 DRAM 分别为 324696、305312、307976、312232 bytes，
  占用 81.36%、76.50%、77.17%、78.23%，全部低于 85% 门槛。
- power 组合确认实际启用了 `PAPERMONO_DRIVER_SELFTEST`、
  `PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH` 和 `POWEROFF`；本轮没有刷入或执行
  电源 HIL，构建结果不能替代休眠实测。
- 默认镜像 2330852 bytes，SHA-256：
  `3924b938e0d33976d99609a283355b8621adf3a7b182baacbefb7a612063679a`。
  USB 写入后 esptool 报告 `Hash of data verified`。
- SSD1677 全刷 4314 ms、局刷 737–738 ms，LVGL 渲染完成；FT6336 的真实轻触
  进入应用，左边缘从 x=5 滑至 x=277 产生 BACK，随后路由回到启动器。
- 后续读到全刷计数 2、局刷计数 10；此前局刷回退计数为 0。不是仅构建验收。
- 关闭 OpenOCD 后，通过原生 USB 应用复位再次启动，日志确认全刷 4313 ms、
  局刷 737 ms、SoftAP 接口校验、BLE HID/同步初始化及真实触摸处理。

## 调试注意事项

使用 Espressif OpenOCD 的 `esp_usb_jtag` 和单核配置。上游 Homebrew OpenOCD
不能代替已验证的 Espressif 版本。当前 simple-boot 镜像不应使用默认 FreeRTOS
探测或 Flash 自动探测：后者会尝试运行 RAM stub，干扰本轮调试。

```sh
openocd -f interface/esp_usb_jtag.cfg \
  -c 'set ESP32_S3_ONLYCPU 1; set ESP_FLASH_SIZE 0; set ESP_RTOS none' \
  -f target/esp32s3.cfg -c 'adapter speed 2000'
```

此配置只用于调试，关闭了 Flash bank，不用于刷机。GDB 检查结束应显式
`monitor resume`，再 `detach`；验收独立运行/休眠前退出 OpenOCD。
串口被动采集使用现有 `open_cdc()`，不要在打开或关闭日志端口时意外切换
DTR/RTS；主动 USB 复位使用 `usb_jtag_application_reset()`。

## 未闭环事项

- USB 复位后检测为插卡，但 SD 初始化报 `Card never left busy state`、-134。
  应用以内容不可用状态继续运行，书库、日历等真实内容读写尚未恢复。
- 本次 simple-boot ROM 仍输出 SHA-256 comparison warning 后继续启动；这是
  未附加 digest 的当前镜像路径，不是本次 Flash 写入 hash 校验失败。未修复此警告。
- 没有将日志验证宣称为屏幕像素/观感验收；断电冷启动、50 次触摸唤醒、
  长时间翻页残影、macOS 实际 BLE 配对和 Wi-Fi 上传仍须分别验证。
