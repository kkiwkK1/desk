# 桌面组件框架

这个框架把 Windows 桌面板的通用能力与具体组件分开：框架提供宿主、组件契约、
绘制、输入、布局规划、配置和系统能力；Account、Servers、PowerShell 及其数据源
属于上层应用。组件用 C++ 源码实现接口，由应用在编译时注册，不需要修改框架源码。

## 分层

```text
应用入口 src/apps/desk/          选择组件、注册工厂、设置应用选项
           │
组件     src/panels/             实现 wg::Module / ModuleT，调用框架接口
数据源   src/data/               具体业务协议、文件格式、终端状态
           │
框架     src/widget/             公共接口 + Windows 宿主实现
```

依赖方向是应用和组件依赖框架；框架不依赖 `src/apps`、`src/panels` 或
`src/data`。`srvctl` 是独立应用，不属于框架。`archive/` 里的旧 Widget/Host
实现仅供历史参考。

框架自己的几个核心接口：

| 头文件 | 职责 |
|---|---|
| `widget/board.h` | `BoardOptions`、`RunBoard`：面板尺寸、单实例、配置和系统选项 |
| `widget/module.h` | 组件契约、尺寸预设声明、键盘/鼠标/热键回调 |
| `widget/size.h` | 尺寸预设表、`MeasureCardSize`、超范围报告 |
| `widget/board_layout.h` | 吸附、卡片矩形、命中测试、单卡落点 |
| `widget/board_plan.h` | 整板布局规划（原子、确定性、可诊断） |
| `widget/board_grow.h` | 最小面积自动增长搜索（8px 宽度步长、右对齐、有界） |
| `widget/host.h` | `BoardHost` 程序化宿主：生命周期、Metrics、PlanPreview、卡片操作 |
| `widget/display.h` | `DisplaySource`：工作区/DPI 来源（系统实现 + 脚本实现） |
| `widget/diag.h` | 线程安全的诊断日志 sink |
| `widget/skin.h` | 视觉令牌与标准行/卡片/控制台原语 |

## 构建与消费

需要 Windows、支持 C++20 的 MinGW-w64 工具链和 Windows SDK。项目内运行：

```bat
build.bat framework    rem 只构建框架 SDK，不编译业务组件
build.bat example      rem 从 SDK 编译独立示例，不启动它
build.bat              rem 构建 desk 和 srvctl
build.bat test         rem 构建并执行回归测试
```

`framework` 生成 `build/sdk/include/widget/*.h` 和 `build/sdk/lib/libwg.a`。
SDK 头文件与静态库必须来自同一次构建，并与消费者使用兼容的编译器、架构和 C++
运行时；当前没有承诺稳定的跨编译器二进制接口。框架面向 Windows，当前不是 DLL
插件系统，也不提供组件热加载。

示例 [examples/minimal-board](../examples/minimal-board) 分成应用入口和独立的
时钟组件；构建时只使用 SDK 头文件和库，不依赖工程中的业务目录。其产物是
`build/bin/minimal-board.exe`。要在自己的工程中消费，可参照示例使用
`-I<SDK>/include`、`<SDK>/lib/libwg.a`，链接 `shell32`、`user32`、`gdi32`、
`ole32`、`uuid`、`winhttp`、`advapi32`、`imm32` 和 `version`；Windows GUI 程序
使用 `-municode -mwindows`。

`build.bat test` 除业务测试外，还会构建并运行这些无窗口用例：
`board-layout-test`、`board-plan-test`、`size-presets-test`、
`board-grow-test`（最小面积增长、右对齐、候选空间暴力枚举交叉验证，以及
审阅者的非单调反例：工作区 2032×972、面板宽 1016、起始高 850，四张随附预设卡
只在高度 934–941 可行时仍返回 1016×934）、
`board-composition-test`（尺寸预设 + 1280×1310 面板上的整板规划）、
`store-test`（精确快照落盘、删除键不复活）、`module-body-test`
（`ClampBodyHeight` 的抬升/夹紧/非法上下界规则）、`display-source-test`
（系统来源 + `ScriptedDisplaySource` 的解析、重读与回退）、`diag-test`
（sink 线程安全、异常吞噬、关闭时 no-op）、`board-host-test`（BoardHost
生命周期/事件顺序/PlanPreview/显示适配/恢复），以及 `render-test` 和
`desk-render-test`。冻结验收：**17 个场景 / 437 项检查 / 0 失败**。

## 注册组件

组件继承 `wg::ModuleT<Data>`（刷新式组件）或直接实现 `wg::Module`（自行管理
流式状态的组件），暴露工厂函数，由应用创建并交给宿主：

```cpp
std::vector<std::unique_ptr<wg::Module>> modules;
modules.push_back(CreateClockPanel());

wg::BoardOptions options;
options.id = L"my-board";             // 单实例和窗口类标识
options.title = L"My Board";           // 窗口、托盘及回退配置目录名称
options.boardTitle = L"MY BOARD";      // 板面标题
options.configFile = L"board.ini";
options.startWithWindows = false;
return wg::RunBoard(std::move(modules), options);
```

`BoardOptions` 还可设置尺寸、吸附网格、提示文字和快捷键。框架默认不启用开机
启动，也不删除任何旧启动项；应用确有迁移需求时，可显式设置
`legacyAutostartNames`。`desk` 自己指定原有的 `desk.ini`、单实例 ID 和迁移项，
以保留已有配置。程序化嵌入/测试还可以设置：

- `onEvent`：`BoardEvent` 回调（UI 线程，见「程序化宿主、事件与诊断」）；
- `display`：自定义 `DisplaySource`（空则用系统来源）；
- `enableTray` / `enableHotkeys`：嵌入或测试时关掉托盘图标和全局热键。

组件的 `Describe()` 返回稳定且唯一的 `ModuleSpec.id`、标题、尺寸预设、刷新周期和
可选的菜单/热键配置。`id` 同时用于持久化键和默认配置文件名，应使用安全文件名。
组件默认不需要键盘；`WantsKeyboard()` 可认领点击，然后通过 `OnKey`、`OnChar`、
`OnPaste` 和 `CaretPos` 处理键盘及输入法。`OnWheel` 接收 Windows 原始增量，
每格通常为 ±120；需要累积高精度增量，并在状态变化时调用 `RequestRepaint()`。

## 程序化宿主、事件与诊断

`RunBoard()` 是最短入口（内部就是 `BoardHost` + `Run`）。需要自己控制生命周期的
应用或测试用 `wg::BoardHost`（`widget/host.h`）：

| 成员 | 语义 |
|---|---|
| `BoardHost(modules, options)` | 只保存参数，不创建窗口 |
| `StartResult Start()` | 创建窗口、恢复状态、跑第一次布局、安装托盘/热键（`enableTray`/`enableHotkeys` 可关），**不进入**消息循环。返回 `Started` / `AlreadyRunning`（同 id 已有实例占着互斥体）/ `Failed`；重复调用返回 `AlreadyRunning`。`BoardEvent` 回调可能在 `Start()` 返回前触发 |
| `void Run()` | 进入消息循环直到 `Quit()`/`WM_QUIT`；未 Start 时立即返回 |
| `void Quit()` | 优雅退出：保存状态、移除托盘、注销热键、join worker；Start 前调用安全，可重复 |
| `bool running()` / `HWND hwnd()` | 运行状态与窗口句柄 |
| `BoardMetrics Metrics()` | `{board, interior, work, dpi}`；Start 之前全 0 |
| `bool PlanPreview(board, cards)` | 只规划不落地：按给定面板矩形试算可见卡片，返回注册顺序的板内矩形；失败或未 Start 返回 false 且不改 `cards` |
| `ReadaptToDisplay()` | 与 `WM_DISPLAYCHANGE`/`SPI_SETWORKAREA` 同一条工作区适配路径 |
| `ApplySuggestedRect(RECT)` | 与 `WM_DPICHANGED` 同一条路径（建议矩形） |
| `ShowCard / SetCardExpanded / RefreshCard / Tidy / ResetToStandardPanel / RestorePreviousLayout` | 菜单动作的程序化等价物；索引非法、未 Start 或被规划器拒绝时返回 false 并保留原状态 |

`BoardHost` 析构时若仍处于 Start 状态，会做一次同步 `Finish()`（销毁窗口 +
可重复的 Shutdown），所以测试可以不进消息循环直接析构。

### BoardEvent

`BoardOptions::onEvent` 只在 UI 线程被调用（包括 `Start()` 期间）：

| kind | 何时 |
|---|---|
| `LayoutApplied` | 一次布局提交成功（启动首个布局、迁移、展开/收起、增删卡、Reset、Tidy、Restore、显示适配），在提交的**最后**发出 |
| `LayoutIncomplete` | 布局排不下（卡片显示 `NO SPACE`），板仍启动/继续运行；窗口被夹在工作区内 |
| `LayoutRestorePending` | 刚保存了「最早那份」恢复快照（`rect` 是**原始**矩形） |
| `LayoutRestored` | 快照严格恢复成功 |
| `LayoutRestoreFailed` | 这次显示仍无法严格恢复；快照保留 |
| `DisplayAdapted` | 工作区/DPI 变化后布局已重新规划 |
| `MigrationApplied` / `MigrationDeferred` | 一次性几何迁移成功 / 失败（失败只发这一个事件） |
| `CardShown` / `CardHidden` | `ShowCard`/Add module 与 Remove card；`card` 是注册索引，`rect` 是卡片矩形 |
| `SizeRejected` | 组件 `BodyHeight()` 被 `MaxBodyHeight()` 向下夹紧 |

同一线程内事件按以下顺序串行发出：

- 每次提交：先模块 `OnBodySizeChanged` 回调（以及被夹紧时的 `SizeRejected`），
  最后 `LayoutApplied`。
- 启动 + 旧配置迁移：`LayoutApplied`（迁移后的布局）→ `MigrationApplied`；
  迁移失败只有 `MigrationDeferred`。
- 启动时保存的矩形被适配：`LayoutApplied`（首个布局）→ `LayoutRestorePending`
  （`rect` = 原始保存矩形）。启动时恢复成功：`LayoutApplied`（恢复后的布局）→
  `LayoutRestored`；失败：`LayoutRestoreFailed`。
- 工作区变化：若快照恢复成功 `LayoutApplied` → `LayoutRestored`；否则
  `LayoutRestoreFailed`，然后适配：`LayoutRestorePending`（仅当没有更早快照）→
  `LayoutApplied` → `DisplayAdapted`；适配后仍排不下则 `LayoutIncomplete`，
  不出现 `DisplayAdapted`。
- DPI 变化：与工作区变化同一条链（`LayoutRestorePending` → `LayoutApplied` →
  `DisplayAdapted`；来回切换回到原布局时 `LayoutApplied` → `LayoutRestored`）。
  建议矩形排不下时退回工作区适配（可能以 `LayoutIncomplete` 结束）。
- 展开/收起：`LayoutApplied`；模块 `OnExpandChanged` 在提交之后调用，不是事件。

**线程与重入**：`onEvent` 只在 UI 线程调用；回调抛出的异常被吞掉并写日志；
回调内部再触发的 `BoardEvent` 会被丢弃并写日志（也不允许在回调里调用
`Start`/`Quit`/`ReadaptToDisplay`）。`OnBodySizeChanged` 等模块回调是在布局
提交里同步调用的，不属于 `BoardEvent` 流。

### DisplaySource

板子不再直接读桌面，而是通过 `widget/display.h` 的 `DisplaySource`：
`PrimaryWorkArea()`、`WorkAreaForPoint(POINT)`、`DpiForWindow(HWND)`、
`SystemDpi()`。选择规则：

1. `BoardOptions::display` 非空就用它（应用可注入自己的实现）；
2. 否则 `DESK_TEST_DISPLAY_FILE` 指向的文件 → `ScriptedDisplaySource`（测试钩子，
   见 [desk.md](desk.md) 的验收一节）；
3. 否则 `CreateSystemDisplaySource()`：`SPI_GETWORKAREA` + `GetMonitorInfoW` +
   `GetDpiForWindow`/`GetDpiForSystem`。

`DisplaySource` 返回非法工作区/DPI 时回退到系统值并写一条 warn 日志，不会崩。
`ScriptedDisplaySource` 每次查询都重新读文件：`work=x,y,w,h`（必需，模拟工作区）、
`dpi=N`（48..960，越界夹取）、`rect=x,y,w,h`（可选，DPI 变化路径的建议矩形，
只对当前这次读取有效）；非法行忽略，文件缺失/不可读时保留上一次的有效值。

### 诊断日志

`widget/diag.h`：应用在启动板子前安装 sink（`SetLogSink`；空/`nullptr` 关闭），
默认没有 sink 时 `Log` 是廉价 no-op。`SetLogSink`/`Log` 都线程安全：sink 在锁外
以按值拷贝的 `LogRecord` 调用，所以 sink 可以再记日志或替换自己；sink 抛出的异常
会被吞掉。框架在这些地方记录：迁移成功/延后、恢复快照/恢复成功/失败、布局不完整、
保存失败、显示源回退、`SizeRejected`、被丢弃的嵌套事件、回调/模块异常。

## 面板几何与增长

面板外框不是硬编码坐标，而是由工作区和 DPI 推导：

- `BoardOptions::width/height` 是 **96 DPI 下的名义外框尺寸**，默认
  `1280×1310`；最小值 `minWidth×minHeight = 400×420`。
- 首次运行：外框从 96 DPI 名义尺寸按**系统 DPI** 缩放
  （`MulDiv(width, dpi, 96)`；此时还没有窗口，拿不到目标显示器 DPI），然后
  `w = min(缩放后宽度, max(minWidth, 工作区宽度/2))`、
  `h = min(max(缩放后高度, minHeight), 工作区高度)`；位置固定为贴右上角：
  `x = work.right - w`、`y = work.top`。
- 本机参考（2560×1440 显示器、工作区 2560×1392、96 DPI）：面板
  **1280×1310，位于 (1280,0)**，竖直方向还有 82px 可增长。
- 之后每次启动读 `desk.ini` 的 `board.x/y/w/h`，再夹到当前显示器的工作区
  （原始矩形会先记下来，供启动适配后的恢复，见「配置迁移与恢复」）。
  运行中收到 `WM_DPICHANGED`、`WM_DISPLAYCHANGE` 或工作区变化
  （`WM_SETTINGCHANGE` + `SPI_SETWORKAREA`）都走**同一条**「先尝试恢复 →
  必要时快照 → 再适配/规划建议矩形」的路径：DPI 改变不再单独走一条分支，所以
  DPI 来回切换也能恢复切换前的布局。变化前的矩形保留在 `saved.*` 键里。
- 只有**外框**按 DPI 推导：卡片尺寸、行高、字号和图标仍是 96 DPI 基准的物理
  像素令牌。加上「宽度不超过工作区一半、高度不超过工作区」的限制，高 DPI 显示器
  通常得到和 100% 时相近的面板尺寸，卡片相对面板会显得更小。

板面内部（卡片可用区域，`Board::InteriorFor`）：

```text
interior.x = 板内边距 14
interior.y = 板标题条 26 + 6
interior.w = 板宽 - 2 * 14
interior.h = 板高 - 26 - 6 - 14 - 16（右下缩放把手）
```

1280×1310 的面板因此得到 `x=14, y=32, w=1252, h=1248` 的卡片区。

首次运行的卡片位置由框架按「参考书架」播种，而不是全部堆在左上角：
**按内容高度从左到右排布、放不下时换行（Account 与 Servers 并排，PowerShell
在下一行），卡片之间留 12px**。播种结果和拖动/规划一样对齐到同一套 8px 网格
（原点是卡片区左上角）：行间距 12px 向上取整成 16px，所以每行起点都落在 8px
网格上。坐标随组件内容高度与面板内宽计算，不是硬编码；已经保存过位置的卡片
不受影响。空配置（Servers 没有主机 → 1 行 → Standard 最小高度 112）在本机
播种为 Account (14,32) 340×144、Servers (366,32) 340×112、
PowerShell (14,192) 568×168；Servers 有 4 台主机时它是 176 高，PowerShell 落到
(14,224)。

### 自动增长

只有当现有面板放不下卡片时才会增长，而且先规划、后应用：

1. 先用当前面板规划一次；成功就什么都不改。
2. 失败时用 `board::FindMinimalGrow` 找**面积最小**的可行面板。它**不假设
   "尺寸越大越容易放下"**（有界贪心规划器可能在更矮时成功、更高时失败；`Full`
   卡还会随面板变宽）：
   - 宽度从 `max(当前宽度, 最小宽度)` 开始，每次 +8px，**一直扫到右半屏上限**
     （`min(名义宽度按 DPI 缩放, max(minWidth, 工作区宽/2))`）。上限只是
     「不许自动超过」：用户手动把面板拉得比它宽时不会被自动缩窄，该宽度会作为
     唯一候选参与尝试。
   - 每个宽度下，高度从下限开始按 8px 向上采样；**第一个通过的采样打开一个
     窗口**，再把窗口内跳过的像素逐个向上扫，取最小的可行高度。高度不强制 8 的
     倍数（最终高度可以是任意整数像素）。
   - 候选面板**贴工作区右边缘**：`x = work.right - w`，`y` 夹在工作区内。
     所以即使面板原来在屏幕左侧，需要增长时也会回到右半屏，而不是以旧窗口的
     右边缘为锚点。面积最小的候选胜出；同面积取更窄的，再同则取更矮的。
   - 残余风险（有界搜索的固有代价）：如果某段可行高度比 8px 还窄、且完全落在
     一个 8px 采样全部失败的窗口内部，它就发现不了；`step=1` 时高度扫描是
     穷举的。宽度始终穷举扫描。
3. 搜索有界：最多 `W * (floor((highH - lowH) / step) + step + 1)` 次可行性
   检查（W 是宽度候选数，受工作区宽度限制；括号内是每宽度的采样数加上最多
   `step-1` 次像素细化）。面积剪枝会提前停掉不可能更小的宽度，实际远低于
   上界；没有递归、没有分配、没有随机。
4. 规划失败时窗口尺寸、卡片位置和状态全部保持原样，不产生半成品布局。

### 状态与修复

框架不会隐瞒布局问题：

| 现象 | 含义 |
|---|---|
| 卡片标题条 `NO SPACE`（黄） | 这张卡当前没有被安排进合法位置（启动时放不下 / 刷新后变高放不下） |
| 卡片标题条 `TOO TALL`（黄） | 组件要求的高度超出预设范围，框架照实测高度报告，不截短内容 |
| 卡片标题条 `ERROR`（红） | 组件 Refresh 抛异常或提交失败，仍显示上一份数据 |
| 底部提示 `LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL` | 启动时布局不完整，板仍然启动，可在菜单里修复 |

底部提示栏（板标题条右侧）还会出现这些一次性/持续提示：

| 提示 | 触发 |
|---|---|
| `NO LEGAL POSITION FOR THIS CARD - IT STAYED WHERE IT WAS` | 拖放松手时整板没有合法位，卡片回到拾起位置 |
| `DISPLAY CHANGED - MENU: RESTORE PREVIOUS LAYOUT` | 显示器/DPI/工作区变化后布局被重新安排，旧矩形已存好 |
| `DISPLAY SMALLER THAN THE STANDARD PANEL - SIZED TO FIT` | 工作区放不下 96 DPI 名义面板，按工作区缩小启动 |
| `BOARD MIGRATION DEFERRED - LAYOUT DID NOT FIT` | 旧配置迁移时规划失败，保留原布局，下次启动重试 |
| `BOARD RESIZED TO THE RIGHT HALF - CONFIG BACKED UP` | 旧配置迁移成功，备份了 `pre-v2.bak` |
| `STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT` | 启动时保存的面板矩形被工作区收缩过，原布局已存入 `saved.*` |
| `PREVIOUS LAYOUT RESTORED` / `PREVIOUS LAYOUT DOES NOT FIT THIS DISPLAY YET` | 菜单恢复旧布局的结果；后者表示**严格恢复失败**，快照继续保留 |
| `STANDARD RIGHT PANEL RESTORED` | 菜单 Reset 成功 |
| `CARDS COLLAPSED TO FIT THE BOARD` | Fit cards 靠收起详情才排下 |

板菜单提供三个修复动作：`Fit cards (collapse details)`（先尝试原样重排，
不行就收起全部详情再排）、`Reset to right standard panel`（回到标准右半屏
面板，先备份 `desk.ini.pre-reset.bak`）、`Restore previous layout`（恢复
显示变化前的矩形，没有快照时置灰）。需要用户确认语义的操作失败时弹一个
消息框，并明确区分「空间确实不够（Insufficient）」和「搜索上限内没找到
（NotFound）」。

## 组件尺寸与位置

组件应在 `ModuleSpec::sizePreset` 中显式选择用途相符的预设。宽度由预设给出，
高度是**卡片外框高度**，由框架按 8px 向上取整，并且不会压缩组件要求的 body：

| 预设 | 宽度 | 外框高度范围 | 典型用途 |
|---|---:|---:|---|
| `Compact` | 256px | 96–512px | 简短状态或单一指标 |
| `Standard` | 340px | 112–640px | 列表、账户或服务器状态 |
| `Wide` | 568px | 168–720px | 终端、较宽的数据表 |
| `Full` | 面板卡片区宽度（本机 1252px） | 208–720px | 需要跨整板展示的内容 |

`Full` 是**面板卡片区宽度**，不是写死的数字：它等于 `availableWidth`，也就是
面板 `InteriorFor()` 的宽度，面板缩放时重新测量。调用 `MeasureCardSize` 时
必须把卡片区宽度传进去；不传（0）表示「没有面板上下文」，此时 `Full`
不可测量，框架不会编造一个宽度。

高度的计算方式（`kCardChromeHeight = 30 + 8 + 10 = 48`）：

```text
外框高度 = 8px 向上取整( max(预设最小高度, bodyHeight + 48) )
```

`MeasureCardSize(preset, legacyWidth, bodyHeight, availableWidth = 0)` 返回
`CardSize`：

| 字段 | 含义 |
|---|---|
| `width` / `height` | 实测宽度与高度；高度不会被截短 |
| `representable` | 输入是否可测量（非法预设、负 body、`Full` 缺面板宽度等） |
| `heightWithinRange` | 高度是否落在预设的 min/max 之间 |
| `withinPreset` | 宽度等于预设宽度**且**高度在范围内 |

高度范围是**校验边界，不是裁切指令**：超范围时框架照样返回实测高度，由宿主
显示 `TOO TALL` 并继续正常布局；组件应减少内容、提供自己的滚动或换更高的
预设，而不是让卡片压到别的卡片上。宽度同理：`withinPreset=false` 只表示
「宽度不是这个预设的标准宽度」。

组件还可以声明自己「最矮/最高能画多高的 body」：

- `MinBodyHeight()`（默认 0）与 `MaxBodyHeight()`（默认 `INT_MAX`）给出可渲染
  范围；框架用 `ClampBodyHeight(requested, min, max, &exceeded)` 把
  `BodyHeight()`/`PendingBodyHeight()` 夹进这个范围**再测卡片**：低于 min 抬到
  min；高于 max 夹到 max，并把 `exceeded` 置位（宿主据此发 `SizeRejected`）；
  负 min 视为 0；负的或 max < min 是契约错误，只应用 min、max 被忽略。
  规划、启动播种、`PlanPreview` 和失败回退都走同一套夹取，所以组件可以声明
  「可滚动范围」而不必担心板子变小。
- `OnBodySizeChanged(body)`：布局提交后、这张卡的 body 矩形发生变化（尺寸或位置，
  包括第一次变为可见）时在 UI 线程调用；卡片隐藏时不调用。终端可以在这里改行数，
  组件也可以据此重排；抛出的异常被吞掉并写日志。它发生在 `LayoutApplied` 之前，
  不属于 `BoardEvent` 流。
- `OnExpandChanged()` 只在展开/收起的布局**被接受之后**调用（不是事件）。

旧组件没有声明预设时使用 `Auto`：按 `ModuleSpec::width` 向上选择能容纳内容的
最小宽度档，并**原样保留声明的宽度**（兼容映射绝不擅自收窄旧内容）；解析出的
预设只负责高度范围和分类。现有 340px 信息卡和 568px、80 列终端因此保持原宽度。

位置使用以卡片区左上角为原点的网格；组件不需要自己算吸附坐标：

- 首次运行由框架把卡片摆成参考书架（左右并排、超出换行，见「面板几何」）。
- 拖动时卡片跟手，按 `BoardOptions::grid`（默认 8px）吸附并夹在卡片区内；
  拖动过程中**不做**合法位置搜索。拖动被中断（失去鼠标捕获，例如 Alt+Tab）
  时卡片回到拾起它的位置，不会停在别的卡片上。
- 松手时找离落点最近的合法位置（同距离先比 y，再比 x），并与其它卡片保持
  `kCardGap = 12px` 间距；完全没有合法位置时卡片回到拖动前的位置，底部提示
  `NO LEGAL POSITION FOR THIS CARD - IT STAYED WHERE IT WAS`。
- 添加/移除卡片、展开/收起详情、刷新后高度变化、面板缩放都会重新规划整板，
  规划是原子的：失败时不动布局。

## 布局规划与拖放

`board::TryPlanLayout(desired, occupied, interior, gap, grid, out, priorityIndex, status, blockedIndex)`
是纯函数，无 HWND，可被 `tests/board_plan.cpp` 和
`tests/board_composition.cpp` 直接测试。

- **确定性**：依次尝试 4 种候选顺序（请求顺序 / 面积从大到小 / 高度从大到小 /
  「已经合法的卡保持原位」），每种顺序每张卡最多落一次，因此整个搜索最多
  `4 * N` 次 `TryResolveDrop`；没有随机、没有递归、不穷举网格。
- **最优性**：所有成功候选里取「相对请求矩形总平方位移」最小的；同分保留
  更早的候选，所以结果只取决于参数。
- **失败要可诊断**：返回 `PlanStatus`。`Ok` 成功；`NotFound` 表示搜索上限内
  没找到（解可能仍存在）；`Insufficient` 只在**能证明无解**时报告，证明有三条：
  P1 单张卡比卡片区还宽/高；P2 面积界（把每张卡向外扩 `gap/2`，总面积必须
  装得下卡片区外扩后的面积）；P3 行列界（任意两张卡都无法叠放时，所有卡的
  宽度加间距必须排进一行，反之亦然）。`occupied` 不参与证明，所以证明只会
  少报、不会多报。
- **原子性**：失败时 `*out`、输入和 `occupied` 都不变（`*out` 与输入别名时
  也一样），`*blockedIndex` 指认导致失败的卡（P2/P3 为 -1）。

`Board::PlanLayout` 在它之上负责「测量 + 决定是否增长」：每次尝试都用**候选
面板**的卡片区重新测量所有卡（`Full` 卡因此跟着变宽），刷新中的那张卡用
`PendingBodyHeight()` 预测提交后的高度。

`HitTest` 的顺序是 Resize → BoardTitle → CardTitle/Card → AddSlot → Empty；
Empty 且 `board.solid=0` 时返回 `HTTRANSPARENT`（空白点交给桌面），其余
`HTCLIENT`。这不是 `WS_EX_TRANSPARENT` 整窗穿透。

## 生命周期与线程

1. 宿主在 UI 线程调用 `Describe()`，绑定配置和回调，恢复卡片状态，再调用一次
   `Load()`。`Describe()` 中不能访问 `store()`；隐藏的已注册组件也会加载。
2. `ModuleT::LoadData()` 在 UI 线程初始化数据。每次刷新时，worker 先复制当前
   显示数据到 staging，再执行 `RefreshData(staging)`；成功后宿主等待 worker
   结束，在 UI 线程交换快照并调用 `OnCommit()`。
3. `BodyHeightOf()`、`PaintBodyOf()`、菜单和键鼠回调在 UI 线程运行。
   刷新与绘制可以并发：刷新只写 staging，绘制只读 shown；组件中其他共享成员仍需
   自行同步。`Data` 需要可默认构造、复制赋值和交换；大对象复制会带来开销。
4. 刷新抛异常时不提交该次 staging，继续显示上一份数据；宿主把 `Commit` 抛异常
   视为失败。`OnCommit()` 在交换后执行，应避免抛异常；它不是可回滚的事务。
5. `RequestRepaint()` 可用于后台线程，但高频流式数据需要组件自行节流。
   `RequestFocus()` 和剪贴板操作应在 UI 线程调用。卡片的 Remove 只改变可见性，
   不销毁组件；退出时宿主等待刷新 worker，组件应为阻塞操作设置超时并在析构时
   停止自己的线程和回调。

宿主侧：一个 `SetTimer` 对齐最近到期的刷新（50ms–600s 夹取）；有键盘焦点的卡片
另有一个 530ms 的光标闪烁定时器；拖动重绘按 16ms 节流。持久化在后台保存线程
完成：`SaveState()` 只做内存快照并入队，落盘线程用 `Store::SaveSnapshotFile`
把这份快照**精确**写入临时文件（再用 `MoveFileEx` 原子替换，不与旧文件合并），
退出时排入最后一份快照并 join。

绘制回调收到的是画布坐标，宿主会裁剪到 body；鼠标回调使用 body 局部坐标。
不要从 worker 直接操作窗口、Canvas 或 UI 状态。组件可采用独立锁管理流式状态，
但必须自己处理并发和生命周期。

## 配置迁移与恢复

- 配置文件优先放在可执行文件目录；目录不可写时回退到 `%APPDATA%/<应用标题>`，
  全部文件一起回退。
- `board.version` 是几何版本标记（当前 `2`）。**全新安装**在第一次运行时直接
  写入版本号，之后不再迁移。
- 旧配置（没有版本号或小于 2）：先把文件复制为 `<配置>.pre-v2.bak`，再按
  「标准右半屏面板 + 整板布局规划」在内存里迁移；只有规划成功才写入新几何和
  `board.version`。失败时几何和卡片状态还原、**不写入新几何，也不写
  `board.version`**，底部提示 `BOARD MIGRATION DEFERRED - LAYOUT DID NOT FIT`，
  下次启动重试。注意启动路径仍会调用一次 `SaveState()`，把同一组旧值重新写进
  文件——所以文件不是逐字节不变，但布局内容和原文件一致，`pre-v2.bak` 也还在。
- 显示器/DPI/工作区变化：内存里按新工作区重新规划，变化前的面板矩形和卡片
  位置存到 `saved.board.*` / `saved.card.<id>.*`，并置 `saved.pending=1`；
  恢复原来的工作区时自动尝试恢复，也可以随时用菜单 `Restore previous layout`。
  **保留最早的那份**：一旦 `saved.pending` 存在，后续的显示/DPI 适配**不会**
  覆盖它——连续缩小两次时 `saved.*` 仍然是用户最初选择的布局，第二次适配只改
  当前矩形。恢复成功或任何一次用户主动改布局（拖动落位、展开、增删卡、Tidy、
  Fit、Reset）才会清除快照。
- **启动适配不会破坏原布局**：启动时先记下 `desk.ini` 里的原始矩形和卡片位置，
  再做工作区夹取。如果这次启动确实缩小/移动了面板，原矩形和原卡片位置会先写入
  `saved.*`，并提示 8 秒：
  `STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT`。之后换回大
  显示器可以恢复；布局本身不完整（`NO SPACE`）时只显示不完整提示。
- **恢复是严格的**：快照里的面板矩形必须完整落在当前工作区内，卡片位置必须
  **原样规划成功**（不允许增长，也不允许任何一张卡被挪动）。否则不算恢复成功：
  快照继续待命、当前（已适配的）布局原样保留，菜单恢复时提示
  `PREVIOUS LAYOUT DOES NOT FIT THIS DISPLAY YET`。不会把「夹取后的矩形」当成
  恢复成功，那样会永久丢掉真正的布局。
- `Reset to right standard panel` 先把当前文件备份成
  `<配置>.pre-reset.bak`，再应用标准面板方案。
- **落盘写的是精确快照**：后台保存线程用 `Store::SaveSnapshotFile`（临时文件 +
  `MoveFileEx` 原子替换）写入内存快照的**全部内容**，不会与磁盘上的旧文件
  合并；所以从内存里删掉的键（例如已清除的 `saved.*`）不会在下次启动时复活。

## 常用底层接口

| 头文件 | 能力 |
|---|---|
| `widget/board.h`、`widget/module.h` | 宿主选项、注册、组件和刷新契约 |
| `widget/host.h` | `BoardHost`：程序化生命周期、Metrics、PlanPreview、卡片操作 |
| `widget/display.h` | `DisplaySource`（系统/脚本来源）：工作区与 DPI |
| `widget/diag.h` | 线程安全诊断日志（`SetLogSink`/`Log`） |
| `widget/size.h` | 尺寸预设、`MeasureCardSize`、超范围报告 |
| `widget/board_plan.h` | 整板布局规划、`PlanStatus`、不充分性证明 |
| `widget/canvas.h`、`widget/skin.h`、`widget/view.h` | 像素画布、主题、卡片与标准行 |
| `widget/store.h` | UTF-16 键值配置，`Set` 后需显式 `Save()`；保存是原子的 |
| `widget/http.h` | 同步 HTTPS 请求；阻塞调用应放在 worker |
| `widget/secrets.h`、`widget/prompt.h` | Windows 凭据管理器和输入框 |
| `widget/proc.h`、`widget/conpty.h` | 子进程和 Windows 伪控制台 |

`Store` 的单个方法可跨线程调用，但多步操作和跨进程写入不是事务；`Save()` 可能
失败，应根据组件需要处理结果。敏感凭据使用凭据接口，不要放入普通配置。

## 示例与新增组件

在当前 desk 中新增组件，可使用 `tools/new-panel.ps1` 生成骨架，再在
`src/apps/desk/main.cpp` 注册工厂。模板会显式声明 `SizePreset::Standard`、
用 `BodyHeightOf()` 声明内容高度，并演示展开详情时的 body 高度；生成的模块
可直接被 `build.bat` 编译。对于独立应用，可直接参考示例，不需要复制
Account、Servers 或 PowerShell 源码。
