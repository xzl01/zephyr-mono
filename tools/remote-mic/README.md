# remote-mic：PaperMono C153 BLE 语音接收器

把 PaperMono C153「遥控器 → 语音」的 ATVV v1.0 音频流（16 kHz 单声道
IMA/DVI ADPCM）解码后写入任意音频输出设备。选择 BlackHole 2ch（macOS）、
VB-CABLE（Windows）或 PulseAudio null sink（Linux）作为输出设备，即可让其他
应用把该虚拟回环设备当作麦克风使用。

- 协议规范：[docs/remote-mic-protocol.md](../../docs/remote-mic-protocol.md)
- 安装、虚拟设备与故障排查：[docs/remote-mic.md](../../docs/remote-mic.md)
- 状态：源码与主机测试完成；**实机语音链路尚未验证**，不声称已通过硬件验收。
  本工具不安装或替换任何系统驱动。

## 快速开始

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/remote-mic/requirements.txt

python3 tools/remote-mic/papermono_mic.py --list-devices
python3 tools/remote-mic/papermono_mic.py --device "BlackHole 2ch"
```

按住设备「遥控器 → 语音」页的语音区域开始说话，松开结束；接收器会持续运行并
在断连后自动重连。`Ctrl-C` 退出。

不需要蓝牙和声卡也能运行的自检：

```sh
python3 tools/remote-mic/papermono_mic.py --selftest
```

## 文件布局

| 路径 | 说明 |
| --- | --- |
| `papermono_mic.py` | 命令行入口（按路径运行，无需安装） |
| `remote_mic/protocol.py` | ATVV 帧、能力/事件解析、ADPCM 编解码、帧重组；仅标准库 |
| `remote_mic/audio.py` | 环形缓冲、DC 阻断、增益、WAV 录制；仅标准库 |
| `remote_mic/receiver.py` | bleak + sounddevice 粘合层，二者按需导入 |
| `remote_mic/cli.py` | 参数解析、`--selftest` |
| `tests/` | 纯 Python 单元测试（无蓝牙/声卡） |

## 测试

```sh
# 单元测试（纯 Python）
python3 -m unittest discover -s tools/remote-mic/tests -v

# 仓库主机测试入口（单元测试 + 导入契约 + CLI 检查）
python3 tests/host/test_remote_mic.py

# 语法检查
python3 -m py_compile tools/remote-mic/papermono_mic.py \
  tools/remote-mic/remote_mic/*.py tools/remote-mic/tests/*.py
```

单元测试覆盖能力帧、控制事件（含截断/非法包）、ADPCM 已知向量与往返
信噪比、任意分片重组、环形缓冲欠载/溢出、DC 阻断、增益钳位与 WAV 回读。
`--selftest` 另外在无硬件条件下跑通「编码 → 分片 → 重组 → 解码 → 音频处理 →
WAV」全链路。

## 依赖

Python 3.10+，`bleak`（BLE）、`numpy`（sounddevice 非 Raw 流内部依赖）与
`sounddevice`（PortAudio）。`bleak`/`sounddevice` 只在真正需要硬件时导入，
因此 `--help`、`--selftest` 和单元测试不需要安装它们；打开音频设备前必须已
安装 `numpy`。
