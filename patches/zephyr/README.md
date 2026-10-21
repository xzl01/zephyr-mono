# Zephyr 树本地补丁

本目录是 PaperMono 固件依赖的 **Zephyr 4.4.1** 本地修改。仓库本身不包含 Zephyr
源码树（通过 west 管理），因此以补丁形式随应用一起版本化。

## 应用方式

```sh
# WEST_TOPDIR 下的 zephyr 检出，基于 v4.4.1（release 1f6485eca）
cd "$WEST_TOPDIR/zephyr"
git am /path/to/papermono/patches/zephyr/*.patch
```

或手工 `git apply` 后自行提交。补丁顺序按文件名排序。

## 补丁内容

| 文件 | 作用 |
| --- | --- |
| `0001-loader-exception-and-digest.patch` | ① `__start` 在 `hardware_init()` 前把 PS.INTLEVEL 设为 `XCHAL_NUM_INTLEVELS`，避免整芯片复位后早期中断取到零 IRQ 栈而触发双异常；② 段扫描跳过镜像尾部追加的 SHA-256 摘要，避免把摘要当段头解析 |
| `0002-cmake-append-image-digest.patch` | simple boot 的 `--ram-only-header` 不会生成摘要，构建后调用 `scripts/append_esp_digest.py` 原位补齐，消除 ROM `SHA-256 comparison failed` |
| `0003-wifi-apsta-driver.patch` | ESP32 Wi-Fi 驱动：AP+STA 并存时的收发路由、包长上限、PMF capable、关闭省电以提升认证帧响应 |
| `0004-dma-braces.patch` | GDMA `switch` 分支加大括号，消除编译告警 |

## 验证

应用全部补丁后：

```sh
west build -p always -d build -b m5stack_papermono/esp32s3/procpu .
esptool --chip esp32s3 image-info build/zephyr/zephyr.bin   # 应含 Validation hash ... (valid)
```

实机证据见 [冷复位异常定位](../docs/boot-crash-20261021.md) 与
[镜像摘要](../docs/firmware-image-digest.md)。
