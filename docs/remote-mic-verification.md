# 遥控器语音（BLE remote microphone）独立验收报告

- 验收对象：PaperMono C153 / Zephyr 4.4.1 / ESP32-S3，「遥控器 → 语音」ATVV v1.0 语音流 + `tools/remote-mic` PC 接收器。
- 验收人：独立验收（verifier），不参与实现；**只写本文件，未改动任何源码**。
- 验收时间：2026-10-22 10:24–10:52 (Asia/Shanghai)；含 10:38–10:40 修复后的复核轮（§10）。
- 结论：**构建、主机测试、协议/编解码跨语言一致性全部通过。**首轮发现 8 处轻微缺陷（D1–D8）与 1 处隐私文档缺口（O3），实现方已全部修复；复核轮逐条确认修复有效、无回归、无新缺陷（§10）。**实机 BLE 音频链路、音质与虚拟麦克风端到端仍未验证——本次验收不构成硬件验收。**

## 0. 硬件边界（必须先读）

本次验收**没有**真机、没有 BLE 主机、没有声卡，因此以下内容**完全没有**被验证：

- PDM 麦克风真实采样率/左右声道排布、电源时序、采集是否真的每 15 ms 产出 240 个样本；
- ATT MTU 协商（123）、连接间隔（15 ms）、2M PHY、实际吞吐与丢包；
- e-paper 刷新对音频线程的抢占、真实端到端延迟；
- `sounddevice` → BlackHole / VB-CABLE / null sink 的写入与目标应用取流；
- 60 秒上限、休眠/断连/松开在真机上的时序表现。

本报告只覆盖：固件两种配置的构建与内存水位、主机测试、固件源码 ↔ 接收器的协议与 ADPCM 线格式一致性、静态并发/生命周期审查、UI 交互路径审查、文档与代码逐条对照、改动范围检查。

## 1. 基线（被验收的代码状态）

仓库 HEAD `31dcc72`，功能全部为未提交的工作区改动（12 个已修改文件 + 22 个未跟踪文件，含本报告）。验收期间工作区被实现方更新过两轮：首轮（`prj.conf` 10:29、`docs/delivery-status.md` 10:31、`src/papermono_ble_mic.c` 10:34）与修复轮（10:38–10:39，D1–D8/O3）。下表是本报告对应的**最终**状态；§2 的构建/测试数字与 §10 的复核结论均在该状态下重新执行得到。

| 文件 | SHA-256（前 16 位） |
| --- | --- |
| `src/papermono_ble_mic.c` | `061aa7d054e2b11f` |
| `src/papermono_adpcm.c` | `d2215fb3056c913c` |
| `src/papermono_ui.c` | `f6f960cad25a29ca` |
| `src/papermono_hid.c` | `d914d1c20c8d4b63` |
| `src/papermono_power.c` | `4854504449cd88a7` |
| `src/main.c` | `465a81460a5f9328` |
| `prj.conf` | `f957c00fa06958ed` |
| `Kconfig` | `4ff012abe9dc6e18` |
| `CMakeLists.txt` | `ecdc91aa84bd02d1` |
| `include/papermono/ble_mic.h` | `a7cdb3462496a1e3` |
| `include/papermono/adpcm.h` | `b97e9f87fdf37b5a` |
| `tools/remote-mic/remote_mic/protocol.py` | `1640b2fb2f5b20df` |
| `tools/remote-mic/remote_mic/receiver.py` | `71d075cc071f9aa7` |
| `tools/remote-mic/remote_mic/cli.py` | `c073d6ff92624f7f` |
| `tools/remote-mic/remote_mic/audio.py` | `e806dbb0edc683b6` |
| `tests/host/test_adpcm.c` | `e6b34cc98faa76e7` |
| `tools/remote-mic/tests/test_protocol.py` | `f34dd931f8232c49` |
| `tests/host/test_remote_mic.py` | `c919feb0445ca8dc` |
| `tests/host/run.sh` | `bc00dac32e177b3e` |
| `docs/remote-mic-protocol.md` | `f715291cdfe768b3` |
| `docs/remote-mic.md` | `671d6e54230d4e28` |

## 2. 执行的命令与结果

### 2.1 固件构建（feature-on）

```sh
cd /Volumes/extend/Dev
CCACHE_DIR=/Volumes/extend/Dev/zephyr-mono/.cache/ccache \
CCACHE_TEMPDIR=/Volumes/extend/Dev/zephyr-mono/.cache/ccache-tmp \
ZEPHYR_SDK_INSTALL_DIR=/Volumes/extend/Dev/toolchains/zephyr-sdk-1.0.1 \
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
west build -p always -d zephyr-mono/build-verify -b m5stack_papermono/esp32s3/procpu zephyr-mono \
  -- -DZEPHYR_TOOLCHAIN_CAPABILITY_CACHE_DIR=/Volumes/extend/Dev/zephyr-mono/.cache/toolchain-capability
```

结果：`exit=0`，`[1165/1165] Linking C executable zephyr/zephyr.elf`，`warning:` 计数 **0**。

```
Memory region         Used Size  Region Size  %age Used
           FLASH:     4551220 B   16776960 B     27.13%
     iram0_0_seg:       79948 B     415492 B     19.24%
     dram0_0_seg:      334976 B     399108 B     83.93%
     irom0_0_seg:      881824 B        32 MB      2.63%
     drom0_0_seg:     4420148 B        32 MB     13.17%
    ext_dram_seg:     7246384 B         8 MB     86.38%
```

### 2.2 固件构建（feature-off）

同一条命令，`-d zephyr-mono/build-verify-off` 并追加 `-DCONFIG_PAPERMONO_BLE_MIC=n`。

结果：`exit=0`，`warning:` 计数 **0**。

```
           FLASH:     4549812 B   16776960 B     27.12%
     dram0_0_seg:      329280 B     399108 B     82.50%
```

`build-verify-off/zephyr/.config`：`# CONFIG_PAPERMONO_BLE_MIC is not set`。

### 2.3 内存预算

```sh
python3 scripts/check_memory_budget.py build-verify/zephyr/zephyr.map      # exit=0
python3 scripts/check_memory_budget.py build-verify-off/zephyr/zephyr.map  # exit=0
```

```
internal DRAM: 334,976 / 399,108 bytes (83.93%, limit 85.00%)
internal DRAM: 329,280 / 399,108 bytes (82.50%, limit 85.00%)
```

功能净开销：内部 DRAM **+5,696 B**，FLASH **+1,408 B**。距 85% 上限（339,241 B）仅剩 **4,265 B（1.07 个百分点）**。

### 2.4 主机测试

```sh
sh tests/host/run.sh                      # exit=0
python3 tests/host/test_remote_mic.py     # exit=0，"all remote-mic host checks passed"
python3 -m unittest discover -s tools/remote-mic/tests   # Ran 101 tests ... OK
python3 -m py_compile tools/remote-mic/papermono_mic.py tools/remote-mic/remote_mic/*.py tools/remote-mic/tests/*.py   # exit=0
```

`run.sh` 中与本次功能相关的行：`test_adpcm: PASS`；`[ok] unit suite — 101 test(s), 0 failure(s), 0 error(s)`；`[ok] papermono_mic.py --selftest — selftest passed`；`[ok] --list-devices without sounddevice reports the dependency — exit 2`。

### 2.5 接收器自检与命令行

```sh
python3 tools/remote-mic/papermono_mic.py --selftest    # exit=0
python3 tools/remote-mic/papermono_mic.py --help        # exit=0，15 个选项
```

`--selftest` 逐项：capabilities vector / silence frame / ADPCM round trip (chunked notifications, SNR 30.6 dB) / ring buffer overrun / ring buffer underrun / gain clamp / DC blocker / WAV capture / lazy dependencies —— 全部 `[ok]`，`selftest passed in 0.01 s`。

`--help` 包含 `--address --name --device --list-devices --gain-db --wav --verbose --once --send-mic-open --mic-open-seconds --scan-timeout --ring-ms --no-dc-blocker --selftest --version`。

### 2.6 固件源码 ↔ 接收器协议交叉校验（本次新增，非仓库测试）

用脚本直接解析 `src/papermono_ble_mic.c` 的 `#define` 与 `send_caps()` 初始化列表，把固件真实会发出的字节喂给 `remote_mic.protocol`：

```
firmware caps bytes      : 0b 01 00 02 03 00 78 00 00
receiver parses it       : capabilities v1.0 ADPCM 16 kHz frame=120B (15.0 ms) interaction=0x03
firmware AUDIO_START     : 04 00 02 07
firmware AUDIO_SYNC      : 0A 02 0032 fb2e 2a -> seq/predictor(-1234)/step 全部还原
firmware MIC_OPEN_ERROR  : 0c 00 03 -> MIC_OPEN_ERROR 0x0003 (device or microphone unavailable)
receiver GET_CAPS       : 0a 01 00 00 03 03 -> firmware opcode 0x0a
receiver MIC_OPEN       : 0c 00 / 0c 00 02   -> firmware opcode 0x0c
receiver MIC_CLOSE      : 0d 03              -> firmware opcode 0x0d
receiver MIC_EXTEND     : 0e 03              -> firmware opcode 0x0e
firmware UUIDs           : ab5e0001/0002/0003/0004-5a21-4f05-bc7d-af01f617b664
frame geometry           : 120 bytes / 240 samples / sync every 50 frames
send_audio_sync call sites : 2 (periodic in send_frame + resync after failure)
D3 check                 : start_session() emits no sync; frame 0 sync comes from send_frame()
firmware <-> receiver cross-check passed
```

### 2.7 ADPCM 线格式跨语言校验（本次新增）

用真实 `src/papermono_adpcm.c` 编译一个小程序，按固件规则（`seq % 50 == 0` 的帧编码前发 SYNC，携带编码前状态）输出 120 帧 × 240 样本（含 220 Hz、1 kHz 满幅、静音、满幅方波），再用接收器**实际使用**的 `AdpcmDecoder` + `FrameAccumulator` 解码：

```
220Hz tone  C->Py 39.14 dB   Py->Py 39.14 dB
1kHz loud   C->Py 32.95 dB   Py->Py 32.95 dB
decoded streams bit-identical: True
C encoder final state: (-32767, 11)  Python decoder final state: (-32767, 11)
nibble probe [1000,-1000]: 7f
```

C 编码器 → Python 解码的结果与 Python 编码器 → Python 解码**逐样本完全相同**，且 120 帧后两端预测器/步长索引完全一致。这排除了步长表、索引表、半字节顺序、跨帧状态连续性、有符号预测值传输等所有可能的线格式偏差。静音段在 76 个样本内收敛到精确 0（此前是 1 kHz 满幅音，属正常收敛）。

### 2.8 修复后会话起始顺序的接收器仿真

按 D3 修复后的固件顺序（caps → AUDIO_START → 首帧前 `AUDIO_SYNC seq=0` → 音频），用接收器真实的 `VoiceReceiver`（桩 sink）验证三种情况：

```
new order  : frames=50 gaps=0 expected_seq=50 samples=12000
no first sync: frames=50 gaps=0
duplicate seq=0 sync still harmless (backward compatible with old firmware)
```

新顺序不产生伪 gap；即使首条 SYNC 丢失（`AUDIO_START` 已复位解码器）仍能连续解码；旧固件连发两条 seq=0 也仍兼容。

## 3. 逐项结论

| # | 检查项 | 结论 |
| --- | --- | --- |
| 1 | feature-on 构建 + 内部 DRAM < 85% | **PASS**（83.93%，0 warning） |
| 2 | feature-off 构建（`-DCONFIG_PAPERMONO_BLE_MIC=n`），无第四页/语音代码、无 unused 警告 | **PASS**（0 warning；ELF 中无任何语音符号/字符串） |
| 3 | `sh tests/host/run.sh` | **PASS**（exit 0，含 `test_adpcm: PASS`、101 项 Python 测试） |
| 4 | `--selftest` 通过、`--help` 列出必需选项 | **PASS** |
| 5 | 固件 ↔ 接收器协议交叉（UUID / caps / START / SYNC / STOP / ADPCM / MTU / GET_CAPS / MIC_OPEN/CLOSE） | **PASS**（首轮缺口 D1 已修复并复核，§10） |
| 6 | 并发、锁、bt_conn 生命周期、CCC、abort/power、断连、会话上限、失败路径 | **PASS**（未发现死锁/UAF/卡死；见 §5 审查记录与 O5/O6/O7） |
| 7 | UI 按住说话：按下/松开、刷新保护、换页/换模式/休眠拆除、无卡住接触、无点击泄漏、无条件编译 | **PASS**（见 §5、O1/O2） |
| 8 | 安全与隐私：加密特征、仅按住时采音、不落盘、60 s 上限、主机 MIC_OPEN | **PASS**（O3 的隐私文档缺口已补齐并复核，§10） |
| 9 | 文档声明与代码逐条对照 | **PASS（修复后）**：首轮 D2/D3/D4/D8 已按代码改写并复核（§10），其余逐条为真 |
| 10 | 改动范围（无越界改动/生成物/构建产物入库） | **PASS**（见 §6） |

## 4. 缺陷清单

全部为轻微级，均不影响构建与主机测试。以下保留首轮原始记录（缺陷描述、证据与行号均按首轮代码写，`src/papermono_ble_mic.c` 修复后整体下移数行）；实现方已按本清单修复，当前行号与复核证据见 §10。

### D1（轻微，接收器行为）`MIC_OPEN_ERROR 0x0003` 未翻译 —— **已修复**（§10）

- 位置：`tools/remote-mic/remote_mic/protocol.py:202-206`（`MicOpenError.description` 只映射 `0x0001`/`0x0002`）。
- 事实：固件会发 `0x0003`（`src/papermono_ble_mic.c:45-47`、`:257-273`），文档也定义了它（`docs/remote-mic-protocol.md:108-109`、`docs/remote-mic.md:249-251`）。
- 证据：交叉校验脚本输出 `MIC_OPEN_ERROR 0x0003 (unknown error 0x0003)`。
- 影响：设备/麦克风不可用时，用户看到 "unknown error" 而不是"设备或麦克风不可用"，与 `docs/remote-mic.md:260-261` 的排障表不一致。纯显示问题，不影响音频。
- 建议：在 `description` 中补 `0x0003: "device or microphone unavailable"`，并在 `tools/remote-mic/tests/test_protocol.py:114-126` 增加一条断言。

### D2（轻微，文档）`0x0003` 的示例不成立 —— **已修复**（§10）

- 位置：`docs/remote-mic.md:250-251`：「`0x0003` 表示设备或麦克风当前不可用（例如 PDM 启动失败）」。
- 事实：**运行期** PDM 启动失败（`src/papermono_ble_mic.c:372-382`）不会向主机发 `MIC_OPEN_ERROR`，只写 `mic_state.last_error` 并让 UI 显示失败；`0x0003` 只来自 `request_start_internal()` 因 `!initialized` 返回 `-ENODEV`（`:312-313`），即 `papermono_ble_mic_init()` 里 `device_is_ready(microphone)` 失败（`:748-750`）。
- 影响：真机排障时，用户按「0x0003 = PDM 启动失败」去查会找不到该事件（实际表现为 UI 报错、主机侧无事件）。
- 建议：改为「设备未初始化/麦克风设备不可用（`papermono_ble_mic_init()` 失败）；运行期 PDM 启动失败只体现在设备 UI」。

### D3（轻微，固件/文档）会话开始会连发两条 `AUDIO_SYNC seq=0` —— **已修复**（§10）

- 位置：`src/papermono_ble_mic.c:403`（`start_session()` 发一次）与 `:467-473`（`send_frame()` 在 `frame_sequence == 0` 时再发一次）。
- 事实：`docs/remote-mic-protocol.md:176-186` 的「典型会话开头」只画了一条 `0A 02 00 00 00 00 00`；`docs/remote-mic.md:328-329` 的验收步骤 3 也只期望一条。
- 影响：无害（接收器对重复 seq=0 不报 gap，已由 `tools/remote-mic/tests/test_receiver_session.py:87-107` 的语义覆盖），但日志会出现两条相同的 `AUDIO_SYNC seq=0`，与文档不符，也让真机日志核对多一个疑问点。
- 建议：`start_session()` 不再单独发 sync（让 `send_frame()` 的第 0 帧同步覆盖），或在文档里注明会连发两条。

### D4（轻微，文档）`MIC_CLOSE` 并非总会回复 `AUDIO_STOP` —— **已修复**（§10）

- 位置：`docs/remote-mic-protocol.md:62-63`：「主机发送 MIC_CLOSE 时设备停止并回复 AUDIO_STOP」。
- 事实：`stop_session()`（`src/papermono_ble_mic.c:427-433`）在没有活动会话时直接 `pdm_mic_stop()` + 置 IDLE 并返回，不发 `AUDIO_STOP`。因此空闲状态下的 `MIC_CLOSE`（含接收器 `--send-mic-open` 超时后补发的那条）不会收到回复。
- 影响：仅文档精度问题；"重复关闭是幂等的"这一句仍成立。
- 建议：文档改为「有活动会话时回复 AUDIO_STOP；空闲时幂等无回复」。

### D5（轻微，头文件注释）`papermono_ble_mic_abort()` 的超时路径与注释不符 —— **已修复**（§10）

- 位置：`include/papermono/ble_mic.h:67-73`：「Sends AUDIO_STOP when possible and returns after the worker has released the PDM stream」。
- 事实：超时分支（`src/papermono_ble_mic.c:799-802`）在 1 s 内没等到 worker 时**直接**调用 `papermono_pdm_mic_stop()` 并返回，此时 worker 未必已经释放 PDM 流（随后它会在下一次 `papermono_pdm_mic_read()` 拿到 `-EACCES` 并自行收敛）。
- 影响：注释承诺强于实现；该路径是休眠/关机的兜底，风险低。
- 建议：注释补一句「on timeout the microphone is force-stopped and the worker converges on its next read」。

### D6（轻微，日志）停止日志的 "dropped frames" 是开机累计值 —— **已修复**（§10）

- 位置：`src/papermono_ble_mic.c:446-449`：`dropped = mic_state.frames_dropped;` 与同一行的 `session_ms`/`session_bytes`（会话内值）一起打印为 "BLE voice stopped after %u ms, %u bytes, %u dropped frames"。
- 事实：`frames_dropped`/`frames_sent` 从不按会话清零，只有 `session_ms`/`session_bytes` 清零（`:396-397`）。
- 影响：多会话后该行数字会误导真机排障（例如第二次 1 秒会话显示「3 dropped frames」其实来自上一次）。
- 建议：打印前记录会话起点值并求差，或把字段改名为累计值。

### D7（提示，仓库卫生）新增文件权限为 `0600` —— **已修复**（§10）

- 位置：`docs/remote-mic*.md`、`tools/remote-mic/**`、`src/papermono_ble_mic.c`、`include/papermono/ble_mic.h`、`tests/host/test_adpcm.c` 均为 `-rw-------`，仓库既有文件为 `-rw-r--r--`。
- 影响：`git add` 会归一化为 100644，提交后自愈；提交前其他用户/CI 账号可能读不到。建议 `chmod 644`。
- 说明：这是提交前的工作区状态问题，不是代码缺陷。

### D8（提示，文档）MTU 回退的可持续性没有说明 —— **已修复**（§10）

- 位置：`docs/remote-mic-protocol.md:125`「设备在 MTU 不足时按 `MTU-3` 分片」。
- 事实：代码确实这样分片（`src/papermono_ble_mic.c:174-192`），但 MTU=23 时一帧要拆成 6 条通知/15 ms，能否在 15 ms 连接间隔内发完没有依据，文档也没写「仅保证字节流完整，不保证小 MTU 下不丢帧」。
- 建议：补一句小 MTU 下可能丢帧、依赖 AUDIO_SYNC 恢复；实机验收时固定用 MTU 123 的链路。

## 5. 并发 / 生命周期 / UI 审查记录（未发现阻塞问题）

**bt_conn 生命周期**：所有访问都在 `mic_lock` 下取 `bt_conn_ref()`，使用后立即 `bt_conn_unref()`（`connection_ref()` `:109-117`，`start_session()` `:369/409`，`stop_session()` `:427/441`，`caps_work_handler()` `:276-289`）。`mic_disconnected()` 在锁内比对指针后 unref 并置 NULL（`:703-710`），栈自身仍持有引用，不存在回调内提前释放。`caps_connection` 由 `schedule_caps()` 持引用、`caps_work_handler()` 消费、断连时兜底释放，二者互斥于同一把锁，不会双重 unref。**未发现 use-after-free。**

**死锁**：`mic_lock` 内只有原子/短操作，`bt_gatt_notify()`/`bt_gatt_is_subscribed()` 全部在锁外调用，不与 BT host 内部锁形成反向顺序。`caps_work_handler` 运行在系统工作队列（cooperative）并取 `mic_lock`，但持锁段不阻塞；worker（prio 9）与 BT RX（prio 8）、FT6336（prio 8）、UI（prio 10）的关系符合 Kconfig 注释的意图（`CONFIG_PAPERMONO_BLE_MIC_THREAD_PRIORITY=9`）。`papermono_ble_mic_abort()` 等待 `mic_idle` 最长 1 s，worker 的 `i2s_read` 超时为 250 ms（`src/papermono_pdm_mic.c:88` 的 `timeout = 250`），不会无限等待；超时还有强制关麦兜底。

**状态机**：`start_pending/stop_pending/abort_pending` 只在锁内读写，worker 每轮先取快照再执行 `stop_session()`→`start_session()`。已核对：按下后立刻松开、松开后才轮到 worker、会话上限触发、CCC 取消订阅、断连、休眠/关机、初始化失败、PDM 启动失败、通知返回 `-ENOTCONN`/`-EACCES`/`-ENOMEM` 各路径都能回到 IDLE，没有"卡在 STREAMING"的路径。`mic_state.streaming` 与 `phase` 在每条路径上同步更新。

**UI 按住说话**：`voice_hold` 在按下时用 `remote_voice_epoch` 的奇偶位 + 命中矩形判定（`src/papermono_ui.c:3909-3930`），按住期间 ABS 事件直接返回，不会泄漏成指针移动或边缘导航（`:3855-3861`）；松开走 CAS（`:3962-3970`）保证「页面拆除」和「用户松开」只停一次；`remote_voice_set_armed(false)` 在换模式（`:2233`）、离开页面 `remote_close()`（`:2328`）、`render_current()` 的兜底（`:4074`）、休眠/关机（`:4136`）四处都调用。页面无定时器、无电平表，只在开始/结束/后台刷新时重绘。条件编译：`REMOTE_TAB_COUNT` 3/4、页签 140/103 px、`remote_voice_pad`、`voice_hold`、原子量全部由 `CONFIG_PAPERMONO_BLE_MIC` 包裹，与 `CONFIG_PAPERMONO_INTERACTIVE_UI` 的交叉路径也核对过（`papermono_ui_request_background_refresh()` 无条件定义，`:4495-4505`）。

**安全/隐私**：Transmit 为 `WRITE_ENCRYPT`（`src/papermono_ble_mic.c:131`），Audio/Control 的 CCC 为 `READ_ENCRYPT|WRITE_ENCRYPT`（`:135`、`:139`），未加密链路上 `request_start_internal()` 直接 `-EACCES`；只在已订阅且加密时通知；`MIC_SESSION_LIMIT_MS=60000`（`:28`，`:501-509`）；无任何音频落盘或内容日志（只记录帧数/字节数/时长）。见 O3 关于主机侧 `MIC_OPEN` 的隐私提示。

**feature-off 隔离**：`build-verify-off/zephyr/zephyr.elf` 中 `nm` 无 `papermono_ble_mic_*`/`papermono_adpcm_*`/`adpcm_step_table`/`attr_papermono_ble_mic_service`；`strings` 无「语音」「语音输入」「按住此区域说话」「等待电脑订阅语音服务」「BLE voice session」；仅剩 Kconfig 的 `CONFIG_PAPERMONO_BLE_MIC_LOG_LEVEL*` 符号（Kconfig 模板无条件生成，无副作用）。feature-on 中上述符号/字符串全部存在。

## 6. 改动范围检查

`git status` 只有 12 个已修改文件（`CMakeLists.txt`、`DESIGN.md`、`Kconfig`、`README.md`、`docs/application.md`、`docs/delivery-status.md`、`prj.conf`、`src/main.c`、`src/papermono_hid.c`、`src/papermono_power.c`、`src/papermono_ui.c`、`tests/host/run.sh`）与 21 个新增文件（本功能源码/头文件/文档/`tools/remote-mic/**`/两个测试）。逐个查看 diff 后确认：

- 无生成资源、截图、二进制或构建产物入库；`build*`、`__pycache__/`、`*.elf/*.map/*.bin` 均被 `.gitignore` 覆盖（`git status -uall` 未出现 `__pycache__`）。
- `prj.conf` 的改动都在功能范围内（MTU/ACL/heap），且注释与实测一致：`CONFIG_BT_L2CAP_TX_MTU=123` + `CONFIG_BT_BUF_ACL_TX_SIZE=127` = 4 B L2CAP 头 + 123 B ATT PDU，正好一帧 120 B 负载 + 3 B ATT 头不分片。
- `src/papermono_hid.c` 只在广播数据里加了 128-bit ATVV UUID（3 + 6 + 18 = 27 B ≤ 31 B），名称仍在 scan response；`src/main.c`、`src/papermono_power.c` 的接入点都在 `#if defined(CONFIG_PAPERMONO_BLE_MIC)` 内。

## 7. 残余风险（需要真机确认）

- **O1 内存余量只有 4,265 B（1.07 pp）**。功能本身净增 5,696 B 内部 DRAM，其中一部分来自把 `CONFIG_HEAP_MEM_POOL_SIZE` 从 65,536 降到 53,248（−12,288 B，`prj.conf:140-144`）。仓库内无 `k_malloc/k_calloc` 调用点（`grep -rn 'k_malloc\|k_calloc\|k_aligned_alloc' src/ include/` 为 0），Zephyr 侧该版本 HTTP server 也不用内核堆（只 `getaddrinfo`/`socketpair`/`wifi_certs` 使用，各有 `HEAP_MEM_POOL_ADD_SIZE`），因此判断风险可接受；但"Wi-Fi 上传 + 语音并发"这条组合没有任何主机测试覆盖，建议实机跑一次大文件上传同时按住说话。
- **O2 每次按住/松开最多触发 4 次整页重绘**：按下 `queue_voice_refresh()`（`src/papermono_ui.c:3726-3730`）、worker 开始 `papermono_ui_request_background_refresh()`（`ble_mic.c:366`/`:410`）、松开 `queue_voice_refresh()`（`ui.c:3967`）、worker 停止（`ble_mic.c:454`）。`pending_background_refresh` 只对系统事件去重，触摸路径不去重。不是死循环，但在 e-paper 上可能表现为一段话 2–4 次闪烁/数百毫秒的刷新，建议实机观察后再决定是否合并。
- **O3 主机可无手势开启麦克风（文档缺口已修复，行为本身按设计保留）**：`MIC_OPEN`（`:665-670`）等价于用户按住，任何已配对主机都可以在设备无任何操作的情况下采音最长 60 秒（屏幕显示"正在传音"，不请求确认）。`docs/remote-mic.md` §8 隐私段已补充该说明与「不要让不可信主机保持配对」的建议（复核见 §10）；行为未改，仍是 ATVV 兼容性的一部分。
- **O4 掉帧后的恢复窗口最长 750 ms（文档已按 D8 补充）**：固件每 50 帧（750 ms）发一次同步（`:24`、`:471`），主机侧丢通知造成的解码器失步要等到下一次同步才恢复；固件侧发送失败会立刻补发同步（`:517-525`），但 BLE 空口丢包不会。`docs/remote-mic-protocol.md:134-136` 现已写明该 750 ms 量级（§10/D8）。
- **O5 2,048 B 的采集线程栈未经实测**（`CONFIG_PAPERMONO_BLE_MIC_THREAD_STACK_SIZE=2048`）：ADPCM 编码 + `bt_gatt_notify` 的调用栈 + 日志在最坏情况下是否够用只能实测（建议 `CONFIG_INIT_STACKS` + `k_thread_stack_space_get()` 或在 HIL 里看 high-water mark）。
- **O6 采集线程优先级 9 高于 UI（10）、低于 BT RX/触摸（8）**：设计合理（音频不会被 LVGL 布局拖慢），但 BT RX 与触摸线程仍可短暂抢占采集线程；I2S DMA 只有 4 个 512 B 块（`src/papermono_pdm_mic.c:24-28`，约 64 ms），若被连续抢占超过 64 ms 会出现丢块/杂音，需实机在"滑动鼠标 + 按住说话"和"Wi-Fi 上传 + 按住说话"两种负载下确认。
- **O7 按住时不做 `input_blocked` 检查**：刷新期间的触摸噪声若落在语音区会直接开启会话（`src/papermono_ui.c:3909-3930` 在 `submit_input_event()` 的输入保护之外）。这与鼠标板的设计一致，但对麦克风更敏感；实机应观察刷新期间是否出现"未按却开始传音"。
- **O8 会话中途被用户手势截断**：主机 `MIC_OPEN` 建立的会话，只要用户在语音页按一下再松开就会被 `request_stop()` 结束（`:3962-3970` 无条件停止）。`--send-mic-open` 验收时不要碰屏幕。
- **O9 60 秒上限与 `MIC_EXTEND`**：`MIC_EXTEND` 被忽略（`:675-677`），依赖它的第三方 ATVV 主机每 60 秒会被强制断开，文档已写明，但对"兼容现有 ATVV 工具"的说法要有保留。

## 8. 仍需在真机上执行的验收步骤

以下步骤本报告**未执行**，按顺序做，任一步失败即视为功能未验收：

1. **烧录与启动**：`west flash -d build-verify`，串口确认 `BLE voice microphone ready: 16 kHz IMA ADPCM, 120-byte frames`，且无 `PDM microphone start failed`。
2. **广播与配对**：`nRF Connect` / `bluetoothctl` 确认广播含 `AB5E0001-…`；完成配对后确认 Transmit/Audio/Control 三个特征存在且 CCC 需要加密（未加密时写 `0A 01 00 00 03 03` 应被拒绝或触发配对）。
3. **能力帧**：接收器 `python3 tools/remote-mic/papermono_mic.py --device "<虚拟设备>" --verbose`，日志应出现 `capabilities v1.0 ADPCM 16 kHz frame=120B (15.0 ms) interaction=0x03`。
4. **MTU 与链路**：确认协商 MTU ≥ 123（`--verbose` 或系统侧抓包）；确认连接间隔 15 ms、PHY 2M；否则记录实际值并观察是否仍能 1 帧/通知。
5. **按住说话**：按住语音区，日志出现 `AUDIO_START session=0` 与 `AUDIO_SYNC seq=0 predictor=0 step=0`（修复后只有一条，见 §10），`frames` 持续增长、`gaps` 为 0、`underruns` 缓慢增长；松开后出现 `AUDIO_STOP`。
6. **音质**：`--wav /tmp/voice.wav` 录制 ≥ 10 秒朗读，用 Audacity 确认 16 kHz 单声道、语音清晰、无持续直流、无明显金属声/断续；对比"静音房间"与"说话"的峰值差异。
7. **60 秒上限**：按住不放，确认 60 s 时设备主动停止、接收器收到 `AUDIO_STOP`、设备 UI 回到可再次按住状态。
8. **异常路径**：a) 按住时断开电脑蓝牙 → 设备 UI 回到未连接、麦克风电源关闭（可用电流表/热成像或听麦克风供电脚）；b) 按住时按电源键休眠 → 先停流再休眠；c) 接收器退出（取消订阅）→ 设备停止；d) 接收器 `--send-mic-open --mic-open-seconds 5 --once` 验证主机侧开启与 `MIC_CLOSE`（按 D4 修复后的文档：空闲关闭幂等、无 AUDIO_STOP）。
9. **虚拟麦克风端到端**：目标应用（QuickTime/Teams/Audacity）选 BlackHole 2ch（或 CABLE Output / Monitor of null sink）作为输入，确认有电平、无回声/双声道错位（验证 PDM 单声道在双声道 I2S 槽中的排布，这是 `src/papermono_pdm_mic.c:44-73` 的 `I2S_SLOT_MODE_MONO` + `I2S_PDM_SLOT_RIGHT` 在真机上的关键未知项）。
10. **并发与资源**：按住说话同时滑动鼠标板、同时触发 Wi-Fi 上传；观察丢帧率、栈 high-water mark、内部 DRAM 剩余堆（`k_heap` 统计）是否触底。
11. **重连**：设备重启/走远再回来，接收器按 1 s→20 s 退避重连后，重新按住可继续出音。

## 9. 复现方式

本报告中新增的两个交叉校验脚本是临时文件（`/tmp/pm_wire_encode.c`、`/tmp/pm_source_crosscheck.py`），未入库；如需回归，建议把它们整理进 `tests/host/`（`pm_source_crosscheck.py` 用标准库解析 `src/papermono_ble_mic.c`，已在复核轮扩展为同时断言「`start_session()` 不发 SYNC、首帧 SYNC 由 `send_frame()` 发出」，可在 CI 里防住固件/接收器协议漂移）。其余命令全部是仓库内既有入口，可直接复现 §2 的所有数字。

## 10. 后续修复复核（2026-10-22 10:38–10:52）

实现方按首轮清单修复后，我在最终树上重新执行了两种配置的构建、内存预算、主机测试、自检、协议交叉校验、C→Python 线格式校验与逐文件检查。结果：**D1–D8 与 O3 全部关闭，无回归、无新缺陷。**

| 缺陷 | 修复内容 | 复核证据（最终树） |
| --- | --- | --- |
| D1 | `tools/remote-mic/remote_mic/protocol.py:202-208` 新增 `0x0003: "device or microphone unavailable"`；`tools/remote-mic/tests/test_protocol.py:127-130` 增加断言 | 交叉校验脚本输出 `MIC_OPEN_ERROR 0x0003 (device or microphone unavailable)`（首轮为 `unknown error 0x0003`）；101 项测试通过 |
| D2 | `docs/remote-mic.md:249-252` 改为「设备语音服务未初始化或麦克风设备不可用（`papermono_ble_mic_init()` 失败）；运行期 PDM 启动失败不发该事件，只在设备 UI 显示失败」 | 与 `src/papermono_ble_mic.c:374-386`（运行期失败只写 `last_error`）及 `:752-754`（init 失败）逐条对应 |
| D3 | `start_session()` 删除显式 `send_audio_sync()`，只保留 `send_frame()` 在 frame 0 前的那一条（源码注释已说明） | 脚本断言 `start_session()` 体内无 `send_audio_sync`、`send_frame()` 有周期同步；全文件仅 2 处调用点（周期 + 发送失败后重同步）；接收器仿真（§2.8）gaps=0、50 帧正确解码、旧固件双 seq=0 仍兼容 |
| D4 | `docs/remote-mic-protocol.md:62-65` 改为「有活动会话时回复 AUDIO_STOP；空闲关闭幂等、无回复」 | 与 `stop_session()` 的 `!was_streaming` 早退分支（`ble_mic.c:431-437`）一致 |
| D5 | `include/papermono/ble_mic.h:67-76` 改为「最多等待 1 秒；超时则强制关麦，worker 在下一次 read 收敛」 | 与 `papermono_ble_mic_abort()` 的 `K_MSEC(1000)` + 强制 `pdm_mic_stop()` 分支一致 |
| D6 | 新增 `session_dropped_start`（`ble_mic.c:101`），`start_session()` 记录（`:400`），停止日志打印差值（`:450`） | 源码检查；构建后 `irom0_0_seg` 881,808 → 881,824 B（+16 B 代码），内部 DRAM 不变 |
| D7 | 全部新增文件 `chmod 644` | `ls -l`：本报告与所有新增源码/文档/工具均为 `-rw-r--r--` |
| D8 | `docs/remote-mic-protocol.md:134-136` 新增「分片回退只保证字节流完整；MTU 23 时一帧 6 条通知未必发得完；丢帧后最长约 750 ms 由 AUDIO_SYNC 恢复；实机验收固定 MTU 123」 | 与 `notify_audio()` 的 `mtu-3` 分片及 `MIC_SYNC_INTERVAL_FRAMES=50`（750 ms）一致 |
| O3 | `docs/remote-mic.md` §8 新增「已配对电脑可无手势 `MIC_OPEN`，最长 60 秒，屏幕显示正在传音；勿让不可信主机保持配对」 | 与 `write_tx()` 的 MIC_OPEN 分支、`MIC_SESSION_LIMIT_MS=60000` 一致；行为未改，仅补齐文档 |

复核轮重跑结果（与 §2 一致）：

- feature-on：exit 0、0 warning、内部 DRAM **334,976 B（83.93%）**、FLASH 4,551,220 B、irom 881,824 B；
- feature-off：exit 0、0 warning、内部 DRAM **329,280 B（82.50%）**；`nm`/`strings` 仍无任何语音符号/字符串（仅剩 `CONFIG_PAPERMONO_BLE_MIC_LOG_LEVEL` 与 `..._LOG_LEVEL_DEFAULT` 两个 Kconfig 常量）；
- `sh tests/host/run.sh` exit 0（`test_adpcm: PASS`）；unit suite **101 项 0 失败**（D1 的断言写在既有 `test_mic_open_error` 内，用例总数不变）；`--selftest` 通过；`py_compile` 通过；
- 协议交叉校验全部通过，caps/UUID/帧几何常量未变；
- C 编码器 → Python 解码仍与 Python 往返**逐样本相同**（220 Hz 39.14 dB、1 kHz 32.95 dB，最终状态 `-32767/11`）。

仍未关闭的项都不是本次修复目标，属实机风险，见 §7：O1（4,265 B DRAM 余量）、O2（每次按住/松开最多 4 次整页重绘）、O4（空口丢帧最长 750 ms 恢复，文档已按 D8 补充）、O5（2 KB 栈）、O6（采集线程抢占）、O7（刷新期触摸噪声）、O8（主机会话被手势截断）、O9（`MIC_EXTEND` 被忽略）。这些只能在真机上按 §8 验证。

## 11. 总结

- 构建、内存预算、主机测试、接收器自检、固件↔接收器协议与 ADPCM 线格式一致性：**全部通过**。
- 代码审查未发现死锁、bt_conn 释放后使用、卡死状态机或重绘死循环；`bt_conn` 引用、CCC、abort/休眠、断连、会话上限的路径都有明确收敛点。
- 首轮发现 8 处轻微缺陷（D1–D8）与 1 处隐私文档缺口（O3），**已全部修复并在 §10 逐条复核通过**，无回归、无新缺陷；当前没有未关闭的代码或文档缺陷。
- 剩余工作只有实机验收（§8 的 11 个步骤）与 §7 的实机风险观察；**本功能当前状态应表述为「源码、主机测试与固件构建完成，实机未验收」**，与 `docs/remote-mic.md`、`README.md`、`docs/delivery-status.md` 的措辞一致；不得据本报告声称语音链路可用。
