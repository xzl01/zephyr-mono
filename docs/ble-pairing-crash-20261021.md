# 遥控器蓝牙配对故障

2026-10-21，默认固件 `81e723e` 在遥控器页面连接电脑、显示配对码后挂起，
串口捕获到：

```text
BLE assert ke_event.c 200, param 00000000 00000000
```

故障包含两个配置衔接问题。仅开启控制器安全支持可以消除这条断言，
但 Mac 实测仍以 `0x3d`（MIC 校验失败）断开，需要同时恢复 HAL 的加密 ROM 修复。
原始串口、Mac 日志及内存快照保存在本地忽略的 `artifacts/remote-crash-*`。

## 定位与修改

1. 主机 `BT_SMP` 和 `PAPERMONO_BLE_HID` 已启用，厂商控制器的
   `ESP32_BT_CTLR_LE_SECURITY_ENABLE` 却未启用，启动特性为 `SMP:0`。
   该开关默认依赖 `BT_CTLR_LE_ENC`，当前 ESP HCI 配置没有随主机 SMP 自动打开它。
   HAL 的 `esp_bt.h` 将它映射为控制器初始化参数 `BLE_SECURITY_ENABLE`。
   断言现场 `ke_event_env` 待处理位图为 `0x0c`，其中事件 2 回调为零；
   控制器调度函数对缺少回调的待处理事件触发断言。
   HID 状态仍为已连接、未加密、配对码可见，两个 BT 栈的哨兵均完整。

   `PAPERMONO_BLE_HID` 现在在 ESP32-S3/C3 HCI 平台显式选择控制器安全支持；
   HID 源文件增加编译期断言，防止再生成缺少此能力的固件。

2. HAL `19f979c` 的头文件使用新的 `ESP32_BT_CTLR_*` 开关，
   C3/S3 控制器适配层安装加密 ROM 修复时却仍判断旧宏
   `CONFIG_BT_CTRL_BLE_SECURITY_ENABLE`，导致 `ble_enc_funcs_reset()` 被编译掉。
   见 [固定版本 HAL 源码](https://github.com/zephyrproject-rtos/hal_espressif/blob/19f979cfe66bcab09abe3b0b3aa419a664c1606c/components/bt/controller/esp32c3/bt.c#L1418)。

   `cmake/esp32_ble_compat.cmake` 将新开关的实际布尔值传给旧宏，
   仅作用于该控制器源文件；不修改工作区 HAL、不向其他源文件传播别名。
   实际 HAL 预处理结果和最终 ELF 均确认包含加密 ROM 修复初始化函数。

连接、配对码显示、安全级别、配对成功/失败和断开阶段现在都有日志，
不输出配对码或对端地址。启动证据检查器也将 `BLE assert` 纳入失败标记。

## 验证

- 主机回归通过。CMake 编译回归覆盖 S3/C3 启用、安全开关关闭、其他 SoC 及
  宏不泄漏到其他源文件；模拟禁用控制器安全支持时，HID 编译期断言按预期失败。
- 原生 LVGL 的全部 37 个页面预览通过，包括未连接、正在配对、配对码和
  已加密的媒体/演示/鼠标六种遥控状态。使用真实生产页面和固定 64 KiB 堆，
  状态数据为模拟值，不替代蓝牙实机验收。
- 默认、局刷演示、驱动自检三种固件构建及内部 DRAM 预算检查通过。
  默认固件内部 DRAM 为 337816 / 399108 字节（84.64%，项目限制 85%）。
- 最终候选镜像 SHA-256：
  `6d4d5ab338e345e23e9579fe5f92b88c6614a6e41d2ffb8a54bc2b8965bb0470`。
  USB-JTAG 写入、设备端校验及独立 ROM 完整回读通过。
  启动前 NVS 回读与本次备份逐字节一致，未格式化 TF。
- 新启动日志包含 UI、TF、BLE 就绪，控制器特性为 `SMP:1`。
  控制器配置修复后事件 2 已注册为 `r_rwip_crypt_evt_handler`，原缺失回调已恢复。
- Mac 实际输入配对码后的连接、遥控按键及断开重连结果待确认。
