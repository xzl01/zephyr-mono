# 电子纸刷新算法研究与改造方案

> 2026-10-22 · 目标设备 M5Stack PaperMono C153（ESP32-S3 + 480×800 单色 SSD1677）
> 触发问题：**阅读翻页时，每翻若干页会出现一次黑/白整屏闪**；并假设「理论上可以用
> 快速刷新/局部刷新避免闪屏」。
>
> 研究阶段未修改源码；2026-10-22 起按决策实施了 P0-lite/P2/P4（P0 未采纳、P1
> 因收益/风险比低暂缓、P3 复核后因会突破厂商连续局刷上限而放弃）。**P2 默认启用；
> P0-lite/P4 是 Kconfig 开关且默认关闭**，需 HIL 后再启用。实现状态见 §10。结论基于当前工作树（含尚未提交的
> 遥控器语音改动）逐行核对源码、厂商代码与数据手册，以及本机原生 LVGL 预览的像素
> 测量。研究底稿（本地、未入库）：`tmp/refresh-research/vendor-protocol.md`、
> `firmware-trace.md`、`pixel-diff.md`。
>
> **决策记录（2026-10-22）**：保持 `CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT=10`
> （厂商建议值），**不采用** P0 的「放宽到 20 次」。阅读中每约 10 页一次的全刷作为
> 面板寿命代价接受；剩余可做项为 P1（唤醒无闪）、P2（刷新去重）、P4（缩短全刷）
> 与可选的 P0-lite（需 HIL 验证）。

## 0. 结论摘要（TL;DR）

1. **现象归因是确定的**：固件每 11 次物理刷新做一次 SSD1677 的 **Mode-1 OTP 全刷**
   （实测 4313–4322 ms，整屏反色），这是清残影/DC 平衡的维护动作；它不是「算法忘了
   用局刷」。局刷本身每次翻页都在用（737–739 ms，差分、不整屏反色）。
2. **计数器是全局的**：`CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT=10` 统计的是所有局刷，
   包括开机后台刷新簇、5 分钟自动刷新、遥控语音的重复刷新。所以纯阅读时全刷落在
   「第 11 次刷新」≈ 每 9–10 页，混合使用时更频繁。
3. **「快速刷新/局部刷新」不能单独消除这个闪屏**：快速刷新（M5GFX
   `epd_fast`/`epd_fastest`、SSD1677 Mode-2 `0xFF`）都属于 Mode-2 家族（差分或
   绝对），不能像 Mode-1 绝对波形（OTP `0x14`）那样把残影和电荷拉回平衡，而后者
   物理上必然反色。厂商明确建议每约 10 次局刷做一次全刷；自制更快波形有面板寿命
   风险。
4. **决策（2026-10-22）：保持 10 次上限，不冒面板寿命风险**。因此连续阅读时每约
   10 页仍会有一次 4.3 s 全刷，这部分无法在不放宽限制的前提下消除；仍可做的是
   去掉「多余的全刷」并降低其观感影响：
   - **P1**：去掉睡眠周期里的第二次全刷——ESP 深睡时保留面板供电与控制器 RAM，
     唤醒走局刷（厂商 deep-sleep mode 1 + HWRESET 保留 RAM 的用法），睡眠周期从
     2 次全刷降到 1 次（甚至 0 次）。
   - **P2**：合并/去重刷新（语音按住松开最多 4 次局刷、启动簇、导航后二次刷新），
     避免它们提前消耗 10 次额度，让全刷精确落在第 11 次真实翻页。
   - **P4**：全刷序列去掉冗余的 `0xF8` 阶段（厂商证据表明该阶段没有 DISPLAY 位，
     不显示画面），可省一次 activation + 48 KB 写入（约 50–150 ms 量级，需 HIL 验证）。
   - **P0-lite（可选）**：不提高上限，仅在计数 ≥9 且出现自然断点时把全刷提前到断点。
5. **明确不建议**：dirty-rect（窗口）局刷（实测最多省 2.3% 时间，见 §3.4）、单纯把
   limit 调到很大（残影/DC 失衡，厂商警告不可逆损伤）、把 M5GFX 自定义 LUT 直接量产
   （M5Stack 官方称其 PaperMono 波形「currently unstable」）。

## 1. 现有刷新链路（源码事实）

全固件只有一个刷新入口（`grep` 确认没有 `lv_timer_handler()`，LVGL 不会自行渲染）：

```text
render_current(quality)                         src/papermono_ui.c:4060-4102
  -> papermono_display_refresh(quality)         src/papermono_display_scheduler.c:33-71
     -> input guard: blocked = true             （触摸/按键在刷新期间只保留一个延迟事件）
     -> if quality == FULL: force_full_next()   display_scheduler.c:50-56
     -> display_blanking_on()
     -> lv_refr_now(NULL)                       LVGL FULL render mode，整屏一次 flush
     -> display_blanking_off()
        -> papermono_refresh_changed_frame()    src/papermono_ssd1677.c:810-842
           -> 无 baseline / 达到 limit / 局刷失败 -> Mode-1 全刷   ssd1677.c:671-731
           -> 否则 -> Mode-2 局刷（0xFF）                          ssd1677.c:753-807
```

关键配置与数据：

| 项 | 值 | 位置 |
| --- | --- | --- |
| LVGL 渲染模式 | `CONFIG_LV_Z_FULL_REFRESH=y`，任何失效都整屏 flush | `prj.conf:148-160` |
| 局刷上限 | `CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT=10` | `Kconfig:29-43` |
| 输入沉降 | 32 ms | `Kconfig:169-176` |
| 自动刷新 | 空闲 5 分钟重渲染一次（仅像素变化时消耗局刷额度） | `Kconfig:149-157`，`ui.c:4448-4452` |
| 空闲休眠 | 默认 10 分钟进入触摸唤醒深睡 | `Kconfig:178-186` |
| 局刷实测 | 737–739 ms（整屏，与改动区域无关） | `docs/ssd1677-hil.md:153`，`docs/acceptance.md:408` |
| 全刷实测 | 4313–4322 ms，整屏反色 | `docs/ssd1677-hil.md:135`，`docs/acceptance.md:406` |
| 单次渲染 CPU | 约 216–343 ms（由页面动作总耗时 985–1112 ms 减去面板与沉降倒推） | `docs/interaction-response-20260930.md:40-43` |
| SPI | 20 MHz，整帧 48,000 B ≈ 19.2 ms（仅占局刷 2.6%） | `boards/.../m5stack_papermono_esp32s3_procpu.dts:257` |

`papermono_write()` 逐字节比较并只置 dirty 标志（`ssd1677.c:891-902`），所以「像素
没变」时面板不会刷新；但 LVGL 整树重建 + 整帧转置的 CPU 成本每次都要付。

### 1.1 什么时候会走全刷（baseline 失效/强制 FULL 清单）

| 触发 | 代码位置 | 用户可见 |
| --- | --- | --- |
| 冷启动首次渲染 | `ui.c:4360`（`FULL`） | 是，4.3 s 闪 |
| 任意唤醒（触摸/定时深睡）后的重启 | `ui.c:4360` | 是，4.3 s 闪 |
| 进入休眠/关机的睡眠页 | `ui.c:4175`（`FULL`） | 是，4.3 s 闪 |
| 第 11 次局刷（全局计数） | `ssd1677.c:819-833` | 是，4.3 s 闪（**阅读中的那一次**） |
| 局刷失败后的回退 | `ssd1677.c:802-804, 823-827` | 是（故障路径） |
| 四灰阶刷新（仅自检镜像） | `ssd1677.c:539-545` | 是（生产固件不启用） |
| 电源/低功耗路径移除面板电 | `ssd1677.c:605-669`，`power.c:96-112` | 下次上电必然全刷 |
| 显式 `FULL` 后帧未变化 | `display_scheduler.c:50-56` + `ssd1677.c:815-817` | 本次不刷但 baseline 失效，**下一次随机全刷**（潜在隐患，实践中少见） |

## 2. 阅读场景的精确归因

阅读页（`render_library_reader`，`ui.c` 阅读器渲染）只有书名、正文、页码和静态页脚，
没有时钟/电池等动态元素；翻页的左右键/点击区域都会走
`request_fast_redraw()`（`ui.c:2990-2994`，调用处 `ui.c:3263`、`ui.c:3275`），即
`PAPERMONO_REFRESH_FAST`。但调度器只对 `FULL` 做特殊处理（`display_scheduler.c:50-56`），
**`FAST` 与 `AUTO` 完全等价**。

因此纯阅读时：

- 每次翻页 = 1 次整屏局刷（737–739 ms），不反色，观感正常；
- 第 11 次刷新 = 维护全刷（4313–4322 ms），整屏黑/白反色 → **用户看到的闪屏**；
- 开机后 Wi-Fi/BLE/SNTP 的后台状态变化会先在首页消耗 1–2 次局刷额度
  （`main.c:473-512` 的子系统初始化 + `W:/B:/T:`/时钟像素变化），所以第一次阅读
  全刷可能落在第 9–10 页；
- 5 分钟自动刷新在阅读页重渲染同一页，像素不变 → 不产生面板刷新、也不消耗额度；
- 睡眠周期另有两次全刷：睡眠页 1 次（`ui.c:4175`）+ 唤醒开机 1 次（`ui.c:4360`）。

> 这与 HIL 记录一致：`partial refresh 1 (1/10)` 出现在启动后的第一个后台刷新
> （`docs/acceptance.md:406-409`），随后每次局刷日志都带 `(n/10)`。

## 3. 「快速刷新/局部刷新」为什么不能单独解决

### 3.1 两种波形族的职责不同

- **Mode-1 绝对波形**（OTP `0x14`）：驱动整屏所有像素做「擦除/振荡 + 目标灰度」，
  负责清除残影并把正负电荷拉回平衡。代价就是 4.3 s 的整屏反色。
- **Mode-2 差分波形**（OTP `0xFF`）：控制器比较 RAM 新/旧帧，只驱动变化像素；
  737–739 ms，不反色，但会累积残影，且长期连续使用会造成 DC 失衡。
  M5GFX 的 `epd_fast`/`epd_fastest`（自定义 LUT）是 Mode-2 家族里的更快变体
  （绝对/差分两种），但没有 Mode-1 的擦除/平衡能力，只能部分缓解残影。

**补充：什么叫「DC 失衡 / 不可逆损伤」**

- 电子纸像素靠电场驱动带电颜料粒子；一次刷新是一串正/负电压脉冲。理想情况下每个
  像素在一轮刷新中的**净电荷为 0**（DC 平衡）。若有净直流偏置，电荷会在电极/介质层
  界面累积（离子迁移、电荷捕获），等效阈值电压漂移，像素的灰度响应随之改变。
- 可见后果分两级：先是残影加重（普通残影可被下一次全刷清掉）；再严重是**图像烧死
  （image sticking）**——全刷之后旧画面仍隐约可见、黑不够黑/白不够白、对比度下降、
  局部发灰或发黑。厂商所说的 "irreversible damage to the panel" 指的就是这一类
  **面板层劣化**：不会烧主板或控制器、没有安全风险，但软件刷新救不回来，严重时
  只能换屏。
- 全刷（Mode-1 OTP）里的擦除/振荡阶段负责把残留电荷和粒子位置拉回平衡；局刷是差分
  短波形，没有这个复位阶段，所以不能无限连续使用。
- 风险分级：
  - **当前生产路径**（OTP 波形 + 每 10 次局刷一次全刷）：厂商推荐用法，正常使用风险低。
    厂商原话是「OTP 波形可直接调用；**使用自定义外部波形时**才要特别注意 DC 平衡」。
  - **P0 把间隔放宽到 15–20 次（本轮已否决）**：是厂商建议值的 1.5–2 倍，属于未验证
    折中，必须用 §6.1 的 soak 观察。
  - **自定义 LUT（P4 实验分支）**：真正的 DC 失衡高危区。M5GFX PR #275 实测非平衡
    LUT 每次刷新净驱动 +58…+72 帧 VSH1，随后出现 sticking（残影固化）。
- 「不可逆」是**累积量变到质变**，不是一次越界就坏；但温度越高、连续局刷越多、图案
  对比越大，累积越快。这也是本报告把 P0 的上限压到 2× 而不是更激进的原因。

所以「用快速刷新/局部刷新替代全刷」= 用不清残影的更新替代清残影的维护。厂商
（M5Stack C153 官方文档）明确写：

> "After approximately 10 partial fast refreshes in software, it is recommended to
> perform one full-screen refresh to clear the screen and avoid severe accumulated
> ghosting."
>
> "Avoid uninterrupted continuous partial fast refreshes, to prevent long-term DC
> imbalance from causing irreversible damage to the panel."

即当前 `limit=10` 的策略与厂商建议完全一致，不是实现缺陷。

### 3.2 「快速刷新」在本项目里的可选空间

| 方案 | 单次 | 清残影 | 风险 |
| --- | --- | --- | --- |
| 现有 OTP 局刷 `0xFF` | 737–739 ms | 否（差分） | 厂商推荐路径 |
| M5GFX `epd_fast`（Mode-2 绝对，自定义 LUT） | 0.34–0.36 s | 部分（厂商用作中间级） | 非 DC 平衡会 sticking，官方称波形不稳定 |
| M5GFX `epd_fastest`（Mode-2 差分，自定义 LUT） | 0.07–0.10 s | 否 | 同上，20–30 次后可见发灰 |
| M5GFX `epd_quality`（Mode-1 绝对，自定义 LUT） | 3.5–4.7 s | 是 | 与 OTP 全刷同类，但同样长；官方称不稳定 |
| M5GFX `epd_text`（Mode-1 绝对，64 帧 LUT） | 0.45–0.58 s | 是（清残影能力与 quality 不等价，待验证） | 可把维护反色缩短 8–10 倍，但面板寿命/DC 平衡风险高 |
| OTP `0xD7` "FastFull"（`0x1A=0x5A` 固定温度带） | ≈0.6–0.74 s | 否（仍是 Mode-2） | 不是清残影方案 |

结论：**快速刷新可以让普通更新更快，但不能替代维护全刷**；能缩短「闪屏本身」的
只有自定义 Mode-1 LUT（`epd_text` 级别），但它能否等价于 quality 的清残影效果尚未
验证。M5Stack 官方明确不建议依赖 M5GFX 的 PaperMono 波形（称其 "currently
unstable"，推荐 OTP 示例）。因此本项目应保留 OTP 波形，把优化放在「何时全刷」而
不是「把全刷换成局刷」。

### 3.3 全刷序列里有一段可省的冗余

数据手册 `0x22` 位定义（p28 + 独立 X4 SDK 位表，见 §8）：

| bit | mask | 含义 |
| --- | --- | --- |
| A7 | `0x80` | 使能时钟 |
| A6 | `0x40` | 使能模拟电源 |
| A5 | `0x20` | 加载温度值 |
| A4 | `0x10` | 加载 LUT |
| A3 | `0x08` | Mode 选择（0=Mode 1，1=Mode 2） |
| A2 | `0x04` | **DISPLAY（真正驱动面板）** |
| A1 | `0x02` | 关模拟电源 |
| A0 | `0x01` | 关时钟 |

当前全刷是 `0xF8`（clock+analog+temp+LUT Mode2，**没有 A2**）→ 写 RAM_1 → `0x20`
→ 再 `0x14`（LUT Mode1 + DISPLAY Mode1）→ `0x20`。按位定义（并与厂商 OTP 示例注释
「先同步控制器驱动状态」一致），`0xF8` 阶段不会显示任何东西，它的 48 KB 写入随后会
被 `0x14` 阶段覆盖。即真正产生 4.3 s 反色的是 `0x14`；第一阶段只做了「上电 + 装
Mode-2 LUT + 一次冗余写入」。可以用一条自包含的 `0xF4`/`0xF7`
（clock+analog+temp+LUT Mode1+DISPLAY Mode1）替代两段式，省一次 activation 与
48 KB SPI（预计 50–150 ms，需 HIL 验证），但**不会消除反色**。

### 3.4 窗口/dirty-rect 局刷：实测不值得做

- SSD1677 的波形时长由 LUT 帧数决定，**不随 RAM 窗口缩放**；窗口只影响 SPI 字节数
  （整帧 19.2 ms，占 737 ms 的 2.6%）。官方手册没有给出「窗口缩短波形/栅极扫描」
  的明确支持，本机 HIL 也是整屏局刷与局部改动同为 737–739 ms。
- 本机面板是旋转的：控制器 RAM-Y（栅极扫描轴）对应 UI 的**水平**方向。对 32 组真实
  页面切换做像素 diff，最小的单一窗口仍需覆盖 12.7%–97.0% 的面板（中位 71.8%），
  栅极行跨度中位 90.8%。
- 即在最乐观的「窗口缩短扫描时间」模型下，中位也只省 9.4%，最好的一组（确认框文案
  变化）省 60%；而在数据手册支持的「波形固定」模型下最多省 2.3%。
- 结论：dirty-rect 是 CPU/SPI 优化，不是闪屏或延迟优化，本轮不纳入。

## 4. 改造方案

以下方案按「对阅读闪屏的收益 / 风险」排序。均以保留 OTP 波形、不引入自定义 LUT 为
前提。

### P0（未采纳，保留记录）：维护全刷「择时」需要放宽局刷间隔

**原方案**：让 `FAST` 翻页在维护到期后继续局刷，把全刷顺延到离开阅读/回首页/休眠
等自然断点，硬上限 2×（20 次）。

**未采纳原因（2026-10-22 决策）**：该方案必须把连续局刷上限从 10 放宽到 15–20，
超出 M5Stack「约 10 次」的建议，涉及不可逆的 DC 失衡/图像烧死风险（见 §3.1 补充）。
决定保持 10 次上限，优先面板寿命。

**保持 10 次的后果**：连续阅读时仍会在第 11 次刷新（约每 10 页）遇到一次 4.3 s
全刷。这是当前策略下无法消除的物理维护成本；剩余可做的优化见 P0-lite/P2/P4。

### P0-lite（可选）：不提高上限的「机会式清理」

**实现状态（2026-10-22）**：已实现，开关 `CONFIG_PAPERMONO_MAINTENANCE_OPPORTUNISTIC`
默认 n（需 HIL 计数验证后再启用）。策略模块
`include/papermono/refresh_policy.h` / `src/papermono_refresh_policy.c`，
驱动决策位于 `src/papermono_ssd1677.c` 的 `papermono_refresh_changed_frame()`。

**设计**：仍以 10 次为硬上限；当 `consecutive_partials` 已接近上限（例如 ≥9）且
出现自然断点（离开阅读/回首页/进入休眠页）时，**提前**在该断点做维护全刷，而不是
等第 11 次刷新。它只把一次本就要发生的全刷挪到更合适的时刻，并且让全刷更早发生、
对面板更保守，不提高任何限制。

**收益**：如果用户在 9–10 次翻页内离开阅读，全刷落在切页/休眠而不是下一次翻页。
**限制**：连续读满 10 次仍按原规则全刷，无法覆盖长时间连续阅读；阈值 9 会让部分
本来不会在本次阅读触发的全刷提前出现（全刷总数可能略增），取决于产品更在意「闪在
断点」还是「少闪一次」。
**验证**：只需 HIL 计数与日志原因标记；不涉及放宽限制，无需额外 DC 残影 soak
（但仍应与 10 次基线对比残影）。

### P1：消掉睡眠周期里的额外全刷（唤醒全刷）

**现状**：睡眠页 `FULL`（4.3 s 闪）→ `papermono_power_prepare_peripherals()` →
`papermono_ssd1677_prepare_poweroff()` 移除 EPD_EN 面板电、`baseline_ready=false`
（`ssd1677.c:605-669`，`power.c:96-112`）→ ESP 深睡 → 唤醒后重新走启动流程，
`ui.c:4360` 再次 `FULL`（第二次 4.3 s 闪）。**每个睡眠周期 2 次全刷。**

**方案（需 HIL 可行性验证）**：ESP 深睡（触摸/定时唤醒）不切断面板电，只让控制器
进入保留 RAM 的 deep-sleep mode 1（现驱动的 `0x10=0x01` 就是这个语义，HWRESET 退出
且 RAM 保留，与厂商 OTP 示例一致）；把「baseline 有效 + 上一帧身份」写入 RTC
retained 内存。唤醒后：

- 检测到 ESP 深睡唤醒且 baseline 标记有效 → 跳过 `FULL`，首帧走 `AUTO` 局刷
  （差分自睡眠页过渡到恢复的页面）；
- 冷启动/掉电/PMIC 关机仍走现有 `FULL`；
- PMIC 关机（`M5PM1_SYSTEM_SHUTDOWN`）仍必须移除 EPD_EN。

**预期**：睡眠周期 2 次全刷 → 1 次（睡眠页那一次保留为自然清理点）；若把睡眠页也改
局刷，则可做到 0 次，但睡眠画面会带前一页残影，需产品决策。
**代价**：深睡时面板控制器保持 25 µA 级静态电流（数据手册 sleep mode 25 µA typ，
40 µA max），对小时级深睡可接受，但需要实测整机深睡电流预算。
**风险**：中高。涉及电源时序、RESET 卡死历史问题、唤醒后 RAM/乒乓面一致性，必须
单独 HIL（见 §6），并保留回滚 Kconfig。

### P2：合并/去重刷新，停止「偷吃」维护额度

**实现状态（2026-10-22）**：语音合并已实现并默认启用：`queue_voice_refresh()` 改为
走 `papermono_ui_request_background_refresh()` 的 pending 标志（`src/papermono_ui.c`），
按住时不再绕过刷新输入保护。启动簇合流与「导航后清 pending」未实现（需要更多 UI
状态版本信息，见下方表格的剩余项）。

| 现象 | 证据 | 处理 |
| --- | --- | --- |
| 按住/松开语音一次最多 4 次整页刷新 | 触摸线程 `queue_voice_refresh()`（`ui.c:3726-3730`，绕过输入保护与 pending 去重）+ worker 的 `papermono_ui_request_background_refresh()`（`ble_mic.c:448,477`） | 把语音刷新并入 pending 标志（或独立 `pending_voice_refresh`），worker 的状态为准；按住时禁止在 `input_blocked` 期间开启麦克风（顺带修 O7） |
| 启动后台刷新簇 | Wi-Fi/BLE/SNTP 初始化各自请求刷新（`main.c:473-512` 及各模块） | 启动时设几百 ms 的「合流窗口」，首屏之后合并成一次渲染；`CONTENT_CHANGED` 不延迟 |
| 导航后紧跟二次刷新 | 导航渲染期间后台事件进 pending，guard 释放后重放（`ui.c:4495-4506`、`ui.c:4040-4051`） | 导航渲染完成后清掉仅状态类的 pending（该渲染已包含最新状态） |
| 显式 FULL 但帧未变导致 baseline 失效 | `display_scheduler.c:50-56` + `ssd1677.c:815-817` | 请求 FULL 时若帧未变，仍执行一次全刷（符合语义），或恢复 baseline，避免下一次「随机全刷」 |

**预期**：一次语音会话最坏 4 次局刷的路径被消除，通常 2 次（按下/松开各一次；输入
线程与 worker 请求并发到达时合并为 1 次）；启动簇 2–3→1 次与导航后二次刷新消失
尚未实现（见 §10）。对阅读闪屏是间接收益：额度消耗变慢，第一次全刷回到第 11 次
翻页。实机刷新计数需按 §6.3 确认。

### P3（未实施）：按「等效全屏驱动量」计费会突破厂商连续局刷上限

**未实施原因（2026-10-22 复核）**：原计划按变化字节/面积给局刷计费，让小状态更新
不占维护额度。评审发现：页面切换实际只改动约 1%–11% 的像素，按面积计费会让一次
翻页只记约 0.1–0.3 个信用，维护全刷可能被推迟到 30–100 次连续局刷之后，直接突破
M5Stack 的「约 10 次」限制，带来残影/DC 失衡风险。若同时保留「10 次连续局刷」硬
上限，按面积计费又不会带来任何收益。因此本轮不实施，维护额度继续按「每次局刷
1 次」计算。残影/电荷是按像素累积的，但厂商的 10 次建议是保守的整体限制，不宜
用面积近似绕过。

### P4：缩短全刷序列（微优化，不是消除闪屏）

**实现状态（2026-10-22）**：已实现为 `CONFIG_PAPERMONO_FULL_REFRESH_COMPACT`，默认 n。
启用后全刷只发一条自包含 `0xF4`（clock+analog+temp+LUT Mode1+DISPLAY Mode1）并写
两个 RAM 平面，省掉 `0xF8` 阶段；必须先在 C153 上验证像素结果、BUSY 时序与深睡前
电源状态，再进入生产配置。

- 用单条自包含 Mode-1 命令（`0xF4`/`0xF7`）替代 `0xF8` + 写入 + `0x20` + `0x14`
  两段式，去掉无 DISPLAY 位的阶段与一次 48 KB 冗余写（预计 50–150 ms）。需 HIL
  逐项确认：像素结果、BUSY 时序、深睡前的电源状态。
- 可选实验分支：M5GFX `epd_text` 级自定义 Mode-1 LUT 能把维护反色从约 4.3 s 缩到
  约 0.45–0.58 s（观感从「长时间闪」变成「短促一下」），但 M5Stack 明确称其
  PaperMono 波形不稳定，且有 DC 失衡 sticking 的实测记录（M5GFX PR #275）。建议
  只做实验镜像 + 长时间 soak，不进入生产默认配置。

### 实施顺序建议

1. **P2**（合并/去重）：无面板风险，先做，顺便清掉语音 O2 与启动簇，并让全刷精确
   落在第 11 次真实翻页。
2. **P0-lite**（可选）：阈值 9 的「机会式清理」，只改策略不改上限；先 HIL 计数验证
   全刷总数与位置。
3. **P4**（全刷序列微优化）：独立 HIL 项，省 50–150 ms，需先验证命令序列。
4. **P1**（唤醒无闪）：独立的中高风险项目，建议单独立项；当前判断收益/风险比低，暂缓。
5. **P0 不放宽上限**（已决策）：除非未来有长期 soak 证明 15–20 次安全，否则不重启。

## 5. 预期效果汇总

| 场景 | 现在（limit=10） | P2+P4（+P0-lite 可选） | 再加 P1 |
| --- | --- | --- | --- |
| 阅读正文 | 每 9–10 次翻页 1 次 4.3 s 黑闪 | 每 10–11 次翻页 1 次（P0-lite：若 9–10 次内离开阅读，则移到断点） | 同左 |
| 触摸/定时唤醒 | 4.3 s 黑闪 | 4.3 s 黑闪 | 0（局刷过渡） |
| 进入休眠 | 4.3 s 黑闪 | 4.3 s 黑闪（自然清理点） | 1 次（保留） |
| 睡眠周期合计 | 2 次全刷 | 2 次 | 1 次（或 0，若睡眠页也改局刷） |
| 语音按住→松开 | 最多 4 次局刷 / 4 额度 | 通常 2 次（并发时 1 次）/ ~2 额度 | 同左 |
| 启动后台簇 | 2–3 次局刷 | 1 次 | 同左 |
| 单次全刷时长 | 4.3 s | 4.15–4.27 s（P4） | 同左 |
| 普通更新 | 737–739 ms | 不变（不引入自定义 LUT） | 同左 |

> 关键预期说明：**保持 10 次上限后，正文阅读中每约 10 页一次的全刷无法消除**。
> P2 只能消除「额度被无关刷新提前消耗」，让全刷回到第 11 次真实翻页；P0-lite
> 只能在 9–10 次内恰好出现断点时把这次全刷挪到断点。

## 6. 验证计划

### 6.1 前置实验：局刷基线是否可靠（确认 10 次仍是安全值）

厂商 OTP 局刷只写 RAM_1（`0x24`），依赖 Mode-2 RAM 乒乓面在 HWRESET 后仍保留作为
「上一帧」。这是当前最大的技术不确定性：若 HWRESET 复位了乒乓面，残影会比 10 次
更早出现。本轮已决定保持 10 次上限，因此实验目的不是放宽上限，而是确认 10 次是否
仍安全、以及是否应进一步收紧。实验：

1. 交替显示两幅差异极大的测试页，连续 30–50 次局刷，每次记录/拍照；
2. 对照组：每次局刷前把新帧同时写入 `0x26`（上一帧平面），比较残影出现时间；
3. 观察灰度/黑度衰减、白底灰斑、边框残影；
4. 若残影在 10 次前就明显，应把上限调低于 10，而不是维持现状。

### 6.2 主机测试（可回归）

- 把 P0-lite 的策略抽成可移植模块（不依赖 Zephyr 驱动），新增
  `tests/host/test_refresh_policy.c`：覆盖 baseline 无效→全刷、未到期→局刷、
  计数≥9 且遇到 AUTO 断点→提前全刷、计数<9 的断点不提前、达到 10 次→全刷、
  失败→全刷。
- `tests/preview/scripts/patch_ui.py` 增加刷新计数断言：一次语音按住→松开最多 2 次
  `papermono_display_refresh`；导航后不再紧跟第二次 pending 渲染。
- `tests/host/test_app.c` 现有 FAST 传播断言保留；新增策略模块的单测进
  `tests/host/run.sh`。
- 构建配置断言：`CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT` 保持 10（新 Kconfig 若有
  默认值，测试/文档需固定该值）。

### 6.3 HIL（设备 `/dev/cu.usbmodem21201`）

- 串口计数（驱动已有日志）：`SSD1677 full refresh N` 之间隔了多少次
  `partial refresh ... (n/10)`；阅读 50 页，记录每次全刷落在哪一页/哪个操作；
  确认上限仍是 10（日志中的 `(n/10)` 不变）。
- 残影照片：固定测试页（大字号 + 灰阶块），每 5 次局刷拍照；与基线对比。
- 睡眠 20 周期：记录全刷次数、唤醒首帧耗时（P1 落地后应为局刷）、深睡电流。
- 深睡电流：万用表/功耗仪测 P1 前后整机 µA 级差异。
- 失败恢复：人为制造 BUSY 超时，确认全刷回退与 15 s 深放电恢复未回归。
- 全刷日志新增原因标记（`limit / breakpoint-early / explicit`），便于验收。

### 6.4 验收标准（建议）

- `PARTIAL_REFRESH_LIMIT` 保持 10；连续阅读 30 页时，全刷仍每 11 次刷新一次
  4.3 s（接受项），但不再被后台/语音刷新提前触发（P2 后首次全刷落在第 11 次翻页）。
- 若启用 P0-lite：9–10 次计数内离开阅读时，全刷发生在该断点而不是下一次翻页。
- 睡眠→唤醒周期全刷次数 ≤1（P1 落地后）。
- 语音一次按住→松开 ≤2 次面板刷新。
- 50 页 + 20 睡眠周期 soak 后，测试页残影/黑度不劣于当前 10 次基线。
- 主机测试全绿、默认/partial/selftest 三个 profile 构建 0 warning。

## 7. 不建议 / 本轮不做的方向

1. **dirty-rect 窗口局刷**：波形时长与窗口无关，实测最多省 2.3%；面板旋转导致窗口
   接近满屏。除非厂商确认窗口能明显缩短扫描（当前手册与 HIL 都不支持），否则收益
   不足以覆盖驱动改动与验证成本。
2. **把 `PARTIAL_REFRESH_LIMIT` 调大**：厂商明确 ~10 次后清屏；DC 失衡可能不可逆。
   本轮已决定**保持 10**，不再把放宽上限作为优化手段；若未来重新评估，必须先有
   长期 soak 证明 15–20 次安全。
3. **把 M5GFX 自定义 LUT 直接用于生产**：M5Stack 官方称 PaperMono 波形 unstable，
   推荐 OTP 示例；PR #275 记录了非 DC 平衡 LUT 的 sticking。若要更快的全刷/局刷，
   先做独立实验镜像与长 soak。
4. **靠 `lv_timer`/局部重绘降低闪屏**：当前 UI 每次导航都整树重建
   （`lv_obj_clean`），LVGL FULL 模式下都收敛为整屏刷新；只影响 CPU，不影响闪屏。

## 8. 待确认问题

1. **已决策（2026-10-22）**：保持 `PARTIAL_REFRESH_LIMIT=10`；除非未来有长期 soak
   证明 15–20 次安全，否则不再把放宽上限作为优化手段。
2. 睡眠页是否接受局刷（省掉最后一次 4.3 s 闪，但睡眠画面带上一页残影）？
3. 是否接受深睡时面板控制器保留 25 µA 静态电流（P1 的前提）？
4. 是否实施 P0-lite（计数 ≥9 时遇到断点提前全刷）？阈值取 9 还是 10？
5. `0xF8` 阶段删除后用 `0xF4` 还是 `0xF7`，需要 HIL 逐项验证（像素结果/BUSY）。
6. 若 §6.1 soak 显示残影早于 10 次出现，是否把上限收紧到 8 或更低？
7. 是否需要给用户一个「机会式清理开/关」的设置项（映射到 P0-lite）？

## 9. 证据与参考

### 9.1 本仓库源码/文档

- 调度器：`src/papermono_display_scheduler.c:33-71`
- 驱动全刷/局刷/策略：`src/papermono_ssd1677.c:671-731`、`:753-807`、`:810-842`
- 帧 diff 与 dirty 门：`src/papermono_ssd1677.c:884-910`
- 低功耗与面板电：`src/papermono_ssd1677.c:605-669`，`src/papermono_power.c:96-112,286-311,365-403`
- UI 渲染/睡眠页/启动/翻页/语音：`src/papermono_ui.c:4060-4102,4175,4360,2990-2994,3263,3275,3726-3730,3930,3967,4495-4506`
- 语音 worker 刷新：`src/papermono_ble_mic.c:448,477`
- 配置：`prj.conf:148-160`，`Kconfig:29-43,149-157,169-176,178-186`
- 实测时间：`docs/ssd1677-hil.md:135,153`，`docs/acceptance.md:406-409`
- 阅读刷新意图文档：`docs/application.md:145-147`

### 9.2 外部资料（厂商/数据手册）

- SSD1677 数据手册 Rev 1.0（Solomon Systech）：
  <https://files.waveshare.com/upload/2/2a/SSD1677_1.0.pdf>
  （`0x22` p28、`0x21` p27、RAM p29、`0x32/0x37` p31-32、`0x3C` p33、`0x10` p24）
- M5Stack C153 产品文档（约 10 次局刷后全刷、DC 平衡警告）：
  <https://docs.m5stack.com/en/products/sku/C153>
- M5Stack PaperMono 静态 PDF（M5GFX 各模式实测时间 4.71/0.45/0.34/0.07 s，
  "E-Paper Driver Notes" 推荐 OTP 示例）：
  <https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/en/core/PaperMono.pdf>
- M5PaperMono-OTP-Demo（本面板的 OTP 全刷/局刷/四灰阶参考序列）：
  <https://github.com/m5stack/M5PaperMono-OTP-Demo>
- M5PaperMono-UserDemo（厂商刷新策略阈值）：
  <https://github.com/m5stack/M5PaperMono-UserDemo>
- M5GFX 0.2.34（`Panel_SSD1677_4Gray`、`epd_mode_t` 映射）：
  <https://github.com/m5stack/M5GFX/tree/0.2.34/src/lgfx/v1/panel>
- M5GFX PR #232（PaperMono 四模式实测时间）、PR #223（fastest 20–30 次后降质）、
  PR #275（非 DC 平衡 LUT 导致 sticking）：
  <https://github.com/m5stack/M5GFX/pull/232>、<https://github.com/m5stack/M5GFX/pull/223>、
  <https://github.com/m5stack/M5GFX/pull/275>
- open-x4-epaper SSD1677 指南（`0x22` 位表、RAM_1/RAM_2 差分机制、X 8 像素对齐）：
  <https://github.com/open-x4-epaper/community-sdk/blob/c39f253a7dabbc193a8d7d310fb8777dca0ab8f1/libs/display/EInkDisplay/doc/SSD1677_GUIDE.md>
- epdsi `Ssd1677RefreshMode`（`0xFF` 局刷、`0xD7` FastFull 的独立映射）：
  <https://docs.rs/epdsi/latest/epdsi/controllers/ssd1677/enum.Ssd1677RefreshMode.html>

### 9.3 本机研究底稿（`tmp/` 已列入 `.gitignore`，不随仓库分发）

- `tmp/refresh-research/vendor-protocol.md`：`0x22` 位语义、厂商序列、LUT/DC 平衡证据、
  VERIFIED/INFERRED/UNKNOWN 清单。
- `tmp/refresh-research/firmware-trace.md`：完整调用图、25 行场景矩阵、14 处 baseline
  失效点、12 处直接刷新与 37 处后台刷新调用点、重复刷新分析。
- `tmp/refresh-research/pixel-diff.md`：32 组真实 UI 切换的像素变化/窗口测量与代价
  模型；`tmp/refresh-research/diffs/` 为叠加图。

## 10. 实现状态（2026-10-22）

已按决策实施：**P0 未采纳（保持 limit=10）、P1 暂缓（收益/风险比低）**，其余落地。

| 项 | 实现 | 默认 | 验证状态 |
| --- | --- | --- | --- |
| P2 语音合并 | `queue_voice_refresh()` 改走 `papermono_ui_request_background_refresh()`；按住时检查 `input_blocked` | **启用** | host `test_ui_refresh_coalescing.py` + 默认构建 |
| P0-lite 机会式清理 | 可移植策略 `papermono_refresh_policy_decide()`，开关 `CONFIG_PAPERMONO_MAINTENANCE_OPPORTUNISTIC` | 关闭 | host `test_refresh_policy.c` + 全开构建 |
| P4 单命令全刷 | `CONFIG_PAPERMONO_FULL_REFRESH_COMPACT`（`0xF4` 单命令） | 关闭 | 全开构建；HIL 待做 |
| P1 唤醒无闪 | 未实现（深睡保留面板电/RAM） | — | — |
| P0 放宽上限 | 未采纳 | — | — |

**代码落点**

- 新增可移植策略：`include/papermono/refresh_policy.h`、`src/papermono_refresh_policy.c`
  （不依赖 Zephyr，可在主机测试）。
- 新增驱动 API：`papermono_ssd1677_set_refresh_hint()`（`include/papermono/ssd1677.h`），
  `src/papermono_display_scheduler.c` 在 `display_blanking_on()` 前传入 AUTO/FAST/FULL。
- `src/papermono_ssd1677.c`：新增 `refresh_hint`；`papermono_refresh_changed_frame()`
  用策略决策替换原来的 `consecutive_partials < LIMIT` 判断；全刷序列可按 P4 开关切换。
- 测试：`tests/host/test_refresh_policy.c`、`tests/host/test_ui_refresh_coalescing.py`，
  已接入 `tests/host/run.sh`。

**默认行为变化**：只有 P2 生效（无面板风险）；P0-lite/P4 需显式打开 Kconfig，
并由 HIL/残影 soak 决定是否量产。P2 的「启动簇合流」与「导航后清 pending」未实现
（需要更多 UI 状态版本信息），留待后续。

**验证记录（2026-10-22，本机）**

- `sh tests/host/run.sh`：全绿，含新增的 `test_refresh_policy`（limit/断点决策）与
  `test_ui_refresh_coalescing`（语音合并 + 刷新期间拒绝开麦）。
- 默认配置固件：`west build -p always -d build-refresh-default -b
  m5stack_papermono/esp32s3/procpu .` 通过，**0 warning**，产出
  `build-refresh-default/zephyr/zephyr.bin`（4,551,460 B）。
- 全开配置固件：追加 `EXTRA_CONF_FILE=tmp/refresh-features.conf`
  （P0-lite/P4 = y）构建通过，**0 warning**，确认 `0xF4` 单命令全刷分支可编译。
- 原生预览：`OUT=$PWD/tmp/refresh-preview bash tests/preview/build.sh` 通过，
  37 个真实 UI fixture 全部渲染。
- 日志：`tmp/build-default.log`、`tmp/build-features.log`、`tmp/preview-build.log`
  （`tmp/` 未入库）。
- **未做**：实机烧录/HIL。P0-lite 的全刷位置与 P4 的像素/BUSY 时序都必须在 C153
  上按 §6.3/§6.4 验证后才能默认启用。
