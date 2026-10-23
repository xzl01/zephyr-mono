# 天气数据源评估

休眠屏已能显示天气，但设备目前没有天气来源。两条路线与实测结论如下。

## 路线 A：外部推送（推荐先做）

复用内容中心已配对会话，新增 `POST /api/v1/info/weather`，设备写入现有信息卡片缓存。
手机快捷指令 / Tasker / cron 脚本负责取数。

- 设备侧改动小，不碰网络栈。
- 数据源任选（和风、彩云、Open-Meteo）。
- 缺点：依赖外部定时推送。

## 路线 B：设备直连 Open-Meteo（暂缓）

Open-Meteo 免费、无需 key，提供当前天气与逐小时预报，正好覆盖“未来 8 小时”。

实际尝试（已回滚，未入库）：

1. 打开 `NET_SOCKETS_SOCKOPT_TLS` 后，Zephyr 的 `sockets_tls.c` 无条件调用
   `mbedtls_ssl_get_session()` / `mbedtls_ssl_set_session()`，因此必须启用
   `MBEDTLS_SSL_SESSION_TICKETS`。
2. 该符号依赖 `MBEDTLS_SSL_PROTO_TLS1_3`，而 TLS 1.3 又要求 `MBEDTLS_SSL_TICKET_C`
   的完整密码套件前置条件；当前为 BLE 裁剪过的 mbedTLS 配置不满足，直接编译失败。
3. 绕过 sockets TLS、改用 mbedTLS 原生 API 时，`MBEDTLS_SSL_CLI_C` 等符号在该
   Zephyr 版本未暴露为 Kconfig，需要自定义 `MBEDTLS_CONFIG_FILE`，属于独立工程。

结论：B 可行，但不是“开几个开关”的量级，需要单独一轮做 mbedTLS 配置与真机压测
（预计额外 30–50 KB RAM，且必须在 SNTP 校时后启用证书校验）。

## 建议

先做 A 打通数据与展示（含“截至”时间与 stale 标记），B 作为后续独立任务；
A 的实现也能在 B 上线后作为断网兜底。
