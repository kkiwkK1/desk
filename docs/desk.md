# 桌面洞洞板（desk.exe）

> 一个进程、一块板、若干张可自由摆放的模块卡片。本文是模块作者和排错的主要文档；
> 视觉/交互参考见 [README](../README.md)。

## 1. 设计原则

- **板是容器，模块是卡片**：不在一个面板里硬塞所有内容；每张卡有自己的标题条、
  尺寸、刷新节奏和设置。
- **一个进程**：一个分层窗口、一张 DIB、一个刷新定时器、一个托盘、一套热键
  （拿到键盘的卡片另有一个光标闪烁定时器）。卡片是板内矩形，不是独立 HWND。
- **自由但有边界**：卡片在板内自由拖动，8px 吸附；松手落到最近合法空位，
  卡片之间至少留 12px；整板重新规划是原子的，放不下时保持原布局并给出修复入口。
- **视觉统一**：模块只画 body，板负责外框/标题条/间距/状态色；所有排版走
  `src/widget/skin.*`。
- **不整窗穿透**：卡片和控件可点；没有卡片的空白点交给桌面。
- **置底**：始终压在普通窗口之下，点击不抢前台焦点。

## 2. 运行结构

```
desk.exe
└─ Board（src/widget/board.cpp）一个 wg.desk 分层窗口
   ├─ BoardChrome：tint + 16px 点阵 + 边框 + 标题/拖动手柄 + 缩放把手
   ├─ 卡片层：标题条 30px + 上 8px + body + 下 10px（外框 = 8px 取整的 body + 48）
   ├─ 调度：1 个 SetTimer 对齐最近到期；每模块最多 1 个 worker
   │        焦点卡片另有 530ms 光标定时器；拖动/刷新重绘按 16ms 节流
   ├─ 命中：卡片/标题条/缩放手柄/Add 槽 → HTCLIENT
   │        空白点 → HTTRANSPARENT（桌面可点；board.solid=1 可关）
   ├─ 菜单/托盘/热键/持久化/自启
   └─ Module 注册表（src/apps/desk/main.cpp）
       ├─ AccountModule（Account 卡）
       ├─ ServersModule（Servers 卡）
       ├─ PsModule（POWERSHELL 卡：常驻 pwsh 控制台）
       ├─ AssistantModule（ASSISTANT 卡：BRIEF + 输入 + 流式回答）
       └─ MemoryModule（MEMORY 卡：与助理共享长期事实）
```

最后两张卡只在 `assistant.ini` 的 `enabled=1`（默认）时注册；`enabled=0` 时
**ASSISTANT/MEMORY 都不加载**，也不会创建引擎。

`LoadData` 初始化一次；worker 每次将 shown 复制到 staging，然后在
`RefreshData` 中修改 staging；成功后 `Commit` 在 UI 线程交换数据。
`PaintBodyOf` 只读 shown。刷新与绘制可以并发，组件中其他共享成员仍需自行同步。

**唯一例外是 TERMINAL 卡**：它直接继承 `Module`，状态由 `ps::Terminal` 自己加锁
持有（ConPTY + VT 解析器 + 字符网格），`refreshSeconds=0`。读线程解析完一块输出后
调 `RequestRepaint()`（33ms 节流），卡片绘制时取一份可视窗口快照 —— 终端是流，没有
「一次刷新一份快照」的边界。

## 3. 交互

| 操作 | 行为 |
|---|---|
| 拖卡片标题条 | 卡片跟手并夹在卡片区内，按 `board.grid`（默认 8px）吸附；拖动中**不**解布局；松手落到最近合法空位（同距离先 y 后 x）；没有空位则回到拾起位置并提示 |
| 拖动被中断（失去鼠标捕获，如 Alt+Tab） | 卡片回到拾起它的位置 |
| 点标题条右侧 28px | 切换 `expanded`；展开后高度增加，整板重新规划，位置尽量保持 |
| 拖板标题条 | 移动整块板（限制在工作区）；松手按工作区重新适配 |
| 拖右下角 | 缩放板（最小 400×420，最大工作区）；当前卡片放不下时拒绝缩小并提示 |
| 左键 Add 槽 | 打开板菜单 |
| 右键卡片 | 模块菜单 + Details / Refresh / Remove card |
| 右键板标题条 / 托盘 | Add module / Tidy board / Refresh all / **Fit cards (collapse details)** / **Reset to right standard panel** / **Restore previous layout** / Show grid / Empty area passes clicks / Start with Windows / Exit |
| `Ctrl+Alt+W` | 板菜单 |
| `Ctrl+Alt+A` | 召唤/收起 ASSISTANT：临时把板抬到最前、展开并聚焦助理卡；再次按下、失焦或前台切换时恢复置底（热键可在 `assistant.ini` 改） |
| `Ctrl+Alt+R` | 刷新所有可见卡片 |
| `Ctrl+Alt+D` | 全部展开/收起 |
| `Ctrl+Alt+Q` | 退出 |
| 托盘双击 | 全部展开/收起 |
| 左键 PS 卡 | 把键盘交给这张卡，之后按键直接进 PTY |
| 方向键 / `Home` `End` / `Ins` `Del` / `PgUp` `PgDn` / `F1-F12` | PS 卡：按 VT 序列送给应用（应用光标键模式自动切换 `ESC O`） |
| `Ctrl+字母` / `Alt+字母` | PS 卡：控制字节 / ESC 前缀（`Ctrl+C` 就是中断） |
| 在 PS 卡正文上按住左键拖动 | 选中文字（蓝色高亮，跨行、跨回滚） |
| `Ctrl+Shift+C` / `Ctrl+Shift+A` | PS 卡：复制选中 / 全选可见区 |
| `Ctrl+V` | PS 卡：粘贴（应用开了 bracketed paste 时自动加 `200~`/`201~`） |
| 滚轮 | PS 卡：普通终端翻回滚 3 行/格；Pi 等开启鼠标模式的应用接收滚轮；右下角 `↓ BOTTOM` 可返回最新输出 |
| 在 ASSISTANT 输入行按 `Enter` / `Esc` | 发送问题 / 取消在飞问答；流式回答时状态为 `THINKING`，Esc 后迟到片段不会复活 |
| 右键 MEMORY 卡 / 点选一行 | 菜单 Pin/Unpin、Delete、Edit、Clear all、Refresh；按事实 id 保持选中，失败显示错误并保留原事实 |

POWERSHELL 卡是**真终端**：`CreatePseudoConsole` 给 shell 一个控制台，PSReadLine
（或 vim/less/pi agent）自己处理行编辑、历史和全屏绘制；卡片只把按键编码成字节送进
PTY，再把回来的 VT 流解析成字符网格。板子只在点中这张卡时拿键盘
（`WM_MOUSEACTIVATE` 对其它位置仍返回 `MA_NOACTIVATE`），失去焦点即释放。

选中模型是 `(行 id, 列)`（`src/data/ps/selection.*`）：屏幕给每行一个稳定 id，新输出、
滚动、回滚环丢行都不会让选中错位；`Ctrl+Shift+C` 复制时按 seq 范围取文本，
列越界自动夹取，行间用 CRLF。

**空白板面不接收右键**（HTTRANSPARENT），所以板菜单放在标题条/托盘/Add 槽上；
`board.solid=1` 时整板接收点击，空白处右键也能打开板菜单。

## 4. 尺寸与视觉令牌

| 项 | 值 |
|---|---|
| 首次运行的面板 | **1280×1310**（96 DPI 名义尺寸，按系统 DPI 缩放），贴工作区右上角；本机工作区 2560×1392 → 位于 (1280,0)，竖直方向还有 82px 增长空间 |
| 自动增长 | 放不下时找**面积最小**的可行面板：宽度从当前宽度 +8px 一直扫到右半屏上限（只是上限，已手动拉宽的面板不会自动缩窄）；每个宽度下高度按 8px 向上采样，第一个通过的采样再逐像素细化出最小可行高度（不假设"越高越容易放下"，最终高度可为任意整数像素）；候选贴工作区右边缘（`x = work.right - w`），面板原来在左侧也会移回右半屏；失败时保持原布局。本机 cannot-fit 实测长到 952×478 @ (1608,300)，面积 455,056 px² |
| 板内边距 / 标题条 | 14px / 26px（+6px 间隙；卡片区 y=32） |
| 卡片区（本机） | 1252×1248（1280-2×14 × 1310-26-6-14-16） |
| 板边框 / 圆角 | 2px / 10px 像素圆角 |
| 卡片边框 / 圆角 | 1px / 6px 像素圆角 |
| 点阵 | 16px 间距，1px 点 |
| 卡片标题条 | 30px |
| 卡片内边距 | 左右 12 / 上 8 / 下 10（外框 = body + 48） |
| 行距 | 32px |
| 进度条 | 4px |
| 终端格子 / 内缩 | 7×14px / 4px（80 列 × 8 行 body 120，× 24 行 body 344） |
| Add 槽 | 340×118，虚线 4/4 |
| 字号 | 标题/行 1.5；区块/状态/备注 1.0；详情大数 3.0 |

尺寸预设（宽度固定，高度按内容、8px 向上取整，并受范围校验）：

| 预设 | 宽度 | 外框高度范围 | 本机组件 |
|---|---:|---:|---|
| `Compact` | 256px | 96–512px | — |
| `Standard` | 340px | 112–640px | Account、Servers、MEMORY |
| `Wide` | 568px | 168–720px | PowerShell（80 列 × 7px + 2×4px）、ASSISTANT |
| `Full` | 卡片区宽度（本机 1252px，面板缩放时重算） | 208–720px | — |

| 卡片 | 收起 | 展开 |
|---|---|---|
| Account（Codex + 2 账号，收起 3 行） | 144（开 DeepSeek 后 176） | 456（聚合 + 每账号详情 + token + 7 天图） |
| Servers（4 台） | 176 | 224（详情原始高度 222，向上取整） |
| Servers（8 台） | 304 | 352（详情原始高度 350，向上取整） |
| Servers（无主机，1 行占位） | 112（Standard 最小高度） | 128 |
| PowerShell（80×8 / 80×24） | 168 | 392（宽 568） |
| ASSISTANT（无问题/1 行 BRIEF） | 168（Wide 最小高度） | 198–688（按 BRIEF 与答案行数，最多 6 + 32 行） |
| MEMORY（空 / 1 / 2 / 3 行） | 112 / 112 / 134 / 168 | 112–628（最多 17 行 + MORE + 错误行） |

卡片高度先按 `30 + 8 + BodyHeight() + 10`（= `BodyHeight() + 48`）计算，再
按所选预设的最小高度和 8px 步长向上取整；`BodyHeight` 已经包含展开详情。
预设范围是**校验边界，不是裁切指令**：超范围时框架照实测高度布局，并在标题条
显示 `TOO TALL`，不会悄悄截断组件内容。

## 5. 配置与持久化

配置目录：exe 同目录；不可写则 `%APPDATA%\Desktop Desk`（全部文件一起回退）。

`desk.ini`：

| 键 | 说明 |
|---|---|
| `board.x/y/w/h` | 板屏幕矩形（首次运行由工作区/DPI 计算，之后保存） |
| `board.version` | 几何版本标记，当前 `2`；全新安装第一次运行就写入 |
| `board.grid` | 1 = 显示点阵（视觉；吸附步长由 `BoardOptions::grid` 决定） |
| `board.solid` | 1 = 空白板面也接收点击 |
| `card.<id>.x/y` | 卡片在板内的位置 |
| `card.<id>.visible` | 0 = 移除；仍保留位置，Add module 可恢复 |
| `card.<id>.expanded` | 展开详情 |
| `saved.board.*` / `saved.card.<id>.*` / `saved.pending` | 显示器/DPI/工作区变化前（以及启动被适配前）的矩形快照，供 `Restore previous layout`；**保留最早那份**，后续适配不会覆盖它。用户改布局会清掉，清掉后不会再复活（落盘写精确快照） |

模块数据分别保存在 `aimeter.ini` / `server.ini` / `ps.ini` / `assistant.ini`，不迁移。首次运行（没有
`desk.ini`）会：按工作区/DPI 计算面板矩形、按内容高度把卡片摆成参考书架
（Account 与 Servers 并排，PowerShell 下一行；坐标由内容高度计算）、写入
`board.version=2`；移除旧的 `AiMeter` / `Server Status` / `SSH Pad` 自启值；
按需写 `Desktop Desk` 自启。

旧 `desk.ini`（没有 `board.version`）会先备份为 `desk.ini.pre-v2.bak`，再一次性
迁移到标准右半屏面板：迁移成功才写入新几何和版本号；失败时保留原布局、不写版本
号，底部提示 `BOARD MIGRATION DEFERRED - LAYOUT DID NOT FIT`，下次启动重试。
菜单 `Reset to right standard panel` 会先备份 `desk.ini.pre-reset.bak`。
保存始终是原子的（临时文件 + `MoveFileEx`），由后台保存线程完成；写的是**内存
快照的全部内容**（`Store::SaveSnapshotFile`），不与磁盘旧文件合并，所以清掉的
`saved.*` 不会在重启后复活。

启动时如果保存的面板矩形被当前工作区收缩/移动过，框架会先把**原始矩形和原始
卡片位置**写进 `saved.*`，并显示 8 秒提示：
`STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT`。然后显示适配后
的布局 —— 换回大显示器后还能恢复，小屏启动不会毁掉原有布局。

`ps.ini`（POWERSHELL 卡）：

| 键 | 说明 |
|---|---|
| `shell` | 显式 shell 路径；留空按「override → `%PWSH_EXE%` → PATH 的 pwsh → `%ProgramFiles%\PowerShell\7` → `L:\powershell` → 5.1」解析 |
| `autostart` | 0 = 不预热，第一条命令时才拉起 shell |

子进程用 `-NoLogo -NoProfile -NoExit -Command "…"` 起成**交互式** shell（有 ConPTY，
不再需要 `-Command -`/`-NonInteractive`），工作目录 `%USERPROFILE%`。那条 `-Command`
只做一件事：`Set-PSReadLineOption -HistorySavePath '<配置目录>\ps-history.txt'`，把
PSReadLine 的历史从全局 `ConsoleHost_history.txt` 换成卡片自己的文件（try/catch 包着，
模块缺失时照常起 shell）。这样卡片不会改你的终端历史，也不会在 `%APPDATA%` 不可写时
每次启动都刷 `Access is denied`。

伪控制台输出的字节流逐块解码：UTF-8 严格解码失败就按 ANSI 代码页（本机 936）解 ——
VT 转义是 ASCII，两种编码都不会被破坏。

`assistant.ini`（ASSISTANT 卡 / 引擎）：

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | 1 | 0 = 不注册 ASSISTANT/MEMORY 两张卡 |
| `hotkey` | `Ctrl+Alt+A` | 召唤热键；非法文本回退默认，不静默换键 |
| `context_tokens` | 6000 | 上下文预算；小于 500 按 500 |
| `idle_stop_minutes` | 10 | Pi 空闲退出分钟数，0 = 不自动退出 |
| `daily_token_limit` | 0 | 每日 token 上限，0 = 不限制 |
| `pi_command` | 空 | 显式 Pi 启动器；空 = `node <managed cli.js>` 或 PATH `pi.cmd` |
| `provider` / `model` | `command-code` / `deepseek/deepseek-v4.1-flash-fast` | 显式渠道与模型；空 = 交给 Pi 默认 |
| `key_ref` | `COMMANDCODE_API_KEY_2` | DSH 凭据引用名，不是密钥值 |
| `provider_extension` | 空 | 已安装官方 provider 扩展的绝对路径 |
| `tools` | 空 | 仅兼容：非空时加载强制清空并回写，不再向 Pi 传 `read/edit/...` |

M1 状态文件（都在配置目录）：`persona.md`、`assistant-episodes.jsonl`（只追加问答）、
`assistant-facts.jsonl`（长期事实，含软删除墓碑和可选 `pinned`）、
`assistant-memory-reset.txt`（清空见证）、`assistant-session.ini`（会话路径与摘要）、
`assistant-import.ini`（项目导入 checkpoint）、`assistant-cost.ini`（每日 token/调用/耗时）、
`assistant-send.log`（只记 id/文件名/大小，不含正文）、`pi-sessions/`、`pi-agent/`、
`pi-work/`。密钥只通过子进程私有环境变量传递，不写 ini、不进 argv、不进日志。

**新旧不要同时运行**：同一份 `aimeter.ini` 的当日快照会被两边同时推进。

## 6. 模块契约

`src/widget/module.h`：

```cpp
struct ModuleSpec {
    std::wstring id;          // "account"：配置键/菜单键/目录 id
    std::wstring title;       // "ACCOUNT"
    std::wstring configFile;  // "aimeter.ini"；空则 "<id>.ini"
    int width = 340;          // 旧组件的兼容宽度：Auto 原样保留，不擅自收窄
    int refreshSeconds = 0;   // 0 = 只手动
    bool hasDetails = false;
    bool expandedByDefault = false;
    std::vector<Hotkey> hotkeys;  // 本地 id 1..99
    SizePreset sizePreset = SizePreset::Auto;  // 新组件必须显式选择
    bool visibleByDefault = true;  // 首次运行可见；Remove card 的持久化值优先
};

class Module {
public:
    virtual ModuleSpec Describe() const = 0;   // 不要用 store()
    virtual void Load() = 0;      // UI，一次
    virtual void Refresh() = 0;   // worker，可阻塞
    virtual void Commit() = 0;    // UI，swap 后
    virtual int BodyHeight() const = 0;
    virtual int PendingBodyHeight() const;  // 刷新完成后、提交前的预期高度
    virtual void PaintBody(Canvas&, const Rect& body) = 0;
    virtual StatusChip Status() const;
    virtual std::vector<MenuItem> Menu() const;
    virtual void OnMenu(int id);
    virtual bool OnHotkey(int localId);
    virtual void OnClick(int x, int y);   // body 局部坐标
    virtual void OnHover(int x, int y);

    // 键盘：只有点中的位置 WantsKeyboard() 返回 true，板才会为这张卡抢焦点
    virtual bool WantsKeyboard(int x, int y) const;
    virtual void OnFocusChanged(bool focused);
    virtual bool OnKey(const KeyEvent& key);   // vk + ctrl/alt/shift
    virtual void OnChar(wchar_t c);
    virtual void OnPaste(std::wstring_view text);
    virtual bool CaretPos(int* x, int* y) const;  // 供 IME 候选窗定位

    // 鼠标与尺寸
    virtual void OnPress(int x, int y);        // 按下（拖选起点）
    virtual void OnDrag(int x, int y);         // 按住拖动
    virtual void OnWheel(int x, int y, int delta);  // 原始增量，通常一格 = ±120
    virtual void OnExpandChanged(bool expanded);    // 收起/展开，改行数用
};

template <class Data>
class ModuleT : public Module { /* Load/Refresh/Commit/BodyHeight/PaintBody */ };
```

- `RefreshData` 只写 staging；`PaintBodyOf` 只读 shown；不要从 worker 碰
  Canvas / 窗口。
- 组件通过 `sizePreset` 选择框架尺寸：Account/Servers 用 `Standard`（340，
  112–640），终端用 `Wide`（568 = 80 列 × 7px + 2×4px，168–720）。`BodyHeightOf()`
  只声明**内容高度**；框架加 48px 外框、按 8px 向上取整并校验范围，
  超范围会报 `TOO TALL` 而不是裁切。具体规则见 [框架文档](framework.md)。
  本机的实际高度：Account 144/456（2 个 CommandCode 账号；开 DeepSeek 收起 176），
  Servers 176/224（8 台 304/352），PowerShell 168/392。
- PowerShell 卡常驻 **80 列**（568px 宽）：展开/收起时 `OnExpandChanged()` 调
  `terminal_.Resize(kCols, rows_)`，把 ConPTY 从 8 行改到 24 行（卡片同步
  168↔392）；滚轮 `Auto`/`Terminal history`/`Application (SGR)` 三种模式和
  Pi 路由（`data/ps/pi_process`、`ps::ChooseWheelRoute`）保持原行为。
- 高度不是只有一个 `BodyHeight()`：组件还可以声明 `MinBodyHeight()` /
  `MaxBodyHeight()`（默认 0 / `INT_MAX`），框架用 `ClampBodyHeight()` 把请求
  夹进这个范围再测卡片（低于 min 抬高，高于 max 夹紧并触发 `SizeRejected` 事件）；
  `OnBodySizeChanged(body)` 在布局提交后 body 矩形变化时（含首次可见）被调用，
  终端就是在这里改行数的。展开/收起的 `OnExpandChanged()` 只在布局被接受后调用。
- `Status()` 返回标题条右侧的文字和颜色；颜色为 0 用 `theme.faint`。
- `Menu()` 的条目会出现在卡片右键菜单；`OnMenu` 收到条目 id。
- `hotkeys` 的本地 id 必须 1..99；Board 注册为 `1000 + index*100 + id`。
- 模块自己的配置通过 `store()` 读写；Board 负责打开/绑定。
- 键盘：默认 `WantsKeyboard` 为 false，板照旧"点了也不抢焦点"。返回 true 的
  卡片在点击时会拿到焦点（`SetForegroundWindow` + `SetFocus`，同时板 `PinToDesktop()`
  保持贴底），之后板的 `WM_KEYDOWN`/`WM_CHAR`/`WM_IME_CHAR`/`Ctrl+V` 都转给这张卡；
  `WM_KILLFOCUS`、点其它卡片、开菜单会调用 `OnFocusChanged(false)`。`Ctrl+V`
  由板读剪贴板后调 `OnPaste`，模块不需要自己碰剪贴板。

### 新增模块

```bat
powershell -ExecutionPolicy Bypass -File tools\new-panel.ps1 -Name weather -Title WEATHER
rem 编辑 src\panels\weather\weather.cpp
rem 在 src\apps\desk\main.cpp 加一行 modules.push_back(CreateWeatherModule());
build.bat
```

构建脚本会自动发现 `src/panels/*` 并编进 `desk.exe`；模块自动出现在
**Add module** 菜单里。

### 视觉清单

- 只使用 `skin::Rows` / `skin::Divider` / `skin::FitText` 以及规范里的
  字号/颜色；不画外框、不嵌套卡片、不建窗口、不硬编码偏移。
- 空状态给“代码 + 下一步”（例如 `NO HOSTS` + `SSH HOSTS...`）。
- 状态色只用 `theme.ok` / `warn` / `bad`。
- 详情块每个最多一个 3.0 字号的大数，其余 1.0/1.5。

## 7. 布局、规划与命中

纯函数，无 HWND，可被 `tests/board_layout.cpp` / `tests/board_plan.cpp` /
`tests/board_grow.cpp` / `tests/board_composition.cpp` 直接测试。

### 单卡几何（src/widget/board_layout.*）

- `Snap(v, grid)`：四舍五入到网格；网格原点固定在卡片区左上角。
- `CardRect / CardBody / CardHeight`：标题条、body、底边距的同一套常量。
- `ClampCard`：把卡片夹在卡片区内；卡片比卡片区大时贴左上。
- `TryResolveDrop`：找离请求点最近的合法网格点（同距离先 y 后 x），要求至少
  `gap=12px` 间距；找不到时返回失败且不改 `*out`，不把重叠落点当作成功。
- `ClampBoard`：板的最小尺寸和工作区夹取。
- `HitTest`：Resize → BoardTitle → CardTitle/Card → AddSlot → Empty。

命中结果驱动 `WM_NCHITTEST`：Empty 且 `!board.solid` → `HTTRANSPARENT`，
其余 `HTCLIENT`。这不是 `WS_EX_TRANSPARENT` 整窗穿透。

### 整板规划（src/widget/board_plan.*）

`TryPlanLayout(desired, occupied, interior, gap, grid, out, priorityIndex, status, blockedIndex)`
是确定性、有界的：

- 4 种候选顺序（请求顺序 / 面积从大到小 / 高度从大到小 / 已合法的卡保持原位），
  每张卡每种顺序最多落一次，所以最多 `4 * N` 次 `TryResolveDrop`；没有随机、
  没有递归、不穷举网格。
- 成功候选里取「相对请求矩形总平方位移」最小的；同分取更早的候选。
- `PlanStatus`：`Ok` 成功；`NotFound` 搜索上限内没找到（解可能仍存在）；
  `Insufficient` 只在能证明无解时报告（P1 单卡比卡片区大；P2 面积界；
  P3 行列界）。`occupied` 不参与证明，因此只会少报、不会多报。
- 失败是原子的：`*out`、输入和 `occupied` 都不变；`blockedIndex` 指认
  导致失败的卡（P2/P3 为 -1）。

### 自动增长（src/widget/board_grow.*）

`board::FindMinimalGrow(target, work, limits, fits, out)` 只做几何搜索，`fits`
由调用方注入（`board.cpp` 传 `TryPlanLayout`，测试传合成判据）：

- **不假设尺寸越大越容易放下**：`fits` 可能不随高度增加而改善（有界贪心
  规划器可能在更矮时成功、更高时失败），也可能不随宽度增加而改善（`Full` 卡随
  面板变宽）。因此宽度全部枚举，高度用「8px 采样 + 像素细化」实现。
- 宽度从 `max(当前宽度, 最小宽度)` 开始，每次 +8px 扫到右半屏上限（上限只约束
  自动增长，已手动拉宽的面板不会被缩窄）。
- 每个宽度下，高度从下限开始按 8px 向上采样；第一个通过的采样打开一个窗口，
  再把窗口内跳过的像素逐个向上扫，取最小的可行高度（最终高度是任意整数像素，
  不强制 8 的倍数）。
- 残余风险：如果一段可行高度比 8px 还窄、且整个落在一个 8px 采样全部失败的
  窗口内，就发现不了；`step=1` 时高度扫描是穷举的。
- 取面积最小；同面积取更窄、再取更矮。搜索最多
  `W * (floor((highH - lowH) / step) + step + 1)` 次 `fits`（每宽度采样数 +
  最多 `step-1` 次像素细化），面积剪枝会提前停掉不可能更小的宽度。
- 本机 cannot-fit 场景实测（保存 400×420、卡片展开后放不下）：面板长到
  **952×478 @ (1608,300)**，面积 455,056 px²；`x = 2560 - 952 = 1608` 贴工作区
  右边缘（右半屏从 1280 开始），`y` 保持原目标高度并夹在工作区内。比旧的
  粗略结果 1280×484（619,520 px²）小 **26.5%**。
- 增长只在启动/放不下时发生，开销不可见：本机 cannot-fit 首次窗口 173 ms、
  稳定 1,038 ms，和 fresh 基线 225 ms / 1,090 ms 基本一致。
- 审阅者的非单调反例已固化为 `tests/board_grow.cpp` 的用例：工作区 2032×972、
  面板宽 1016、起始高 850，四张随附预设卡（`Full` 988×392、Account 340×272、
  Servers 340×224、Standard 340×128）只在高度 **934–941** 可行时，
  `FindMinimalGrow` 返回 **1016×934**；旧的（假设"更高一定更容易放下"的）
  实现会失败。
- 没有可行候选时返回失败且不改 `*out`。

`Board::PlanLayout` 在候选面板尺寸上重新测量每张卡（body 先按
`MinBodyHeight()`/`MaxBodyHeight()` 夹取，`Full` 卡随面板变宽），并用
`PendingBodyHeight()` 预测刷新后的高度；放不下时用
`board::FindMinimalGrow`（§4 的自动增长）先规划、后应用。启动、刷新、
展开/收起、增删卡、拖放松手、面板缩放都走同一套规划；失败时保持原布局，
卡片标题条显示 `NO SPACE`，底部提示
`LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL`，菜单里可以
Fit cards / Reset / Restore。

## 8. 构建、测试与签名

```bat
build.bat            rem 框架 + desk.exe + srvctl.exe
build.bat test       rem 无窗口测试；签名测试 exe 后运行
```

测试：

- `tests/board_layout.cpp`：吸附/夹取/避让/命中/Canvas 裁剪/FitText。
- `tests/board_plan.cpp`：整板规划的 4 种顺序、确定性、最小位移、原子失败、
  `NotFound`/`Insufficient` 诊断与三条不充分性证明。
- `tests/size_presets.cpp`：预设表（Compact 256 / Standard 340 / Wide 568 /
  Full = 卡片区宽度）、`Auto` 保持旧宽度、8px 取整与范围报告。
- `tests/board_grow.cpp`：最小面积自动增长（8px 宽度全扫描；每个宽度把高度
  按 8px 采样后逐像素细化、不假设"越高越容易放下"；贴工作区右边缘、同面积取更窄/更矮、
  有界调用次数），与暴力枚举候选空间交叉验证，并固化审阅者的非单调反例
  （2032×972 工作区、宽 1016、起始高 850、仅 934–941 可行 → 返回 1016×934）。
- `tests/store_tests.cpp`：`Store::SaveSnapshotFile` 写精确快照（临时文件 +
  原子替换）；键的「创建 → 清除 → 重新打开」不会复活。临时文件写在测试 exe
  旁边（不依赖 `%TEMP%`，受限令牌下也能跑），仍然由 `build.bat test` 运行。
- `tests/module_body.cpp`（`module-body-test`）：`ClampBodyHeight` 的
  min/max 规则（低于 min 抬高、高于 max 夹紧并置 exceeded、负 min 当 0、
  负/倒置 max 忽略）。
- `tests/display_source.cpp`（`display-source-test`）：系统来源的健全性，
  以及 `ScriptedDisplaySource` 的解析、每次查询重读、非法行忽略、缺文件保留
  上次有效值、`SuggestedRect` 只对本次读取有效。
- `tests/diag.cpp`（`diag-test`）：sink 线程安全、按值记录、sink 抛异常被吞、
  没有 sink 时 no-op。
- `tests/board_host.cpp`（`board-host-test`）：`BoardHost` 生命周期
  （Start/Run/Quit/Failed/AlreadyRunning）、Metrics、`PlanPreview` 不落地、
  `BoardEvent` 顺序、`OnBodySizeChanged`、脚本显示源下的适配与恢复。
- `tests/board_composition.cpp`：1280×1310 面板的整板组合（Account/Servers/
  PowerShell 的预设尺寸、8px 网格、12px 间距、最近落点、`Full=1252`、
  新增一张卡不动已有合法位置）。
- `tests/desk_render.cpp`：760×920 整板渲染（Account + Servers + 终端卡展开态，
  带真实 VT 脚本产生的彩色输出、反显标题条和选中高亮），断言 Account 456、
  Servers 224、终端 168/392、宽 568，输出 `build\desk-board-preview.bmp`；
  M1 追加 ASSISTANT/MEMORY 两卡快照与 Pin/Edit/Clear 菜单回归。
- `tests/assistant_memory.cpp` / `assistant_context.cpp` / `pi_client.cpp` /
  `assistant_engine.cpp`：事实持久化/共享实例/通知/清空、上下文预算与敏感过滤、
  Pi RPC 生命周期、引擎本地记忆命令与失败/取消语义；`fake_pi_rpc.cpp` 构建
  `build\fake-pi-rpc.exe` 作合成 RPC 对端，engine 测试自身也可充当 peer。
  **最终官方 19/19、关键五项独立重复、SDK/example 构建均通过；隔离 GUI offline 83/83、
  probe 22/22、real 40/40，但“real”使用合成资料/assistant-only，不等于生产验收；
  证据与边界见 [architecture.md](architecture.md) §13.8。**
- `tests/ps_tests.cpp`：UTF-8/GBK 解码、字符网格与自动换行、宽字符占两格、CSI
  （CUP/ED/EL/IL/DL/滚动区）、SGR（16/256/真彩、粗体/下划线/反显）、备用屏、
  DECSET 25/1/2004、未知序列、回滚与跨回滚选中、键位编码（方向键/应用光标键/
  Ctrl/Alt/Home/Delete/bracketed paste）。
- `tests/pty_helper.cpp`：**无控制台**的 GUI helper（先 `FreeConsole()`），起一个
  真的 ConPTY、敲 `echo HELPER-OK`、把屏幕内容写进 `build\pty-report.txt`；
  `ps-test` 只负责拉起它并读回报告 —— 这是唯一能证明伪控制台可用的检查。
- 既有 `tests/render.cpp` / `tests/cli_tests.cpp` 保持不变；旧的
  `tests/server_status.cpp` 随旧 Server 组件归档。
- `build.bat test` 会调用 `tools\sign-local.ps1 -Path "build\*-test.exe"`；
  Smart App Control 下这是测试能启动的前提。

此前冻结源码（早于当前 M1 基线）的 QA 验收：**17 个场景 / 437 项检查 / 0 失败**，覆盖 fresh、
legacy760、corrupt、corrupt-badvalues、oob、cannot-fit、cannot-fit-huge、
drag-release、drag-small-board、reset-panel、drag-cancel、snapshot-clear、
adapt-clamp、restore-strict、consecutive-adapt、dpi-roundtrip，
以及程序化 `BoardHost` 路径的 `display-shim` 场景；结论以 `build\qa\runs\`
的 `summary.json` 为准。
测试用的未签名内容 sha256 是
`3C324C8FCB422F3C29380FC3049BCDBBB673A31E22B37FD7448286EB6569BDFD`
（1,814,016 B），发布文件签名后是
`91C05991CFD70E040279F2C40F6A8C4B8F7B751EAC8549C006F56AA319214EBA`
（1,815,472 B）：签名只是在末尾追加证书/签名，被测试的代码内容完全一致。

构建后运行 `tools\sign-local.ps1` / `-Path build\bin\desk.exe` 给主程序签名。
发布产物就是 `build\bin\desk.exe`（已用当前用户的本地证书签名；Smart App
Control 需要它才能启动）。安装到用户目录用 `tools\install-desk.ps1`，**只在
你准备替换正在运行的实例时执行**：构建、测试和验收脚本都不会碰已安装的
760×800 实例，也不会改它的配置或 HKCU Run。

### 隔离的 GUI 验收（build\testrun）

GUI 测试不能碰正在运行的 desk，也不能共用一个配置目录。`desk.exe` 读取环境变量
`DESK_TEST_INSTANCE=<后缀>`（`src/apps/desk/main.cpp`）：设置后

- 单实例 ID 和窗口类变成 `desk-<后缀>`，可以和真实实例同时运行；
- 关闭开机自启和旧启动项迁移，**不改 HKCU Run**；默认不装托盘、不注册任何全局热键；
- 配置目录默认仍是 exe 同目录，所以把 exe 复制到 `build\testrun\<场景>\` 就得到
  完全隔离的 `desk.ini` / `aimeter.ini` / `server.ini` / `ps-history.txt`；
  也可以用 `DESK_TEST_CONFIG_DIR=<绝对目录>` 指向一个**已存在且可写**的独立目录
  （路径缺失/相对/不可写会 fail-closed，不回退真实 `%APPDATA%`）。

M1 助理验收还需要这些隔离变量（详见 `tools\desk-assistant-test.ps1`）：

- `DESK_TEST_ASSISTANT_ONLY=1`：只注册 ASSISTANT/MEMORY，避免读真实用户状态或起 shell；
- `DESK_TEST_DSH_ROOT=<目录>`：隔离 DSH 投影缓存；未设置时测试实例使用
  `<exe>\test-dsh`（通常不存在 = 空 DSH），绝不读真实用户 DSH；
- `DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1`：仅在有效测试实例 + 有效配置目录时注册唯一的
  `Ctrl+Alt+F10` 物理探针，不占用生产 `Ctrl+Alt+A`；默认测试实例零全局热键；
- 像素证据用 PrintWindow；分层窗打印空白时用 own-canvas `WM_APP+11` 导出到配置目录，
  **不截桌面**。

单显示器上没法真的缩小工作区或切换 DPI。板子现在通过框架的
`DisplaySource`（`src/widget/display.cpp`）读工作区/DPI，所以测试钩子就是
**`ScriptedDisplaySource`**：`DESK_TEST_DISPLAY_FILE` 指向一个 ASCII 文本文件
（每行一个键）：

```text
work=x,y,w,h     # 模拟的主工作区（矩形，必须 w,h > 0）
dpi=N            # 可选，模拟 DPI（48..960，越界夹取）
rect=x,y,w,h     # 可选，DPI 变化路径的建议矩形（只对当前这次读取有效）
```

脚本源**每次被查询时都重读文件**：合法行更新对应值、非法行忽略、文件缺失/不可读
时保留上一次的有效值。`DESK_TEST_DISPLAY_FILE` 没设置时板子用系统来源；设置的
话只影响设置它的进程（`DESK_TEST_INSTANCE` 副本），真实实例不受影响。

驱动方式有两条，效果相同：

- **程序化 API**（推荐，嵌入/测试）：`BoardHost::ReadaptToDisplay()` 走工作区
  适配路径；`BoardHost::ApplySuggestedRect(RECT)` 走 DPI 路径。也可以直接给
  `BoardOptions::display` 注入自己的 `DisplaySource`（例如
  `ScriptedDisplaySource::Create(file)`）。
- **兼容消息**（现有 GUI harness 仍用）：向板窗口 PostMessage `WM_APP+9`
  （0x8009）触发 `ReadaptToDisplay()`，`WM_APP+10`（0x800A）读取脚本源的
  `rect=` 建议矩形并触发 DPI 路径（没有 `rect=` 时退回工作区适配）。这两个
  消息是兼容 shim，不是新接口。

`desk.exe` 自己仍然用 `RunBoard()`（内部是 `BoardHost`），所以这些能力对
桌面板是透明的。QA 的 **consecutive-adapt** 场景连续把工作区改成
2032×972 → 1600×800 → 2560×1392，验证最早快照不被覆盖、最后能恢复；
**dpi-roundtrip** 场景用 dpi144/1920×1310 → dpi96/1280×1310 验证 DPI 来回切换
能恢复原布局并清空 `saved.*`；新增的 `board-host-test` 还覆盖程序化路径。

`tools\desk-testrun.ps1`（fresh / legacy760 / corrupt / oob / cannot-fit /
drag-release / consecutive-adapt / dpi-roundtrip 等场景，截图 +
`build\qa\runs\` 证据）、`tools\desk-ui-test.ps1`（PS 卡宽度/列数/宽字符/
焦点/滚轮）和 `tools\desk-perf.ps1`（旧实例只读采样 + 新实例隔离测量）都基于
这套约定；它们只消息/结束自己启动的进程，并在前后对比 HKCU Run 快照。

## 9. 性能

本机实测（Windows 11，2026-11-07，接口批次后的**冻结构建**；下表取
`build\qa\perf\acceptance-clean` / `acceptance-clean2` 三次采样的中位）。
10 s 采样窗口；CPU 是单核百分比。新 = 1280×1310 面板的隔离实例，新 760 =
同一构建按旧尺寸跑。

**旧侧口径**：QA 复测时正在运行的 760×800 安装已经被关掉，所以旧列是同一台
机器、同一个采样工具在 **20:28** 对**未改动的已安装二进制**采的样——不是与
新版同一时刻，但作为同机基线仍然可比。

| 指标 | 旧 760×800（20:28 采样） | 新 1280×1310 | 新 760×800 |
|---|---|---|---|
| 空闲 CPU（10 s） | 0 ms | 0 ms | 0 ms |
| 悬停约 600 次移动 | 31.2 ms（0.31%） | 46.9 ms（0.47%） | 0 ms |
| 拖动约 600 次移动 / 10 s | 265.6 ms（2.59%） | 531.2 ms（5.2%） | 281.2 ms（2.8%） |
| PS 持续输出 10 s | 未测（不打扰在用终端） | 281.2 ms（2.8%） | — |
| PS 首帧延迟 | — | 35 ms（中位；33–52） | — |
| 空闲工作集 | 29.1 MB | 29.5 MB | 21.4 MB |
| 空闲私有内存 | 7.2 MB | 10.1–10.5 MB | 6.0–6.4 MB |
| 线程 | 9 | 12–13 | 12–13 |
| GDI / USER | 14 / 16 | 11 / 7 | 11 / 7 |

- 3 次运行（`acceptance` / `acceptance-clean` / `acceptance-clean2`，同一冻结构建）
  的范围：悬停 15.6–46.9 ms，拖动中位 531.2（500–609），PS 输出中位 281.2
  （172–469），同尺寸拖动中位 281.2（156–406），首帧 33–52 ms。
- 本次接口批次**没有改动任何逐帧路径**；差异属于机器负载波动，空闲 CPU 与几何
  结果与上一轮完全一致（fresh 1280×1310@(1280,0)、cannot-fit 952×478=455,056）。
- 空闲是事件驱动的（0 CPU）；悬停约 0.3–0.5% 单核；拖动成本随画布面积走
  （1280×1310 是 760×800 的 2.76 倍像素），同尺寸 281.2 ms 与旧安装 265.6 ms
  同一量级。
- PS 持续输出约占单核 2.8%（ConPTY 分块让这项波动较大）；首帧中位 35 ms，
  历史上偶尔出现 174 ms 异常值。
- 采集：`tools\desk-perf.ps1`（隔离实例 + 只读采样旧实例），原始证据在
  `build\qa\perf\`；单进程资源可用 `tools\bench-desktop.ps1 -ProcessName desk`。

## 10. 兼容与迁移

- `srvctl.exe` 不变：仍读 `server.ini`，仍使用 `providers.cpp`；
  Servers 卡片的 `SSH Hosts...` 写回同一文件。
- 旧独立组件已归档到 `archive/standalone`，不再构建；旧 exe 在 `archive/bin`。
- 首次运行清理旧自启项；之后 `desk.ini` 存在就不再动注册表。
- 旧的 `aimeter.ini` / `server.ini` 数据直接复用；`desk.ini` 只保存板/卡
  布局，互不覆盖。
- 旧 `desk.ini`（没有 `board.version`）一次性迁移到标准右半屏面板：先备份
  `desk.ini.pre-v2.bak`，规划成功才写入；失败保留原布局、不写版本号，
  下次启动重试（启动时的保存会重写同一组旧值，文件不是逐字节不变，但布局内容
  不变）。
- 显示器/DPI/工作区变化时内存里重新规划，旧矩形存到 `saved.*`；恢复原来的
  工作区会自动尝试恢复，菜单 `Restore previous layout` 随时可用。**保留最早的
  一份**：连续缩小时后续适配不会覆盖最初布局（实测链路见下一条）。任何一次
  用户改布局都会清掉快照。`Reset to right standard panel` 先备份
  `desk.ini.pre-reset.bak`。
- **DPI 变化走同一条路径**：`WM_DPICHANGED` 不再单独处理，而是和工作区变化
  共用「先尝试恢复 → 必要时快照 → 再规划建议矩形」的流程。本机实测：dpi144 +
  建议矩形 0,0,1920×1310 会快照原布局；随后 dpi96 + 建议矩形
  1280,0,1280×1310 能恢复原布局并清空 `saved.*`。
- **恢复是严格的**：只有快照里的面板矩形完整落在当前工作区内、且卡片位置能
  **原样**规划成功（不增长、不挪卡）才算恢复；否则快照继续待命、当前布局不动，
  菜单恢复时提示 `PREVIOUS LAYOUT DOES NOT FIT THIS DISPLAY YET`。不会用
  「夹取后的矩形」假装恢复成功。
- **保留最早快照的实测链路**：2560×1392 → 2032×972（`saved.board` = 最初
  1280×1310 @ (1280,0)，面板变成 1016×972）→ 1600×800（`saved.board` 仍是
  最初那份，面板变成 800×800）→ 回到 2560×1392（恢复 1280×1310 @ (1280,0)、
  卡片回到 (14,32)/(366,32)/(14,192)，清空 `saved.*`）。
- **启动适配先存原布局**：启动时若保存的矩形被工作区收缩/移动，框架先把原始
  矩形和原始卡片位置写进 `saved.*`，并提示
  `STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT`，再显示适配
  结果；换回大显示器可恢复。本机实测：保存 3000×2000 @ (0,0) 在工作区
  2560×1392 上启动 → 窗口 0,0 2560×1392，`saved.pending=1` 且
  `saved.board` 保留 3000×2000；Reset 清空 `saved.*`，保存并重启后确认不再
  出现。
- **落盘精确**：后台保存写的是内存快照的全部内容（`Store::SaveSnapshotFile`，
  临时文件 + `MoveFileEx`），不与磁盘旧文件合并；清掉的 `saved.*` 不会在
  重启后复活（QA 的 create → clear → restart 场景验证过）。

## 11. 限制

- 本机是单显示器。只有**面板外框**按 DPI 推导（96 DPI 名义尺寸 → 系统 DPI，
  之后跟随当前显示器工作区）；卡片尺寸、行高、字号仍是 96 DPI 基准的物理像素，
  所以在高 DPI 显示器上卡片相对面板会显得更小。宽度不超过工作区一半、高度不超过
  工作区，高 DPI 通常得到和 100% 时相近的面板尺寸。
- **启动时保存的布局排不下**不会中止程序：板照常启动，出问题的卡片标题条显示
  `NO SPACE`，底部提示 `LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL`；
  修复菜单就是普通板菜单，入口不变 —— 右键板标题条、托盘图标，或左键
  `ADD MODULE` 槽都能打开，里面有 Fit cards / Reset to right standard panel /
  Restore previous layout。
- **严格恢复的代价**：换显示器后如果快照里的原面板矩形在现在的工作区放不下，
  `Restore previous layout` **不会**成功（不会夹到工作区凑合），快照会一直
  待命，直到换回能完整容纳它的显示器。启动时的适配只影响当前显示，不影响
  已保存的原布局。连续多次显示/DPI 变化时保留的是**最早**那份快照，后一次适配
  不会把它改写成中间状态。
- **程序化接口的测试边界（残余风险）**：`BoardHost`/`BoardEvent`/body 通知/
  `DisplaySource`/`diag` 由单元测试（`module-body-test`、`display-source-test`、
  `diag-test`、`board-host-test`）和 GUI 的 `display-shim` 场景覆盖；但
  `BoardEvent` 流和诊断日志没有端到端 GUI 断言（只有 shim 路由），如果事件顺序
  或日志在真实桌面路径上回归，现有测试不一定能发现。
- 空白板面默认交给桌面；板菜单在标题条/托盘/Add 槽。
- 一个模块类型一张卡；卡片宽度由预设决定、不能单独拉伸；高度按内容排，超出预设
  范围会显示 `TOO TALL` 并被报告（框架不悄悄截断组件内容），比卡片区还高的卡
  在布局上会显示 `NO SPACE`，需要收起详情、移除卡片或放大面板。
- `Win+D` 会把板一起隐藏。
- 退出时若 SSH 探测在飞，最多等 8s。
- Servers 仅 Linux，最多 8 台；`srvctl` 最多 32 台。
- 不并入 SSH Pad / Clock / AI Mini；它们已归档，需要时按模块契约重新接入。
- POWERSHELL 卡：一个常驻子进程 + 一个 conhost（pwsh 7 空闲约 99 MB / 27 线程，
  5.1 约 72 MB），可用 `autostart=0` 推迟；要打字必须先点中卡片（这一刻板会被激活，
  但仍贴底）；行列固定 80×8 / 80×24，不能像真终端那样任意拉伸；回滚 2000 行；
  5×7 字体只有大写；不向应用上报鼠标事件（拖选始终是本地选中）。
- **ConPTY 的调用方必须没有控制台**：父进程若已经挂了控制台，客户端会被挂到那个
  控制台上，伪控制台只收到自己的初始化序列。`desk.exe` 是 GUI 进程，满足条件；
  测试由 `tests/pty_helper.cpp` 先 `FreeConsole()` 再跑（见 §8）。
- CommandCode `/alpha` 接口变化时该行 `NO DATA`。
- **M1 助理不是 OS 沙箱**：Pi 子进程强制 `--no-tools --exclude-tools *`，模型没有
  `read/write/edit/powershell` 等工具，但子进程仍以当前用户权限运行，私有环境变量块
  只隔离凭据的传递方式，不隔离文件/网络。敏感路径过滤、凭据字面量 fail-closed 与日志
  脱敏都是应用层尽力而为。
- M1 记忆的删除是软删除墓碑；`Pin` 只影响列表顺序，不刷新事实年龄、不改变检索语义、
  不保证免于事实数上限淘汰。`Clear all` 是多文件有序 best-effort（先写重置 id，再清
  facts/episodes），**不是事务，也不是安全擦除**；中途崩溃可能让元数据领先于数据。
- M1 已在限定隔离口径下验收：真实模型 + 真实应用桌面路径，但业务资料全部合成、
  assistant-only，未使用真实用户 DSH/其他真实卡。测试 idle 为 1 分钟，默认 10 分钟只由
  配置与毫秒 seam 覆盖，未实际等待 10 分钟。生产默认物理 `Ctrl+Alt+A`/F10 均未按下，
  未操作现网实例。
- Pi/官方 provider 属可信代码，仍可写安装临时锁/缓存；活跃生产目录只记录元数据变化并排除
  静态断言，不声称整个目录不变。密钥扫描是启发式，不是形式化证明；`Clear all` 多文件
  仍为有序 best-effort，非事务、非安全擦除。

## 12. 排错

M1 助理：`Ctrl+Alt+A` 临时召唤整板并聚焦 ASSISTANT，Enter 提问，Esc 取消。
失败优先显示错误文本，不把部分回答伪装成成功；取消后迟到片段不应复活 THINKING。
明确的“记住/忘掉/清空记忆”由本地确定性处理，不调用模型或记网络成本；
普通模型问答仅成功完成才写 episode，错误和取消不会修改长期事实。
MEMORY 与 ASSISTANT 共享记忆：成功“记住/忘掉”后列表自动更新；点选一行后用菜单
Pin/Unpin、Delete、Edit 或 Clear all，修改落盘并跨重启保留。Pin 只影响列表顺序，不刷新
事实年龄或保证永久保留；按事实 id 保持选中，重排不会误删其他行。Refresh 只重读进程内
快照，不要把它当作外部手改文件后的重新加载（需要重启卡片/程序）。写盘失败显示错误、
保留原事实；清空失败不会假装成功。
自动回归与真实 GUI 验收分开记录，最终证据与保留边界见
[architecture.md](architecture.md) §13.8。隔离验收已跑：offline 83/83、probe 22/22、
real 40/40；real 仍使用合成资料与 assistant-only，不等于生产验收。
`assistant.ini` 的 `provider/model/key_ref` 默认指向 CommandCode 的
`deepseek/deepseek-v4.1-flash-fast` 与现有凭据引用；不要把密钥值填进 ini。当前 Pi 子进程
是**零工具**运行（`tools` 键会被强制清空），因此“助理能读文件”不是当前能力，也不是
安全承诺。


| 现象 | 原因 / 处理 |
|---|---|
| 启动/测试 exe 报 `Application Control policy has blocked this file` | 本机 Smart App Control 处于强制（`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState=1`）。先 `tools\sign-local.ps1` 重签；自签根证书不在信任库里，SAC 仍可能拦新 hash，需要把证书导入 `Cert:\CurrentUser\Root` 或关掉 SAC。 |
| 卡里跑 `pi` / `npm` 报 `EPERM ... mkdir` | 写工作区外的路径被外层沙箱拦了（例如 desk.exe 由受限终端/IDE 启动，子进程继承同一限制）。换成正常启动（Explorer 双击），或让它把状态写在工作区内。 |
| 卡片一片空白、`bytes=0` | 伪控制台没挂上客户端：确认启动它的进程没有控制台（见 §11）。 |
| 卡片停在 `CONPTY START FAILED` | `CreatePseudoConsole` 失败（系统太旧或 conhost 起不来），`Restart shell` 重试。 |
| 启动时刷 `Error reading or writing history file ... Access is denied` | 旧版本会把 PSReadLine 历史指向全局 `ConsoleHost_history.txt`；现在改为卡片自己的 `ps-history.txt`（与 `ps.ini` 同目录）。若那条路径本身不可写，历史会丢但不影响 shell。 |
| 卡片拿不到键盘 | 必须先左键点一下卡片；`Type here` 菜单项也能拿。 |
| 卡片显示 `NO SHELL` | `ps.ini` 的 `shell=` 指向的路径不存在，且自动探测也没找到。`pwsh` 不在 PATH 时显式写绝对路径。 |
| 卡片标题条显示 `NO SPACE`（黄） | 这张卡当前没排进合法位置（启动时放不下或刷新后变高）。右键板标题条 → Fit cards（收起详情再排）或 Reset to right standard panel；也可以移除一张卡。 |
| 底部提示 `LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL` | 同上，板本身仍启动；菜单里有 Fit cards / Reset / Restore 三个修复入口。 |
| 卡片标题条显示 `TOO TALL`（黄） | 组件要求的高度超出预设范围。框架按实测高度布局并报告，不会裁切内容；需要组件自己减少内容或换更高的预设。 |
| 提示 `BOARD MIGRATION DEFERRED - LAYOUT DID NOT FIT` | 旧 `desk.ini` 迁移时放不下（例如显示器变小）。保留原布局、下次启动重试；`desk.ini.pre-v2.bak` 已生成。 |
| 提示 `DISPLAY SMALLER THAN THE STANDARD PANEL - SIZED TO FIT` | 工作区放不下 1280×1310 的面板，板按工作区缩小启动；这不是错误。 |
| 提示 `NO LEGAL POSITION FOR THIS CARD - IT STAYED WHERE IT WAS` | 拖放松手时整板没有合法空位，卡片回到拾起位置；先收起详情或移除一张卡。 |
| 提示 `STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT` | 这次启动时保存的面板矩形被工作区收缩/移动，原始布局已写入 `saved.*`。换回原来的显示器后可用菜单 Restore 恢复；当前布局可正常使用。 |
| 菜单 `Restore previous layout` 提示 `PREVIOUS LAYOUT DOES NOT FIT THIS DISPLAY YET` | 严格恢复要求原面板矩形完整放进当前工作区、且卡片能原样排下。当前显示器做不到，快照会继续保留；换回原显示器再试。 |
| 重启后布局/尺寸不对，怀疑测试实例影响 | 只有设置了 `DESK_TEST_INSTANCE` 的副本会改 id/配置目录；正常实例的 `desk.ini` 不会被测试脚本修改，且脚本会对比 HKCU Run 快照。设置 `DESK_TEST_CONFIG_DIR` 时路径必须已存在且可写，否则程序拒绝启动而不是回退真实 `%APPDATA%`。 |
| 看不到 ASSISTANT / MEMORY 卡 | 检查 `assistant.ini` 的 `enabled`；为 0 时两张卡都不注册。把 `enabled` 改回 1 后重启。 |
| 提示 `ASSISTANT HOTKEY UNAVAILABLE - RIGHT CLICK: SUMMON ASSISTANT` | 召唤热键被其他程序占用。用板菜单 `Summon assistant`，或改 `assistant.ini` 的 `hotkey` 后重启。 |
| Pi 启动报 `trust.json.lock` EPERM / `PI CRASH 1` | desk 从受限完整性/受限沙箱启动，子进程继承了限制。用普通权限从 Explorer 启动，或安装到 `%LOCALAPPDATA%\Programs` 下运行；不要靠提权绕过。 |
| 回答报 `Unknown provider` | 本机 CommandCode provider 由扩展提供；`provider_extension` 必须指向已安装的官方扩展，不能把扩展全部关掉。 |

## 13. CARDSPEC（M2.0 开发候选）

**现用 M1 尚无此卡，本轮不部署 M2 候选。** 候选注册固定单张 CARDSPEC，默认隐藏，
入口为 Add module；Wide 568，手动刷新，不自动轮询。JSON 标题显示在 body，外框标题固定。

在隔离测试配置目录创建 UTF-8 `cardspec.json`，例如：

```json
{"version":1,"title":"今日目标","source":{"type":"inline"},"layout":"rows","rows":[{"label":"学习","text":"阅读三十分钟"}]}
```

进度条使用 `layout=bar`，行字段改为 `label/value/max`，如 `value=3,max=5`。
修改该 JSON 后手动刷新，无须重新编译或重启；缺失/非法/超限时卡内显示错误，修复再刷新可恢复。
`cardspec.ini` 是宿主可见性等状态文件，**不要把 JSON 写进 INI**；不支持任意路径或其他数据源。

v1 上限 32KiB、1..8 行，拒绝未知/重复字段、非法编码/控制字符和越界进度。
v1 不支持网络、任意文件数据源、exec 或模型自动生成/上板；完整格式与当前验证状态见
[m2-cardspec-plan.md](m2-cardspec-plan.md)。

## 14. HTTP-only v2（M2.1 离线候选，未部署）

v2 只在 spec 中引用 `profile`，并用 JSON Pointer 选择响应字段；URL 与批准记录在单独的
`cardspec.ini`（UTF-16LE 单 BOM）四个 `http.*` 键中，不能由 spec 自行授予网络权限。
初次加载只显示 Ready/Blocked、不请求网络；用户确认精确端点且保存成功后，手动刷新才取一次数据。
撤销会禁用后续请求、丢弃在飞结果，重新批准不复活旧数据。失败显示错误，不把上次成功值伪装新数据。

仅公开无鉴权 HTTPS/443，默认禁止代理、重定向、Cookie 和自动凭据，不支持任意私网/文件/exec。
源码与 24/24 离线回归已通过，但本机公开探针的 DNS 返回 198.18/15 保留地址，被策略安全拒绝；
**真实 TLS/IP 固定/peer/HTTP 成功链路仍未验收**。不要通过放开保留网段或自动代理兜底凑通过。
现用 M1 没有更新；参数、完整 v2 示例、日志与下一步见 [m2-http-plan.md](m2-http-plan.md)。
