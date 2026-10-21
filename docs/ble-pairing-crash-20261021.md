# 遥控器蓝牙配对故障

2026-10-21，默认固件 `81e723e` 在遥控器页面连接电脑、显示配对码后挂起，
串口捕获到：

```text
BLE assert ke_event.c 200, param 00000000 00000000
```

故障包含控制器配置、加密 ROM 修复、配对完成时的接收线程栈和断开后广播恢复四个问题。
开启控制器安全支持消除了原断言；恢复 HAL 加密 ROM 修复后，Mac 已成功达到
安全等级 4 并保存配对，但随后暴露了 BT RX 栈溢出。
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

3. 加密修复固件 `57bff39` 在 Mac 实测中先后记录：

   ```text
   BLE HID security changed: level=4 error=0
   BLE HID pairing complete; bonded=1
   ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0
   ```

   出错线程地址解析为 `bt_workq`，PC 对应 `z_check_stack_sentinel`；
   当前配置的 `rx_thread_stack` 只有 1200 字节。这个故障发生在成功配对后，
   不能用更早的控制器断言现场排除。哨兵检查函数发现越界后会恢复哨兵，
   因此致命错误后的 JTAG 快照中哨兵值重新正常，不代表没有发生溢出。

   `BT_RX_STACK_SIZE` 增至 2048 字节，并增加编译期下限检查。新保存的 Mac
   配对信息与原设置均保留，未清除 bond 或 NVS。

4. Mac 忽略设备后扫描不到 PaperMono。设备仍能处理页面操作和 HTTP 请求，
   但 HID 状态为未连接、未广播、`last_error=-ENOMEM`。原实现在
   `disconnected()` 中立即启动可连接广播，此时 Zephyr 仍持有连接对象引用；
   `BT_MAX_CONN=1` 时分配连接槽失败，之后没有再启动广播。

   现在等 `bt_conn_cb.recycled` 通知连接对象释放，再通过工作队列启动广播。
   连接失败同样等待回收；清除配对时若断开尚未完成，工作队列跳过，随后由回收
   回调恢复广播。已连接、已广播或尚未初始化时不会重复启动。广播结果增加日志，
   成功恢复后清除此前的错误状态。

连接、配对码显示、安全级别、配对成功/失败和断开阶段现在都有日志，
不输出配对码或对端地址。启动证据检查器也将 `BLE assert` 纳入失败标记。

## 验证

- 主机回归通过。CMake 编译回归覆盖 S3/C3 启用、安全开关关闭、其他 SoC 及
  宏不泄漏到其他源文件；模拟禁用控制器安全支持或恢复 1200 字节 RX 栈时，HID 编译期断言按预期失败。
- HID 生命周期回归编译真实生产源文件，使用单连接槽模拟协议栈持有的引用，
  覆盖断开后回收、连接失败、异步清除配对、过期工作和资源暂不可用后回收恢复。
- 原生 LVGL 的全部 37 个页面预览通过，包括未连接、正在配对、配对码和
  已加密的媒体/演示/鼠标六种遥控状态。使用真实生产页面和固定 64 KiB 堆，
  状态数据为模拟值，不替代蓝牙实机验收。
- 默认、局刷演示、驱动自检三种固件构建及内部 DRAM 预算检查通过。
  默认固件内部 DRAM 为 338680 / 399108 字节（84.86%，项目限制 85%）。
- 蓝牙故障回归镜像 SHA-256（触摸板连续位移修改前）：
  `9d7314c4ee0ceb30da327a1d8a7899a0a017550b3f3067ac650481d400496315`。
  USB-JTAG 写入、设备端校验及独立 ROM 完整回读通过。
  启动前 NVS 回读与本次备份逐字节一致，未格式化 TF。
- 新启动日志包含 UI、TF、BLE 就绪，控制器特性为 `SMP:1`。
  控制器配置修复后事件 2 已注册为 `r_rwip_crypt_evt_handler`，原缺失回调已恢复。
- 2048 字节 RX 栈固件从 NVS 恢复 `bonded=1`；Mac 通过已保存配对重连，
  记录安全等级 4、服务发现成功、BAS 通知订阅和连接后的页面刷新。
  JTAG 栈填充值测得 BT RX 最小未使用空间 268 字节，未出现新的致命错误。
- Mac 忽略设备后的首次尝试失败，日志记录断开；约 24 毫秒后重新广播，
  随后再次显示配对码，达到安全等级 4 并记录 `pairing complete; bonded=1`。
  用户确认随后连接成功，按键操作日志持续完成。新配对路径 BT RX 最小未使用
  空间为 476 字节，栈哨兵完整，没有再次断言或栈溢出。
