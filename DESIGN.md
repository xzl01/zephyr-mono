---
name: PaperMono Field Notes
description: A quiet one-bit paper interface for daily information, local reading and device controls.
colors:
  ink-black: "#000000"
  paper-white: "#ffffff"
typography:
  clock:
    fontFamily: "Libre Bodoni"
    fontSize: "148px"
    lineHeight: "117px"
  headline:
    fontFamily: "Source Han Serif SC Regular"
    fontSize: "48px"
    lineHeight: "55px"
  state:
    fontFamily: "Source Han Serif SC Regular"
    fontSize: "40px"
    lineHeight: "50px"
  body:
    fontFamily: "Source Han Serif SC Regular"
    fontSize: "24px"
    lineHeight: "30px"
  label:
    fontFamily: "Source Han Sans SC Normal"
    fontSize: "16px"
rounded:
  surface: "0px"
spacing:
  side-margin: "22px"
  list-gap: "8px"
  menu-gap: "4px"
components:
  launcher-tile:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    rounded: "{rounded.surface}"
    width: "136px"
    height: "180px"
  detail-header:
    textColor: "{colors.ink-black}"
    typography: "{typography.headline}"
    width: "436px"
    height: "54px"
  list-row:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    rounded: "{rounded.surface}"
    width: "436px"
    height: "104px"
  menu-row:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    rounded: "{rounded.surface}"
    width: "436px"
    height: "76px"
  button-primary:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    rounded: "{rounded.surface}"
    width: "436px"
    height: "64px"
  button-secondary:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    rounded: "{rounded.surface}"
    width: "436px"
    height: "64px"
  import-button:
    backgroundColor: "{colors.paper-white}"
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    rounded: "{rounded.surface}"
    width: "352px"
    height: "64px"
  return-footer:
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    width: "436px"
    height: "76px"
  empty-state:
    textColor: "{colors.ink-black}"
    typography: "{typography.state}"
    width: "436px"
    height: "334px"
  frontlight-setting:
    textColor: "{colors.ink-black}"
    typography: "{typography.body}"
    width: "436px"
    height: "100px"
---

# Design System: PaperMono Field Notes

## Overview

**Creative North Star: "Field Notes"**

纸白、衬线时钟、细横线和独立图标索引构成安静的日常手记；首页强调正在使用的
内容，阅读页把空间留给正文。列表、设置、内容传输、遥控器和电源页沿用同一
纸面语言，以清晰的状态、操作和返回路径组织信息。

这是固定 480 × 800 的 Zephyr/LVGL 界面。固件屏幕保持 I1；字体为 1 bpp，
图标虽以 A8 存储但只有 0/255 透明度。无灰阶、渐变、阴影或抖动模拟。
视觉参考规定方向，显示内容来自应用模型；参考中的示例数据不进入固件。

**Key Characteristics:**

- 纸白为主、黑色细线分区，靠字号与留白形成层级。
- 衬线标题、正文和状态标题配合紧凑的无衬线元数据。
- 原创几何图标对应应用、内容类型、确认动作和缺少内容的状态。
- 可见控件与命中区域共用布局，确认取消回到操作入口。

已确认参考包括 `docs/design-references/papermono-field-notes-v1.png`、
`docs/design-references/field-notes-expanded/` 和 `docs/design-references/field-notes-remaining/`。
最终原生渲染与出处记录位于 `docs/design-references/field-notes-applied/`：42 张
480 × 800 I1 截图及 overview-1 至 overview-8 来自实际 UI/LVGL 主机渲染，
硬件与内容数据是合成 fixture，不是设备照片。软件检查、构建及实机验收边界见
该目录的 `validation.md`。

## Colors

Paper White 是工作底色；Ink Black 用于文字、细线、图标和紧凑进度标记。
层级依靠字号、留白和线重，普通导航与变化信息保持黑字白底。

**The Paper Field Rule.** 纸白保持主要工作面；整块黑色只用于紧凑标记与图标内部，
不把普通导航或状态行改成大面积反色。

## Typography

- **Clock:** `papermono_clock_148`，Libre Bodoni 148 px；字形行框 117 px，
  首页标签框 440 × 122。RTC 无效时使用 Montserrat 48 的 `TIME` 和明确校时提示。
- **Headline:** `papermono_heading_48`，Source Han Serif SC 48 px、55 px 行框、
  基线 14 px；覆盖当前全部页头和确认标题使用的中文字符及 ASCII。
  这是固定标题子集，不是任意中文大字号字体。
- **State title:** `papermono_state_40`，Source Han Serif SC 40 px、50 px 行框、
  基线 10 px；覆盖当前书库、微信读书、日历、待办、便签、信息、书签和 Wi-Fi 空状态的标题。
- **Body:** `papermono_reading_24`，Source Han Serif SC 24 px、30 px 行框，
  基线 8 px；完整覆盖基础 CJK，另含 ASCII、常用标点、全角范围及 `°`、`℃`。
  用于阅读正文、首页内容标题、应用名称、列表主文与按钮；缺字回退到 `papermono_cjk_16`。
- **Label:** `papermono_cjk_16`，Source Han Sans SC 16 px，用于状态和紧凑说明；
  Montserrat 18 用于 `PAPERMONO` 字标。旧 28/58/96 px 字库已从构建中移除，
  它们既无引用又占用约 132 KiB Flash。

字体范围、来源和生成方式见 `docs/font-licenses.md`。不要用 emoji 或 Unicode
符号代替可追溯图标。

**The Subset Coverage Rule.** 新增页头、确认标题或空状态标题时，同时扩展对应
生成子集并验证字形覆盖；不要用正文回退掩盖缺失的大字号标题。

## Layout

绘制和命中区域共享 `include/papermono/ui_layout.h`；正文分页和排版共享
`include/papermono/reader_layout.h`。坐标来自实现，不要求旧版的统一 4 px 格。

- **首页：**字标 `(22,14)`，状态右对齐至 `x=458`，电压在 `y=38`；日期或
  校时说明在 `y=66`，有效时钟在 `(18,94)`。三条内容区分别为
  `(22,220,436,216)` 阅读、`(22,436,436,120)` 日程、
  `(22,556,436,168)` 待办，区首使用 1 px 横线。
- **应用索引：**标题 `(22,36)`，页码 `(346,48)`，横线 `y=94`。
  3 × 3 独立描边格从 `(22,124)` 开始，每格 136 × 180，列间距 14、行间距 16；
  覆盖 `x=22..457, y=124..695`。图标 96 × 96，格内顶部偏移 22，名称在
  局部 `y=138`。11 个应用分两页，第二页仅设置与电源，空格不绘制且不可操作。
- **底部导航：**首页、应用、设置各占 160 px；命中区域从 `y=724` 至屏幕底部。
- **通用详情页：**左右边距 22，内容宽 436；字标和右对齐日期在 `y=18`，
  标题 `(22,58,436,54)`，横线 `y=116`。常规列表从 `y=148` 开始；
  页脚横线 `y=724`，返回文字在 `(42,742)`，右侧操作提示允许两行。
  整个底部 `y=724..799` 都用于详情页返回。
- **列表与菜单：**通用列表为五行，每行高 104、间距 8；阅读菜单为七行，
  每行高 76、间距 4。微信读书在书架计数与同步时间下方从 `y=186` 开始，
  五行各高 98、间距 6。图标行的图标在局部 `(14,12)`，主文从 `x=76` 开始。
- **确认：**96 × 96 动作图标在 `y=216` 居中，标题在 `y=352`；确认与取消
  分别为 `(22,556,436,64)`、`(22,634,436,64)`，两按钮之间保持 14 px 空隙。
- **空状态：**通用焦点图标 176 × 176，在 `y=180` 居中；状态标题在
  `(22,384,436,50)`，恢复说明在 `(32,450,416,64)`。微信读书分别提前至
  `y=174/376/440`，其导入按钮为 `(64,490,352,64)`，说明与只读摘要区继续置于下方。
- **设置：**日常偏好、连接和设备分组分别使用共享行矩形。行的 `y/height`
  依次为休眠 `174/56`、时间 `230/56`、前光 `286/100`、Wi-Fi `424/60`、
  蓝牙 `484/60`、恢复 `582/60`、电源 `642/60`；所有行均为 `x=22,width=436`。
  工具页首项“系统与触摸”同样共享 `(22,148,436,104)` 的绘制与点击范围。
- **阅读页：**标题 `(22,22)`、宽 336；右上目录/菜单 `(380,12,78,40)`；
  头部横线 `y=66`。正文从 `y=88` 开始，高 636，底部在 `y=724`。
  页脚分隔线 `y=742`，页码与书签在 `y=758`，进度底线在 `y=790`。

新增的设备端 Wi-Fi 流程沿用同一详情页页头、细横线、列表和返回页脚。
“忘记网络”和存储格式化等确认使用共享的 `detail_confirmation`：96 px 图标、
`y=352` 的 48 px 居中标题、`y=434` 的说明和原有确认/取消矩形。新标题必须
纳入标题字库；“存储”“格式化 TF 卡？”和“确认忘记网络？”均已覆盖。

Wi-Fi 空结果保留上方已保存网络与扫描操作，焦点图标为 176 px、从 `y=306`
开始，40 px 状态标题在 `y=502`，恢复说明在 `y=560`；不把已有网络操作覆盖
成独立的全屏空状态。密码键盘保留 44 px 字符键及其既有间隙：字母、符号与
底排功能键使用 24 px 衬线字；窄字符键中的双字“空格”使用 16 px，避免裁字。
可见键与命中区域始终来自同一输入模型，密码缓冲与提交逻辑不因排版变化改变。

2026-10-21 的一致性复核、32 个真实 UI/I1 主机渲染和修正前后对比见
`docs/design-references/field-notes-review-20261021/`。它们使用合成数据，不能替代
真机显示、触摸、联网或休眠验证。

阅读密度与分页同步：紧凑为边距 16、行间距 4、16 列 × 18 行；标准为
24、10、16 × 16；宽松为 32、14、15 × 14。每行另保留两个收尾标点字格，避免禁则溢出导致 LVGL 二次换行。顶部 72 px 和底部 58 px 为
独立操作区；中部左右各 160 px 翻页，中间 160 px 打开菜单。

**The Shared Geometry Rule.** 页面使用自己的共享矩形或网格同时绘制和命中，
间隙与未绘制项目不可操作；阅读页保留独立头尾与分页坐标，不套详情页布局。

大字号时钟在绘制时需要 A8 字形缓冲。LVGL 专用池为 64 KiB，位于 PSRAM；
主机预览也限制为 64 KiB。主机为 64 位 ABI，设备为 32 位，预览分配统计不能
替代实机堆余量验收。该平台没有响应式断点或滚动式页面密度切换。

## Elevation & Depth

界面完全平面化。细横线分段、轮廓分组和空白建立层级，不增加阴影、半透明层
或瞬时按压动画。确定坐标和字形减少无意义的墨水屏变化。

## Shapes

矩形内容面与应用格为直角；应用格 1 px 描边，首页书脊为 2 px 描边。
详情列表以底部 1 px 横线分段，不为每行增加完整卡片框；选中行用窄黑标记。
实际按钮才使用完整描边，主操作 2 px、次操作 1 px，均保持黑字白底。
图标内部允许符合物体语义的圆弧和圆角，例如遥控器、日历装订环和电源符号；
前光离散档位也用圆点表示，不把它们扩展为面板圆角规则。

## Components

### Daily brief

阅读入口用描边文字书脊和真实书名，不伪造封面或作者。没有最近阅读时显示
“打开书库”和导入说明；已存在记录但未加载对应页面时显示“已有记录”。
只有加载了同一书籍的页面才显示其近似进度。电池显示实测电压或 `--`，不推算百分比。

下一日程来自有效 RTC 与应用查询，跨日事件附日期；未校时与暂无日程各自说明。
待办显示最多两条未完成任务，空状态区分全部完成与没有任务。任务图标内的勾号
是固定识别图形，不代表真实任务完成状态。

### Application index

每个应用使用原创几何图标，统一独立描边格和居中名称。日历图标内叠加真实日号，
RTC 无效时为 `--`；页数由应用表计算。内容中心采用本地传输图形，不暗示云存储。

### Reading surface

书名为截断单行标题，正文使用衬线字与共享密度配置。页脚显示实际当前页和
近似百分比，不显示未知总页数；书签文字反映当前页状态。读取失败时呈现错误
说明，不保留成功进度条。底部左侧打开菜单，右侧操作书签。

### Ruled detail pages

书库、日历、待办、便签、信息卡片、微信读书、阅读菜单/目录/书签、内容中心、
遥控器、工具、系统、设置、电源与确认页均已应用扩展参考的纸面布局。标题、
主文、元数据按固定层级呈现；内容中心和系统用横线分组，列表用有间距的规则行。
任务复选框反映实际完成状态，目录缩进反映实际层级，信息行显示来源、状态和更新时间。

微信读书书架按电子书、有声书、文章使用书本、耳机、文档图标；书签、折角便签文档、
信息与阅读菜单使用各自的 48 px 图标通道。作者、进度和同步时间来自本地摘要，
未知值明确说明，详情书脊不伪造书封。空状态以大图标、40 px 标题和简短恢复说明
构成完整页面，支持格式、导入入口及离线缓存说明保留独立位置。
微信读书详情的已提供百分比使用 48 px 数字；没有进度时，“未提供进度”使用
完整覆盖中文的 24 px 主文字体，不能把该动态说明交给有限的标题子集。

### News refresh

资讯沿用无品牌页头布局：顶部订阅标签替代旧订阅名称行，共享区域为
`(22,12,436,44)`，按实际订阅数等分；默认 Gold、AI，最多四个独立来源。
列表仍从 `y=80` 起最多八行，更新时间及同步结果保留在 `y=56` 元数据行。
切换标签读取对应来源的缓存、状态和修订；第一页触摸下拉只同步当前来源。
失败保留已有条目与时间，显示实际原因和重试入口；无订阅和空内容分别说明。
第二页下拉仍回上一页，正文和实体键继续翻页。页脚保持「下拉刷新 · 长按返回」，
不加入拖动动画或额外刷新对象。

文章保持无品牌页头的正文布局。HTML 表格与前后文字保持文档顺序：首列固定，
每组最多搭配两列数据并重复表头；宽表分列组阅读，长行沿完整文字行续页，
不靠缩小字号容纳。只创建当前页对象并共享 64 KiB 正文；有界 PSRAM 索引不足
时明确提示并回退到完整纯文本，不静默丢失表格内容。

内容中心网页沿用现有管理表单与认证方式，支持订阅名称/地址新增、编辑、删除及
整组保存；窄屏纵向排列字段。操作反馈区分读取、保存及错误；慢速重新加载期间
发生的新编辑保留在表单中，并明确提示，避免旧响应覆盖草稿。

最终参考位于 `docs/design-references/rss-multisource-20261028/`：18 张原生与
网页状态截图，来源见 `SOURCES.txt`、`WEB-SOURCES.txt`。原生图为实际 LVGL I1
主机渲染，内容与连接状态为合成 fixture；网页图用于浏览器表单检查。它们不代表
实机显示、触摸、网络或残影验收。

### Device actions and confirmation

遥控器只有在 HID 已初始化、已连接、已安全配对、主机未挂起且无可见配对码时
才显示并接受发送控件。等待连接、配对和恢复时展示指导文字，模式标签和返回仍可使用；
失败信息来自实际发送或连接结果，不用成功式提示覆盖错误。

启用语音输入时，遥控器标签行由三格 140 px 改为四格 103 px、间距 8 px，新增
「语音」页：436 × 250 描边语音区、几何麦克风与 24 px「语音输入」标题，状态
文字来自麦克风工作线程（启动、传音、停止、失败、等待订阅，以及上次会话时长和
字节数）。页面没有计时器和电平表，只在开始、结束与后台刷新时更新；按住语音区
开始传输，松开停止，离开页面、切换模式、返回、回首页、休眠或关机都会停止并
关闭麦克风电源。电脑端需要接收器和虚拟麦克风，设备无法验证，因此只作为提示；
失败与未订阅状态来自实际请求结果，不用成功式提示覆盖。

休眠、关机、清除蓝牙配对和恢复默认设置均使用对应的月牙、电源、蓝牙和恢复图标，
以及分离的确认/取消按钮。恢复默认设置先长按，再确认。
取消、返回手势和返回页脚清除确认，并回到入口：蓝牙/恢复操作回设置，休眠/关机回电源。
回首页或离开电源路由也会清除未完成确认。Wi-Fi 配网及忘记确认由下述独立路由处理。

前光以五个离散档位表示 0/25/50/75/100%，当前档位是实心圆点。
设置与阅读菜单都显示模型中的已应用偏好；调整失败显示失败，不显示未保存的成功状态。

**The Actual State Rule.** 用户内容、计数、日期、连接状态、读数、错误和进度只能来自
当前应用模型；只有实测电压，缺失读数用 `--`，不从参考图补写状态。

### Standby page

进入休眠前绘制一帧静态画面，保持到唤醒。**不显示时钟**：画面冻结后时间立即失真。
版式沿用 22/458 边距与 116/724 锚点：Bodoni 大号"日"、月份与星期、右侧节气/纪念日栏、
天气行（带数据时间）、固定 424–640 内容带、下一日程与唤醒提示。内容带按设置切换
（摘抄 / 未来 8 小时 / 阅读进度 / 明日预告 / 留白），其余元素坐标不随选择改变。
摘抄使用内置公版短句按日轮换；节气只展示内置表覆盖的年份，表外不猜测。

### On-device Wi-Fi setup

设置中的「Wi-Fi 网络」打开同一条设备端配网路由，列表、密码和忘记确认作为内部状态，
返回逐级退出，不借助浏览器。沿用「连接设置」48 px 标题；四行 436 × 76 网络列表
复用绘制/命中几何，展示真实 SSID、安全类型和信号信息。忘记网络使用独立确认与取消，
取消回到网络列表，不清除已保存配置。

密码键盘字符键为 44 × 60、底栏为 88 × 60，黑白描边，大小写、符号、退格、清空与显示
均有明确文字。完整密码以 16 px 字体在固定三行区域显示或遮罩，不截掉前部/最新字符。
后台扫描与连接不会重置草稿。未配置或互斥处理中不可用的操作取消按钮描边，不接收动作；
无上一页/下一页时隐藏对应按钮。失败和成功空列表分别呈现，不把扫描失败归因于路由器。

触摸唤醒已关闭，仅侧键与电源键可唤醒（面板贴纸/布料时误触过频）。原生预览入口为 `tests/preview/build.sh`；其截图使用合成网络状态，不证明设备端触摸或联网。

### Asset and preview provenance

33 个原创二值透明度 A8 图标分为 17 个 96 px 应用/电源/确认符号、10 个 48 px
图标通道和 6 个 176 px 空状态焦点符号。生成脚本、C 构建输入和许可证记录见
`docs/font-licenses.md`。`.impeccable/design.json` 的 HTML/CSS 是原生组件的网页
展示片段；它不参与固件构建，也不替代实际 I1 截图或设备验收。

## Do's and Don'ts

- **Do** 从应用模型读取书名、日期、电压、任务、页码、书签和近似进度。
- **Do** 同时调整绘制、命中区域与分页配置，避免两套坐标。
- **Do** 保持纸白背景、细线和可读的 1 bpp 字体。
- **Do** 在缺少内容、连接恢复和操作失败时给出实际状态及可用的返回或恢复入口。
- **Do** 使用动作与内容类型对应的原创图标，并保留主文和元数据通道。
- **Don't** 恢复旧版共享边框网格、Phosphor 猫首页标记或卡片式时钟。
- **Don't** 补写作者、总页数、电量百分比或装饰性状态数值。
- **Don't** 将主机预览、构建或测试结果当作 C153 实机显示、触摸、残影或堆内存验收。
- **Don't** 在未完成安全连接、主机挂起或显示配对码时呈现可发送的遥控操作。
