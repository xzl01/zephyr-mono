# 2026-10-21 内容上传的两个实机缺陷

验收内容中心 HTTP 端到端时，在真机上发现两个会直接影响用户的问题。

## 1. 大文件上传在验证阶段失败（已修复）

**现象**：`POST /api/v1/content` 对 ≤500 B 的文件返回 201，对 ≥1 KB 的文件返回
`400 {"error":"commit failed","code":-5}`，设备日志为 `fs: file read error (-5)`。
EPUB 无论大小都失败。

**定位**：验证缓冲（`validation_buffer`、ZIP 的 EOCD/输入缓冲）放在 PSRAM
（`.ext_ram.bss`）以节省内部 DRAM。ESP32-S3 的 SDHC/FATFS 路径把超过 512 字节的
读取直接落到调用者缓冲；目标在 PSRAM 时底层返回 `-EIO`。把 4 KiB 编码采样缓冲
临时改到内部 RAM 后，1 KB/4 KB/20 KB 的 txt 全部 201 —— 确认是缓冲位置问题，
不是容量或 FAT 损坏。

**修复**：新增 [papermono_fs_read.c](<src/papermono_fs_read.c>) 的
`papermono_fs_read_chunked()`，用 512 B 内部 RAM 弹跳缓冲分块读取并拷贝到目标。
内容验证、EPUB ZIP 读取、阅读器、阅读缓存、WeRead 目录、组织器读取全部改用它。
主机测试 [test_fs_read.c](<tests/host/test_fs_read.c>) 模拟“大于一个窗口的读会
-EIO”，断言每次底层请求 ≤512 B 且大数据仍能完整读出。

**实机验证（TXT，经 HTTP）**：修复后的验证镜像上 1 KB / 4 KB / 20 KB txt 上传
均为 201；`GET /api/v1/content` 列表包含这些条目，`DELETE` 后消失，
未带会话返回 401，空 body 上传返回 411。

**实机验证（EPUB，设备内自检）**：网络复测受阻，改用设备内自检镜像走同一条
导入事务（import_begin → write → commit → delete）。内嵌一个确定性最小 EPUB
（[生成脚本](<scripts/generate_selftest_epub.py>)、
[生成文件](<src/generated/papermono_selftest_epub.c>)），实机日志：

```text
HIL content import check passed
papermono_epub: EPUB opened: Selftest (1 spine items)
HIL EPUB import committed, validated and removed
...
HIL automatic checks passed
```

说明 EPUB 校验、ZIP 读取（含分块弹跳读取）与存储事务在真机上可用；
尚未覆盖的是 HTTP 传输层把 EPUB 字节送到设备这一段。

## 2. STA 连接失败后不回退 SoftAP（已修复并实机验证）

**现象**：设备预置家庭 Wi-Fi 后启动，日志出现
`Wi-Fi station disconnect event (status=-1)`，随后既不连接路由器，也不广播
SoftAP，设备完全不可达（USB 仍在、CPU 在 `arch_cpu_idle`）。

**影响**：用户一旦配置过家庭 Wi-Fi，而路由器暂时不可用/密码变更，设备将失去
重新配网的入口。当前代码只在 `connect_station()` **同步返回错误**时才启用 AP，
连接请求被接受但随后断开（异步事件）时不会回退。

**修复**：

- `NET_EVENT_WIFI_DISCONNECT_RESULT` 不再依赖事件里的 `success`
  （实测断开事件常带 `status=-1`）：一律把 station 标记为未连接，AP 未启用时调度
  恢复 AP。
- 新增 `station_retry` 延迟工作：已保存凭据且未连接时按
  5 s → 10 s → 20 s → …（上限 300 s）退避重连；连接成功清零退避并取消重试。
- 设备保持 SoftAP，用户始终有配网入口。

**实机验证**（无效路由器凭据镜像，生产代码路径）：

```text
Wi-Fi station disconnect event (status=-1)
Wi-Fi station retry scheduled in 5000 ms
SoftAP enable complete (status=0, channel=6)
SoftAP DHCP server ready at 192.168.4.1
SoftAP interface verified: SSID PaperMono-TEST, channel 6
... 下一次重试退避为 10000 ms
```

**尚未闭环**：AP 启用后再重连 STA 在当前路由器上没有成功（连续 50 s 未重新
入网），需要单独排查 APSTA 并存时的重连路径；EPUB 上传复测也因此未完成。

## 复现环境

- 验证镜像：固定配对码 `TESTCODE`、固定 AP 密码、预置 2.4 GHz STA 凭据（仅验证用，
  未进入生产镜像）。
- 设备经路由器动态获取 `192.168.8.198`，开发机经以太网访问。
- 生产镜像已恢复为随机凭据版本，包含本轮启动与摘要修复。

## 3. DELETE 返回 405（已修复）

**现象**：`DELETE /api/v1/content/<id>` 恒返回 `405 Method Not Allowed`，
而 `GET`/`POST /api/v1/content` 正常。

**根因**：Zephyr HTTP 服务器的通配匹配使用 `fnmatch(..., FNM_PATHNAME |
FNM_LEADING_DIR)`，因此精确资源 `/api/v1/content` **也会匹配**
`/api/v1/content/<id>`。资源按链接顺序遍历时精确资源先命中，它的方法位掩码
只有 GET|POST，于是返回 405，通配的 DELETE 资源永远轮不到。

**修复**：让内容资源本身声明 `GET | POST | DELETE`，并在
`content_endpoint_handler()` 中把 DELETE 分派给 `delete_handler()`；
通配资源保留作为兜底。删除处理按 URL 末段解析 id，两条路径等价。

**实机验证**（手机热点、2.4 GHz）：

```text
status                     -> 200  station_ipv4 10.95.243.134
pairing ticket             -> 200
session                    -> 200
upload hs-note.txt (25 B)  -> 201
upload hs-big.txt (10400 B) -> 201
upload hs-book.epub (1020 B) -> 201
upload hs-empty.txt (0 B)  -> 411 content-length required
list                       -> 200  ... hs-note, hs-big, hs-book
delete hs-note             -> 200  {"ok":true}
list after delete          -> 200  ... hs-big, hs-book
list without session       -> 401  pairing required
```

至此内容中心 HTTP 端到端（含 EPUB 上传与删除）在真机通过。
