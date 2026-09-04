# CrossMux / FreeInk 参考边界

本项目参考 CrossMux 和 FreeInk SDK 的产品级分层思路，但保持 Zephyr、LVGL、
现有板级驱动和本地优先内容模型，不直接移植 Arduino/ESP-IDF 业务层。

## 已采用的设计

- 将阅读页索引视为可重建缓存，与用户的阅读进度和书签分开。
- 索引采用定长小端格式、版本、书籍/排版签名和头部校验；损坏或版本不
  匹配时重建，不会让缓存错误阻断阅读。
- 统一应用动作、输入来源和触摸命中区域，让实体键与触摸路径保持一致。
- 统一分类触摸手势：左边缘右滑返回、底边上滑回首页；阅读页保留普通滑动翻页，
  并按子页面层级处理返回。沿用参考实现的连续阈值：短按释放位移 59 px 仍为点击，
  60 px 起才进入滑动，消除轻微手指滚动造成的 31–59 px 输入死区；点击命中使用
  首次接触点，滑动方向使用最后触点。
- 对照 FreeInk 的 PaperMono 板级实现，将 FT6336 的 M5IOE1 供电/复位、启动重试、
  独立线程 8 ms 完整帧轮询、坐标边界校验和陈旧触点释放集中到专用 Zephyr 驱动；不会用
  全零芯片 ID 错误判定控制器缺失。FreeInk 的换轴/翻转发生在 800x480 面板坐标
  层，合成回 480x800 竖屏逻辑坐标后仍是控制器原始 x/y，Zephyr 端不能再翻转 Y。
- 采用“刷新任务与输入快照分离”的原则：SSD1677 高压刷新期间继续采样触摸，
  首个已完成手势在刷新结束后投递，避免短按被 0.7–4.3 秒刷新吞掉。
- C153 的触摸唤醒继续使用本项目的 Zephyr EXT0 路径；CrossMux 当前 Paper Mono
  关机路径依赖 M5PM1 电源键冷启动，并不能替代触摸唤醒验收。ESP32-S3 上必须
  单独配置 GPIO4 的 RTC IO 上拉，不能只依赖运行态数字 GPIO 上拉。
- 为 TXT/EPUB 使用简体中文行首/行尾标点禁则，并用主机测试固定分页行为。
- 网络失败不破坏当前 UI 状态；保存网络不可用时恢复本地 AP，并采用退避重试。
- 页面状态没有变化时不提交墨水屏刷新；单本书打开失败时保留可用书库列表。
- 单色小字号统一使用带 ASCII 的 1-bit CJK 字体，并通过设备帧缓冲还原检查
  480 x 800 页面，避免抗锯齿细笔画和 LVGL 延迟布局造成断字或正文消失。
- 把应用 ID 与应用内部恢复意图分开保存：只有确实停留在阅读页时才恢复阅读，
  普通进入书库或主动返回列表不会被旧状态劫持。
- 缓存文件只存在 TF 卡；NVS 仍只保存小型设置和用户状态。
- 微信读书先采用“同步侧规范化、设备侧只读”的窄接口：内容中心只接收
  `weread-v1.json` 书架摘要，应用内部管理列表/详情状态，不把远端协议耦合进
  路由、LVGL 或内容存储层。

## 页索引格式

| 区域 | 内容 |
| --- | --- |
| 48 字节文件头 | `PMPI` magic、格式版本、书籍/排版签名、16 字节书籍 ID、FNV-1a 校验 |
| 8 字节索引项 | 页起始偏移 `uint32`、EPUB 章节 `uint16`、保留字段 |

当书籍 ID、文件大小、修改时间、解析器类型、TXT 编码、密度、每行字符数
或每页行数变化时，旧索引自动失效。分支翻页会截断旧后缀再追加，防止页链
中混入过期偏移。

## 不直接采用的部分

- CrossMux 的应用框架和 FreeInk SDK 是 Arduino/ESP-IDF 技术栈，本项目保持 Zephyr
  的线程、Settings/NVS、FATFS、Bluetooth 和 LVGL 集成。
- 当前 FreeInk SDK 与 M5Stack 官方 OTP Demo 均将 Paper Mono 建模为 SSD1677。
  本项目继续使用已实机建立基线的 SSD1677 驱动和 OTP 时序，不跨面板移植
  未验证 LUT。
- 参考项目的 Web 刷机成功与否不属于本次代码验收；当前产物使用 `west flash`
  通过 ESP32-S3 USB Serial/JTAG 写入。
- CrossMux 的微信读书实现会在设备上扫码登录，通过非官方 Web 协议读取书架、
  下载为离线 EPUB 并同步进度，同时把 session、书架和章节缓存放在 TF 卡。
  其上游也明确提示设备构建未校验服务端证书。当前固件不移植这条凭据与协议链；
  若后续增加直连，必须单独完成凭据保护、证书/主机名校验、接口稳定性和合规评审。

## 后续候选优化

1. 增加英文单词与段落孤行规则；中文标点禁则已完成。
2. 为显示调度器增加可查询的刷新统计和最近失败诊断；应用命令携带
   AUTO/FAST/FULL 刷新质量、阅读翻页请求 FAST、驱动维护全刷回退已经完成。
3. 增加主机端页面截图模拟器，在不写入真机时回归 480 × 800 布局。
4. 在确定字体授权、运行时内存和 TF 卡拔出退化策略后，评估可选的 SD 字体。

## 上游链接

- [CrossMux 架构](https://github.com/0x1abin/crossmux/blob/main/docs/contributing/architecture.md)
- [CrossMux 缓存管理](https://github.com/0x1abin/crossmux/blob/main/docs/engineering/cache-management.md)
- [CrossMux 触摸与 UI](https://github.com/0x1abin/crossmux/blob/main/docs/contributing/touch-and-ui.md)
- [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk)
- [CrossMux 使用的 FreeInk SDK 分支](https://github.com/0x1abin/freeink-sdk)
- [M5Stack PaperMono OTP Demo](https://github.com/m5stack/M5PaperMono-OTP-Demo)
- [M5Stack PaperMono UserDemo](https://github.com/m5stack/M5PaperMono-UserDemo)
