# 天气数据源评估

休眠屏已能显示天气，但设备目前没有天气来源。两条路线与实测结论如下。

## 路线 A：外部推送（推荐先做）

复用内容中心已配对会话，新增 `POST /api/v1/info/weather`，设备写入现有信息卡片缓存。
手机快捷指令 / Tasker / cron 脚本负责取数。

- 设备侧改动小，不碰网络栈。
- 数据源任选（和风、彩云、Open-Meteo）。
- 缺点：依赖外部定时推送。

## 路线 B：设备直连天气接口（已实现）

当前使用国内可达的 sojson 深圳接口（城市代码 101280601，中文天气）：

- 明文 HTTP（`t.weather.sojson.com`），DNS 失败或首个地址超时时轮流使用
  `61.160.237.2` / `61.160.237.4`；
- 启动后重试 10 次、每次间隔 20 秒，覆盖 DHCP 刚完成时的不稳定窗口；
- 解析 `data.wendu` 与 `data.forecast[0].type`，按设置时区显示“截至 HH:MM”；
- 每 30 分钟刷新一次，失败保留上一次快照；
- 早期试过 wttr.in，但在本网络下时通时断，已放弃。
- 休眠屏天气行优先使用该快照，无数据时回退到信息卡片。

### 一个必须记住的 Zephyr 坑

设备能拿到 DHCP 地址、能连网关，但**公网直接 `ENOENT`**。根因是这棵 Zephyr 树的
DHCP 客户端只调用 `net_if_ipv4_set_gw()` 保存网关，**没有把它安装为默认路由器**，
于是路由表为空、跨网段流量无路由。修复是在拿到地址后显式调用
`net_if_ipv4_router_add(iface, &gw, true, 0)`（见 `papermono_weather.c` 的
`ensure_default_router()`）。

验证日志：

```text
Installed default router 192.168.8.1
wttr.in connected to 5.9.243.187
wttr.in response 6143 bytes
```

### 代价与限制

- 明文 HTTP：天气数据可被中间人篡改，只用于展示，不参与任何决策；后续若需要完整性，
  应改为自定义 PSA/mbedTLS 的 HTTPS 客户端（普通 TLS socket 在本树不可用，见上）。
- sojson 不提供逐小时数据，因此“未来 8 小时”目前显示“天气数据尚未同步”；
  需要该视图时再接入支持 hourly 的数据源。
- 固定回退 IP 可能随服务迁移失效；DNS 正常时不使用它。
