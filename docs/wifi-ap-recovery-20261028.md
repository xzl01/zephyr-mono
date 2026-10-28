# 2026-10-28 热点恢复停机排查

## 实机现象与定位

用户已确认多源栏目、表格及翻页正常。随后通过设备屏幕配对码进行正常网页认证：
ticket/session 成功，但首次认证后的 RSS 订阅 GET 超时，尚未执行任何订阅 PUT。
随后公开状态、ping 也失去响应，用户确认界面卡住。

对当时运行的容量修复镜像（SHA-256 `a02757560980dcc3ca137a578c939f0f03a387d77c3736bd75f70f2280e5768e`）
读取有限内核异常元数据，结果如下：

| 项目 | 实际记录 |
| --- | --- |
| 停机位置 | `arch_system_halt` |
| 当前线程 | `wifi_task_handle` |
| RSS/request mutex | owner/count 均为 0/0 |
| HTTP 线程 | 正常挂起在内核 poll，未等待 RSS 锁 |
| 原始异常 | LoadProhibited，cause 28，访问地址 `0x2c` |
| 原始 PC | `0x4209f29d`，`ieee80211_hostap_attach+0x111` |
| 上层调用 | `wifi_softap_start` |

原始异常保存区与当前 CPU 寄存器分开检查：fatal 日志处理期间的 ALLOCA/window 状态
不能代替最初异常。匹配镜像反汇编显示，热点启动选择第二个 beacon buffer 后，在
空指针检查前读取其 `+44` 字段。底层 beacon 申请失败是明确证据；内部堆不足与
Wi-Fi 缓冲配额耗尽尚不能仅由这个现场区分。不能把认证 GET 与热点启动的时间重合
直接表述为 GET 导致崩溃。脱敏摘要见
[fault-summary.json](design-references/rss-multisource-20261028/fault-summary.json)。

## 修复范围

- STA 连接成功时取消过期的 AP 恢复及健康检查；恢复 worker 执行时再次检查状态，
  已出队的旧任务也不能在 STA 已连接、正在连接或命令执行中启动热点。
- STA 断线取消旧的 AP handoff。handoff 执行时确认 STA 仍连接，避免关闭唯一可用热点。
- STA 关联期间延后热点健康判断；APSTA 的信道会跟随路由器，不能把这段变化当作
  固定配网信道异常反复重启 AP。状态查询返回后也再次检查，成功后交给 handoff。
- AP 启动完成事件若晚于 STA 成功，补排 20 秒 handoff；STA 失败时继续保留并验证 AP。
- 静态 Wi-Fi RX 从 10 调为 6，与 RX BA 窗口 6 一致。硬件 RX 实际走内部 `k_malloc`，
  不受通用 PSRAM 分配配置影响。少四组缓冲可减少约 6,464 B 内部堆申请量，另有分配器
  开销，为 beacon 等分配留下余量；代价是突发 RX 余量减少。这是资源余量改善，不能
  独自视为已证明消除所有 beacon 分配失败。

不修改闭源 Wi-Fi blob，也不扩大 socket 或 TCP slab 配额。显式 Wi-Fi 重置、暂停、
路由器不可达时的配网 AP 恢复均保留。

## 验证范围

真实函数的主机生命周期回归覆盖已排队/已出队任务、查询期间状态改变、APSTA 信道、
路由器失败、显式命令和暂停；独立复核指出的晚到 AP_ENABLE 顺序也已补充精确回归。
完整 host 套件及固件构建通过，内部静态 DRAM 仍为 339,176 / 399,108 B（84.98%）。

修复镜像为 `a632cb86811f5390b469c43b0d7e91693c8981a1d0d00af2931b014f79884eb8`。
刷写、实际认证 CRUD、超时释放和观察时长的最终结果记录在
[RSS 验收](design-references/rss-multisource-20261028/validation.md)，不得以主机测试代替。

本轮实机结果：完整回读与 NVS 保留通过，正常认证后的订阅 API 增改删及恢复原配置
通过；不完整请求 10.04 秒被 HTTP 空闲期限关闭，随后读取正常。255.2 秒被动日志无
fatal，type4 分配及配额失败计数前后均为 0。正常重启释放测试会话后，用户再次确认
修复版触摸、切源与表格翻页正常。这里只记录本轮验收，不扩大为长期稳定性结论。
