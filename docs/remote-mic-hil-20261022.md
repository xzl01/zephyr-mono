# 遥控器语音实机验收记录（2026-10-22）

本文件记录 PaperMono C153 上「遥控器 → 语音」功能的第一次真机验收。
软件侧验收见 [独立验收报告](remote-mic-verification.md)；本文件只记录实机结果。

## 1. 设备与环境

- 设备：M5Stack PaperMono C153（ESP32-S3R8，16 MB Flash，8 MB PSRAM）
- USB：`/dev/cu.usbmodem21201`（USB Serial/JTAG）
- 主机：macOS，系统蓝牙已与 C153 完成 HID 绑定（security level 4）
- 虚拟麦克风：系统已安装 `MiRemoteV 2ch`（SayAll 的 BlackHole 派生驱动）
- 接收器：`tools/remote-mic`，Python 3.14 venv，bleak 3.0.2 / sounddevice 0.5.6 / numpy 2.5.4

烧录/日志命令（workspace 外需要 full-access，且必须指定 Espressif OpenOCD）：

```sh
python3 scripts/papermono_usb_recover.py build-hil/zephyr/zephyr.bin \
  --method full \
  --openocd /Volumes/extend/tmpwork/papermono-reset-tools/openocd-esp32/bin/openocd \
  --timeout 20 --log /tmp/flash.log --reset-log /tmp/flash-reset.log
```

启动日志确认：

```
papermono_pdm_mic: PaperMono PDM microphone transport ready
papermono_ble_mic: BLE voice microphone ready: 16 kHz IMA ADPCM, 120-byte frames
papermono_hid: BLE HID connected; security request result=0
papermono_hid: BLE HID security changed: level=4 error=0
```

## 2. 实机发现与修复

### 2.1 PDM 实际采样率是 8 kHz，而不是 16 kHz（已修复）

首次会话（10 秒 MIC_OPEN）设备日志为：

```
BLE voice stopped after 9959 ms, 39480 bytes, 0 dropped frames
```

39,480 字节 = 329 帧 = 78,960 样本；16 kHz 下只有 4.94 秒音频，实际墙钟
9.96 秒，正好一半。根因是 PDM 时钟公式：

- 原配置：`mclk = 16000 * 128 = 2.048 MHz`，`bclk_div = 2`，
  `DSR_16S`（要求 PDM clock = 16k × 128 = 2.048 MHz）
- 实际 PDM clock 只有 `mclk / bclk_div = 1.024 MHz`，所以 PCM 率是 8 kHz。

修复为 ESP-IDF 默认的 8S 组合（见 `src/papermono_pdm_mic.c`）：

```
bclk = 16000 * 64 = 1.024 MHz
bclk_div = 8
mclk = 1.024 MHz * 8 = 8.192 MHz
DSR_8S
```

修复后同一 10 秒会话：

```
BLE voice stopped after 9973 ms, 79440 bytes, 0 enqueue drops, 616 notified, 46 notify failures
```

79,440 字节 = 662 帧 = 159,200 样本 ≈ 9.95 秒 @ 16 kHz，采集时长正确。
**这是本轮最重要的实机缺陷。**

### 2.2 I2S RX 在约 1 秒后 `-EIO`（已修复）

修复采样率后，会话在 ~1 秒时以 `PDM microphone read failed (-5)` 中止。
根因：`bt_gatt_notify()` 在普通线程中分配 ATT buffer 使用 `K_FOREVER`，
主机/控制器回收慢时采集线程被阻塞，PDM DMA 4 个 512 B 块（约 64 ms）溢出，
I2S 驱动进入 `I2S_STATE_ERROR`，随后读超时返回 `-EIO`。

修复：采集线程不再直接发 BLE。音频/控制通知进入 8 槽消息队列，由系统
workqueue 发送（该上下文使用 `K_NO_WAIT`，不会阻塞），并按 15 ms 间隔
逐条发送，避免一次灌满 ATT buffer。修复后连续 60 秒会话不再出现 `-EIO`。

### 2.3 连接间隔初始为 30 ms（已请求 15 ms）

连接建立后日志：

```
BLE voice link interval=30000 us latency=0
```

30 ms 只能承载约 33 帧/秒，而 16 kHz ADPCM 需要 66.7 帧/秒。会话开始时
调用 `bt_conn_le_param_update()` 请求 15 ms，返回 0，HID 日志确认：

```
papermono_ble_mic: BLE voice requested 15 ms connection interval (0)
papermono_hid: BLE HID interval=15000 us latency=0
```

### 2.4 Wi-Fi 共存下 16 kHz 链路容量不足（未修复，需要产品决策）

同一 10 秒会话在不同 Wi-Fi 状态下的对比：

| 配置 | 设备发送 | 接收器收到 | notify 失败 | 比例 |
| --- | --- | --- | --- | --- |
| Wi-Fi 内容中心启用（SoftAP 恢复重试活跃） | 662 帧 | 366–420 帧 | 251 | ~62% |
| `CONFIG_PAPERMONO_CONTENT_SERVER=n`（无 Wi-Fi 业务） | 662 帧 | 616 帧 | 46 | ~93% |

设备端 `frames_sent` 是按队列入队计数；`notify failures` 是系统 workqueue
中 `bt_gatt_notify()` 返回错误（主要是 `-ENOMEM`，ATT buffer 满）。这说明
问题在 BLE 空口/共存容量，而不是 PDM 或接收器。

结论：**16 kHz ADPCM（64 kbps）在 Wi-Fi 活跃时超出本机 BLE 实际吞吐
（约 40 kbps）**；无 Wi-Fi 时基本可用（7% 丢帧，靠 50 帧一次的 AUDIO_SYNC
恢复）。

另外尝试了 `coex_preference_set(ESP_COEX_PREFER_BT)`（`CONFIG_ESP32_SW_COEXIST_ENABLE`），
在会话期间把射频优先级偏向 BLE，实测接收帧数 404/662，与未调用时
（366–420/662）没有可测改善；Wi-Fi 侧的信标/重试仍占用空口。因此该调用保留为
无害的偏向声明，但不作为共存问题的修复。

按产品选择实现 **语音会话期间暂停 Wi-Fi**（保留 16 kHz 音质）：

- 新增 `papermono_content_server_pause_wifi()` / `resume_wifi()`：
  会话开始前取消内容中心的重试/扫描/AP 工作，断开 STA 并关闭 SoftAP，
  会话结束后恢复 STA 连接或 SoftAP。
- 实测 6 秒会话：352/400 帧送达（88%），35 次 notify 失败；10 秒会话
  584/662 帧（88%）。相比未暂停时的 366–420/662（55–63%）明显改善，
  已接近无 Wi-Fi 的 616/662（93%）。
- 停止日志随后出现 `Wi-Fi resumed after BLE voice session`，并在约 7 秒后
  重新 `Wi-Fi station connect complete`；内容中心在会话期间不可用，会话结束
  后恢复。
- 残余约 9% 丢帧由 50 帧一次的 AUDIO_SYNC 恢复；实机听感仍需人工确认。

## 3. 已验证通过的路径

### 3.1 主机主动 MIC_OPEN

接收器 `--send-mic-open --mic-open-seconds N` 可让设备在无触摸时开始会话；
设备发送 caps、AUDIO_START、AUDIO_SYNC、音频帧，N 秒后 MIC_CLOSE 触发
AUDIO_STOP。

### 3.2 按住说话 UI

用户在「全部应用 → 遥控器 → 语音」页按住语音区约 3 秒后松开，接收器日志：

```
AUDIO_START session=0 ADPCM 16 kHz reason=0
AUDIO_STOP reason=0
session 0: 159 frames, 2.38 s, peak 2087, gaps 3 (device stopped)
```

说明触摸按住/松开、页面状态、设备主动开始/停止均工作。

### 3.3 虚拟麦克风端到端

接收器写入 `MiRemoteV 2ch`，同时用 sounddevice 从同一设备的录音侧抓取
8 秒：

```
MiRemoteV 2ch: peak 2100, rms 39.2, 8.0 s
```

录音包含与接收器 WAV 对应的语音能量，说明
「BLE → 接收器 → 虚拟设备输出 → 虚拟设备输入」链路已打通。目标应用选择
`MiRemoteV 2ch` 作为麦克风即可（与 BlackHole 用法相同）。

### 3.4 60 秒上限

`--mic-open-seconds 70` 时设备在约 60 秒主动停止并发送 AUDIO_STOP；接收器
在 60 秒前结束会话（因丢帧，解码音频为 53.1 秒），没有超过上限。

## 4. 接收器实机修复

- `requirements.txt` 补 `numpy`：sounddevice 的非 Raw 流内部导入 numpy，
  缺它会在打开音频设备时报 `No module named 'numpy'`。
- macOS 已连接外设支持：设备同时作为 HID 键盘被系统连接时不再广播，普通
  `BleakScanner` 扫描不到。接收器现在通过 CoreBluetooth
  `retrieveConnectedPeripherals` 复用系统连接，并支持 `--address
  <CoreBluetooth UUID>`。本次使用的 UUID 为
  `640A7240-E23A-46F2-EBAE-2DB60B04E70D`。

## 5. 当前设备状态与复现

- 已烧回完整功能镜像 `build-hil2/zephyr/zephyr.bin`（Wi-Fi 内容中心启用、
  16 kHz、PDM 修复、TX 队列、15 ms 请求、BLE 共存偏向）。该目录同时包含
  工作区中并行的 SSD1677 刷新策略改动，不是本功能的代码。
- 诊断镜像 `build-hil-nowifi/zephyr/zephyr.bin` 仅用于无 Wi-Fi 对比，
  不建议日常使用。
- 关键日志：
  - `/tmp/serial-nowifi.log`（无 Wi-Fi，616/662）
  - `/tmp/serial-fast.log`（Wi-Fi，411/662）
  - `/tmp/serial-txq.log`（TX 队列修复前后对比）
  - `/tmp/papermono-hold.wav`（按住说话）
  - `/tmp/vmic-capture.wav`（虚拟设备录音侧）
- 未验证：目标 App 实际取流（本轮用录音侧代替）、更长朗读、Wi-Fi 已连接
  但空闲场景、2M PHY 实际值、接收器在 Windows/Linux 上的实机行为。
