# 固件镜像 SHA-256 摘要（消除 ROM `SHA-256 comparison failed`）

## 问题

Zephyr 4.4.1 simple boot 的构建规则给 esptool 传 `--ram-only-header`
（[soc/espressif/common/CMakeLists.txt](</Volumes/extend/Dev/zephyr/soc/espressif/common/CMakeLists.txt>)），
而 esptool 5.3.1 在该模式下硬编码关闭摘要（`--ram-only-header` 隐含
`--dont-append-digest`）。ESP32-S3 ROM 仍会在“校验和之后的第一个 16 字节边界”
读取 32 字节摘要；那里原本是隐藏填充段的段头，于是每次启动都打印：

```text
SHA-256 comparison failed:
Calculated: ...
Expected: 00000000e0610000...
Attempting to boot anyway...
```

非安全启动下 ROM 继续启动，但日志出现假故障，且开启 Secure Boot / Flash
Encryption 时会变成致命错误。

## 修复

两层改动：

1. **构建后原位写入摘要**：[scripts/append_esp_digest.py](<scripts/append_esp_digest.py>)。
   - 解析可见 RAM 段，算出校验和位置（16 字节块最后一字节），摘要紧随其后。
   - 置 `hash_appended`（头部偏移 `0x17`）为 1，再对 `[0, digest_offset)` 计算
     SHA-256 并写入。
   - 摘要占用原本的 32 字节零填充；把紧随其后的隐藏填充段头后移 32 字节并把
     长度减 32，使所有真实段的绝对偏移（IROM `0x20000`、DROM `0x100000`）不变，
     **文件大小不变**。
2. **加载器兼容**：`soc/espressif/common/loader.c` 在跳过校验和块后，如果
   `hash_appended` 为真，再跳过 32 字节摘要，避免把摘要当成段头解析。

构建已自动挂接（`extra_post_build_commands`），无需手工步骤。

## 实机验证

```text
$ esptool --chip esp32s3 image-info build/zephyr/zephyr.bin
Checksum: 0x00 (valid)
Validation hash: c85689b0... (valid)
```

启动日志中 `SHA-256 comparison failed` 计数为 **0**，并且照常完成
TF 挂载（6.255 s）、LVGL 渲染（10.928 s）、BLE 同步就绪（12.364 s）。

主机测试 [tests/host/test_esp_digest.py](<tests/host/test_esp_digest.py>) 覆盖
摘要写入、幂等、损坏摘要拒绝、异常段头拒绝和命令行落盘。

## 边界

- 只对 simple boot（无分区表、镜像位于 `0x0`）生效；MCUboot 路径本来就会追加摘要。
- 刷写时必须保持头部一致（`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 关闭），
  否则摘要会失效。
- 该修复消除 ROM 摘要告警，**不**解决整芯片复位后的早期双异常（另见
  [冷复位异常定位](<docs/boot-crash-20261021.md>)）。
