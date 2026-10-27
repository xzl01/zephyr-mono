# RSS 资讯阅读

固件内置 RSS 2.0 订阅：抓取只在打开资讯页时发生，正文缓存在 SD 卡，RAM 里只保留
标题索引和当前一篇文章。实现分数据层（`src/papermono_rss.c`）与 UI
（`PAPERMONO_APP_NEWS`）。

## 配置

订阅地址存在 Settings 的 `papermono/rss_url`，默认值来自
`CONFIG_PAPERMONO_RSS_URL`（`prj.conf`）。只支持 `http://`，`https://` 返回
`-EPROTONOSUPPORT`。运行期可用 `papermono_settings_set_rss_url()` 修改。

## 按需加载

- 开机**不联网**：worker 线程创建后阻塞等信号量，`papermono_rss_init()` 只尝试
  读取 SD 索引（2 KB）。
- 打开资讯页时 UI 调 `papermono_rss_request()`：缓存 30 分钟内直接复用，否则后台
  抓取；失败最多尝试 3 次（间隔 20 秒），之后等下一次打开页面。
- 列表只用 RAM 索引；点开文章时才 `papermono_rss_body_load()` 从 SD 读正文到
  单篇 64 KB 缓冲，`lv_label_set_text_static()` 避免 LVGL 再复制一份。

## SD 缓存

目录 `/SD:/papermono/rss/`：

| 文件 | 内容 |
|---|---|
| `index.bin` | 2112 B 固定记录：magic/version/count/updated_at/feed 标题/12×(标题+日期+正文长度) |
| `bodies.bin` | 各正文纯文本按序拼接，按索引长度累加定位 |

写入顺序 `bodies.tmp` → `index.tmp` → rename（带 `.bak` 回滚）；解析失败或写入
中断时保留上一份可用缓存，启动时做 `.bak` 崩溃恢复。

## 正文完整性

- 单篇上限 64 KB（约 2 万个汉字），日报类 6–7 KB 正文完整保存；
- 单篇超限按 UTF-8 边界截断并记日志；缓存总量超过 256 KB 时跳过后续条目并记日志；
- 超过 128 KB 的 feed 会截断尾部条目，需要更大时再改流式解析。

## UI

- 入口：启动器第 1 页「资讯」。
- 列表：feed 标题、更新时间、页码，6 条/页；侧键/触摸上下翻页。
- 正文：标题 + published + 24 px 自动换行，按可视区整页位移；点按回列表，
  长按/右滑/底栏返回；正文读取失败显示错误页。
- 空态：`正在同步资讯`；失败且无缓存：`暂无资讯 · 请检查网络后重试`。

## 内存

- 内部 DRAM：RSS 只增加约 150 B（worker 对象与同步原语）。
- PSRAM：约 142 KB（128 KB 响应缓冲 + 索引/暂存 2 KB×3 + worker 栈 8 KB）；
  UI 侧 64 KB 正文缓冲。旧设计常驻约 430 KB，已移出。

## 已知限制

- 明文 HTTP，可被中间人篡改；只做展示。
- `papermono_rss_item_meta` 没有 link 字段，暂无「打开原文」。
- 预览 fixture 在 `tests/preview`，PNG 见 `docs/design-references/news/`。
