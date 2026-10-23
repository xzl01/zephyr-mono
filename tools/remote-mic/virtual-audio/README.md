# PaperMono 虚拟音频设备（macOS）

本目录构建并安装一个名为 **PaperMono** 的 macOS 回环音频设备（AudioServerPlugIn，
无内核扩展），让接收器把 BLE 语音写成一个其他 App 可选的麦克风。

- 设备名：`PaperMono`（隐藏的镜像设备为 `PaperMono Mirror`）
- 通道：2 ch，支持 8/16/24/44.1/48 kHz 等采样率
- 输入/输出：同时提供录音与播放侧
- 许可证：GPL-3.0-only，派生自 [BlackHole v0.7.1](https://github.com/ExistentialAudio/BlackHole)
  （GPL-3.0）。构建脚本在构建时下载上游源码，不在本仓库分发上游源码或二进制。

> 需要 macOS 命令行工具（`clang`、`codesign`）和安装时的管理员权限。
> 这是本地开发/个人使用构建；正式分发需要 Developer ID 签名与公证。

## 构建

```sh
./tools/remote-mic/virtual-audio/build-papermono-driver.sh
```

产物：`tools/remote-mic/virtual-audio/build/PaperMono.driver`。

## 安装 / 卸载

```sh
./tools/remote-mic/virtual-audio/install-papermono-driver.sh
```

脚本会要求管理员密码，把驱动复制到 `/Library/Audio/Plug-Ins/HAL/` 并重启
`coreaudiod`。安装后：

```sh
system_profiler SPAudioDataType | grep -A2 PaperMono
```

音频设备列表里应出现 `PaperMono`（录音与播放两侧）。接收器使用：

```sh
./.venv-mic/bin/python tools/remote-mic/papermono_mic.py --device "PaperMono"
```

目标应用（Zoom、微信输入法、听写等）把麦克风选为 `PaperMono` 即可。

卸载：

```sh
./tools/remote-mic/virtual-audio/uninstall-papermono-driver.sh
```

## 兼容与回退

- 已安装 BlackHole 2ch / SayAll 的 `MiRemoteV 2ch` 时，接收器也可以用
  `--device "BlackHole 2ch"` 或 `--device "MiRemoteV 2ch"`；`PaperMono` 只是
  本项目的独立命名，不依赖这些 App。
- 驱动与 BlackHole / MiRemoteV 使用不同的 bundle ID、UID 和 CFPlugIn 工厂
  UUID，可以并存。
- 当前安装包是 ad-hoc 签名；如果系统拒绝加载，开发时可在“隐私与安全性”中
  允许，或使用自己的 Developer ID 重新签名。正式分发请走签名与公证流程。

## 来源与许可

- 上游：[ExistentialAudio/BlackHole](https://github.com/ExistentialAudio/BlackHole)
  固定 tag `v0.7.1`。
- 本项目仅保留构建参数（名称、bundle ID、通道、采样率）与安装脚本；上游
  源码的版权与 GPL-3.0 条款见其仓库 `LICENSE`。
