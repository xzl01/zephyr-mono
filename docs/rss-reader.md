# RSS 资讯阅读

固件内置 RSS 2.0 订阅，明文 HTTP 抓取，列表与正文两个页面。实现拆成数据层
（`src/papermono_rss.c`）和 UI（`PAPERMONO_APP_NEWS`）。

## 配置

订阅地址存在 Settings 的 `papermono/rss_url`，默认值来自
`CONFIG_PAPERMONO_RSS_URL`（`prj.conf`）。当前默认：

```
http://192.168.100.122:8080/rss/<token>/ai-news.xml
```

只支持 `http://`，`https://` 明确返回 `-EPROTONOSUPPORT`。改动不需要重新编译，
设置接口见 `papermono_settings_set_rss_url()`。

## 数据层

- 后台线程：等待 IPv4 就绪 → 抓取 → 成功每 30 分钟刷新，失败 2 分钟后重试；
  启动期最多 10 次、每次间隔 20 秒。
- 抓取前若默认路由缺失，用 DHCP 网关补装默认路由（与天气同因同治）。
- 手写 XML 扫描器：CDATA、named/decimal/hex 实体、HTML 标签剥离、空白折叠、
  UTF-8 安全截断；畸形输入只降级不崩溃。
- 快照、暂存、72 KB 响应缓冲与 worker 栈都在 PSRAM（`.ext_ram.bss`），
  内部 DRAM 只增加约 200 B——内部 DRAM 已到 84.96%，后续加功能前必须先算预算。

## UI

- 入口：启动器第 1 页「资讯」。
- 列表：feed 标题、更新时间、页码，6 条/页；条目标题 24 px 单行省略，
  日期 16 px；侧键/触摸上下翻页。
- 正文：标题 + published + 24 px 自动换行正文，按可视区高度整页位移；
  点按回列表，长按/右滑/底栏返回。
- 空态：`正在同步资讯`；失败且无缓存：`暂无资讯 · 请检查网络后重试`。

## 已知限制

- `papermono_rss_item` 没有 `link` 字段，抓到的链接目前不上屏；需要"打开原文"
  时再扩展结构体。
- 正文被 `PAPERMONO_RSS_BODY_MAX`（511 字节）截断，长文只能看到开头。
- RSS 是明文 HTTP，可被中间人篡改；只做展示，不参与任何决策。
- worker 线程栈放在 PSRAM 是首次使用；实机抓取已验证，若后续怀疑栈问题，
  可改回内部栈，但需要先腾出 5 KB 内部 DRAM。

## 验证

- 主机：`sh tests/host/run.sh`，含 `rss feed parsing` 12 组用例。
- 固件：0 warning / 0 error，内部 DRAM 84.96%。
- 实机日志：

```text
papermono_rss: RSS thread started
papermono_rss: RSS using DHCP default router 192.168.8.1
papermono_rss: RSS attempt 1/10 failed (-2)
papermono_rss: RSS feed "AI新闻": 4 items (58651 bytes)
```

- 预览：`docs/design-references/news/`（news / news-detail / news-empty / news-error）。
- 预览 fixture 代码目前在并行分支 `auto/paper-index-refresh`（主线 preview 文件正被
  另一任务改动）；拍照产物已随本提交入库，合并该分支后 fixture 会回到主线。
