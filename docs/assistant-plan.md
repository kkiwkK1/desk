# Desktop Desk 助理化实施计划（v1.0）

> 需求来源：[assistant-prd.md](assistant-prd.md)。本文是可执行计划：里程碑、M0a 详细设计、
> Git 工作流、清理规则与验收。所有实现以本文 + PRD 为准，冲突时先改文档再改代码。

## 1. 背景与定位

现状是一个"需要你主动去看"的桌面仪表盘：三张卡（Account / Servers / PowerShell），
数据是快照、没有通知、没有交互，CommandCode 只看得到第一个账号，DeepSeek 余额口径
已经没有参考价值。目标（PRD §0）：**只在该说话时说话的桌面助理**——数据比官网更省事、
异常会主动找你、能按自然语言长出新卡片、能记住你在推进什么。

路线：先把数据变可信（M0a），再让助理有入口与主动能力（M0b/M1），最后才是生成卡片
（M2）与看板/行动（M3/M4）。`board.cpp` 的结构拆分放到合并之后（R2），避免与并行
会话对撞。

## 2. 目标与非目标

**M0a 目标**：Account 卡的数据可信、完整、可回溯。

- CommandCode 多账号（2 个）+ 聚合视图；
- 5h / 周 / 月额度、sandbox 配额、重置倒计时；
- 已用 token（本机 DSH，标 `LOCAL`）；
- 7 天历史快照与迷你柱状图；
- 每条数据带 as-of，过期标 `STALE`；
- DeepSeek 行默认隐藏（代码保留，可配置恢复）。

**非目标**：通知/热键/汇总简报（M0b）、pi 助理与长期记忆（M1）、自然语言生成卡片（M2）、
看板与任务（M3）、行动能力（M4）。

## 3. 里程碑

| 阶段 | 内容 | 出口条件 |
|---|---|---|
| M0a | 数据可信层（本文 §4） | `build.bat test` 全绿；双账号/月额度/历史/as-of 可见 |
| M0b | 通知通道 + 热键 + 静态 BRIEF | 异常可主动提示；每天 ≤3 条；可静音 |
| M1 | pi（RPC）+ 长期记忆（G 组） | 呼出提问带来源；记忆可查/可改/可忘 |
| M2 | CardSpec + 自然语言生成卡片 | 一句话生成只读卡，预览后上板 |
| M3 | 看板 / 任务 / 周报 | 今日计划每日更新，完成率可追踪 |
| M4 | 行动能力（开项目/写笔记/跑命令） | 每次执行需授权，可撤销 |

## 4. M0a 详细设计

### 4.1 数据模型

```cpp
struct AccountConfig {
    int index = 0;                 // 1..N，配置键与历史键都用它
    std::wstring label;            // "账号1"
    std::wstring keyRef;           // DSH ref 名，如 COMMANDCODE_API_KEY
    Source source = Source::Dsh;   // dsh | credman
    long long monthlyLimit = 0;    // 可选手填，0 = 未设
};

struct QuotaWindow {
    double used = 0, cap = 0;
    long long resetAtMs = 0;       // 0 = 无窗口
    bool exceeded = false;
    bool present() const { return cap > 0 || used > 0 || resetAtMs > 0; }
};

struct AccountSnapshot {
    std::wstring label;
    bool ok = false;
    std::wstring error;            // NET / BAD KEY / ERR n / NO DATA
    QuotaWindow fiveHour, weekly;
    double monthlyCredits = 0;
    bool belowThreshold = false;
    long long sandboxUsed = 0, sandboxLimit = 0, sandboxResetMs = 0;
    double sandboxOverageUsd = 0;
    long long asOfMs = 0;
};

struct AccountBoardData {
    std::vector<AccountSnapshot> accounts;
    // 聚合：最紧窗口、月余额合计、是否全部失败
    double tightestPct = 0;
    double monthlyCreditsTotal = 0;
    bool anyOk = false;
};
```

聚合规则：
- `tightestPct = max(used/cap)`，cap>0 的窗口参与；
- 月余额合计 = 各账号 `monthlyCredits` 之和；
- 单账号失败不影响其他账号；全部失败时标题状态为 `ERROR`，不全绿。

### 4.2 配置与凭据（`aimeter.ini`）

```ini
accounts=2
account.1.label=账号1
account.1.keyref=COMMANDCODE_API_KEY
account.1.source=dsh
account.1.monthly_limit=
account.2.label=账号2
account.2.keyref=COMMANDCODE_API_KEY_2
account.2.source=dsh
show_deepseek=0
```

- 没有 `accounts` 键时：扫描 `~/.dsh/.credentials.yaml` 的 refs，找到
  `COMMANDCODE_API_KEY` 与 `COMMANDCODE_API_KEY_2`（可扩展 `_N`）自动生成账号，
  并写回一次（幂等）；默认 label 为 `账号1`、`账号2`。
- 凭据解析顺序：
  1. `source=dsh` → 同名 ref；
  2. `source=credman` → Windows 凭据管理器 `AiMeter/CommandCode/<label>`；
  3. 兼容：账号 1 找不到时回退旧的 `AiMeter/CommandCode`。
- key 只在内存中传给 WinHTTP；日志、UI、历史文件只记录来源名，不记录值。
- 支持既有 `LoadCommandCodeKey()` 语义（账号 1 + DSH 回退）以保持兼容。

### 4.3 抓取与聚合

- 账号数 ≤ 4；每账号一个 worker 线程，`wg::HttpGet`，`receiveTimeoutMs = 8000`；
- 解析（在现有 `ParseCommandCodeBody` 上扩展）：
  - `credits.monthlyCredits`、`credits.belowThreshold`、`credits.creditThreshold`
  - `windowLimits.fiveHour|weekly.{used,cap,exceeded,resetAt}`
  - `sandboxMinutes.{usedMinutes,limitMinutes,overageUsd,resetAt}`
- `resetAt=0` → 显示"无窗口"，不能显示"已重置"；
- 错误分类：`NET ERR`（transport 失败）、`BAD KEY`（401/403）、`ERR <status>`、
  `NO DATA`（200 但字段缺失）；
- 刷新节奏沿用 300s；账号抓取与 DSH token 统计同一次 `RefreshData` 内完成。

### 4.4 历史与存储

- 文件：配置目录下 `aimeter-history.ini`（UTF-16，`wg::Store` 原子保存）；
- 键：

```text
day.<YYYY-MM-DD>.cc.<i>.five.used=5.4077
day.<YYYY-MM-DD>.cc.<i>.five.cap=14
day.<YYYY-MM-DD>.cc.<i>.week.used=25.45
day.<YYYY-MM-DD>.cc.<i>.week.cap=35
day.<YYYY-MM-DD>.cc.<i>.monthly=0.78
day.<YYYY-MM-DD>.tokens.input=<n>
day.<YYYY-MM-DD>.tokens.output=<n>
day.<YYYY-MM-DD>.tokens.cacheRead=<n>
day.<YYYY-MM-DD>.tokens.cacheWrite=<n>
```

- 每次刷新写入当日值（覆盖）；保留 90 天，超期 `RemovePrefix` 裁剪；
- 历史从启用日开始累积；更早只保留 DSH 可归因的部分，不伪造；
- 读取只在 worker 线程；UI 只拿快照。

### 4.5 UI 与图表

- 收起（`Standard` 宽 340）：
  - 聚合行：`2 ACCOUNTS · 最紧 73% · 月余额 60.45`
  - 每账号一行：`账号1   5H 39%  W 73%   CHY 0.78`（列宽按可用宽度压缩）
  - 状态色取最紧窗口：<75% 绿 / ≥75% 黄 / ≥90% 红；`exceeded` 红并加 `!`
- 展开：
  - 每账号：重置倒计时（`3H 12M` / `5D 6H`）、月额度（或手动上限百分比）、
    sandbox 用量（`0/0 MIN`）、数据来源（`DSH REF` / `CRED` / `API`）、as-of；
  - 底部 7 天迷你柱状图：每日 `weekly cap 使用率` 的柱高，缺失日留空；
  - 图例：`7D`，后续可切 `30D`。
- 新增 `skin::MiniBars(Canvas&, const Rect&, const std::vector<float>&, Color)`：
  1–2px 柱、无坐标轴、按 rect 高度归一化；数值全 0 时画基线。
- DeepSeek 行：`show_deepseek=0` 时完全不渲染（不占高度）；`=1` 时恢复旧行为。
- 高度：`BodyHeightOf` 按账号数 + 是否展开计算，保持在 `Standard` 112–640；
  超限由框架报 `TOO TALL`，不裁切。

### 4.6 新鲜度与错误

- 每个 `AccountSnapshot.asOfMs` 在成功抓取时更新；
- `asOfMs` 超过 2 个刷新周期（600s）→ UI 显示 `STALE <age>` 并变灰；
- 失败的账号保留上次成功值（若有），并在行尾标 `STALE`；从未成功则显示错误码；
- 标题状态：全部成功且无 STALE → `OK`；有部分失败 → `WARN`；全部失败 → `ERROR`。

### 4.7 测试

新增 `tests/aimeter_accounts.cpp`（无窗口，退出码断言）：

1. 两份真实响应 fixture（账号 1/2）→ 字段解析、`resetAt=0`、`belowThreshold`；
2. 聚合：最紧窗口、月余额合计、单账号失败隔离；
3. 历史：写两天的键 → 读回、90 天裁剪、缺日不补零；
4. 陈旧：`asOf` 超过阈值 → `STALE` 判定；
5. `show_deepseek=0` → 数据行不包含 DeepSeek（视图层可用纯函数测试）。

并挂进 `build.bat test`（Lead 维护 build.bat）。渲染层：`tests/desk_render.cpp`
增加双账号 Account 分支或新快照，验证高度与不裁切。

### 4.8 文件落点

| 文件 | 改动 |
|---|---|
| `src/data/aimeter/accounts.h/.cpp` | 新增：配置、凭据、抓取、聚合 |
| `src/data/aimeter/history.h/.cpp` | 新增：每日快照、序列读取、裁剪 |
| `src/data/aimeter/aimeter_data.h` | 扩展 `AimeterData`（账号列表/历史/陈旧） |
| `src/data/aimeter/collect.cpp` | 接线：抓取账号、写历史、DeepSeek 开关 |
| `src/panels/account/account_view.h/.cpp` | 渲染聚合/账号行/详情/图表 |
| `src/panels/account/account.cpp` | 高度与菜单（如需） |
| `src/widget/skin.h/.cpp` | `MiniBars` 原语 |
| `tests/aimeter_accounts.cpp` | 新增测试 |
| `build.bat` | 测试目标 + 源清单（仅 Lead 改） |

## 5. Git 工作流

基线：`dev` 上 `baseline-pre-assistant-2026-11-07`（WIP 快照，已打 tag）。
开发：`_worktrees/assistant-m0a` 内 `chore/project-cleanup` → `feat/assistant-m0a`。
规则：

- 只有 Lead 执行 git 写操作；teammate 只改文件并汇报。
- 一个提交一件事，提交前能编译；不 amend、不 force-push、不重写 `dev`。
- tag：`baseline-pre-assistant-2026-11-07` → `cleanup-r1` → `m0a-data`。
- 回滚：worktree 内 `git reset --hard <tag>`；单点 `git revert <sha>`；
  删除分支用 `git worktree remove` + `git branch -D`。
- 提交前扫描密钥（`sk-`、`Bearer`、`PRIVATE KEY`、`.credentials`、`*.pem`）。
- 远端备份可选：`git push origin baseline-pre-assistant-2026-11-07 cleanup-r1 m0a-data`；
  失败不阻塞。

## 6. 项目清理规则

- 三类：keep / archive / delete。
- archive：`archive/<yyyy-mm>-<topic>/`，必须在 `archive/README.md` 有索引与恢复步骤，
  不参与构建；本次审计确认 `src/` 无可安全删除的死文件，**不删现有源码**。
- delete：只用于构建产物与确认无用的临时文件（`build/` 已被忽略）。
- 未跟踪的活文件必须纳入版本控制；`tools/check-sources.ps1` 保证每个 `.cpp` 被编译、
  每个 `.h` 被引用。
- 分支：旧分支先打 `archive/<name>` tag 再删本地；远端与 `main` 不动。
- 文档：`docs/README.md` 索引、`docs/architecture.md`、`docs/refactor-notes.md`、
  `AGENTS.md`；不引入 `.gitattributes` 重规范化行尾。

## 7. 验收清单

- [ ] `git log --oneline --decorate` 可见 baseline/cleanup/m0a；`git status` 干净；
- [ ] `build.bat` 与 `build.bat test` 全绿（worktree 内）；
- [ ] 隔离实例（`DESK_TEST_INSTANCE=assistant`）截图：双账号 + 聚合 + 详情 + 7 天图；
- [ ] 单账号失败（临时改 key）只影响该行；
- [ ] 重启后历史仍在，90 天裁剪；
- [ ] `show_deepseek=0` 时无 DeepSeek 行，`=1` 恢复；
- [ ] 空闲 CPU 0，Account 高度不越界，无 key 泄漏；
- [ ] verifier 独立复跑并签收。

## 8. 风险

| 风险 | 应对 |
|---|---|
| 另一会话恢复写入主工作区 | 只在 worktree 工作；主工作区保持 `dev` 干净 |
| baseline 含 WIP 半成品 | 构建门禁；失败用 `build\backup-baseline-20261107` 恢复 |
| `build.bat` 冲突 | 只由 Lead 改；测试目标最小化 |
| `monthlyCredits` 语义未定 | 暂标"月余额"+ 可选手填上限；官网核对后改文案 |
| `/alpha` 接口变更 | 每账号独立失败；`NO DATA` 给"接口可能变更" |
| SAC 拦截 exe | `tools\sign-local.ps1` 重签 |
| 历史文件增长 | 90 天裁剪；单文件 < 100KB |

## 9. 开放问题

1. `monthlyCredits` 是"剩余"还是"已用"？官网核对后确定文案与阈值；
2. 两个账号的 label 最终叫什么（默认"账号1/账号2"）；
3. M0b 的通知默认值（每天 ≤3 条、03:00–12:00 免打扰）是否按 PRD §11 执行；
4. M1 的 pi 接法（RPC 直连 vs Node 桥）在 M1 启动前再确认一次。

## 10. 团队协作（Agent Team）

| 角色 | 负责 | 写作用域 |
|---|---|---|
| Lead | 基线/worktree/collect/build.bat/提交/tag/合并 | 见任务板 |
| cleanup-docs | 文档四件套 + archive 索引 | `docs/`、`AGENTS.md`、`archive/README.md`、`README.md` |
| data-core | accounts/history/测试 | `src/data/aimeter/accounts.*`、`history.*`、`tests/aimeter_accounts.cpp` |
| ui-account | account_view/skin MiniBars | `src/panels/account/*`、`src/widget/skin.*` |
| verifier | 独立构建/测试/截图/Git 审计 | 只读 + `build/` |

协作规则：所有人在 `_worktrees/assistant-m0a` 内工作；不并发构建；不执行 git 写操作；
越界先停并通知 Lead；报告含文件、命令、结果、风险。
