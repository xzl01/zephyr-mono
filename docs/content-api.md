# 内容与同步接口

## TF 卡布局

卡片使用 FAT 文件系统。首次成功挂载时固件创建：

```text
/papermono/
├── books/                 # .txt、.epub
├── calendar/              # .ics
├── tasks/
│   └── tasks-v1.json
├── notes/                 # .md、.txt
└── weread/
    └── weread-v1.json     # 无凭据的规范化书架摘要
```

文件名必须是合法 UTF-8，最长 255 字节，不能以 `.` 开头，也不能包含控制字符、
`..`、`/`、`\` 或 `:`。书籍上限 64 MiB，ICS/JSON/便签上限 256 KiB，
`weread-v1.json` 上限 64 KiB。

## 本地网页

未配置 Wi-Fi 时，设备创建 WPA2 SoftAP：

- SSID：`PaperMono-XXXX`
- 独立密码：屏幕“内容中心”页显示
- 地址：`http://192.168.4.1/`
- DHCP 地址池从 `192.168.4.11` 开始

网页首先用屏幕上的八位 Crockford Base32 一次性配对码换取 16 位十六进制、
60 秒有效且只能兑换一次的 ticket，再通过 `POST /api/v1/session` 换取会话
token。配对码在当前启动周期只能成功使用一次；重新配网会清除会话并生成新
凭据。服务仅绑定设备的 SoftAP/局域网接口，不提供公网入口或云端中继。APSTA
配网期间为 AP 和 STA 分别保留 IPv4 地址；STA 成功接入并获得 DHCP 地址后，
固件继续保留配网 SoftAP 20 秒。网页利用这段窗口轮询公开状态，随后跳转到
局域网地址。

浏览器将有效 token 保存在当前 origin 的 `localStorage`，刷新页面时先验证再
恢复会话。由于浏览器存储不能跨 origin 共享，SoftAP 页面在跳转前用现有会话
向 `POST /api/v1/session` 申请新的短期 ticket，再通过
`http://<station-ip>/#ticket=<one-time-ticket>` 接力。局域网页面立即用
`history.replaceState()` 清除 fragment，然后在 POST header 中兑换 ticket；
长期 token 始终不会进入 URL。认证失效或设备重置后，网页会清除本地 token
并重新显示配对页。

固件不会只相信 AP 启用回调：接口启用后还会检查其实际模式、SSID 和信道 6；
连续三次自检失败会关闭 AP 并进入有上限的退避恢复。该自检证明的是驱动接口
状态，实际 beacon 可发现性仍须用手机和电脑实测。

首次配网且尚无 STA 凭据时，ESP32-S3 最终运行于 AP-only。为规避控制器报告
AP_START 但外部无 beacon 的状态，启动时允许一次受控 APSTA 到 AP-only 切换；
DHCP 在每次成功 AP_START 后重建。这次切换在日志中会表现为一次
`SoftAP disable complete` 加 `Recovery SoftAP retry scheduled`，随后重新
`enable complete` 并通过接口自检，属于预期行为，不是异常重启。提交凭据后连接路径自动切换到 APSTA。配网 AP 使用固定信道 6、
20 MHz、802.11b/g/n、关闭省电及 78 qdBm（19.5 dBm）功率上限，并在日志中
回读这些参数。固件不强制国家码；功率值是控制器上限，实际发射仍受 regulatory
domain、芯片校准、eFuse 和天线链路约束。HTTP server 与 Wi-Fi sockets service
各保留一个 eventfd；Wi-Fi 接口完成自检后才启动 BLE。

## HTTP API v1

除公开状态和首次配对外，请求必须携带：

```http
X-PaperMono-Session: <session-token>
```

| 接口 | 认证 | 说明 |
| --- | --- | --- |
| `GET /api/v1/status` | 否 | API 版本、运行时间、TF、Wi-Fi、触摸、唤醒来源和内容 generation |
| `GET /api/v1/state` | 配对码或会话 | 首次换取配对 ticket；已有会话时导出 overlay |
| `POST /api/v1/session` | ticket 或会话 | 兑换 ticket；或为跨 origin 接力签发一次性 ticket |
| `GET /api/v1/content` | 是 | 返回内容条目的 JSON 数组 |
| `POST /api/v1/content` | 是 | 原始请求体流式导入内容 |
| `DELETE /api/v1/content/{id}` | 是 | 按稳定内容 ID 删除 |
| `POST /api/v1/wifi` | 是 | 保存 STA 凭据并后台连接 |

`POST /api/v1/content` 的响应码（客户端应按此处理，不要只看 2xx）：

| 状态码 | 含义 |
| --- | --- |
| `201 Created` | 已提交；文件通过校验、原子替换并重新可枚举 |
| `400 Bad Request` | 提交失败或写入被拒，body 带 `code` 字段给出负 errno |
| `401 Unauthorized` | 会话缺失/失效，需要重新配对 |
| `408 Request Timeout` | 流式请求在截止时间内没有完成 |
| `409 Conflict` | 另一个导入正在进行，或与删除操作冲突 |
| `411 Length Required` | 缺少 `Content-Length`；这是「上传了 0 字节」的常见根因 |
| `413 Payload Too Large` | 超出单文件大小上限 |
| `429 Too Many Requests` | 配对尝试过于频繁 |

公开状态的 Wi-Fi 字段示例：

```json
{
  "wifi": {
    "ap": true,
    "ap_verified": true,
    "connected": true,
    "station_ipv4": "192.168.1.23"
  }
}
```

`ap_verified` 只表示固件已通过 SoftAP 接口状态核对；`station_ipv4` 在 DHCP
完成前为空字符串。网页只在取得非空局域网地址后执行 origin 接力。
`touch.press_events`、`touch.release_events` 和 `touch.max_poll_gap_ms` 分别记录
本次启动的触点边沿与最差采样间隔；它们用于区分“控制器持续读到空闲帧”、
“触摸已经到达驱动”和“UI/显示没有处理”三种故障，不能只看 `touch.online`。
`power.wake_cause` 保存本次启动最早读取到的唤醒来源，可取 `cold_boot`、
`touch`、`esp_timer`、`pmic_timer`、`pmic_button`、`pmic_external` 或 `other`；
PMIC 中断状态随后被确认清除时，该字段仍保持不变。

首次配对：

```http
GET /api/v1/state
X-PaperMono-Code: 0123ABCD
```

响应中的 `ticket` 需立即兑换：

```http
POST /api/v1/session
X-PaperMono-Ticket: 0011223344556677
```

上传使用原始文件作为 body，并携带：

```http
X-PaperMono-Name: %E4%B9%A6%E5%90%8D.txt
X-PaperMono-Kind: txt
X-PaperMono-CRC32: 89ABCDEF
```

`X-PaperMono-Kind` 可取 `txt`、`epub`、`ics`、`tasks`、`note`、`weread`；
CRC32 可省略。`weread` 类型只接受固定文件名 `weread-v1.json`。
内置网页使用 `b64:` 加无填充 base64url 编码传输 UTF-8 文件名；接口仍接受
较短文件名的百分号编码。255 字节 UTF-8 名称编码后不超过 340 字节，能够在
Zephyr 的 512 字节单头上限内完整传输。若提供 CRC32，则必须是 1 至 8 位
十六进制数，非法值会直接拒绝。
文件写入同目录隐藏的 `.upload/` 事务目录，完成长度、CRC 和格式校验后才替换
目标文件。若原文件存在，先移入 `.backup/`，新文件原子替换成功后删除备份。
事务目录与文件名是不同的 FAT 路径组件，因此完整支持 255-byte UTF-8 原文件名，
不需要为临时后缀缩短书名。启动挂载时会删除残留上传并在目标缺失时恢复备份；
升级后也会继续清理旧版 `.<name>.upload/.backup` 格式的残留文件。

TF 卡在上传、同步落盘或格式验证期间被拔出时，存储层会撤销当前活动事务、
关闭已打开文件并删除 `.upload`；已经取得事务句柄的后续写入、提交和中止都
返回 `-ENODEV`，不能在重新插卡后误提交旧事务。`fs_sync()` 失败时仍会执行
`fs_close()`，并保留第一个错误返回，避免错误路径泄漏文件句柄。

内容上传和 Wi-Fi 配置各自只允许一个活动请求拥有事务缓冲。后到的并发请求
返回 `409 Conflict`，超限的 Wi-Fi 请求返回 `413 Payload Too Large`；它们不会
写入、提交或中止另一请求的临时文件/缓冲。连接中止时只有事务所有者能够执行
清理。内置网页以 XHR 显示上传进度，阻止重复提交，并在 HTTP/网络错误时保留
可操作的中文错误信息。

Wi-Fi 配置 body：

```json
{"ssid":"local-network","psk":"minimum-8-chars"}
```

个人状态导出/导入模型：

```json
{
  "version": 1,
  "tasks": [{"id":"task-id","done":true}],
  "notes": [{"id":"note-id","favorite":true,"read":false}]
}
```

这些 overlay 通过稳定 ID 与重新导入的源内容合并，不会因为覆盖 ICS、JSON 或
便签文件而无条件清空。

## 微信读书实验书架

第一阶段只导入不含 Cookie、token 和正文的规范化书架摘要。设备不会连接微信
读书私有接口，也不会下载或解码云端章节。文件格式：

```json
{
  "version": 1,
  "source": "desktop-companion",
  "synced_at": "2026-08-31T18:30:00Z",
  "total": 2,
  "books": [
    {
      "id": "stable-book-id",
      "title": "书名",
      "author": "作者",
      "kind": "book",
      "progress": 42,
      "reading_seconds": 5400,
      "updated_at": "2026-08-31",
      "finished": false
    }
  ]
}
```

`kind` 可取 `book`、`audio`、`articles`；`progress` 为 0–100。`id` 和
`title` 必填，其他书籍字段可省略。最多在屏幕显示 32 条记录，超出部分会明确
标记为已截断；`total` 必须不小于实际保留的记录数。未知字段会跳过，以便格式
向后扩展。

## BLE 小数据同步

BLE 同步服务只接受经过认证的连接，使用以下 128-bit UUID：

| 对象 | UUID 前缀 |
| --- | --- |
| Service | `c1530001-6f4d-4f6e-6f53-796e63000001` |
| Control (Write) | `c1530002-6f4d-4f6e-6f53-796e63000001` |
| Data (Write) | `c1530003-6f4d-4f6e-6f53-796e63000001` |
| Status (Notify) | `c1530004-6f4d-4f6e-6f53-796e63000001` |

Control START 是小端二进制：

```text
offset  size  field
0       1     op = 1
1       1     kind: 1=ICS, 2=tasks, 3=note, 4=overlay state
2       4     total size
6       4     CRC32; 0 表示不校验
10      1     UTF-8 file-name length
11      N     file name
```

随后按每包不超过 220 字节写 Data，最后写单字节 Control `2` 提交；Control
`3` 中止。断线会自动中止并清理临时文件。状态通知共 11 字节：操作码、16 位
有符号 errno、已收字节数和期望字节数。overlay 最大 8 KiB；TXT/EPUB 大文件
不允许走 BLE，必须使用 Wi-Fi。

## 线程边界

- HTTP 回调只流式写入内容事务或投递网络工作。
- GATT 回调只把有界消息写入 `sync_messages` 队列。
- 内容验证和提交在存储 worker 中执行。
- 完成后只调用 `papermono_ui_request_content_refresh()` 投递 UI 消息，不直接
  访问 LVGL 或 SSD1677。


## RSS 订阅源

`GET /api/v1/rss/sources` 与 `PUT /api/v1/rss/sources` 均需
`X-PaperMono-Session`。地址可能包含私有订阅令牌，不通过未配对接口返回。

GET 返回 `max_sources`、`generation` 和 `sources`。每源是稳定 `id`、`name`、`url`：

```json
{"max_sources":4,"generation":1,"sources":[{"id":"gold","name":"金价","url":"http://rss.example/gold.xml"}]}
```

PUT 提交完整列表；新增、修改、移除与排序均通过同一原子保存接口完成：

```json
{"sources":[{"id":"gold","name":"金价","url":"http://rss.example/gold.xml"}]}
```

允许 `{"sources":[]}`。最多 4 源，ID 最多 15 字节、名称最多 47 UTF-8 字节、URL 最多
191 字节。名称支持原始 UTF-8 及合法 JSON Unicode 转义；拒绝 NUL、非法代理项和控制字符。
只支持可解析的 `http://` 地址，不支持 HTTPS、userinfo 或片段。重复 ID/地址被拒绝。

成功返回 `200 {"ok":true}`，写入并校验 SD 注册表后才替换运行配置。失败保留原列表：
未配对 `401`；其它请求占用或读取时配置变化 `409`；格式/校验错误 `400`；请求超过
4096 B `413`；流式请求超过期限 `408`；TF 未就绪或存储失败 `503`。未完成的未授权、
过大请求会直接关闭连接，停止发送数据的请求在 30 秒截止后关闭；只有进入最终回调时
才返回上述 JSON 错误。授权在首块绑定请求客户端，后续块不重新使用已被 HTTP 栈清除
的头。终止、过大、超时请求清理私有地址缓冲，并由 HTTP 栈释放占用后接收新请求。
