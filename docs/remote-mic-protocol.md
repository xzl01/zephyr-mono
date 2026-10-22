# PaperMono BLE 语音遥控协议

本文件定义 PaperMono C153「遥控器 → 语音」功能的设备端与主机端接口。
设备使用与小米蓝牙语音遥控器 2 / 2 Pro 相同的 **ATVV v1.0** 服务传输语音，
因此主机端既可以使用本仓库的接收器，也可以尝试已有的 ATVV 语音工具。

- 状态：源码与主机测试完成前不代表实机语音链路已验证。
- 音频只用于语音输入，不保存到 TF 卡、NVS 或日志。
- 设备只在用户按住语音区域时采音；松开、离开页面、断连、休眠或超过
  60 秒都会停止并关闭麦克风电源。

## 1. GATT 服务

| 角色 | UUID |
| --- | --- |
| Service | `AB5E0001-5A21-4F05-BC7D-AF01F617B664` |
| Transmit（主机 → 设备） | `AB5E0002-5A21-4F05-BC7D-AF01F617B664` |
| Audio（设备 → 主机，Notify） | `AB5E0003-5A21-4F05-BC7D-AF01F617B664` |
| Control（设备 → 主机，Notify） | `AB5E0004-5A21-4F05-BC7D-AF01F617B664` |

- Transmit 接受 Write 与 Write Without Response；设备要求链路已加密。
- Audio 与 Control 只发送 Notify；订阅需要加密链路。
- 设备广播中包含 ATVV Service UUID，名称仍在 scan response 中。
- 设备同时保留标准 HID 服务；一个连接上可以同时使用遥控按键和语音。

## 2. 能力协商

主机向 Transmit 写入：

```
0A 01 00 00 03 03        GET_CAPS
```

设备通过 Control 通知能力：

```
0B 01 00 02 03 00 78 00 00
```

| 字节 | 含义 |
| --- | --- |
| `0B` | CAPABILITIES 事件 |
| `01 00` | ATVV 版本 1.0（大端） |
| `02` | 编解码器：ADPCM 16 kHz（`01` = 8 kHz） |
| `03` | 交互模型：按住说话 |
| `00 78` | 音频帧长 120 字节 |
| `00 00` | 保留 |

主机也可以不先发 GET_CAPS，直接等待设备在用户按住语音区域时发送
AUDIO_START；设备在每次会话开始时都会重新发送能力帧和同步帧。

## 3. 主机命令

| 命令 | 字节 | 设备行为 |
| --- | --- | --- |
| GET_CAPS | `0A 01 00 00 03 03` | 通过 Control 回复能力帧 |
| MIC_OPEN v1.0 | `0C 00` | 开始一次会话（等价于用户按住） |
| MIC_CLOSE v1.0 | `0D <session-id>` | 结束当前会话 |
| MIC_EXTEND v1.0 | `0E <session-id>` | 保持会话；设备忽略，60 秒上限仍生效 |
| MIC_OPEN v0.4 | `0C 00 <codec>` | 兼容旧主机，按 16 kHz 开始 |

会话 ID 由设备在 AUDIO_START 中给出，从 0 开始递增。主机发送 MIC_CLOSE
时，如果当前有活动会话，设备停止并回复 AUDIO_STOP；空闲状态下关闭是幂等的，
不会额外发送 AUDIO_STOP。

## 4. 设备事件

### AUDIO_START

```
04 00 02 <session-id>
```

- `04`：AUDIO_START
- `00`：原因（保留）
- `02`：ADPCM 16 kHz
- `<session-id>`：本次会话 ID

### AUDIO_SYNC

```
0A 02 <seq-hi> <seq-lo> <pred-hi> <pred-lo> <step-index>
```

- `0A`：AUDIO_SYNC
- `02`：ADPCM 16 kHz
- `seq`：**下一帧**的 16 位序号
- `predictor`：下一帧编码前的有符号 16 位预测值（大端）
- `step-index`：下一帧编码前的步长索引（0–88）

设备在每帧序号为 50 的倍数前发送一次同步，并在会话开始时发送
`seq=0, predictor=0, step=0`。主机收到同步后必须丢弃未满一帧的缓存，
并用新的预测值和步长索引重置解码器。

### AUDIO_STOP

```
00 <reason>
```

`00` 表示停止；`reason` 保留（0 表示用户松开或设备主动停止）。

### MIC_OPEN_ERROR

```
0C <error-hi> <error-lo>
```

设备在无法开始会话时回复该事件；当前使用 `0x0001`（链路未加密）、
`0x0002`（主机未订阅 Audio 或 Control）和 `0x0003`（设备或麦克风不可用）。

## 5. 音频帧

- 采样率 16 kHz，单声道，有符号 16 位 PCM 输入。
- IMA/DVI ADPCM，4 bit/样本，**高半字节在前**。
- 每帧 120 字节 = 240 个样本 = 15 ms。
- 帧之间共享同一个预测器和步长索引；不要每帧重置。
- 设备每 15 ms 发送一帧；连接间隔 15 ms 时正好一帧一个连接事件。
- 丢帧时主机解码器会暂时失步，下一次 AUDIO_SYNC 会恢复。

### 带宽与链路

| 项目 | 值 |
| --- | --- |
| 音频码率 | 64 kbps（120 B / 15 ms） |
| ATT MTU | 123（120 字节负载 + 3 字节 ATT 头）；设备在 MTU 不足时按 `MTU-3` 分片 |
| 数据长度 | 127 字节 ACL（123 字节 L2CAP + 4 字节头），1M PHY 即可 |
| 连接间隔 | 15 ms（设备偏好 12 × 1.25 ms） |

设备端配置 `CONFIG_BT_L2CAP_TX_MTU=123`、`CONFIG_BT_BUF_ACL_RX_SIZE=127`
和 `CONFIG_BT_BUF_ACL_TX_SIZE=127`，因此 macOS/Windows 协商后可以直接
发送 120 字节通知。

分片回退只保证字节流完整，不保证小 MTU 下不丢帧：MTU 为 23 时一帧要拆成
6 条通知，15 ms 连接间隔内未必能全部发出；丢帧后解码器会在下一次
`AUDIO_SYNC`（最长约 750 ms）恢复。实机验收应固定使用 MTU 123 的链路。

## 6. 设备端行为

- 用户在「遥控器 → 语音」页按住语音区域：请求开始；松开：请求停止。
- 开始前要求：已连接、链路已加密、Audio 与 Control 均已订阅。
- 麦克风通过 PDM 采集，电源只在会话期间打开；停止后关闭。
- 单次会话上限 60 秒；到达上限后设备主动停止并通知主机。
- 断开连接、离开页面、进入休眠或关机都会停止会话。
- 设备不保存音频，不写日志内容，只记录帧数、丢弃数和时长。

## 7. 主机接收器

本仓库的 `tools/remote-mic` 提供跨平台 Python 接收器：

1. 扫描并连接 `PaperMono C153`（或指定地址）；
2. 订阅 Audio/Control，写入 GET_CAPS；
3. 解码 ADPCM 并写入选定的音频输出设备；
4. 目标应用选择同一个虚拟设备作为麦克风。

虚拟麦克风由回环音频设备提供：

- macOS：BlackHole 2ch（`brew install blackhole-2ch`）；
- Windows：VB-CABLE（安装后在接收器中选择 `CABLE Input`，目标应用选择
  `CABLE Output`）；
- Linux：PipeWire/PulseAudio 的 null sink 或 `snd-aloop`。

接收器不安装或替换系统驱动；这与参考项目捆绑 BlackHole 派生驱动的做法
不同，但不需要签名和内核扩展。

## 8. 与现有 ATVV 工具的兼容性

设备协议与小米遥控器的 ATVV v1.0 一致，但：

- SayAll 等工具按设备名白名单过滤，默认不会接受 `PaperMono C153`；
  在系统蓝牙设置中把设备改名为受支持的小米名称可能可用，但不受支持；
- 这些工具可能依赖小米遥控器的 HID 语音键映射；设备在 ATVV 会话开始时
  直接发送 AUDIO_START，默认 Fn 模式下主机应自行注入语音键；
- 设备不伪造小米的 VID/PID，也不承诺与任何第三方 App 的兼容性。

本仓库的接收器是受支持的路径。

## 9. 测试向量

静音帧（240 个 0 样本）编码为 120 个 `0x00` 字节；解码后仍为 0。
设备发送的典型会话开头：

```
Control: 0B 01 00 02 03 00 78 00 00
Control: 04 00 02 00
Control: 0A 02 00 00 00 00 00
Audio:   120 字节 ADPCM
Audio:   120 字节 ADPCM
...
Control: 00 00
```

## 10. 参考

- ATVV 服务与命令：[mi-ao PROTOCOL.md](https://github.com/fanxeon/mi-ao/blob/main/docs/PROTOCOL.md)
- 参考主机实现：[SayAll / remote-mic-app](https://github.com/HD838A/remote-mic-app)
- IMA ADPCM 步长表：IMA/DVI 公开规范
