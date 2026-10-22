# 遥控器语音接收器（PC 端虚拟麦克风）

本文件说明如何在 macOS、Windows 和 Linux 上运行
[`tools/remote-mic`](../tools/remote-mic) 的 Python 接收器，把 PaperMono C153
「遥控器 → 语音」的音频流变成其他应用可选的虚拟麦克风。

协议本身以 [遥控器语音协议](remote-mic-protocol.md) 为准；本文件只描述主机端
安装、设备选择、使用与故障排查。

## 1. 状态与边界

- 主机端代码、单元测试、离线自检与命令行入口已完成并通过本机验证。
- **实机语音链路尚未验证**：本文不声称已通过硬件验收，也没有实机录音样本。
  第 10 节给出需要人工执行的验收清单。
- 接收器**不安装、不替换、不捆绑任何内核驱动、HAL 插件或系统扩展**。虚拟
  麦克风由第三方回环音频设备提供（BlackHole 2ch、VB-CABLE、PulseAudio null
  sink 等），接收器只是把解码后的 PCM 写入该设备。
- 设备端音频不落盘；主机端默认也不落盘，只有显式传入 `--wav` 才会写文件。
- 设备只在用户按住「遥控器 → 语音」页的语音区域时采音，松开、离开页面、
  断连、休眠或超过 60 秒都会停止并关闭麦克风电源。

## 2. 工作原理

```
遥控器 (PDM 麦克风)
  └─ IMA/DVI ADPCM 16 kHz 单声道，120 B/帧（15 ms）
      └─ BLE ATVV v1.0 Notify（ATT MTU 不足时按 MTU-3 分片）
          └─ papermono_mic（bleak）
              ├─ 分片重组 → 120 B 帧
              ├─ ADPCM 解码（跨帧共享预测器/步长，AUDIO_SYNC 时重置）
              ├─ 可选 DC 阻断 + 增益钳位
              ├─ 环形缓冲（默认 400 ms，欠载补静音）
              └─ sounddevice → BlackHole 2ch / CABLE Input / null sink
                    └─ 目标应用选择该虚拟设备作为「麦克风」
```

关键行为：

| 行为 | 说明 |
| --- | --- |
| 通知分片 | 设备在 MTU 不足时把一帧拆成多个 Notify；接收器按字节流重组，不假设一次通知等于一帧 |
| 同步与丢帧 | 收到 `AUDIO_SYNC` 时丢弃未满一帧的缓存，并用事件中的预测器/步长索引重置解码器 |
| 欠载 | 环形缓冲没有数据时输出静音并计数，不会阻塞 BLE 线程或输出陈旧样本 |
| 增益 | `--gain-db` 钳位在 −24…+24 dB，逐样本裁剪到 int16，避免误输入导致溢出 |
| DC 阻断 | 默认开启 40 Hz 一阶高通，消除 ADPCM 丢帧后的直流偏置；`--no-dc-blocker` 关闭 |
| 断连 | 断开后按 1 s → 20 s 指数退避重连；`--once` 时只连接一次 |

## 3. 前置条件与配对

设备同时提供标准 BLE HID 与 ATVV 语音服务，ATVV 的 Transmit/Audio/Control
特征要求**加密链路**。

1. 设备通常已经作为 BLE HID 遥控器与电脑完成配对（bond），此时接收器直接
   复用已有加密链路，不需要额外操作。
2. 如果设备从未与该电脑配对，请先在**系统蓝牙设置**里配对，再运行接收器：
   - macOS：系统设置 → 蓝牙 → 选择 `PaperMono C153`；
   - Windows：设置 → 蓝牙和其他设备 → 添加设备 → 蓝牙 → `PaperMono C153`；
   - Linux：`bluetoothctl` → `agent on` → `scan on` → `pair <地址>` →
     `trust <地址>`。
3. 如果接收器报告「the ATVV characteristics require an encrypted (bonded)
   link」，说明当前链路未加密。**bleak 无法在所有平台上触发配对**：只有
   BlueZ 后端提供 `pair()`，macOS 会在访问受保护特征时由系统弹出配对提示，
   Windows 则由系统蓝牙设置负责。接收器只在 Linux 上尝试调用 `pair()`，
   其余平台请按上一条在系统蓝牙设置中完成配对。
4. 设备休眠、关机或离开「遥控器 → 语音」页后不会建立会话；请先唤醒设备。
5. 如果设备在加入语音服务**之前**已经与这台电脑配对过，系统可能缓存了旧的
   GATT 服务列表。升级固件后请删除系统蓝牙中的旧配对并重新配对，或关闭再打开
   电脑蓝牙，让系统重新发现 ATVV 服务；否则接收器可能只能看到 HID 服务。

> 配对过程中设备屏幕会显示配对码时，请在电脑端确认同一个码。清除设备配对
> 会同时移除 HID 和语音服务的访问权限，需要重新配对。

## 4. 安装

需要 Python 3.10 或更新版本。

```sh
cd <仓库根目录>
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/remote-mic/requirements.txt
```

依赖有三个：`bleak`（BLE）、`numpy`（sounddevice 的非 Raw 流内部需要）和
`sounddevice`（PortAudio）。`bleak`/`sounddevice` 只在真正需要硬件时才导入，
因此 `--help`、`--selftest` 和单元测试无需安装它们；打开音频设备前必须已安装
`numpy`。

Linux 还需要系统 PortAudio 库，例如 Debian/Ubuntu：

```sh
sudo apt install libportaudio2
```

macOS 上 `sounddevice` 会自带 PortAudio wheel；若加载失败可
`brew install portaudio`。

## 5. 虚拟麦克风设备

接收器写入的是**播放（输出）**设备；目标应用读取的是**录音（输入）**设备。
回环设备把两者连起来，因此两边的名字经常不对称（尤其是 Windows）。

### 5.1 macOS：BlackHole 2ch

```sh
brew install blackhole-2ch
```

1. 安装后确认「音频 MIDI 设置」中出现 `BlackHole 2ch`；
2. 接收器：`--device "BlackHole 2ch"`；
3. 目标应用（Zoom、Teams、QuickTime、OBS、浏览器等）的麦克风选择
   `BlackHole 2ch`；
4. 如果同时想听到声音，在「音频 MIDI 设置」中创建**多输出设备**，勾选
   扬声器和 `BlackHole 2ch`，并把系统输出指向该多输出设备。本接收器本身
   只写入一个输出设备。

> 首次运行时 macOS 会要求给终端（或 Python）授予「蓝牙」权限；拒绝后扫描
> 结果为空。授权路径：系统设置 → 隐私与安全性 → 蓝牙。

### 5.2 Windows：VB-CABLE

安装 [VB-CABLE](https://vb-audio.com/Cable/) 后会出现一对设备：

| 方向 | 设备名 | 谁使用 |
| --- | --- | --- |
| 播放 | `CABLE Input (VB-Audio Virtual Cable)` | 接收器（`--device "CABLE Input"`） |
| 录音 | `CABLE Output (VB-Audio Virtual Cable)` | 目标应用的麦克风 |

Windows 上需要 Windows 10 1809 或更新版本（WinRT BLE）。VB-CABLE 安装后
建议重启一次，确保设备出现在 PortAudio 列表中。

### 5.3 Linux：PipeWire / PulseAudio null sink

```sh
# PulseAudio
pactl load-module module-null-sink sink_name=papermono_mic \
  sink_properties=device.description=PaperMonoMic

# PipeWire（自带 pulse 兼容层，命令相同）
pactl load-module module-null-sink sink_name=papermono_mic
```

- 接收器：`--device papermono_mic`（或 `--device "PaperMonoMic"`）；
- 目标应用的麦克风选择 `Monitor of PaperMonoMic`（即
  `papermono_mic.monitor`）；
- 若使用 ALSA 回环：

```sh
sudo modprobe snd-aloop
python3 tools/remote-mic/papermono_mic.py --device "Loopback"
```

`snd-aloop` 需要内核模块，属于内核自带功能，不是本仓库提供的驱动。若设备
在重启后消失，需要重新加载模块或写入 `/etc/modules-load.d/`。

### 5.4 选择设备

```sh
python3 tools/remote-mic/papermono_mic.py --list-devices
```

输出示例（`*` 标记系统默认输出）：

```
*  0  MacBook Pro Speakers  [out]  out=2ch  api=0
   1  BlackHole 2ch  [out]  out=2ch  api=0
   2  Built-in Microphone  [in]  out=0ch  api=0
```

`--device` 接受索引或名称子串（大小写不敏感）；不传时使用系统默认输出。
名称匹配到多个设备时会取第一个并打印警告，此时建议改用索引。

## 6. 使用

### 6.1 基本用法

```sh
# 扫描名称 "PaperMono C153" 并写入 BlackHole 2ch
python3 tools/remote-mic/papermono_mic.py --device "BlackHole 2ch"

# 指定地址（跳过扫描）
python3 tools/remote-mic/papermono_mic.py --address AA:BB:CC:DD:EE:FF --device 3

# 录制 30 秒用于检查解码质量
python3 tools/remote-mic/papermono_mic.py --device "BlackHole 2ch" \
  --wav /tmp/papermono-voice.wav

# 只连接一次，会话结束或断链后退出（便于脚本化验证）
python3 tools/remote-mic/papermono_mic.py --once --verbose
```

接收器启动后：

1. 扫描并连接设备，订阅 Audio/Control，写入 `GET_CAPS`；
2. 打印能力帧（`capabilities v1.0 ADPCM 16 kHz frame=120B (15.0 ms)`）；
3. 等待设备会话：在设备上进入「遥控器 → 语音」页并**按住**语音区域；
4. 收到 `AUDIO_START` 后开始解码并写入音频设备；
5. 松开后收到 `AUDIO_STOP`，会话统计写入日志；
6. 断链时自动重连，直到 `Ctrl-C`（或 `--once` 结束）。

### 6.2 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--address ADDR` | 扫描 | 直接连接该 BLE 地址/UUID，跳过扫描 |
| `--name NAME` | `PaperMono C153` | 扫描时匹配的设备名（也接受子串） |
| `--device DEV` | 系统默认输出 | 输出设备索引或名称子串 |
| `--list-devices` | — | 列出音频设备后退出 |
| `--gain-db DB` | `0` | 增益，钳位到 −24…+24 dB |
| `--wav PATH` | 关闭 | 把解码后的 PCM（增益/DC 阻断之前）写成 16 kHz 单声道 WAV |
| `--verbose`, `-v` | 关闭 | 打印每条控制事件、同步事件与逐帧细节 |
| `--once` | 关闭 | 只连接一次；首个语音会话结束或断链后退出 |
| `--send-mic-open` | 关闭 | 测试模式：主动写 `MIC_OPEN`，让设备在未按住时开始会话 |
| `--mic-open-seconds SEC` | `10` | 测试模式会话时长，到时写 `MIC_CLOSE` |
| `--scan-timeout SEC` | `8` | 扫描时长 |
| `--ring-ms MS` | `400` | 环形缓冲长度；网络抖动大时可增大 |
| `--no-dc-blocker` | 关闭 | 关闭 40 Hz 直流阻断 |
| `--selftest` | — | 无硬件离线自检（见 6.4） |

环形缓冲决定额外的端到端延迟：稳态延迟接近缓冲占用量，默认上限 400 ms。
对延迟敏感（如实时通话）时可降到 `--ring-ms 150`，代价是更容易出现欠载；
网络或系统负载高时反而应增大到 600–800 ms。

### 6.3 设备侧操作

- 按住语音区域开始，松开结束；单次会话最长 60 秒，设备到时会主动停止并
  发送 `AUDIO_STOP`，接收器回到等待状态。
- 会话开始时设备会重新发送能力帧与 `AUDIO_SYNC`（`seq=0, predictor=0,
  step=0`），因此丢包或重连后无需重启接收器。
- 接收器也会在断连后自动重连；重连成功后需要重新在设备上按住语音区域。

### 6.4 自检与录音验证

不需要蓝牙或声卡：

```sh
python3 tools/remote-mic/papermono_mic.py --selftest
```

它会检查能力帧、静音帧、ADPCM 往返信噪比（分片输入）、环形缓冲欠载/溢出、
增益钳位、DC 阻断、WAV 回读，并确认没有导入 `bleak`/`sounddevice`。任何一项
失败都会以非零状态退出。

有设备时，建议先用 `--send-mic-open` 验证链路与订阅，再验证按住说话：

```sh
python3 tools/remote-mic/papermono_mic.py --device "BlackHole 2ch" \
  --send-mic-open --mic-open-seconds 5 --once --verbose
```

若设备回复 `MIC_OPEN_ERROR 0x0001`，说明链路未加密；`0x0002` 表示主机尚未
订阅 Audio 与 Control（通常是订阅失败，见第 7 节）；`0x0003` 表示设备语音服务
未初始化或麦克风设备不可用（`papermono_ble_mic_init()` 失败）。运行期 PDM
启动失败不会发这个事件，只在设备 UI 上显示失败。

## 7. 故障排查

| 现象 | 可能原因与处理 |
| --- | --- |
| `no Bluetooth LE devices found` | 蓝牙未开启；macOS 未授权终端访问蓝牙；设备已休眠/关机；扫描时间过短（`--scan-timeout 15`） |
| `no device named ... and no ATVV advertiser found` | 设备名称被系统改写；用 `--address` 指定地址，或 `--name` 传入系统里显示的名字 |
| `the ATVV characteristics require an encrypted (bonded) link` | 未配对/未信任：按第 3 节在系统蓝牙设置中配对；Linux 上确认 `trust` 已设置 |
| `MIC_OPEN_ERROR 0x0001` | 同上，链路未加密 |
| `MIC_OPEN_ERROR 0x0002` | Audio/Control 订阅失败：确认设备未被其他主机占用，重启接收器 |
| `no output device name contains ...` | 虚拟设备未安装或名称不同；先 `--list-devices` |
| 目标应用没有电平 | 方向选错：Windows 接收器写 `CABLE Input`、应用读 `CABLE Output`；Linux 应用要选 `Monitor of ...`；应用切换设备后需要重新打开录音 |
| 声音断续/爆音 | 2.4 GHz 干扰（Wi-Fi 与 BLE 共存）；增大 `--ring-ms 800`；关闭蓝牙省电；靠近设备 |
| Wi-Fi 开启时大量丢帧 | 固件在语音会话期间会自动暂停 Wi-Fi（断开 STA、关闭 SoftAP），结束后恢复；实机送达率从约 60% 提升到约 88%。若仍断续，检查是否已烧入含暂停逻辑的镜像；数据见 [HIL 记录](remote-mic-hil-20261022.md) |
| 只有 60 秒 | 设备端单次会话上限，属预期行为 |
| 声音很小 | `--gain-db 6`（最多 +24）；BlackHole 没有系统音量控制，音量由源决定 |
| 声音像快放/慢放 | 设备以 8 kHz 编解码时接收器不做重采样（日志会警告）；本固件固定 16 kHz，出现该警告请反馈 |
| `sounddevice` 加载失败 | 缺少 PortAudio：Linux 安装 `libportaudio2`，macOS `brew install portaudio` |
| 一直重连 | 设备休眠或超出范围；确认设备未与其他主机保持连接 |

`--verbose` 会打印每条控制事件、同步序号与逐帧统计，便于区分「没有通知」与
「通知解析失败」。

## 8. 隐私

- 默认不保存任何音频。只有显式传入 `--wav PATH` 才会把解码后的 PCM 写入
  文件，该文件是**明文语音**，请自行保管并及时删除。
- 日志只记录帧数、丢帧数、缓冲欠载、峰值幅度、会话时长等计数，不记录音频
  内容，也不记录设备地址之外的个人信息。
- 接收器不联网、不上传、不写入 NVS/TF 卡；设备端同样只统计帧数与时长。
- 已配对的电脑可以通过 ATVV `MIC_OPEN` 在设备上**没有手势**的情况下开启一次
  会话（最长 60 秒，设备屏幕会显示「正在传音」）。这是 ATVV 兼容性的一部分；
  如果不需要，请勿让不可信主机保持配对，并在不用时关闭电脑蓝牙。
- `--wav` 记录的是增益与 DC 阻断之前的原始解码结果，便于判断是解码问题还是
  后处理问题。

## 9. 与现有 ATVV 工具的关系（实验、不受支持）

设备协议与小米蓝牙语音遥控器 2 / 2 Pro 使用的 ATVV v1.0 一致，因此已有工具
在理论上可以识别它，但：

- [SayAll / remote-mic-app](https://github.com/HD838A/remote-mic-app) 等工具按
  设备名白名单过滤，默认不会接受 `PaperMono C153`；
- 在系统蓝牙设置中把设备**改名**为受支持的小米名称后，这些工具**可能**可用，
  但这属于**实验性、不受支持**的用法：名称改写会影响 HID 配对记录与后续
  识别，且第三方工具可能依赖小米遥控器的 HID 语音键映射；
- 本设备在 ATVV 会话开始时直接发送 `AUDIO_START`，不伪造小米的 VID/PID，
  也不承诺与任何第三方 App 兼容。

本仓库的接收器是受支持的路径；第 3 节的配对要求和第 10 节的验收边界同样适用于
第三方工具，不能因为「能连上」就认为链路已验证。

## 10. 测试与验证边界

已在本机（macOS，Python 3.14）执行：

```sh
# 单元测试（纯 Python，无需蓝牙/声卡）
python3 -m unittest discover -s tools/remote-mic/tests -v
# -> Ran 101 tests ... OK

# 仓库主机测试入口（单元测试 + 导入契约 + CLI 检查）
python3 tests/host/test_remote_mic.py
# -> all remote-mic host checks passed

# 语法检查
python3 -m py_compile tools/remote-mic/papermono_mic.py \
  tools/remote-mic/remote_mic/*.py tools/remote-mic/tests/*.py

# 命令行入口
python3 tools/remote-mic/papermono_mic.py --help
python3 tools/remote-mic/papermono_mic.py --selftest
# -> selftest passed
```

2026-10-22 已在 C153 上完成第一轮真机验收：启动、按住说话、虚拟设备链路和
60 秒上限通过；修复 PDM 实际 8 kHz / I2S 阻塞溢出问题。16 kHz 与 Wi-Fi
共存吞吐不足已通过「语音期间暂停 Wi-Fi」缓解（约 88% 送达）。完整记录见
[实机验收记录](remote-mic-hil-20261022.md)。

以下主机侧结果只证明解析、解码、缓冲与命令行行为；下列项目仍需人工在真机上执行：

1. `--list-devices` 能列出目标虚拟设备；
2. 运行接收器，日志出现能力帧 `v1.0 / ADPCM 16 kHz / 120B`；
3. 按住语音区域，日志出现 `AUDIO_START session=0` 与
   `AUDIO_SYNC seq=0 predictor=0 step=0`；
4. `frames` 持续增长、`gaps` 保持 0、`underruns` 增长缓慢；
5. `--wav` 录制的文件为 16 kHz 单声道，语音清晰、无持续直流偏置；
6. 目标应用（如 QuickTime/Teams）选择虚拟设备为麦克风后能看到电平；
7. 松开语音区域后会话结束，60 秒上限生效；
8. 断开设备后接收器自动重连，再次按住可继续使用。

在完成上述检查前，请把本功能视为「源码与主机测试完成、实机未验证」。

## 11. 参考

- ATVV 服务与命令：[mi-ao PROTOCOL.md](https://github.com/fanxeon/mi-ao/blob/main/docs/PROTOCOL.md)
- 参考主机实现：[SayAll / remote-mic-app](https://github.com/HD838A/remote-mic-app)
- 本仓库协议规范：[遥控器语音协议](remote-mic-protocol.md)
- IMA/DVI ADPCM 步长表：IMA/DVI 公开规范（见 `remote_mic/protocol.py` 中的表与注释）
- [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole)
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)
- [bleak](https://github.com/hbldh/bleak)、[python-sounddevice](https://python-sounddevice.readthedocs.io/)

本仓库接收器为原创实现（GPL-3.0-only，与仓库其余代码一致），没有逐行复制上述
参考项目或任何 GPL 代码。
