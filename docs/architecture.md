# 架构：Desktop Desk

> 本文描述**当前代码**的结构、数据流、构建目标、状态文件与线程模型；新增模块或改动
> 构建/配置前先读这里。文档索引见 [README.md](README.md)（建设中）；用户向说明见
> [../README.md](../README.md)；框架 API 细节见 [framework.md](framework.md)，
> 卡片交互见 [desk.md](desk.md)，SSH 与 CLI 见 [server.md](server.md)、[cli.md](cli.md)。
>
> 核对基线：`_worktrees/assistant-m1` 的 `feat/assistant-m1` 工作树。M0a 已落地
> （多账号/月额度/沙箱/token/7 天历史/as-of），M1 助理/记忆/Pi RPC 已实现并通过
> 限定隔离验收；
> 最新串行官方 test 已完成 19/19，关键五项独立重复、framework/example 均已通过；
> 隔离 GUI/真实模型通道也已完成限定验收（证据与边界见 §13.8），不以自动回归代替。
> 需求与计划是冻结快照（[assistant-prd.md](assistant-prd.md)、
> [assistant-plan.md](assistant-plan.md)、[assistant-m1-plan.md](assistant-m1-plan.md)），
> 冲突时以本文 + 代码为准，差异另见 [refactor-notes.md](refactor-notes.md) §6。

## 1. 项目定位

一个常驻桌面层的**洞洞板**：一个进程、一块板、若干可自由摆放的卡片。当前五个注册模块；
ASSISTANT/MEMORY 只在 `assistant.ini` 的 `enabled=1`（默认）时加入，`enabled=0` 时**两张卡都不注册**（见 §13）：

| 模块 | id | 卡片 | 宽度 / 高度 | 刷新 |
|---|---|---|---|---:|
| Account（AiMeter） | `account` | CommandCode 多账号、Codex 额度、本机 token、7 天历史 | Standard 340，144（开 DeepSeek 176）/ 456（2 账号展开） | 300s |
| Servers | `servers` | SSH 只读服务器指标 | Standard 340，176 / 224（8 台 304 / 352） | 30s |
| PowerShell | `ps` | ConPTY 真终端 | Wide 568，168 / 392（80×8 / 80×24） | 0（推送式） |
| Assistant | `assistant` | Pi RPC 流式问答、引用、BRIEF | Wide 568，168 / 198–688（按 BRIEF/答案行数） | 0（推送式） |
| Memory | `memory` | 与助理共享长期事实、Pin/Edit/Delete/Clear、变更订阅 | Standard 340，112–628（3/17 行，按事实数） | 0（推送式） |

附带的 `srvctl.exe` 是独立控制台程序，与 Servers 卡共享 `src/data/server` 与
`server.ini`，不是框架的一部分。

## 2. 分层与模块地图

依赖方向：应用 → 组件 → 数据源 → 框架；框架**不**依赖 `src/apps`、`src/panels`、
`src/data`。

```text
src/apps/desk/main.cpp        应用入口：创建模块、BoardOptions、DESK_TEST_INSTANCE
         │
src/panels/{account,servers,ps}/   卡片组件：Module/ModuleT<Data> 实现 + 视图
         │
src/data/{aimeter,server,ps}/      业务协议、文件格式、终端状态（无窗口）
         │
src/widget/                       框架：公共头 + Windows 宿主实现（libwg）
src/cli/                          srvctl（独立应用，复用 data/server + widget/proc）
```

### 2.1 入口与注册

`src/apps/desk/main.cpp` 负责注册与宿主选项：

1. 先解析配置目录（`wg::appenv::ConfigDir`，失败则弹窗退出），再从同一目录的
   `assistant.ini` 读 `enabled` 与 `hotkey`（默认 `Ctrl+Alt+A`，非法值回退到默认并写
   调试输出）；
2. `CreateAccountModule()`、`CreateServersModule()`、`CreatePsModule()`，随后在
   `enabled!=0` 时追加 `CreateAssistantModule()`、`CreateMemoryModule()`；注册顺序就是
   卡片索引（测试实例可用 `DESK_TEST_ASSISTANT_ONLY=1` 跳过前三张卡）；
3. 填 `wg::BoardOptions`：`id=desk`、`title=Desktop Desk`、`configFile=desk.ini`、
   `boardTitle=DESKTOP BOARD`、`startWithWindows=true`、
   `legacyAutostartNames={AiMeter, Server Status, SSH Pad}`，以及助理召唤热键；
4. 若设置了 `DESK_TEST_INSTANCE=<后缀>`（只允许字母/数字/`-`/`_`），id 改成
   `desk-<后缀>`、关闭自启/托盘/全局热键与旧启动项迁移（隔离测试实例，见 §8.4）。
   只有同时通过 `DESK_TEST_CONFIG_DIR` 校验并显式设置
   `DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1`，才注册唯一的 `Ctrl+Alt+F10` 探针热键。

`wg::RunBoard(...)` → `wg::BoardHost` → `Board::Start()/Run()`（`src/widget/board.cpp`）。
组件的注册顺序就是卡片索引（菜单、`BoardEvent::card`、`PlanPreview` 都用它）。

### 2.2 框架（`src/widget/`）

| 单元 | 职责 |
|---|---|
| `board.h` / `board.cpp` | `BoardOptions`、`RunBoard`、`Board` 宿主：窗口、DIB、调度、拖动、菜单、托盘、热键、持久化、布局规划 |
| `host.h` | `BoardHost`：Start/Run/Quit、Metrics、PlanPreview、显示适配、程序化卡片操作 |
| `module.h` | 组件契约 `Module` / `ModuleT<Data>`、`ModuleSpec`、`StatusChip`、`MenuItem`、`ClampBodyHeight` |
| `size.h` | 尺寸预设、`MeasureCardSize`、`kCardChromeHeight=48`、超范围报告 |
| `board_layout.h/.cpp` | 单卡纯几何：吸附、夹取、落点、命中测试 |
| `board_plan.h/.cpp` | 整板规划：确定性候选顺序、原子失败、可诊断状态 |
| `board_grow.h/.cpp` | 最小面积自动增长搜索（8px 宽度扫描、右对齐、有界） |
| `display.h/.cpp` | `DisplaySource`：系统/脚本工作区与 DPI 来源 |
| `diag.h/.cpp` | 线程安全诊断日志 sink |
| `skin.h/.cpp` | 板面/卡片/行/详情/终端/Add 槽的视觉令牌与原语 |
| `canvas` / `cjk` / `view` / `layout` / `geom` | 像素画布、汉字排版、行视图、通用布局、`Rect` |
| `store` / `appenv` | UTF-16 键值配置与原子保存；配置目录、自启 |
| `http` / `secrets` / `prompt` | WinHTTP（同步，worker 用）、Windows 凭据、输入框 |
| `proc` / `conpty` | 子进程封装、Windows 伪控制台 |

框架公共头都在 `src/widget/*.h` 顶层；`build.bat framework` 只复制**顶层**头文件，
所以未来若拆出内部头（例如 `board_impl.h`），应放到 `src/widget/internal/` 子目录，
避免污染 SDK。

### 2.3 组件（`src/panels/`）

每个目录一个模块，`build.bat` 自动 glob `src/panels/*` 并设置 `PANELCOUNT` 下限 1：

| 目录 | 类 | 数据 | 说明 |
|---|---|---|---|
| `account/` | `AccountModule : ModuleT<aimeter::AimeterData>` | `data/aimeter` | 3 行固定 provider；展开显示本机 token 明细 |
| `servers/` | `ServersModule : ModuleT<server::ServerData>` | `data/server` | 每台两行；详情补充失败原因/CORES/ssh 路径/UPDATED |
| `ps/` | `PsModule : Module` | `data/ps` | 直接实现 `Module`（流式终端，自行加锁持有 `ps::Terminal`） |
| `assistant/` | `AssistantModule : Module` | `data/assistant` | Wide 输入/流式回答/引用页脚/BRIEF；Enter 发送、Esc 取消，不轮询 |
| `memory/` | `MemoryModule : Module` | `data/assistant/memory.*` | 与引擎共享同一 `MemoryStore`；菜单 Pin/Unpin、Delete、Edit、Clear all、Refresh |

组件只画 body，不画外框/标题条、不建窗口。菜单、热键、键盘焦点都由板转发。

### 2.4 数据源（`src/data/`）

| 目录 | 单元 | 职责 |
|---|---|---|
| `aimeter/` | `providers`（DeepSeek 余额、Codex 离线额度、格式化/金额）、`token_usage`（DSH projcache 本机 token）、`commandcode`（`/alpha/billing/credits` 解析与抓取）、`accounts`（多账号配置/凭据/抓取/聚合/CarryForward）、`history`（每日快照、90 天裁剪、7 天序列）、`collect`（模块级 `Refresh`，拼行并落快照） | Account 卡数据 |
| `server/` | `providers`（ssh 探测、`/proc` 脚本、错误分类、状态码）、`collect`（主机列表与刷新）、`status_view`（共享行文本/状态） | Servers 卡 + srvctl |
| `ps/` | `text`（UTF-8/GBK 解码）、`shell`（shell 解析与启动）、`selection`、`screen`、`vt`（VT 解析）、`terminal`（ConPTY + 读线程 + 回滚）、`keymap`（按键编码）、`pi_process`（Pi 滚轮路由） | PowerShell 卡 |
| `assistant/` | `json`（JSON + UTF-8↔UTF-16）、`pi_client`（Pi RPC 子进程）、`memory`（事实/episodes/重置元数据/共享实例/订阅）、`context`（DSH 投影缓存 + 预算装配 + 发送日志）、`engine`（问答/本地记忆命令/引用/会话/成本） | ASSISTANT 卡 + MEMORY 卡 |

数据源都是无窗口、可被无窗口测试直接调用的代码。

### 2.5 CLI（`src/cli/`）

`args` / `commands` / `config` / `help` / `main` / `output` / `remote` / `targets`。
`build.bat` 额外把 `src/data/server/providers.cpp` 编进 srvctl，链接 `libwg.a`；
配置查找顺序：`--config` → `SRVCTL_CONFIG` → `srvctl.ini` → `server.ini`（只读继承）。

## 3. 构建目标与产物

### 3.1 命令

```bat
build.bat             rem 框架 + desk.exe + srvctl.exe
build.bat framework   rem 只构建框架 SDK -> build\sdk
build.bat example     rem 只用 SDK 构建 examples\minimal-board -> build\bin\minimal-board.exe
build.bat test        rem 上面全部 + 无窗口测试（签名测试 exe 后运行）
```

工具链：MinGW-w64 GCC（C++20）。`build.bat` 默认 `TOOLROOT=C:\Tools\mingw64`，
设置同名环境变量可换工具链；找不到 `g++` 直接失败。公共编译参数：
`-std=c++20 -O2 -pipe -Wall -Wextra -DNOMINMAX -DUNICODE -D_UNICODE`，静态链接
（`-static -static-libgcc -static-libstdc++`），链接 `shell32 user32 gdi32 ole32
uuid winhttp advapi32 imm32 version`。`TMP`/`TEMP` 被改到 `build\tmp`：MinGW 汇编器
无法处理非 ASCII 临时路径（本仓库路径含中文）。

构建门禁使用外层调用与显式完成标记：仅 framework/example/all/test 走到各自成功末尾
才设置内部完成标记并返回 0；中途编译、链接、签名或测试退出都不得被当成成功。
这是对本机观测到的部分 cmd 嵌套块失败返回 0 的防御，不改构建目标或源清单。
串行验收同时核对 `[build] run tests`、各项结果、`check-sources: ok` 与最终完成标记，
不能单凭退出码或旧 exe 存在判绿。

### 3.2 源清单规则（`build.bat` 是唯一清单）

| 源码位置 | 收集方式 |
|---|---|
| `src/widget/*.cpp` | 显式 `FW` 列表（18 个：`canvas cjk view layout store http secrets prompt proc conpty skin board board_layout board_plan board_grow display diag appenv`） |
| `src/apps/desk/*.cpp` | 自动 glob |
| `src/panels/*/*.cpp` | 自动 glob（目录名即模块 id） |
| `src/cli/*.cpp` | 自动 glob |
| `src/data/aimeter/*.cpp` | 显式列表：`providers token_usage commandcode collect` |
| `src/data/server/*.cpp` | 显式列表：`providers status_view collect` |
| `src/data/ps/*.cpp` | 显式列表：`text shell selection screen vt terminal keymap pi_process` |
| `src/data/assistant/*.cpp` | 显式列表：`json pi_client memory context engine` |
| `tests/*.cpp` | 显式列表（含 `pty_helper`、`pty_mouse_client` 两个辅助程序；
  `fake_pi_rpc.cpp` 构建成 `build\fake-pi-rpc.exe` 作为 RPC 测试对端） |

因此：

- 新增**框架单元**或**数据源 `.cpp`** → 必须让 Lead 把它加进 `build.bat`；
- 新增**面板** → 只要放进 `src/panels/<id>/` 就会被编译；
- `tools/check-sources.ps1` 会检查：非自动收集的每个 `.cpp` 必须出现在
  `build.bat` 文本里，每个 `.h` 必须被至少一个源文件引用。改清单后跑一次。

### 3.3 产物

| 产物 | 说明 |
|---|---|
| `build\bin\desk.exe` | 洞洞板主程序（GUI，`-mwindows`） |
| `build\bin\srvctl.exe` | SSH 命令行（控制台，`-mconsole`） |
| `build\bin\minimal-board.exe` | 仅用 SDK 的示例 |
| `build\sdk\include\widget\*.h` | 导出头（`src/widget` 顶层） |
| `build\sdk\lib\libwg.a` | 框架静态库 |
| `build\<name>-test.exe` | 无窗口测试；`build\cli-test.exe` 与 `build\ps-test.exe` 也在根目录 |
| `build\fake-pi-rpc.exe` | `tests/fake_pi_rpc.cpp` 的合成 Pi RPC 对端（测试 exe 自身也可充当 peer） |
| `build\preview.bmp` / `build\desk-board-preview.bmp` | 渲染测试输出 |
| `build\testrun\` / `build\qa\` | 隔离实例目录 / GUI 验收证据（`tools\clean.ps1` 默认保留 `qa`） |

`build.bat test` 构建并运行 19 个测试：`render-test`、`board-layout-test`、
`board-plan-test`、`size-presets-test`、`board-grow-test`、
`board-composition-test`、`module-body-test`、`display-source-test`、`diag-test`、
`board-host-test`、`store-test`、`ps-test`、`desk-render-test`、`cli-test`、
`aimeter-accounts-test`、`assistant-memory-test`、`assistant-context-test`、
`pi-client-test`、`assistant-engine-test`
（另构建 `pty-helper.exe`、`pty-mouse-client.exe` 给 `ps-test` 用）。

### 3.4 签名与安装

- Windows 11 Smart App Control 会拦未签名的新 exe：
  `powershell -ExecutionPolicy Bypass -File tools\sign-local.ps1` 用当前用户的自签
  证书重签（每次重新链接后都要重签）；`build.bat test` 自动给测试 exe 签名。
- `tools\install-desk.ps1` 把 `desk.exe` 和已有 `desk.ini` / `aimeter.ini` /
  `server.ini` / `ps.ini` / `ps-history.txt` 复制到
  `%LOCALAPPDATA%\Programs\Desktop Desk\`；要求中完整性级别（普通 PowerShell），
  不启动程序、不改启动项。构建/测试脚本不会碰已安装实例。

## 4. 运行时结构

```text
desk.exe
└─ Board（一个 wg 分层窗口、一张 DIB、一个 SetTimer、一个托盘、一套热键）
   ├─ BoardChrome：tint + 点阵 + 2px 边框 + 标题/拖动手柄 + 右下缩放把手
   ├─ 卡片层：标题条 30px + 上 8 + body + 下 10（外框 = 8px 取整(body) + 48）
   ├─ 调度：1 个定时器对齐最近到期刷新；每模块最多 1 个 worker
   │        焦点卡另有 530ms 光标定时器；拖动重绘 16ms 节流
   ├─ 命中：卡片/标题条/缩放手柄/Add 槽 → HTCLIENT；空白点 → HTTRANSPARENT
   ├─ 菜单/托盘/热键/持久化/自启/显示适配/BoardEvent
   └─ 模块运行时（每个注册模块一份）：Store + Module + ModuleSpec + 卡片状态 +
      CardSize + StatusChip + worker + busy/again/dueAt
```

`Board::Start()` 的顺序（`board.cpp`）：打开 `desk.ini`（`boardStore_`）→ 恢复板
矩形 → 创建模块运行时并 `Bind()` 配置/重绘/焦点/剪贴板回调 → 恢复卡片状态并
`Load()`（隐藏的已注册模块也会加载）→ 旧配置迁移/恢复快照 → 规划首个布局 →
安装托盘/热键 → 启动后台保存线程。`Run()` 只进消息循环；`Quit()`/`Shutdown()`
保存状态、移除托盘、注销热键、join worker 与保存线程。

## 5. 数据流

### 5.1 通用：Refresh → Commit → Paint（双缓冲）

`module.h` 的实现约定（`ModuleT<Data>` 已封装）：

```text
UI 线程                                        worker 线程
--------                                       -----------
Load()  -> LoadData(staging) -> shown
(空闲)                                         Refresh() -> RefreshData(staging)   // 可阻塞
Commit() <- swap(shown, staging) <------------ PostMessage(WM_APP+1) 完成
PaintBody(shown) / BodyHeight()
```

- worker 先把 `shown` 复制到 `staging`，再在 `RefreshData` 里修改 `staging`；
- 成功返回后，宿主在 UI 线程 **join worker**，用 `PendingBodyHeight()`
  （默认等于 `BodyHeight()`）先规划整板，规划通过才 `Commit()` 交换快照；
- 刷新与绘制可以并发：刷新只写 staging，绘制只读 shown；组件中 staging/shown
  之外的共享成员仍需自行同步（终端就是自己加锁）；
- 刷新抛异常 → 不提交，保留上一份数据，卡片标题条显示 `ERROR`（红）；
- `refreshSeconds=0` 表示只手动刷新（终端卡）。

### 5.2 Account 卡

#### 5.2.1 已提交基线（HEAD `3d02b55`）

`aimeter::Refresh(data, store, theme)` 在 worker 内完成一次全量快照：

1. **DeepSeek**：`wg::LoadSecret(L"AiMeter/DeepSeek")`（Windows 凭据管理器；无 key
   显示 `NO KEY`）→ `FetchDeepSeek` 调 `GET api.deepseek.com/user/balance` →
   余额文本（`CNY x.xx`）/ 分；`SpendToday()` 把当日余额快照写进 `aimeter.ini`
   （`day`/`start`/`last`，跨日时 `start=last`），今日花费 = `start - last`；
2. **Codex**：完全离线，读 `~/.codex/sessions/**/rollout-*.jsonl` 里的
   `rate_limits`，得到 5h / 周两个百分比（无网络）；
3. **CommandCode**：`LoadCommandCodeKey()` 先读凭据管理器
   `AiMeter/CommandCode`，找不到回退 `~/.dsh/.credentials.yaml` 的
   `COMMANDCODE_API_KEY` → `FetchCommandCode` 调未公开的
   `GET api.commandcode.ai/alpha/billing/credits`，解析 5h/周 `used/cap/exceeded`
   与 `monthlyCredits`；失败显示 `NET ERR` / `BAD KEY` / `ERR n` / `NO DATA`；
4. **本机 DSH token**：`ReadDshSessions()` 读
   `%USERPROFILE%\.dsh\storages\session_projcache\sessions\*.json` 的
   `identity.createdAt` 与 `tokenUsage.totals`（uncachedInput / cacheRead /
   cacheWrite / output）；`UpdateTodayTokens()` 用按 session 的快照差计算今日增量，
   写回 `aimeter.ini`（`tok.day`、`tok.sum`、`tok.s.<sessionId>`）；UI 标 `LOCAL`，
   不与账户余额混算；
5. 把结果拼成固定 3 行（DEEPSEEK / CODEX / COMMANDCODE），最后 `store.Save()`。

#### 5.2.2 M0a 数据层（已落地）

- `show_deepseek`（`aimeter.ini`，默认 0）：DeepSeek 行 opt-in；行集合 =
  DeepSeek（可选）+ Codex，CommandCode 不再是单行；
- `accounts.h/.cpp`：`LoadAccounts()` 读 `accounts=N` / `account.<i>.*`（label/keyref/
  source/monthly_limit），缺失时从 `~/.dsh/.credentials.yaml` 的
  `COMMANDCODE_API_KEY[_2...]` 导入一次并写回（幂等）；`FetchAccounts()` 最多 4 个
  账号并行抓取（每账号 8s、失败隔离）；`CarryForward()` 失败时保留上次成功值与
  `asOfMs`（供 `STALE` 判定）；`Aggregate()` 算最紧窗口与月余额合计；
  `IsStale`/`ResetCountdown`/`AggregateText` 是纯函数，供视图与测试共用；
- `history.h/.cpp`：`aimeter-history.ini` 每日快照、90 天裁剪、7 天柱状序列
  （`WeeklyUsageSeries`）；
- `collect.cpp`：每次刷新对每个账号做 `CarryForward`，把账号放进
  `AimeterData::accounts`，并 `SaveDay()` 写历史、`WeeklyUsageSeries()` 出 7 天数据；
- 视图层（`account_view.*`）已接线：聚合行 + 每账号行 + 重置/月额度/sandbox/来源/
  as-of 详情 + `skin::MiniBars` 7 天柱状图；标题状态 OK/WARN/ERROR。


### 5.3 Servers 卡

- `Load`：从 `server.ini` 读 `hosts`（最多 8 台）与 `ssh`，确定 ssh.exe；
- `Refresh`：每台一次只读探测（并行），一次往返内取 `/proc` 的 CPU/内存/根分区/
  load/uptime；单台 8s 超时，互不拖累；
- 错误分类：`TIMEOUT` / `AUTH` / `DNS` / `REFUSED` / `HOSTKEY` / `UNREACH` /
  `CLOSED` / `NO SSH` / `NO DATA`；成功为 `UP <RTT>`；
- 卡片 `SSH Hosts...` 只写 `server.ini` 的 `hosts=`；`srvctl status` / `hosts`
  读同一份配置，改完立即生效（详见 [server.md](server.md)）。

### 5.4 PowerShell 卡

- 直接继承 `wg::Module`（不是 `ModuleT`），状态由 `ps::Terminal` 自己加锁；
- `ps.ini` 决定 shell（`shell=` → `%PWSH_EXE%` → PATH 的 pwsh →
  `%ProgramFiles%\PowerShell\7` → `L:\powershell` → 5.1）与 `autostart`；
- `CreatePseudoConsole` 起交互式 shell，读线程把输出字节解码后喂给 VT 解析器，
  得到字符网格；解析完调 `RequestRepaint()`（33ms 节流），绘制时取可视窗口快照；
- 终端是流，没有"一次刷新一份快照"的边界；`refreshSeconds=0`，手动刷新不会重启
  shell；
- 行列固定 80 列；展开/收起经 `OnBodySizeChanged`/`OnExpandChanged` 调
  `ResizePseudoConsole`（8 行 ↔ 24 行）；回滚 2000 行；历史写在 `ps-history.txt`。

### 5.5 持久化流

UI 线程只改内存里的 `boardStore_` 并调 `SaveState()`（把板/卡键做成
`std::map<wstring,wstring>` 快照入队）；后台保存线程用
`Store::SaveSnapshotFile`（临时文件 + `MoveFileEx` 原子替换）写入**完整快照**，
不与磁盘旧文件合并。所以内存里删掉的键（如已清除的 `saved.*`）不会在重启后复活。
退出时排入最后一份快照并 join。

## 6. 线程与刷新模型

| 线程 | 数量 | 做什么 | 约束 |
|---|---|---|---|
| UI/主线程 | 1 | 窗口消息、布局、绘制、菜单、键盘/输入法、Commit、`BoardEvent`、模块 `OnBodySizeChanged`/`OnExpandChanged` | 不阻塞；不等待网络/SSH |
| 模块 worker | 每模块最多 1 个（同时） | `Module::Refresh()` / `RefreshData()`，可阻塞 | 只写 staging；不碰 Canvas/窗口；完成时 `PostMessage(WM_APP+1)` |
| 终端读线程 | 1（PS 卡） | ConPTY 读取 → 解码 → VT 解析 → `RequestRepaint()` | 由 `ps::Terminal` 自己加锁；快照给 UI 画 |
| 助理 engine worker | 每次问答/本地记忆命令最多 1 个 | 建上下文、提交 prompt、引用校验、episode/成本持久化；pi 事件回调也落在这里 | 只通过卡片线程安全 setter 更新 UI；不阻塞 UI；Stop 静默回收 |
| Pi reader 线程 | 每个 PiClient 1 个 | 读取子进程 stdout JSONL、按请求 id 关联响应、投递 `PiEvent` | 不解析 stderr；无 id 事件只在本地 Ask 拥有 run 时接收 |
| engine 空闲线程 | 1（每 Engine） | `idle_stop_minutes` 到期后串行 Stop Pi 子进程 | 与 worker 启动/Ask 用生命周期锁串行；UI Cancel 不等待它 |
| 后台保存线程 | 1 | 内存快照 → `Store::SaveSnapshotFile` 原子落盘 | 只写文件；不碰 UI 状态 |
| 系统回调 | — | 托盘消息、全局热键、前台窗口事件钩子 | 都回到 UI 线程处理 |

刷新调度：

- `ArmTimer()` 遍历可见且 `refreshSeconds>0` 的模块，取最近 `dueAt` 设**一个**
  `SetTimer`（间隔夹在 50ms–600000ms）；没有到期项就不设定时器（空闲 0 CPU）。
- `WM_TIMER` 对到期的模块调 `StartRefresh(i)`：先把 `dueAt` 推到下一个周期；
  若该模块已有 worker（`busy`），只置 `again=true`，不并发第二个。
- worker 结束 PostMessage → `FinishRefresh(i)`：join → 用
  `PlanLayout(boardRect_, i, i, allowGrow=true)` 以 `PendingBodyHeight()` 预排 →
  规划成功才 `Commit()` 换快照并 `SaveState()`；规划失败则丢弃 staging、保留旧数据，
  卡片标题条显示 `NO SPACE`；刷新失败显示 `ERROR`（保留上一份数据）。
- 成功后重建布局、重绘、重新 `ArmTimer()`；若期间又到期（`again`）立即再跑一次。
- 手动刷新（卡片菜单 / `Ctrl+Alt+R` / `RefreshCard`）走同一个入口。
- `RefreshAll()` 只对可见卡逐个 `StartRefresh`；终端卡的 `Refresh()` 是空实现。
- 展开/收起、增删卡、拖放落位、面板缩放都走同一套整板规划；失败是原子的
  （不改布局），并给出可修复提示。

## 7. 状态文件

配置目录：**exe 同目录**；若不可写，整体回退到 `%APPDATA%\Desktop Desk`
（`wg::appenv::ConfigDir`，按应用标题命名）。所有 ini 都是 UTF-16 `key=value`，
由 `wg::Store` 读写（`Save()` 原子替换）。

| 文件 | 写入方 | 内容 | 现状 |
|---|---|---|---|
| `desk.ini` | Board（`BoardOptions::configFile`） | 板/卡片几何、展开、可见性、显示变化快照 | 当前 |
| `aimeter.ini` | Account 模块（`configFile`） | DeepSeek 每日余额快照、本机 DSH token 快照 | 当前 |
| `server.ini` | Servers 模块 + srvctl（共享） | `hosts` / `ssh` / `key` / `timeout` | 当前 |
| `ps.ini` | PowerShell 模块 | `shell` / `autostart` / `wheel.mode` | 当前 |
| `ps-history.txt` | `ps::Terminal`（PSReadLine） | 终端历史（卡片私有，不污染全局） | 当前 |
| `srvctl.ini` | srvctl | 命令行自己的 `hosts`/`ssh`/`key`/`timeout`；不存在时只读继承 `server.ini` | 当前 |
| `aimeter-history.ini` | Account 模块（`history.*`） | 90 天每日快照（额度/月余额/token） | 当前 |
| `assistant.ini` | ASSISTANT 模块 / Engine / main | 启用、热键、上下文预算、空闲退出、日 token 上限、渠道与 Pi 命令覆盖（§7.5） | 当前 |
| `persona.md` | Engine | 助理人格与规则；首次缺失时生成默认文本，之后可由用户编辑 | 当前 |
| `assistant-episodes.jsonl` | MemoryStore | 只追加问答回合（JSONL） | 当前 |
| `assistant-facts.jsonl` | MemoryStore | 长期事实；原子改写，含软删除墓碑与可选 `pinned` | 当前 |
| `assistant-memory-reset.txt` | MemoryStore | 一行重置 id：ClearAll 的持久见证，使旧 Pi 会话失效 | 当前 |
| `assistant-session.ini` | Engine | 可续用的 Pi 会话路径 + facts/policy/memory-reset 摘要 | 当前 |
| `assistant-import.ini` | Engine | 已导入 DSH 项目 id 的 checkpoint（防止清空后重新导入） | 当前 |
| `assistant-cost.ini` | Engine | 每日 token/调用次数/耗时 | 当前 |
| `assistant-send.log` | ContextBuilder | 每次发送的 id/文件名/大小摘要，不含 prompt/答案正文 | 当前 |
| `pi-sessions/` / `pi-agent/` / `pi-work/` | PiClient / Engine | Pi 自己的会话目录、私有 agent 配置目录、子进程工作目录 | 当前 |
| `memory.ini` | MEMORY 模块（`ModuleSpec::configFile`） | 仅作为卡片配置锚点，当前没有读写键 | 当前 |
| `desk.ini.pre-v2.bak` / `desk.ini.pre-reset.bak` | Board | 几何迁移 / Reset 前的备份 | 当前 |

旧组件与新版**共享** `aimeter.ini` 的当日快照，不要同时运行新旧程序。

### 7.1 `desk.ini`

| 键 | 含义 |
|---|---|
| `board.x/y/w/h` | 板屏幕矩形（首次运行按工作区/DPI 计算，之后保存） |
| `board.version` | 几何版本，当前 `2`；全新安装首次运行即写入 |
| `board.grid` | 1 = 显示点阵（视觉；吸附步长来自 `BoardOptions::grid`，默认 8px） |
| `board.solid` | 1 = 空白板面也接收点击（默认空白点交给桌面） |
| `card.<id>.x/y` | 卡片在板内的位置 |
| `card.<id>.visible` | 0 = 移除（保留位置，Add module 可恢复） |
| `card.<id>.expanded` | 是否展开详情 |
| `saved.board.*` / `saved.card.<id>.*` / `saved.pending` | 显示/DPI/工作区变化前（或启动被适配前）的矩形快照，供 `Restore previous layout`；**只保留最早一份** |

首次运行（无 `desk.ini`）示例（本机 2560×1392 工作区、1280×1310 面板）：

```ini
board.x=1280
board.y=0
board.w=1280
board.h=1310
board.version=2
board.grid=1
board.solid=0
card.account.x=14
card.account.y=32
card.account.visible=1
card.account.expanded=0
card.servers.x=366
card.servers.y=32
card.servers.visible=1
card.servers.expanded=0
card.ps.x=14
card.ps.y=192
card.ps.visible=1
card.ps.expanded=0
```

旧 `desk.ini`（无 `board.version`）会先备份为 `desk.ini.pre-v2.bak`，再一次性迁移到
标准右半屏面板；规划成功才写新几何与版本号，失败保留原布局、下次启动重试。
`Reset to right standard panel` 先备份 `desk.ini.pre-reset.bak`。

### 7.2 `aimeter.ini`（Account 模块）

| 键 | 含义 |
|---|---|
| `day` / `start` / `last` | DeepSeek 当日快照：日期、当日开盘余额、最近一次余额（单位：分）；跨日时 `start=last` |
| `tok.day` | 本机 DSH token 的统计日期（`YYYY-MM-DD`） |
| `tok.sum` | 当日累计四桶：`uncachedInput,cacheRead,cacheWrite,output` |
| `tok.s.<sessionId>` | 每个 DSH session 的上次累计值（同样的四桶顺序），用于增量差 |

日期变化时 `tok.s.*` 会被 `RemovePrefix` 清空、`tok.sum` 归零。`Refresh` 末尾统一
`Save()` 一次。

### 7.3 `server.ini` / `ps.ini` / `srvctl.ini`

`server.ini`（Servers 卡与 srvctl 共用，srvctl 只读继承、首次编辑才写自己的
`srvctl.ini`）：

```ini
hosts=web-01, db-01
ssh=
key=
timeout=8
```

- `hosts`：逗号/分号/空格分隔，Servers 最多显示 8 台，srvctl 最多 32 台；
- `ssh` / `key`：可选覆盖 ssh.exe 与私钥路径；
- `timeout`：单台探测超时秒数，默认 8；
- `;` 是值的一部分，不要写行尾注释。

`ps.ini`：

```ini
shell=
autostart=1
wheel.mode=auto
```

- `shell`：显式 shell 路径；空则按 §5.4 的顺序探测；
- `autostart=0`：不预热，第一条命令时才拉起 shell（省 ~99MB 工作集）；
- `wheel.mode`：`auto` / `history` / `application`（滚轮路由，见 [desk.md](desk.md)）。

### 7.4 凭据

- DeepSeek / CommandCode key 存 **Windows 凭据管理器**：
  `AiMeter/DeepSeek`、`AiMeter/CommandCode`；
- CommandCode 未手动设置时回退到 `~/.dsh/.credentials.yaml` 的
  `COMMANDCODE_API_KEY`（旧语义）；
- key 只在内存中传给 WinHTTP；日志、UI、ini、历史文件只记录来源名，不记录值；
- DSH token 数据是**本机**口径（`~/.dsh/storages/session_projcache`），标 `LOCAL`；
- M1 助理按 `provider` 推导环境变量名（默认 `COMMANDCODE_API_KEY`），用
  `key_ref`（默认 `COMMANDCODE_API_KEY_2`）从 DSH 凭据库取值，只放进子进程私有
  `childEnv`；**argv 不传 key**，也不改父进程环境（见 §13.3）。日志/错误文本按
  已知凭据值和凭据形态标记脱敏。

### 7.5 `assistant.ini` 与 M1 状态文件

`assistant.ini`（配置目录，UTF-16 key=value）：

| 键 | 默认 | 含义 |
|---|---|---|
| `enabled` | 1 | 0 = 不注册 ASSISTANT/MEMORY 两张卡（引擎/记忆不会被加载） |
| `hotkey` | `Ctrl+Alt+A` | 召唤热键；非法文本回退默认并写调试输出，不静默换键 |
| `context_tokens` | 6000 | 上下文 token 预算；小于 500 按 500 |
| `idle_stop_minutes` | 10 | Pi 空闲退出分钟数；0 = 不自动退出 |
| `daily_token_limit` | 0 | 每日 token 上限；0 = 不限制 |
| `pi_command` | 空 | 显式启动器覆盖；空 = `node <managed cli.js>`，再退 PATH `pi.cmd` |
| `provider` | `command-code` | 显式渠道；空 = 交给 Pi 默认 |
| `model` | `deepseek/deepseek-v4.1-flash-fast` | 显式模型；空 = 交给 Pi 默认 |
| `key_ref` | `COMMANDCODE_API_KEY_2` | DSH 凭据引用名（不是密钥值） |
| `provider_extension` | 空 | 已安装官方 provider 扩展的绝对路径；空 = 解析默认安装路径 |
| `tools` | 空 | **兼容键**：非空时加载清空并回写，旧 `read/edit/...` 白名单不会转发给 Pi |

状态文件：`persona.md`、`assistant-episodes.jsonl`、`assistant-facts.jsonl`、
`assistant-memory-reset.txt`、`assistant-session.ini`（`session_path` /
`facts_digest` / `policy_digest` / `memory_reset`）、`assistant-import.ini`
（`imported`）、`assistant-cost.ini`（`day.<YYYY-MM-DD>.tokens` / `.calls` /
`.ms`）、`assistant-send.log`、`pi-sessions/`、`pi-agent/`、`pi-work/`。
记忆事实与 episode 是 UTF-8 JSONL；其余 ini 由 `wg::Store` 原子保存。

### 7.6 `aimeter-history.ini`（M0a 已落地）

配置目录下的 UTF-16 key=value 文件（`wg::Store` 原子保存）：

```text
day.<YYYY-MM-DD>.cc.<i>.five.used / .five.cap
day.<YYYY-MM-DD>.cc.<i>.week.used / .week.cap
day.<YYYY-MM-DD>.cc.<i>.monthly
day.<YYYY-MM-DD>.tokens.input / .output / .cacheRead / .cacheWrite
```

每次刷新写当日值（覆盖），保留 90 天（超期 `RemovePrefix` 裁剪），读取只在 worker
线程；没有快照的日期保持缺失（`-1`），不补零。首次导入的账号配置写在 `aimeter.ini`
的 `accounts=N` / `account.<i>.*`。

## 8. 显示、几何与布局

### 8.1 面板矩形

- `BoardOptions::width/height` 是 **96 DPI 下的名义外框尺寸**，默认 1280×1310，
  最小 400×420；
- 首次运行按系统 DPI `MulDiv` 缩放，再夹到
  `w ≤ max(minWidth, 工作区宽/2)`、`h ≤ 工作区高`，贴工作区右上角
  （`x = work.right - w`、`y = work.top`）；
- 本机参考：工作区 2560×1392、96 DPI → 面板 1280×1310 @ (1280,0)；
- 之后每次启动读 `board.x/y/w/h` 并夹到当前工作区（被收缩时先把原始矩形写进
  `saved.*`，给 `Restore previous layout` 用）。

卡片区（`Board::InteriorFor`）：

```text
interior.x = 14
interior.y = 26(板标题) + 6
interior.w = 板宽 - 2×14
interior.h = 板高 - 26 - 6 - 14 - 16(右下缩放把手)
```

1280×1310 → 卡片区 `x=14, y=32, w=1252, h=1248`。

### 8.2 尺寸预设

| 预设 | 宽度 | 外框高度范围 | 当前用途 |
|---|---:|---:|---|
| `Compact` | 256 | 96–512 | — |
| `Standard` | 340 | 112–640 | Account、Servers |
| `Wide` | 568 | 168–720 | PowerShell（80 列 × 7px + 2×4px） |
| `Full` | 卡片区宽度（本机 1252，随面板缩放重算） | 208–720 | — |

高度公式：`外框 = 8px 向上取整( max(预设最小高度, bodyHeight + 48) )`，
其中 48 = 标题条 30 + 上 8 + 下 10。范围是**校验边界不是裁切指令**：超出时按实测
高度布局并在标题条显示 `TOO TALL`。

### 8.3 自动增长与恢复

放不下时用 `board::FindMinimalGrow` 找**面积最小**的可行面板：宽度从当前宽度
每次 +8px 扫到右半屏上限；每个宽度把高度按 8px 采样后逐像素细化（不假设"越高越
容易放下"）；候选贴工作区右边缘；失败保持原布局。启动、刷新、展开/收起、增删卡、
拖放、面板缩放都走同一套"先规划、后应用"。显示/DPI/工作区变化共用一条
"先尝试恢复 → 必要时快照 → 再适配"的路径；恢复是**严格**的（原矩形必须完整放得
进工作区且卡片原样排下），否则快照继续保留。

### 8.4 测试钩子

- `DESK_TEST_INSTANCE=<后缀>`：隔离实例（id/窗口类/互斥体加后缀，不迁自启、
  不改 HKCU Run，默认不装托盘、不注册任何全局热键）；
- `DESK_TEST_CONFIG_DIR=<绝对目录>`：必须已存在且可写，否则 `ConfigDir` fail-closed；
  设置后配置不再落在 exe 目录。测试实例没有可写 exe 目录时也**不会**回退到真实
  `%APPDATA%`；
- `DESK_TEST_ASSISTANT_ONLY=1`：只注册 ASSISTANT/MEMORY，跳过 Account/Servers/PS，
  避免读真实用户状态或起 shell；
- `DESK_TEST_DSH_ROOT=<目录>`：隔离 DSH 根；助理测试实例未设置时解析到
  `<exe dir>\test-dsh`（通常不存在 = 空 DSH），绝不读真实 `%USERPROFILE%\.dsh`；
- `DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1`：仅在有效测试实例 + 有效
  `DESK_TEST_CONFIG_DIR` 时注册唯一的 `Ctrl+Alt+F10` 探针热键，不占用生产
  `Ctrl+Alt+A`；默认测试实例零全局热键；
- `DESK_TEST_DISPLAY_FILE=<文件>`：让 `ScriptedDisplaySource` 代替系统来源，
  文件每行 `work=x,y,w,h` / `dpi=N` / `rect=x,y,w,h`，每次查询重读；
- own-canvas 测试钩子：仅测试实例 + 有效 `DESK_TEST_CONFIG_DIR` 接受
  `WM_APP+11`（0x800B），把当前画布写到配置目录的 `assistant-test-frame-<slot>.bmp`；
  GUI 证据优先 PrintWindow，分层窗打印空白时才用这个钩子，**不截桌面**；
- 程序化入口：`BoardHost::ReadaptToDisplay()`、`ApplySuggestedRect(RECT)`、
  `PlanPreview()`；旧 GUI harness 用的 `WM_APP+9`/`WM_APP+10` 只是兼容 shim。

## 9. 新增模块步骤

### 9.1 卡片模块（三件套）

```bat
rem 1) 生成骨架（src\panels\weather\weather.h + weather.cpp）
powershell -ExecutionPolicy Bypass -File tools\new-panel.ps1 -Name weather -Title WEATHER

rem 2) 编辑 weather.cpp：Describe / LoadData / RefreshData / BodyHeightOf / PaintBodyOf
rem 3) 在 src\apps\desk\main.cpp 加 include 和 modules.push_back(CreateWeatherModule());

cmd /c build.bat        rem 编译
cmd /c build.bat test   rem 回归
```

实现清单：

1. `Describe()` 返回稳定且唯一的 `id`（同时是配置前缀/菜单键/`<id>.ini` 默认名）、
   `title`、`configFile`、`sizePreset`、`refreshSeconds`、`hasDetails`；
2. `LoadData()` 初始化（UI 线程一次）；`RefreshData()` 可阻塞、只写 staging；
   `BodyHeightOf()` 只声明内容高度（框架加 48px、按 8px 取整并校验范围）；
   `PaintBodyOf()` 只用 `skin::*` 原语画 body；
3. 需要的话覆写 `Status()`、`Menu()/OnMenu()`、`MinBodyHeight()/MaxBodyHeight()`、
   `OnBodySizeChanged()`、键盘钩子（`WantsKeyboard` 认领后才会拿焦点）；
4. 新面板自动出现在 **Add module** 菜单；位置/展开状态由板持久化；
5. 数据源放到 `src/data/<domain>/` 并让 Lead 加进 `build.bat` 显式列表；
6. 行为、状态文件、尺寸变化时同步更新 `docs/desk.md` 与本文。

### 9.2 框架单元 / 测试

- 新框架 `.cpp` 放进 `src/widget/`，由 Lead 加入 `build.bat` 的 `FW` 列表；
  公共头放顶层（会被导出到 SDK），内部头放子目录；
- 新测试放进 `tests/`，在 `build.bat test` 段显式构建并运行；测试不联网；
- 改完跑 `tools/check-sources.ps1`（每个 `.cpp` 都在清单里、每个 `.h` 都被引用）。

## 10. 扩展边界与已知约束

- **不是 DLL 插件系统**：组件是编译期源码工厂，无热加载；SDK 头/库必须同一次
  构建、同编译器/架构/运行库，不承诺跨编译器 ABI。
- **一个模块类型一张卡**；宽度由预设决定、不能单独拉伸；高度超范围显示
  `TOO TALL`，比卡片区高显示 `NO SPACE`（修复入口：Fit cards / Reset / Restore）。
- **单显示器假设**：只有面板外框按 DPI 推导；卡片尺寸/字号是 96 DPI 基准物理像素。
- **ConPTY 要求调用方没有控制台**（desk 是 GUI 进程；测试用 `FreeConsole()` 的
  `pty-helper`）；PS 卡另有常驻子进程 + conhost（pwsh 7 空闲约 99MB / 27 线程）。
- **CommandCode 用未公开 `/alpha` 接口**，字段变化时该行 `NO DATA`。
- **Smart App Control** 下重建 exe 必须重签（`tools\sign-local.ps1`）。
- `board.cpp` 目前约 2900 行 / 109KB（单文件宿主）；结构拆分列入 R2（合并后执行），
  本轮不动，拆分计划与审计见 `docs/refactor-notes.md`。
- GUI 验收依赖 `build\qa`、`build\testrun` 证据（被忽略，不入库）；构建并发需
  `tools\build-lock.ps1`。

## 11. 与计划的差异

[assistant-plan.md](assistant-plan.md) 的 M0a 已落地；以下是"计划 vs 现状"的收尾说明：

| 计划项（M0a） | 当前状态 |
|---|---|
| 多 CommandCode 账号（`account.<i>.*`，自动导入 DSH refs） | 已实现（默认 `ACC1`/`ACC2`，label 可改；最多 4 个） |
| 月额度 / sandbox 配额 / 重置倒计时展示 | 已实现；`monthlyCredits` 语义仍待官网核对（暂标"MONTH"） |
| 7 天历史 + `aimeter-history.ini` | 已实现（90 天保留、缺日留空） |
| 每条数据 as-of / `STALE` 标记 | 已实现（600s 阈值） |
| DeepSeek 行默认隐藏（`show_deepseek=0`） | 已实现（开启后恢复旧行为） |
| 新文件 `src/data/aimeter/accounts.*`、`history.*`、`tests/aimeter_accounts.cpp` | 已提交，且进入 `build.bat test` |

M1 已在限定隔离口径下完成验收（真实模型 + 应用桌面，合成业务资料；边界见 §13.8），
代码未 commit/merge/tag；用户已授权部署 M1（部署与回滚见 [m1-acceptance.md](m1-acceptance.md) §5）。
M0b（通知/热键/简报）、M3（看板/任务）未在本轮展开；
`board.cpp` 拆分（R2）见 `docs/refactor-notes.md`。

## 12. 相关文档

- [README.md](README.md)：文档索引与阅读路径
- [desk.md](desk.md)：洞洞板/模块/视觉/配置完整说明与排错
- [framework.md](framework.md)：框架 API、组件契约、生命周期、BoardEvent
- [server.md](server.md)：Servers 卡与共享 SSH 探测
- [cli.md](cli.md)：srvctl 命令、JSON、退出码
- [assistant-prd.md](assistant-prd.md) / [assistant-plan.md](assistant-plan.md)：需求与计划的冻结快照
- [assistant-m1-plan.md](assistant-m1-plan.md)：M1 计划与数据文件约定
- [../AGENTS.md](../AGENTS.md)：构建/测试/签名命令、目录与写作用域
- [../archive/README.md](../archive/README.md)：归档内容与恢复步骤

## 13. M1 助理运行时（当前）

### 13.1 模块与启动

- `src/panels/assistant/`（Wide 568）：单行输入、流式回答、引用页脚、BRIEF、状态 chip。
  折叠 168 外框（1 行 BRIEF + 2 行答案），展开按 BRIEF/答案行数在 198–688 之间，
  `refreshSeconds=0`，只由引擎/宿主推送重绘。Enter 发送、Esc 取消；Ctrl+V 走板的粘贴路径，
  其余 `Ctrl+*` 不抢。
- `src/panels/memory/`（Standard 340）：事实列表 + 菜单 Pin/Unpin、Delete、Edit、
  Clear all、Refresh。折叠最多 3 行、展开最多 17 行，高度随事实数变化，`refreshSeconds=0`。
- `src/apps/desk/main.cpp` 从 `assistant.ini` 读 `enabled`/`hotkey` 后决定是否追加两张卡；
  `enabled=0` 时**两张卡都不注册**，引擎与记忆也不会被创建。卡片 `ModuleSpec` 的
  `configFile` 分别是 `assistant.ini` 与 `memory.ini`（后者当前没有读写键）。
- 数据层 `src/data/assistant/`：
  - `json`：无第三方依赖的 JSON 解析/序列化 + UTF-8↔UTF-16；
  - `pi_client`：`pi --mode rpc` 子进程（匿名管道、严格 LF JSONL、请求 id、事件、
    60s Ask 超时、20s `get_state` 握手、abort、崩溃重启一次、session 续用）；
  - `memory`：episodes 追加 + facts 原子改写（去重/cap 2000/90 天降权/敏感拒绝）、
    reset 元数据、按配置目录共享实例、锁外变更通知；
  - `context`：DSH `workspace.json` + `session_projcache` 的 `turnOutline` 摘要 +
    记忆检索，按 token 预算组装；只读投影缓存，不碰 zstd 会话正文；
  - `engine`：上下文→prompt→pi→流式→引用校验→episode/成本/本地记忆命令。

### 13.2 配置与状态

`assistant.ini` 的完整键、默认值与全部 M1 状态文件见 §7.5。要点：

- `context_tokens`（默认 6000，下限 500）、`idle_stop_minutes`（默认 10，0 关闭）、
  `daily_token_limit`（默认 0 = 不限制）、`pi_command`；
- `provider=command-code`、`model=deepseek/deepseek-v4.1-flash-fast`、
  `key_ref=COMMANDCODE_API_KEY_2`、可选 `provider_extension`；渠道凭据值**不写** ini；
- `tools` 只作兼容：非空时加载清空并回写，旧 `read/edit/...` 白名单不会转发；
- 配置与状态都在配置目录（通常 exe 同目录，不可写则整体回退 `%APPDATA%\Desktop Desk`）；
  共享 `MemoryStore` 只在**同一进程**内共享，不提供跨进程文件锁。

### 13.3 Pi 子进程、工具与安全边界

- 启动器：显式 `pi_command` 优先；否则 `node <managed release>/dist/bundle/cli.js`，
  再退到 PATH 的 `pi.cmd`（经 `cmd /d /s /c`）。
- 子进程使用**私有 Unicode 环境块**：基于父环境副本，先剥离凭据形态变量
  （API_KEY/APIKEY/API-KEY、TOKEN、SECRET、PASSWORD/PASSWD、CREDENTIAL、PRIVATE_KEY，
  大小写无关），再合并 `PiClientOptions::childEnv`（大小写无关、后写者胜、保留 caller
  拼写），排序后以双 NUL 结尾交给 `CreateProcessW`。父进程**从不**调用
  `SetEnvironmentVariableW`，也不改自己的环境。
- 助理注入 `PI_CODING_AGENT_DIR=<配置目录>\pi-agent`，并按 `provider` 推导的环境变量名
  （默认 `COMMANDCODE_API_KEY`）从 `key_ref` 解析凭据值，只放进上述 `childEnv`。
  **argv 不出现 key**；子进程工作目录是 `<配置目录>\pi-work`；Pi 自己的会话在
  `pi-sessions/`。凭据值也加入诊断脱敏字面量。
- 工具硬关：在 caller 参数之后追加 `--no-tools --exclude-tools *`，任何
  `extraArgs` 都不能重新打开工具；`PiClientOptions::tools` 仅为兼容保留、从不传给 Pi。
- 资源面显式关闭：`--no-extensions` 后只 `-e <providerExtension>` 加载**一个**已安装的
  官方 provider 扩展，再追加 `--no-mcp --no-skills --no-prompt-templates --no-themes
  --no-context-files --no-approve --offline`；`--provider` / `--model` 显式传入。
- **这不是 OS 沙箱**：零工具只表示模型没有可调用的 `read/write/edit/powershell` 等工具，
  不等于文件系统/网络隔离；Pi/扩展仍以当前用户权限运行，私有环境块也不阻止子进程读取
  自己的环境。敏感路径过滤、凭据字面量 fail-closed、日志脱敏都是**尽力而为的应用层边界**，
  不是形式化安全证明。
- 本机该 provider 由扩展提供，禁用全部扩展会得到 `Unknown provider`；受限完整性环境仍可能
  因 `~/.pi/agent/trust.json.lock` EPERM 启动失败，不能靠提权绕过。

### 13.4 线程、生命周期与完成语义

- engine worker 建上下文并提交 prompt；pi reader 线程投递事件；卡片通过线程安全 setter
  更新，流式重绘 50ms 节流，空闲无 timer。
- pi wrapper 在首次 Ask 的 worker 中懒初始化并同步发布（不提前读取凭据）；生命周期锁串行
  worker 启动/Ask 与空闲 Stop。UI 的 Cancel 只标记取消并发送 abort，不等待生命周期锁。
- 空闲 `idle_stop_minutes`（默认 10）后 Stop，下次 Ask 重启并续会话；`assistant-session.ini`
  保存会话路径及 facts/policy/reset 摘要，记忆变化或清空会使旧 session 失效。
- 每日预算：`daily_token_limit>0` 且已达上限时**不启动 pi**，直接发布本地 BRIEF 并说明
  预算已用完；`0` 表示不限制。`记住/忘掉/清空`是本地确定性命令，不走 pi/网络/成本。
- 完成语义：Error 先缓存，Done 后只发布一次终态错误；取消不发布成功 onDone。失败/取消不执行
  记住/忘掉，不把部分回答写 episode 或成功调用成本；成功时用请求快照持久化后才释放 busy
  并回调 onDone。engine Stop 先禁止新 Ask、清除回调与请求状态，通过 `RequestStop()` 中断
  握手后 join 并回收子进程，不在析构路径发布 UI 回调；下一次 Start 不残留 BUSY。
- RPC 收尾不变式：本地超时不是远端 run 已结束。真实 settled/可信 abort 空闲确认前，不接收
  下一 prompt；空窗期丢弃旧流事件，命令响应按 id 关联，不能凭无 id 的旧 settled 结算新请求。
  `RequestStop()` 为当前 wrapper 的不可撤销关闭请求，只置标志/唤醒，不 join；与正常空闲 Stop
  不同，使用后需新建 wrapper。真实 Pi 的 abort/settled 严格顺序仍未经真实联调证明。

### 13.5 记忆、清空与 Pin

- facts 是 UTF-8 JSONL，原子改写（临时文件 + `MoveFileEx`）；episodes 只追加；
  `OpenSharedMemory(dir,error)` 按规范化目录返回进程内共享实例，注册表仅持 weak_ptr，
  首次 Load 失败返回 nullptr，成功实例不重复 Load。
- Add/Update/Delete/SetPinned/ClearAll 写盘成功后在 store 锁外通知；失败回滚且不通知。
  MEMORY 卡先订阅再发初始快照，通知只捕获弱共享状态，析构清重绘入口并退订。
- `ClearAll(error)`：先原子写新的 `assistant-memory-reset.txt` id 并推进内存 revision，
  再清 facts、最后清 episodes。**多文件是有序 best-effort，不是事务**：中途崩溃可让元数据
  领先于数据（下次清空重试）；文件重写也不是安全擦除。revision 写失败则什么都不动；
  revision 已写而数据清除失败时调用返回失败、不通知，旧 session 仍保持失效（保守）。
- Engine 的 session digest 同时包含 facts digests 与 `ResetRevision`，所以即使 facts/episodes
  原本为空，清空仍会让旧 Pi session 失效，并且跨重启有效。
- 菜单 Delete 是软删除墓碑，旧 id 不会复活；Edit 通常保留 id 并提升 confidence/updatedMs，
  若新文本已存在于另一条活跃事实则合并到该条。
- `pinned` 是 facts JSONL 的可选布尔字段（旧行默认 false）。列表 pinned 优先，再按
  updated/created/id 排序；**Pin 不刷新事实年龄、不改变 Query 语义、不保证免于 cap 淘汰**，
  也不是检索权重。“清空”是删除事实与 episodes，不是安全擦除。

### 13.6 UI、BRIEF 与召唤

- ASSISTANT 卡 BRIEF 来自真实 DSH 项目：项目 title/path/最近活动 + 对应项目自己的最近
  `turnOutline` 结论 + 卡点（无记录时显示“未记录”）；不借用其他项目的结论。BRIEF 由宿主
  worker 在加载、聚焦、回答完成或本地记忆命令后重读，卡片只收 `SetBrief()`。
- 启动时把最多 8 个 DSH 项目作为 `kind=project` 事实**导入一次**，`assistant-import.ini`
  记录已导入 id；用户忘掉/清空后不会再被下次启动重新创建。导入/更新失败不写 checkpoint，
  下次 Start 重试。
- `Ctrl+Alt+A`（`assistant.ini` 可改）把板临时抬到 `HWND_TOP` 并聚焦/展开 ASSISTANT；
  再次按下、失焦或前台切换时回 `HWND_BOTTOM`。热键注册失败时保留菜单
  `Summon assistant` 并显示提示，不静默失败。
- MEMORY 卡与引擎共享同一实例：成功记住/忘掉/编辑/清空后列表自动更新；菜单失败显示可读
  错误并保留原事实。按事实 id 保持选中，Pin/重排不会误删其他行。Refresh 只重读进程内快照，
  不重新 Load 覆盖存活 store；打开失败时可重试。

### 13.7 测试与隔离验收

- 标准测试目标包含 `assistant-memory-test`、`assistant-context-test`、`pi-client-test`、
  `assistant-engine-test`、`desk-render-test`；`tests/assistant_engine.cpp` 自带合成 RPC
  peer，隔离 USERPROFILE/HOME/DSH，覆盖共享实例、成功通知、失败/取消、在飞
  Stop→Start→Ask、握手中断与早退回调重入；记忆/渲染测试补 Pin 持久化、失败回滚、菜单
  操作、变更发布、按 id 选中及销毁后通知。
- `tools/desk-assistant-test.ps1` 分 `offline`（fake Pi）/`real`/`probe` 三阶段；每个 run
  复制 exe 到 `build\qa\assistant-m1\<run>\app`，设置自己的
  `DESK_TEST_INSTANCE` + **已存在、绝对、可写**的 `DESK_TEST_CONFIG_DIR`、fixture DSH、
  `DESK_TEST_ASSISTANT_ONLY=1` 与隔离 APPDATA/TEMP/USERPROFILE。
- 测试实例默认**零全局热键、无托盘**；只有显式 `DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1`
  才注册唯一的 `Ctrl+Alt+F10` 物理探针，生产 `Ctrl+Alt+A` 从不被测试按下。
- GUI 像素证据只用 PrintWindow；分层窗打印空白时用 own-canvas `WM_APP+11` 导出，
  **不截桌面**。清理只结束本脚本记录的 PID 子树，并重新核对 PID 创建时间与映像路径。
- 测试实例的 DSH 由 `DESK_TEST_DSH_ROOT` 或 `<exe>\test-dsh` 隔离，绝不读真实用户 DSH。

### 13.8 最终验收结果与保留边界

- **C++/构建**：最终官方 `build-lock -Target test` 实跑 19/19、exit 0，源清单与 `[build] ok`
  均到位；memory/context/pi/engine/render 五项独立重复、framework/example 均 exit 0；
  三个主产物签名已执行。自签链仍报 `UnknownError`，不是生产证书链证明。最终构建后 C++
  零修改；最终源清单 ok、diff check 干净、38 文件关键字扫描无可疑真实 key（只命中 fixtures
  的过滤器模式）。
- **真实 Pi 策略**：`build/qa/pi-policy/20261108-213605/result.json` exit 0、19/19；工具序列
  `[read] → [] → [read]`。脚本无 BOM，Node 直接 `JSON.parse` 成功，中文 fork 拓扑/hash 正确。
  这证明零工具/续会话配置的协议形态；单看该脚本不是模型问答（模型问答见下方 probe/real）。
- **离线隔离 GUI**：`build/qa/assistant-m1/offline-20261109-000207/evidence/gui-offline.json`，
  作者实跑 exit 0、83 checks；覆盖中文/流式/引用/BRIEF/abort、MEMORY 全部菜单（Pin/Edit/Delete/Clear）、确认
  取消/确认清空、两次重启及 `enabled=0`。独立 reviewer 复核原始 JSON、前后失败/单 BOM、焦点
  与窗口类前置、清理与无残留；这是证据复核，不是 reviewer 二次重跑 83。
- **真实通道 probe**：`build/qa/assistant-m1/probe-20261109-000453/evidence/probe.json` 独立实跑
  exit 0、22 checks；指定 `command-code` / `deepseek/deepseek-v4.1-flash-fast` 返回中文与合法引用，
  记录 cost；Node 峰值 96.2MB、3 秒 CPU 0ms；board 空闲 CPU 0%、WS 26.9MB，正常收尾。
- **真实通道 real**：`build/qa/assistant-m1/real-20261109-001815/evidence/gui-real.json` 独立实跑
  exit 0、40 checks；4 次 dispatch = 首问 + 真实长问按 Esc + 取消后恢复 + 唤醒，无重试，
  3 次成功调用/2186 tokens。取消正控显示 session 用户轮与 send log 增长、无 episode 写入；
  恢复后是新精确 question、中文、合法来源。Node 在测试配置 `idle_stop_minutes=1` 下自然
  60.0s 退出且板仍存活；wake 使用不同 Node 身份、同一 private session path/文件增长、仅 1 个
  jsonl；Node 峰值 97.1MB/CPU 0ms，board CPU 0.31%/27MB。本 run 镜像与子树无残留，HKCU Run
  未变。
- **口径与边界**：“真实”指真实模型 + 真实应用桌面路径，但业务资料全部合成，使用
  assistant-only 隔离实例，未使用用户真实 DSH 或其他真实卡片。测试 idle 为 1 分钟；默认 10 分钟由
  配置与毫秒 seam 覆盖，但没有实际等待 10 分钟。生产默认物理 `Ctrl+Alt+A` 与可选的 F10 均未按下
  （走消息路径）；该隔离验收阶段未操作现网实例。零工具不是 OS 沙箱，Pi/官方 provider 属可信代码，可写安装
  临时锁/缓存；活跃生产目录只记录元数据变化并排除“静态不变”断言，不声称整个目录不变。
  密钥扫描是启发式，不是形式化防泄漏证明；Clear 多文件仍是 best-effort，非事务、非安全擦除。
- **结论**：M1 可按上述限定隔离口径验收完成；未 commit/merge/tag。用户随后明确授权部署，
  已更新现用安装并正常启动，等待用户验收，见 [m1-acceptance.md](m1-acceptance.md) §5；
  主工作区只读、启动项未改，不能表述为用户生产验收通过。共享 store 只在同一进程生效；隔离 GUI 必须使用独立配置目录；`enabled=0`
  只保证两张卡不加载，不清理既有状态文件。证据汇总以 Lead 的验收矩阵与
  [agent-team-handoff.md](agent-team-handoff.md) 为准。
