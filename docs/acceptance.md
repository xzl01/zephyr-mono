# 个人信息终端验收记录

更新日期：2026-09-30

本文严格区分软件实现、主机测试、固件构建、控制器日志和需要人机/附件参与的
真实硬件验收。未通过的实机门槛不能用构建成功替代。

## 当前已刷入候选状态

- 2026-09-30 定位并修复系统工作队列 1 KiB 栈溢出，默认镜像已刷入且写入 hash
  校验通过。工作队列现为 4 KiB，并启用栈哨兵、水位填充和最小栈编译断言。
- 响应优化候选的源码快照通过二十三组主机测试及默认、partial、selftest、
  selftest + touch power 四种配置构建；默认内部 DRAM 为
  324,608 / 399,108 bytes（81.33%）。
- 后续电源键修复快照通过二十四组主机测试及默认、partial、selftest、
  selftest + power-button HIL 构建。保留触摸 EXT0，并补上电源芯片 IRQ 与
  翻页键 EXT1 唤醒、RTC 上拉和 PMIC 按键中断配置。
- 专用测试固件已实机捕获单次触摸唤醒及单次电源键唤醒：后者报告
  `EXT1 wake GPIO mask 0x2 (pmic_irq)`、`wake source 8`、4314 ms 全刷和
  `Power HIL wake verified`；用户确认画面刷新。测试后已刷回日常固件。
- 最新日常镜像通过 USB-JTAG 软件复位运行到首页，显示基线就绪，全刷 2 次、
  局刷 13 次，最近耗时 4315/737 ms；随后退出调试器。恢复过程中 USB/WDT
  复位曾发生内核初始化前的双异常，独立冷启动与重复 USB 复位仍未闭环，
  不能把下面较早候选的独立启动结果直接外推到最新镜像。
- 此前恢复镜像通过 USB-JTAG 捕获到真实触摸、应用切换、左边缘 BACK 和返回
  启动器。响应优化的中间镜像捕获到真实触摸、零排队等待和约 1 秒的页面切换；
  最终候选独立 USB 复位完成启动、4313 ms 全刷和 737 ms 局刷，最终边缘手势
  与实际手感尚待物理交互确认。
- ~~本轮 TF 初始化出现 `Card never left busy state`（-134），内容读写仍未恢复~~
  **2026-10-20 已解决**：根因是板级 pinctrl 把 SD 的 CLK/CMD 接反，卡收不到
  时钟。修正后实机完成卡识别、FAT 挂载、写入与回读校验（见
  [TF 卡打通记录](tf-card-20261020.md)）。内容中心 HTTP 上传的端到端验证仍待
  完成；未完成断电冷启动、50 次休眠唤醒、真实 BLE 配对和长时间阅读验收。
- 已刷入镜像 SHA-256：
  `ab02104eb70da6b3e1946ac0bbbccf2724fab11d800b5df251435b4fde5021eb`。
  该短按休眠实验镜像已捕获真实单击入队及休眠请求；追加启动防误触的
  `4e62692acbd74d00eb5a12d11223c5fb33779e78e6531f47ee7343f87b87ca75`
  已通过 25 组主机测试和四配置构建，尚未刷入，三段电源键循环未验收。
  当前镜像的电源键修复和单次唤醒证据见
  [电源键唤醒记录](button-wake-20260930.md)。
  触摸优先级、32 ms 稳定窗口与边缘返回改动见
  [响应优化记录](interaction-response-20260930.md)；栈溢出根因、栈水位与调试注意
  事项见 [启动恢复记录](boot-recovery-20260930.md)。

## 历史发布基线（2026-09-04，非当前运行状态）

- 二十二组主机测试、默认/partial/selftest 构建和写入 hash 校验通过。2026-09-04
  发布前重新构建默认配置：`zephyr.bin` 为 2,330,852 bytes，内部 DRAM 为
  321,480 / 399,108 bytes（80.55%，低于 85% 门槛），SHA-256 为
  `d84d07cb524c11b0a381cdb76d0fc2bfd3c79d5f7696e439b511ab09e2979999`；
  这次重构建没有重新刷机，不能据此改变下述实机状态。
- V2 镜像写入后仍保留旧屏。单核 USB-JTAG 现场捕获到
  `_DoubleExceptionVector`，`EXCCAUSE=28`、`EXCVADDR=0x2c` 且栈指针无效；
  因此当前 HEAD 尚未通过应用启动、SSD1677 刷新或触控验收。
- 下表和后续附录保留各子系统在历史候选镜像上的最强证据，除非明确写出当前
  SHA-256，否则不得视为当前 HEAD 的回归结果。

## 里程碑状态

| 里程碑 | 软件状态 | 历史最强实机证据与当前边界 |
| --- | --- | --- |
| M1 系统底座 | 路由、调度、Settings/NVS、NTP、确认页和深睡路径完成；默认 10 分钟空闲已实机进入深睡；ESP timer 与单次触摸唤醒、页面恢复和 SSD1677 全刷均已闭环 | 触摸唤醒已取得 `wake source 1`、坐标、显示恢复及防重复动作日志；50 次耐久仍未执行 |
| M2 阅读器 | TXT/GBK、EPUB、分页、目录、书签、进度、安全限制完成 | 未插 TF；真实读写、热移除、长时间翻页残影待测 |
| M3 Wi-Fi 内容中心 | SoftAP、网页、会话接力、API、认证、事务上传和 AP 接口自检完成 | Mac 已通过发现热点、DHCP、公开状态页、首页和未授权拒绝；配对后的 TF 上传/删除及手机浏览器仍待测 |
| M4 BLE HID | 复合 HIDS、配对、bond、三种模式完成 | 控制器和 GATT 初始化通过；真实 macOS 配对/按键/鼠标待测 |
| M5 日历/待办/便签 | ICS、重复规则、JSON、Markdown、overlay 完成 | 依赖 TF 或同步导入后的屏幕交互验证 |
| M6 信息卡片 | Provider 模型、缓存、离线状态完成 | 空槽位 UI 已构建；具体 Provider 按既定边界未绑定 |
| M7 扩展 | BLE 小数据同步、硬件工具入口完成 | 同步协议需真实客户端验收；NFC/LoRa 等维持独立 HIL 边界 |

## 当前已刷入迭代

2026-08-31 的当前工作树修复了应用列表、通用页面和确认页消费左边缘 `BACK`
事件的问题，并把最短有效触摸从 10 ms 调整到 5 ms，以覆盖 8 ms 轮询周期内的
单帧短触摸。设置页和阅读菜单的前光调整现已对 PWM/NVS 失败做回滚，恢复默认
设置和失败的休眠尝试也会立即恢复实体灯光一致性；轻触或右滑增档、左滑减档、
长按关闭。新增 RGB 状态调度器统一合并启动、TF、Wi-Fi、BLE、传输和休眠状态。

该历史版本二十组主机测试全部通过，默认固件构建成功：`zephyr.bin` 为
2,208,564 bytes，内部 DRAM 为 320,704 / 399,108 bytes（80.36%，低于 85%
门槛），SHA-256 为
`ee9e818f227626e3a43c083ae579ad50a621b8088610f5f6923d70363936ba91`。
该镜像已通过 ESP32-S3 原生 USB Serial/JTAG 写入，`esptool` 报告
`Hash of data verified`。受控应用复位后的 USB 日志确认 FT6336 以 8 ms 轮询启动、
SSD1677 全刷在 4313 ms 完成、LVGL 页面完成渲染，随后首次局刷在 738 ms 完成；
SoftAP、BLE HID 和 BLE 小数据同步也已启动，状态调度依次记录 `booting` 和
`connecting`。边缘返回、五档前光的实际观感、各状态灯实体颜色以及深睡熄灯/
失败恢复仍需在 C153 上逐项交互重放。

同一工作树的 `driver_selftest.conf` 最终构建为 2,186,836 bytes，内部 DRAM
304,000 bytes（76.17%）；`driver_selftest.conf;power_touch_hil.conf` 组合构建为
2,187,796 bytes，内部 DRAM 308,264 bytes（77.24%）。后者最终 `.config` 已确认
`DRIVER_SELFTEST=y`、`POWER_HIL_DEEP_SLEEP_TOUCH=y`、`POWEROFF=y`，避免把仅有
构建退出码、实际未选中 HIL action 的镜像误记为触摸唤醒 profile。

## 2026-10-20 启动与 TF 证据补充

- TF 卡打通：板级 pinctrl 的 SD CLK/CMD 接反已修正，实机完成卡识别、FAT 挂载、
  写入、回读与内容导入事务验证。详见 [TF 卡打通记录](tf-card-20261020.md)。
- **2026-10-21 更正**：撤回“132 份完整启动日志、异常 0 次”的统计；此前混入空采集等，
  串口中未出现异常文本也不能排除早期故障。本轮 JTAG 实际读到 `_DoubleExceptionVector`。
  RTC watchdog + JTAG 组合恢复在正常镜像上连续 3/3，最终写入后自动恢复也通过；
  具体日志、镜像 SHA、复位范围与未修复问题见 [USB 恢复实测](<docs/usb-reset-20261021.md>)。
- 启动日志中的一次 `SoftAP disable` 加 500 ms 重试属于 APSTA→AP-only 的受控
  切换（见 [内容与同步接口](content-api.md)），不是异常重启。
- BLE 缓冲对齐：`CONFIG_BT_BUF_ACL_TX_COUNT` 由默认 10 调到 12，与 ESP32-S3
  控制器的 ACL 包数一致，消除了 `Num of Controller's ACL packets != ACL
  bt_conn_tx contexts (12 != 10)` 警告，避免 HID 高负载时 TX 上下文不足。
- 历史默认镜像 Zephyr `wrn`/`err` 计数曾为 0，内部 DRAM 82.96%；
  ROM 的 `SHA-256 comparison failed` 仍存在，不能据此称启动全程零警告。
- 主机测试仍为 25 组全通过；默认固件与 `driver_selftest.conf` 构建均无警告。
- 约束：本机 macOS 的 Wi-Fi 主动扫描受限，无法自动连上设备 SoftAP，内容中心
  HTTP 上传的端到端验证仍待用手机或另一台电脑完成。

## 2026-10-21 验收补充

- **50 次深睡唤醒耐久 PASS**：`driver_selftest.conf;power_timer_hil.conf` +
  `CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT=50`，实机约 35 分钟，
  末行 `Power HIL endurance PASS: 50/50 wake/display cycles verified`。
  过程出现少量 `m5ioe1: I2C read 0x00 failed (-14); recovering bus` 告警，
  总线恢复后继续，未导致失败。
- **ROM SHA-256 警告已消除**：构建自动追加镜像摘要（见
  [镜像摘要](<docs/firmware-image-digest.md>)），启动日志不再出现失败行。
- **冷复位早期双异常已修复**：见 [冷复位异常定位](<docs/boot-crash-20261021.md>)。
- **设备内格式化 UI 走查通过**（2026-10-21，无备用卡，使用非破坏性镜像）：
  系统状态页底部出现「格式化 TF 卡」→ 点开确认页 → 取消返回 → 再次进入并确认，
  日志 `HIL: simulated TF format (card contents preserved)` +
  `TF card formatted from the device UI`，随后状态回到已就绪。
  确认动作在验证镜像中被替换为模拟（`CONFIG_PAPERMONO_HIL_FAKE_UNFORMATTED`），
  真实 `fs_mkfs` 路径此前已由 `HIL microSD FAT check passed` 与实卡格式化验证。
- 内容中心 HTTP 端到端已通过（手机热点 2.4 GHz）：配对、会话、txt（25 B/10 KB）、
  EPUB（1020 B）上传均 201；空 body 411；未授权 401；`DELETE` 返回 200 且列表条目消失。
  同时修复两个缺陷：PSRAM 读缓冲导致 ≥1 KB 上传失败；DELETE 因精确资源遮蔽返回 405。

## 2026-10-20 深睡/唤醒耐久与烧录工具记录

- **深睡/唤醒耐久**：用 `driver_selftest.conf;power_timer_hil.conf` 加
  `CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT=12` 实机跑了 12 个自动周期，
  每个周期都验证了唤醒来源（`esp_timer`）、显示恢复和全刷完成：

  ```text
  Power HIL wake/display cycle 1/12 verified; rearming
  ...
  Power HIL wake/display cycle 11/12 verified; rearming
  Power HIL endurance PASS: 12/12 wake/display cycles verified
  ```

  12 次唤醒的 SSD1677 全刷耗时集中在 4312–4316 ms，无异常、无重启丢失。
  50 次触摸唤醒耐久仍需人工触摸，未执行。
- **烧录工具**：本轮出现 esptool 5.3.1 的 flasher stub 写入校验失败
  （`MD5 of file does not match data in flash`，两次结果一致）。改用 ROM loader
  后写入并校验通过：

  ```text
  esptool --port /dev/cu.usbmodem21201 --baud 460800 --before usb_reset \
    --after hard-reset --no-stub write-flash --flash-mode dio --flash-freq 80m \
    --flash-size 16MB 0x0 zephyr.bin
  ```

  结论：stub 路径校验失败时不要重试同一路径，改用 `--no-stub`。
- **内容导入清理噪声**：新增导入事务有两处无条件删除（`import_begin` 清理上次
  残留的 upload 文件、`commit` 清理旧备份），对「首次上传新文件」这两者都不存在，
  Zephyr 会以 error 级别打印 `fs: failed to unlink path (-2)`。已改为先 `fs_stat`
  再删除（`unlink_if_present`），残留清理行为不变。
- **首次上传成功路径补测**：主机测试此前只覆盖提交的各种失败分支。新增
  `commit-ok.txt` 用例，断言新文件首次提交返回 0、重命名后的文件尺寸正确，
  且全程没有尝试删除不存在的文件。主机测试 25/25 通过。

## 自动测试

`tests/host/run.sh` 当前二十五组全部通过：

- 文本编码：BOM、严格 UTF-8、GBK 识别/转换。
- 文本排版：简体中文行首/行尾禁则和结束标点边距字格。
- 应用路由：首次打开生命周期、切换/返回，以及目标应用打开失败后的回滚。
- 刷新意图：应用命令默认 AUTO，阅读翻页提交 FAST，初始化/异常恢复提交 FULL；
  驱动仍拥有残影阈值、局刷失败回退和维护全刷的最终决定权。
- 交互路由：实体键/触摸来源、兼容输入、命中区域、网格边界和长按筛选。
- 触摸手势：左右/上下滑、轻触、长按、左边缘返回和底边回首页；短按释放
  位移 59 px 仍为点击、60 px 起进入滑动，所有手势的区域命中均锚定首次接触点；
  8 ms 单帧触摸可识别，2 ms 噪声仍被拒绝。
- 前光策略：0/25/50/75/100% 到 0..255 驱动值映射、非档位归一到相邻档位、
  前后循环和非法百分比拒绝。
- RGB 状态策略：休眠、错误、警告、传输、配对、连接、配网、就绪和启动的
  优先级及关键颜色映射。
- C153 触摸坐标：官方竖屏原点/轴向保持、四角边界，以及越界控制器帧拒绝；
  不允许把异常坐标钳位到可点击控件上。
- 电源诊断：七类唤醒来源的稳定 API 名称和非法枚举安全回退。
- BLE HID 报告合约：媒体六个 bit、演示键码、标准 8-byte 键盘报告、4-byte
  相对鼠标报告、按键释放帧、Report ID/长度和非法鼠标按钮拒绝。
- 系统设置：带版本和内嵌 CRC32 的单条原子记录、损坏记录安全回退、旧版分键
  数据迁移、记录优先级、休眠/时区/Wi-Fi/重置写入失败回滚，以及阅读页恢复
  意图和前光五档的合法值、切换及保存失败回滚。Wi-Fi SSID 与密码始终作为
  同一事务提交。
- HTTP 元数据：百分号/base64url 中文长文件名、缓冲边界和严格 CRC32 头解析。
- 阅读分页：UTF-8 自动换行、前后页历史、GBK 边界和空文件。
- 阅读索引：跨越 32 页 RAM 历史回退、重启重用、分支截断、损坏/部分
  文件重建和阅读密度签名失效。
- 内容存储事务：单事务所有权、上传期间 TF 热拔、验证期间热拔、旧事务失效、
  超限写入/元数据、目录遍历 I/O 错误传播，以及注入 `fs_sync()` 失败后仍关闭
  文件并清理事务。
- 个人数据：ICS、日历重复实例、tasks JSON、Markdown、overlay JSON 解析和
  重新上传后的稳定 ID 状态合并。
- EPUB：container/OPF/spine/nav/XHTML 文字提取、EPUB2/3 分层目录与分页。
- 压缩包安全：路径穿越、深度、条目数、中央目录和解压总量限制。
- 字体覆盖：1-bit 编码、21,419 个实际字形、完整基础 CJK，以及界面源码中
  每一个非 ASCII 字符。
- 内容中心网页：无外网资源、八位 Base32 配对控件、会话持久化、一次性 ticket
  的 SoftAP 到局域网接力、fragment 立即清理、状态轮询、上传/配网/进度路径，
  以及可用时的 JavaScript 语法检查。

网页还通过本机 mock API 在 390 x 844 移动端视口完成浏览器检查：无横向滚动，
配对控件高度不小于 44 px，中文长文件名可以换行；换取会话后刷新页面显示
“已恢复本地会话”，并保持在管理界面。可选静态检查器缺少 HTML 解析模块而
退化到正则检查，未发现问题；该退化结果不替代上述浏览器证据。BLE GATT 仍
建议增加可自动运行的 host/integration harness，并保留真实客户端验收。

## 构建回归

以下配置在 Zephyr 4.4.1 上重新配置并构建成功，且检查了最终 `.config`：

- 默认个人信息终端：interactive UI、内容中心、BLE HID、信息卡片、BLE sync。
- `partial_demo.conf`：有限局刷 profile，交互 UI 关闭。
- `driver_selftest.conf`：驱动自检 profile。
- `driver_selftest.conf;power_touch_hil.conf`：触摸深睡 profile。
- `power_touch_endurance_hil.conf`：50 次触摸唤醒、页面/显示恢复计数 profile。

板级 DTS 显式启用了 ESP32-S3 硬件 TRNG；最终配置必须包含
`CONFIG_ENTROPY_ESP32_RNG=y`，不得回退到 Zephyr 测试随机源。

当前未刷入工作树的默认镜像资源快照：

```text
FLASH       2,330,852 / 16,776,960 bytes  (13.89%)
internal DRAM 321,480 / 399,108 bytes      (80.55%)
PSRAM       4,980,256 / 8,388,608 bytes    (59.37%)
```

内部 DRAM 的工程水位固定为 85%。每次最终链接后运行
`python3 scripts/check_memory_budget.py <build-dir>/zephyr/zephyr.map`；当前 80.55%
通过，但后续新增网络、BLE 或显示缓冲时必须继续检查，不能只等链接溢出。

本轮有序轻量 V2 直角九宫格默认镜像 SHA-256 为
`d84d07cb524c11b0a381cdb76d0fc2bfd3c79d5f7696e439b511ab09e2979999`。
默认、`partial_demo.conf` 与 `driver_selftest.conf` 三个 profile 均已构建；后两者
分别为 2,239,892 bytes 和 2,243,444 bytes，SHA-256 分别为
`afe23688ec593f02a078866b70054465533249958fe5b96a526026885aedef44` 和
`4c7ddd11fcb7b2b53a4a4354f88853ebfd6f5765795cc5743bda3108573cc243`，内部 DRAM
分别为 75.70% 和 76.36%，均通过 85% 工程水位。二十二组主机测试与 21,419
字形覆盖检查通过。480×800 纯 1-bit LVGL 离屏布局预览中，首页黑色像素占比为
6.07%，应用页为 8.78%；两者均低于普通页面 15% 的墨色目标。V2 将顶部状态和
电池值收拢到右侧基线，清理重复空态，并以真实应用数、书籍状态和信息卡片数替代
固定音量、百分比及涨跌等伪实时指标。首页阅读与进度共享完整点击区域，
底部三个入口共用基线；九宫格每格共用标题轨、实线分隔和四条内容基线。小号标签
使用白底描边，方阵不存在截断尾条，正负斜率折线均未被 LVGL 对象边界裁切。
该预览只验证布局镜像和视觉层级，不是 SSD1677 实机帧缓冲证据。2026-09-01 已将
上述默认镜像通过 `/dev/cu.usbmodemXXXX` 写入 ESP32-S3 rev 0.2；esptool 识别
8 MiB PSRAM，写入 2,330,852 bytes 后报告 `Hash of data verified`，并通过 RTS
硬复位。随后设备仍保留旧屏；使用 `--before no-reset --after no-reset` 的无控制线
探测可直接连接并报告 `Staying in bootloader`，确认初次硬复位把 ESP32-S3 留在 ROM
下载器。再按 USB-only 恢复流程执行 `--after watchdog-reset` 后，同一无复位探测已
无法同步并报告 `No serial data received`，证明设备已退出 ROM 下载器；USB CDC
仍可被动连接。过滤采集未取得应用启动日志并以 timeout 结束；随后单核 OpenOCD
复位并读取 CPU0，现场落在 `_DoubleExceptionVector`，异常原因为 28、访问地址为
`0x2c`，栈指针为 `0xffffff80`。因此当前 HEAD 已确认启动异常，尚未执行到可证明
SSD1677 显示 V2 首页或触摸可用的阶段。

本轮源码级界面审查已确认：启动器和触控共用 `x=18, y=112`、`148×210`、
3×3 零间距布局，11 个应用按 9+2 分为两页，第二页空格不触发应用；矩形半径被
统一限制为 0，只有真圆点可保留半径；首页六个入口也由渲染和事件共享同一组
坐标。58/96 px 数字子集均包含未校准状态所需的连字符，28 px 标题子集包含
“便签”。这些结论来自源码、最终链接和主机测试，不是实屏证据。当前默认镜像已经
刷入；直角像素、文字裁切、运行时 LVGL 堆、翻页残影和逐格触控仍待当前镜像的
现场验证。

2026-08-31 触摸可靠性优化后最近一次已刷入的默认镜像 SHA-256 为
`16084c2b027c813398bd1875509d1210635a2bf383bcd7dbad857e1838582575`，大小
2,207,620 bytes。它已通过 ESP32-S3 原生 USB Serial/JTAG 写入，`esptool` 报告
`Hash of data verified`。本轮修复包括：

- 把 FT6336 从 20 ms 的全局 system workqueue 工作项改为独立 4 KiB、8 ms
  轮询线程，避免短触摸被工作项合并，并隔离同步 input 回调的栈使用；
- 修复显示刷新结束与触摸回调争用时丢失完整手势的竞态；刷新期间只保留第一个
  完整手势，后续手势合并，刷新结束不再清空或重复投递已经入队的系统事件；
- 增加按下、释放和最大轮询间隔计数；连续 I2C 失败转离线后仍会在 100 ms
  内释放陈旧触点；
- 修复 USB 日志采集器曾把普通复位实现成 ROM 下载模式入口的问题。默认采集
  全程不改变 DTR/RTS，可选应用复位也始终保持 DTR 释放；README 不再建议使用
  可能改变控制线的普通串口监视器。

直接前序候选 `5ca29351d41131b5a47620459196baa8182da644046a085db11de1ffe659f980`
已在同一台实机捕获完整链路：FT6336 在 `(258,409)` 产生按下/释放，UI 将 26 ms
轻触以 `queue=0` 入队并从首页处理，SSD1677 在 738 ms 内完成局刷；随后刷新期间的
滑动记录为 `retained during refresh`，刷新完成后由 UI 继续处理。连续多个触摸、
页面路由和第 3 至第 6 次局刷均无 I2C 错误或 BUSY 卡死。当前最终镜像只在此基础上
补了“后续手势合并”“避免系统事件重复入队”和“离线陈旧释放”三个边界修正；已完成
构建、写入 hash 校验和十八组主机测试，但最终写入后的再次实体触摸没有在本次
240 秒采集窗口内发生，因此最终镜像的实屏页面切换仍保留为一次人工复验，不用
前序候选的结果替代。

以下是 2026-08-28 I2C 唤醒时序修复后的历史候选记录。其 SHA-256 为
`8b749ec29ef2f1f3516dabbff88970174841b42313fef0c987ba4f3285b268a5`，大小
2,204,564 bytes。触摸 HIL 结束后已通过原生 USB/JTAG 刷回设备，`esptool`
报告 `Hash of data verified`。受控复位随后命中默认镜像的 `main()`，且 USB
日志闭环了设置加载、空 TF 处理、4316 ms SSD1677 全刷、LVGL 页面、内容中心、
BLE HID、BLE 小数据同步和 738 ms 局刷。截至 2026-08-28，设备运行的是该默认
候选而不是 HIL profile；它已被上文 2026-08-31 镜像替代。

受控复位时 ESP32-S3 ROM 曾输出一行 `SHA-256 comparison failed`，但镜像的
`esptool image-info` checksum 有效、写入后 hash 校验成功，且上述应用路径全部运行。
Espressif 已记录过 ESP32-S3 ROM 对未编程 digest（全 `0xff`）输出同类提示后继续
启动的问题：[IDFGH-11182](https://github.com/espressif/esp-idf/issues/12349)。本轮没有捕获
该 ROM 行的 calculated/expected 全文，因此不把它当作已签名或 secure-boot 完整性证据；
当前可用性结论依据写入 hash、启动到 `main()` 和完整运行日志。

相较上一版 `0c8ce6dd...`，当前刷入镜像增加了触摸手感和深睡唤醒修正：短按允许
59 px 的释放漂移、
60 px 起分类为滑动，长按仍保持 30 px 静止容差；轻触和长按的 UI 命中使用首次
接触点，滑动保留末端坐标；GPIO4 的 RTC IO 上拉也已补齐。它已通过全部十八组
主机测试、默认构建和 50 次耐久 profile 构建。

当前刷入镜像还把本次启动最早读取到的唤醒来源缓存并显示到硬件工具页及公开状态
API，方便 USB-only 实机验收。它同时让应用路由传递 AUTO/FAST/FULL 刷新意图，
修正强制全刷请求失败时错误被后续操作覆盖的问题，并把“电源”补进应用列表的第十
个空位，使人工验收路径与界面文案一致。配网 SoftAP 不再永久阻止空闲休眠；只有
流式上传或配网请求持有事务时才延迟休眠。上传和备份文件改用同目录隐藏事务目录，
完整支持 255-byte UTF-8 原文件名并兼容清理旧版残留。18 组主机测试与默认/50 次
耐久构建均已通过；“硬件工具”首卡也已成为系统状态页的实际入口，不再保留孤立
页面。50 次 profile 只完成构建，尚未刷入执行。

本轮还重新构建了 partial、driver selftest、单次触摸唤醒和 50 次触摸耐久
profile；其内部 DRAM 使用率分别为 76.03%、76.70%、77.76% 和 77.76%，均通过
85% 工程水位。构建通过不替代对应 HIL 执行结果。

干净工作区还必须完成 `west update`、运行
`west blobs fetch hal_espressif` 获取 Espressif 二进制 blob，并构建默认目标。
sysbuild 必须能配置并链接 MCUboot 子镜像；专用的
`sysbuild/mcuboot.overlay` 会从 bootloader 中移除仅供应用使用的 I2C GPIO
扩展器。

本机当前复用的 west workspace 仍以另一个工程为 manifest，`west list` 中没有
`mcuboot`；本轮 sysbuild 尝试因此在配置阶段把 MCUboot source 解析为不存在的
`/boot/zephyr` 并停止，尚未进入本项目编译。这个环境问题不影响上述普通固件
构建，但 sysbuild 回归仍需在按 README 新建并完成 `west update` 的 workspace
中重跑，不能标记为通过。

## 本次真实硬件证据

目标通过 ESP32-S3 原生 USB Serial/JTAG 识别为 C153：ESP32-S3 rev 0.2、
16 MiB flash、8 MiB PSRAM；固件写入后 hash 校验通过。

### 2026-08-28 ESP timer 深睡唤醒闭环

`driver_selftest.conf;power_timer_hil.conf` 镜像
`e02a9ade04692e10f27b34643a2717e92985dfc1acd1976438f3a69fa44554ae`
通过 USB 写入 2,186,948 bytes，`esptool` 校验 hash 成功。设备完成冷启动、自检、
深睡、10 秒 timer 唤醒和唤醒后页面全刷，最终日志为：

```text
PaperMono resumed from wake source 2
PaperMono SSD1677 transport ready: 480x800, SPI 20000000 Hz
SSD1677 full refresh 1 complete in 4316 ms; baseline ready
PaperMono LVGL page rendered
Power HIL wake path: display restored; skipping unrelated driver self-test
Power HIL wake verified; the power action will not repeat
```

此前唤醒后曾出现 M5IOE1/M5PM1 寄存器读到 `-14`，使 PMIC GPIO、LoRa 和
SSD1677 子设备因父设备未 ready 而失效。根因是这两个器件由 PY32 固件模拟 I2C
寄存器，官方访问要求写寄存器指针后等待约 500 us；常规
`i2c_write_read_dt()` 把两个阶段无间隔合并。当前驱动改为分阶段读、每阶段保护
间隔、有限三次总线恢复重试，并在关闭 I2C idle-sleep 后等待 2 ms。timer HIL
证明这条恢复路径已在真实深睡唤醒中生效。

当前触摸 HIL 镜像 SHA-256 为
`b5603cb522d61fbfbbdd4eb35d7a6b1ca75e2cb5657a568496465b9c635e40d7`，大小
2,186,884 bytes，已经写入并完成单次触摸唤醒闭环。受控冷启动先完成
驱动自检和四灰阶全刷，USB 在深睡时断开；退出 OpenOCD/GDB 后触摸实体屏幕，
设备重新枚举并捕获到：

```text
PaperMono resumed from wake source 1
HIL touch pressed at x=363 y=742
SSD1677 full refresh 1 complete in 4318 ms; baseline ready
HIL touch pressed at x=244 y=642
PaperMono LVGL page rendered
Power HIL wake path: display restored; skipping unrelated driver self-test
Power HIL wake verified; the power action will not repeat
```

唤醒时 M5IOE1 首次读取返回 `-14`，有限总线恢复随后成功，M5PM1、SSD1677 与
页面恢复全部继续完成。该镜像
在 power HIL 中只读 M5PM1 可验证的充电在线状态，跳过本机不应答的可选 IP2315
`0x75` 寄存器探测，避免无关外设超时遮蔽触摸唤醒结果。

完整 1-bit 中文字库镜像已通过同一 USB 端口写入，写入长度为 2,254,196 bytes，
esptool 返回 `Hash of data verified`，硬复位后设备重新枚举。镜像 SHA-256 为
`4bf582f208b5d10f27536f883373d48633e71960ca390cd2cccd9820b7bcea8e`；物理屏幕
是否已无缺字方框仍需人工目视确认，不能由静态覆盖测试代替。

完整冷启动日志已确认：

```text
M5IOE1 uid=0x0d7d revision=0x57 ... ready
M5PM1 id=0x50 model=0x20 hw=0x05 sw=0x54 ready
PaperMono SSD1677 transport ready: 480x800, SPI 20000000 Hz
SSD1677 full refresh 1 complete in 4319-4320 ms; baseline ready
PaperMono LVGL page rendered
SSD1677 partial refresh 1 (1/10) complete in 739 ms
BLE HID ready, bonded=0
Authenticated BLE small-data sync ready
```

本次全刷和随后的局刷均完成，BUSY 为 0，没有冷启动卡死。USB 线状态复位后
调试器曾读到旧的 `_DoubleExceptionVector` 停驻状态；通过 JTAG software reset
复核后 CPU 稳定运行在 `arch_cpu_idle()`，证明该停驻状态不是当前应用崩溃。

系统设置已从旧版多个独立 NVS 键迁移为固定 144-byte 的版本化原子记录；记录
自身包含 CRC32，因此既能拒绝损坏或截断数据，又不会启用会改变旧记录解释方式的
全局 NVS data CRC。所有 setter 先写完整候选记录，只有持久化成功才提交 RAM
状态，相同值不会产生重复擦写。写入当前候选后又执行了一次独立 software reset，
两次启动均由 JTAG 读到：

```text
settings initialized: true
atomic config record loaded: true
idle sleep: 10 min
timezone/use-24h: Asia/Shanghai / true
active app: POWER
Wi-Fi configured: false
FT6336 online: true, I2C read errors: 0
BLE HID initialized/advertising: true/true, last error: 0
HTTP listener fd: 2
```

第二次启动还观察到 FT6336 连续帧计数增长至 5,202，显示全刷计数为 1。这证明
新记录跨重启被实际读取，同时系统服务重新启动成功；它不替代屏幕触点命中和
深睡断电路径的人工验收。

当前触摸恢复候选改用 C153 专用 FT6336 驱动，并通过同一原生 USB/JTAG 读到：

```text
FT6336 online: 1
continuous frames: 11165
I2C read errors: 0
background recoveries: 0
HTTP listener fd: 2
```

这证明触摸供电、复位、0x38 I2C 事务和 10 ms 后台采样已经运行；本轮
2026-08-28 实体短按又捕获到坐标边沿，EXT0 深睡唤醒也已由 `wake source 1`
和显示恢复日志闭环。

随后对照 M5Stack UserDemo 和 CrossMux/FreeInk 的 Paper Mono 变换重新审计坐标：
官方 UserDemo 在 rotation 0 的 480x800 界面直接用 FT6336 raw `x/y` 命中控件；
CrossMux 先换到 800x480 面板坐标再回到竖屏逻辑坐标，两段变换合成后也仍是原始
`x/y`。旧 DTS 额外设置 `inverted-y`，会把上方点击送到下方，且 Zephyr 的
反转公式可能把 raw Y=0 变成越界的 800。当前候选已移除该反转和只为反转服务的
screen-width/height 属性，并在驱动中按显示节点的 480x800 尺寸拒绝越界帧。
刷入并启动后 JTAG 实测：

```text
touch common transform: width=0 height=0 invert-x=false invert-y=false swap=false
touch IRQ: pin=4 dt_flags=81 (ACTIVE_LOW | PULL_UP | INT_WAKEUP)
FT6336 online: 1
FT6336 frames: 6081
FT6336 I2C errors: 0
FT6336 invalid frames: 0
FT6336 recoveries: 0
active app: HOME
last power error: 0
```

此前写入 2026-08-27 默认候选 `7433a92c...` 后再次可靠启动并读取到：

```text
touch IRQ: pin=4 dt_flags=81 (ACTIVE_LOW | PULL_UP | INT_WAKEUP)
FT6336 online: 1
FT6336 frames: 1687
FT6336 I2C errors: 0
FT6336 invalid frames: 0
FT6336 recoveries: 0
active app: HOME
last power error: 0
cached wake cause: cold_boot
content server busy flags: 0
SoftAP enabled/verified: true/true
```

这里的 `cold_boot` 是该次写入后的预期基线。2026-08-28 的专用触摸 HIL
已进一步读到 `wake source 1`（`touch`）、重建全刷基线并恢复 LVGL 页面，
单次深睡唤醒已闭环。

该 2026-08-27 候选 `7433a92c...` 还完成了一次不依赖人工点击的默认空闲策略实测。启动后 5 分钟
例行更新完成第 2 次局刷，耗时 738 ms，且没有重置用户活动计时；到 10:05 固件
启动休眠提示页全刷，随后 USB CDC 返回 `Device not configured` 并从主机断开。
这证明配网 SoftAP 空闲时不再永久阻止自动休眠，也证明系统真正进入深睡而不是仅
停留在“设备休眠中”页面。后续专用触摸 HIL 已完成重新枚举、
`wake_cause=touch` 和页面恢复三项证据。

这已消除一个足以解释“触摸像没反应”的确定性软件错误。在上一版
`0c8ce6dd...` 上，用户已于实体屏幕点击电源页顶部“立即休眠”卡片并看到对应
确认页，闭环了 FT6336 读帧、坐标变换、手势分类、命中区域和页面路由；这些路径
在当前版保持不变，但新增的释放漂移和 RTC 上拉仍需重新实机验证，四角命中也待测。
硬件工具页和公开状态 API 同时显示最近坐标及异常帧计数，后续不再依赖串口才能
区分控制器读帧和页面路由问题。

最终候选还通过 JTAG 从设备 PSRAM 导出了 48,000-byte SSD1677 帧缓冲并按
`480 x 800`、纵向字节、MSB-first 规则还原。还原画面包含电源页全部三张卡片的
标题、说明、英文状态和页脚，不再只有标题；顶部 `Wi-Fi AP  BLE ADV` 也已改用
内置 1-bit 字体，避免抗锯齿小字号落到单色缓冲后只剩断裂笔画。导出同时读到：

```text
SSD1677 full refreshes: 1
SSD1677 partial refreshes: 1
FT6336 online: 1
FT6336 frames: 3725
FT6336 I2C read errors: 0
```

这证明当前 LVGL 帧内容、显示提交和触摸后台采样均在最终刷入镜像中运行；其后
真实玻璃上的顶部卡片命中已由用户确认，其他位置与连续手势仍须逐项实机验证。

TF 检测脚实测为 `inserted=0`。固件已改为先读卡槽检测脚：空槽直接呈现
“未检测到 TF 卡”，不再把无卡误报成 `Card never left busy state`。

较早候选的 SoftAP 在固定 2.4 GHz 信道 6 后记录：

```text
SoftAP enable complete (status=0, channel=6)
```

但同一台 Mac 的 CoreWLAN 扫描和指定 SSID 连接当时仍未找到网络。该结论只适用
于旧镜像，已由下述当前候选的外部发现、关联和数据链路证据替换。

2026-08-27 写入当前候选后，原生 USB 日志进一步确认：

```text
SSD1677 full refresh 1 complete in 4320 ms; baseline ready
PaperMono LVGL page rendered
BLE HID ready, bonded=0
Authenticated BLE small-data sync ready
SoftAP enable complete (status=0, channel=6)
Content center ready: PaperMono-XXXX / <redacted>, pair code <redacted>
SoftAP interface verified: SSID PaperMono-XXXX, channel 6
SSD1677 partial refresh 1 (1/10) complete in 739 ms
```

这里隐去运行时密码和一次性配对码；它们只应从设备屏幕读取。

随后写入的该历史候选修复了两处实机问题：Zephyr sockets service 和 HTTP server
各自需要一个 eventfd，而旧配置只有一个，JTAG 读到 HTTP listener fd 为 `-1`；
当前配置提供两个 eventfd，listener fd 实测为有效值 `2`。DHCP 也改为在每次
`AP_ENABLE_RESULT` 成功后启动，因此 ESP32-S3 为恢复可见 beacon 所需的
APSTA 到 AP-only 受控重启，不会再留下已被 AP_STOP 销毁的 DHCP 实例。

当前候选在同一台 Mac 上取得以下端到端证据：

```text
SoftAP: 802.11b/g/n, channel 6 (20 MHz), WPA2 Personal
DHCP address: 192.168.4.11
DHCP server/router: 192.168.4.1
GET /api/v1/status: HTTP 200
GET /: HTTP 200, 13425 bytes
GET /api/v1/content without session: HTTP 401
Wi-Fi off/on and rejoin: DHCP lease restored, status API HTTP 200
```

因此 Mac 端热点发现、关联、DHCP、HTTP 首页、公开状态和未授权拒绝已通过。
屏幕内容和触摸交互仍须人工确认；认证后上传/删除还需要插入可写 TF 卡。

最终候选把 UI 动作到 HIDS payload 的编码拆为独立、无 Zephyr 依赖的合约层，
主机测试逐项固定三个 Report ID、媒体 bit、演示键码、payload 长度和释放帧；
三个可读 Input Report characteristic 的空闲值也分别固定为 1、8、4 bytes，与
Report Map 的声明保持一致。运行时不缓存跨连接的 CCC 状态，而是对当前连接实时
检查订阅；按下与释放串行发送，并对临时 ATT 缓冲不足重试，避免主机端出现粘键或
鼠标键卡住。HID Control Point 只接受 suspend/exit-suspend，连接安全状态和主机
挂起状态也会反映到遥控器页面。
刷入后 JTAG 读到 `hid_state.initialized=true`、`advertising=true`、
`last_error=0`；这证明控制器/GATT 启动与报告编码合约，但不能替代真实 macOS
订阅 Report characteristic 后的完整操作验收。

为 50 次触摸唤醒门槛新增了独立耐久 profile。它只在预期触摸唤醒且显示完整
刷新成功后增加保存在 RTC RAM 中的计数，未达到 50 次会自动再次入睡；最终
必须出现 `Power HIL endurance PASS: 50/50 wake/display cycles verified`，且
逐次观察物理页面正常，才能通过 M1 耐久验收。

本次已写入的候选还包含以下恢复改动：板级 FT6336 驱动始终为 GPIO4 配置运行态
上拉并只在首次接触/释放时报告按键边沿；最新刷入候选进一步在 DTS 和驱动配置中
显式加入 `GPIO_INT_WAKEUP` 以配置 RTC IO 上拉，因为 ESP32-S3 的两组上拉彼此独立；
触摸唤醒预检只读检查唤醒线，只有
确认空闲后才绘制休眠页；任何休眠失败都会恢复当前页面并保持 UI 循环；显示
刷新期间完成的首个触摸手势会暂存，系统状态事件会合并后重放；保存 Wi-Fi 失效时恢复
SoftAP，并以有上限的退避策略重试；阅读器区分书单和阅读页恢复意图，内容导入
完成后可在书单自动重扫；HTTP 上传和 Wi-Fi 配置请求拥有独立事务所有权，拒绝
并发串写。当前候选进一步核对 SoftAP 接口的模式、SSID 和信道，连续失败后
重启接口；网页会话可在刷新后恢复，并在 STA 获得地址后利用 20 秒 AP 接力窗口
跳转到局域网；存储层会在上传或验证期间 TF 热拔时使旧事务失效，且同步失败
仍关闭文件。当前默认镜像已完成写入和启动日志检查；单次触摸深睡唤醒已闭环，
其余四角/连续手势、认证浏览器与插卡路径仍需按下述边界逐项实机验证。

## 必须继续完成的实机门槛

1. 插入可测试的 FAT32 TF 卡，完成真实创建目录、中文长文件名、TXT/EPUB
   打开、写入、覆盖、拔出和重新插入。
2. 使用可写 TF 卡完成 Mac 和手机浏览器配对、上传、删除、空间不足/中断恢复
   和上传期间 UI 稳定性；Mac 端发现、DHCP、公开页面和未授权拒绝已通过。
3. 在真实 macOS 上完成 BLE 首次配对、passkey、bond 恢复、断线重连、媒体、
   演示和鼠标全部 Report。
4. 执行 50 次触摸深睡/唤醒，逐次确认 USB 重枚举、页面恢复、全刷基线重建、
   SSD1677 不出现 BUSY 卡死。
5. 连续阅读翻页超过局刷阈值，确认维护全刷和残影可接受。
6. Wi-Fi、BLE 和显示局刷并行时观察内存、重启、UI 锁死和响应延迟。

IP2315 地址 `0x75` 不应答继续作为独立硬件问题；充电信息使用 M5PM1 可验证
数据，不阻塞上述个人终端主线。
