# 重构与清理记录（2026-11-07）

## 1. 源码审计结论

用 `tools/check-sources.ps1` 扫描 `src/**/*.cpp` 与 `src/**/*.h`（规则：每个 `.cpp`
必须被 `build.bat` 的清单/通配覆盖，每个 `.h` 必须被至少一个源文件引用）：

- **没有"编译不到"或"无人引用"的源码文件**；结论是本次清理**不需要删源码**；
- 新会话留下的框架文件（`board_plan` / `board_grow` / `display` / `diag` / `host.h`
  / `size.h`）与 10 个新测试都已进入 `build.bat`，随 baseline 提交纳入版本控制；
- `archive/` 里的旧 standalone / Host 框架 / AI Mini / legacy 本来就不参与构建。

因此清理重点放在：版本控制基线、构建产物、未跟踪文件、文档与目录约定，而不是删代码。

## 2. keep / archive / delete 审计

| 对象 | 处置 | 说明 |
|---|---|---|
| `src/widget/*`、`src/apps/desk`、`src/panels/*`、`src/data/*`、`src/cli` | keep | 全部参与构建；`check-sources.ps1` 守住清单 |
| `tests/*` | keep | 全部被 `build.bat test` 编译；新增 `aimeter_accounts.cpp` |
| `tools/*` | keep | 构建/签名/安装/验收/自检；`clean.ps1`、`check-sources.ps1` 新增 |
| `examples/minimal-board` | keep | SDK 消费者示例，`build.bat example` 验证 |
| `archive/**` | keep（不构建） | 旧组件与实验的只读归档，见 `archive/README.md` |
| `build/**` | delete/clean | 生成物，`.gitignore` 忽略；`tools/clean.ps1 -Artifacts` 清理 |
| 旧本地分支 `feat/widget-framework-token-usage` | archive | 已打 `archive/widget-framework-token-usage` tag 后删除本地分支；远端未动 |
| `build/testrun`、`build/gh`、`build/tmp`、`build/sdk-consumer*`、`build/plan-independent-check` | delete | 本次清理约 89.8MB；`build/qa` 作为证据保留 |

## 3. 构建与版本控制

- baseline：`baseline-pre-assistant-2026-11-07`（WIP 快照，含另一会话的框架改动）；
- 清理：`cleanup-r1`（文档、工具、产物清理规则）；
- M0a：`m0a-data`（多账号/月额度/沙箱/已用 token/90 天历史/as-of）；
- M1：`feat/assistant-m1`（本工作树 `_worktrees/assistant-m1`；助理/记忆/Pi RPC 已接线，
  验收未完成，见 §6）；
- 开发在 `_worktrees/assistant-m0a`，主工作区保持 `dev`；
- 提交前扫描 `sk-`、`Bearer`、`PRIVATE KEY`、`.credentials`、`*.pem`。

## 4. `board.cpp` 拆分计划（R2，M0a 合并后）

现状：`src/widget/board.cpp` 约 106KB / 2900 行，是最大的维护点。拆分目标（行为不变）：

```text
src/widget/board/board.h         公共门面（BoardHost/BoardOptions，保持现有 include 路径）
src/widget/board/board_core.cpp  窗口、生命周期、单实例、显示/DPI 适配
src/widget/board/board_input.cpp 命中测试、鼠标、键盘焦点、拖动/缩放
src/widget/board/board_menu.cpp  板菜单、卡片菜单、托盘菜单、命令分发
src/widget/board/board_tray.cpp  托盘图标、通知（M0b 接入点）
src/widget/board/board_persist.cpp desk.ini 保存/恢复、迁移、saved.* 快照
src/widget/board/board_paint.cpp 板面/卡片层绘制、Add 槽、底部提示
```

验收：现有测试 + 渲染截图 + 性能数据不变；一次只拆一块；不与并行会话同时改
`board.cpp`。拆分前先把 `board.h` 的公共接口冻结，`build.bat` 的 FW 列表同步更新，
`check-sources.ps1` 继续守住清单。

## 5. 已知技术债

| 项 | 说明 |
|---|---|
| `board.cpp` 体积 | 见 §4；R2 处理 |
| `build.bat` 硬编码清单 | 新增 `.cpp` 必须进清单；已由 `check-sources.ps1` 在 `build.bat test` 末尾兜底 |
| `monthlyCredits` 语义 | 暂时按"月余额"显示，支持手动上限；官网核对后再改文案/阈值 |
| Codex 数据新鲜度 | 只读本机最新 rollout；M0b 用文件监听 + `STALE` 标记 |
| DeepSeek 行 | 默认隐藏（`show_deepseek=0`）；未来用 CardSpec 一句话重建 |
| 通知/热键 | M0b 才接；当前只有颜色与 4 秒底部提示 |
| 单卡一型 | 一个模块类型只能一张卡；生成卡片（CardSpec）落地后再评估 |
| 行尾 | 仓库存在 `i/lf` 与 `w/crlf` 混用；本次不引入 `.gitattributes` 重规范化 |
| 文档数字 | `docs/desk.md` 的 Account 高度与渲染尺寸已随 M0a 更新；后续改动需同步 |
| 沙箱内验证 | DSH 沙箱里 WinHTTP 连不上外网（`NET ERR`），实时抓取只能在用户正常会话里验证 |
| 历史零值覆盖（低风险） | `collect.cpp` 每次刷新都按账号数重写当天 `aimeter-history.ini`；某账号当天先成功后失败时，失败槽位写 0，会把当天先前成功值覆盖。7 天图用 `cap=0` 跳过、UI 走 `CarryForward`，当前不影响显示；后续可改成"失败账号跳过写当日键" |
| label 默认值差异 | 冻结的 `assistant-plan.md` 写默认"账号1/账号2"，代码实现为 ASCII `ACC1`/`ACC2`（5×7 字体只有 ASCII）；以代码为准，plan 不改 |
| 既有 wprintf cosmetic | `tests/store_tests.cpp`、`tests/board_host.cpp` 用 `%s` 传宽字符串，输出被截成 `o`；测试 exit 0，baseline 既有，本轮不动 |
| **低完整性继承（M1 实测）** | 从 `M:\workspace` 等低完整性/受限环境启动的 `desk.exe` 会把完整性标记继承给 pi，导致 `mkdir ~/.pi/agent/trust.json.lock` 报 `EPERM`，表现为 `PI CRASH 1`。**必须以普通权限启动，或安装到 `%LOCALAPPDATA%\Programs` 下运行**（现网已安装到 `...\Programs\Desktop Desk Assistant`）；`engine.cpp` 已把该错误翻译成中文提示 |
| 安装目录与脚本不一致 | `tools\install-desk.ps1` 默认目标是 `%LOCALAPPDATA%\Programs\Desktop Desk`，现网手工装在 `Desktop Desk Assistant`；更新 exe 时需覆盖实际安装目录，或先改脚本/参数再执行 |
| Pi 工具模型 | 当前 M1 **强制零工具**（`--no-tools --exclude-tools *`）；`assistant.ini` 的 `tools` 只作兼容，非空时加载会被清空并回写，`read/write/edit/powershell` 都不下放。冻结计划里的 `read,grep,find,ls` 只读白名单已不适用；零工具也不等于文件系统沙箱 |
| M1 记忆共享补接线 | 原实现只注册 MEMORY 视图；现用按目录的共享 MemoryStore 与锁外通知，不建立第二个写同一文件的实例。facts 新增兼容的 `pinned`；Pin 只排序，不承诺检索加权、年龄刷新或免淘汰。冻结需求/计划不回改，以 architecture §13 与任务板为准 |
| M1 本地终态不等于远端空闲 | 原超时立即放开下一 Ask 会让旧无 id settled 污染新请求；现要求远端 run 收尾确认再接新 prompt。Engine 在飞 Stop 静默清状态，不能在析构中回调卡片；回归覆盖不代表真实网络/GUI 已验收 |
| M1 文档口径 | 旧文档写着“默认只开 read 工具”“key 经 argv”；当前实现是零工具 + 子进程私有 `childEnv`。以 architecture §13 和本文件 §6 为准 |

## 6. 完整 M1 与冻结计划的差异（2026-11-08）

本节只记录**代码 vs 冻结计划/旧文档**的差异，不改 `assistant-prd.md`、
`assistant-plan.md`、`assistant-m1-plan.md`。验收状态见
[agent-team-handoff.md](agent-team-handoff.md)；架构细节见
[architecture.md](architecture.md) §13。

| 主题 | 冻结计划 / 旧文档 | 当前实现 |
|---|---|---|
| 模型工具 | 只读白名单 `read,grep,find,ls`，后改为默认 `read`；“只读工具”被当成读取边界 | 子进程强制 `--no-tools --exclude-tools *`，caller 参数之后追加，`tools` 键非空时加载会清空并回写。模型没有文件工具；上下文由 C++ 读 DSH 投影缓存 + 记忆后注入。**零工具不等于 OS 沙箱** |
| Pi 版本 | 计划风险表写锁定 1.4.0 | 当前按 pi 1.5 的 RPC/事件语义实现（`agent_settled` 等）；`pi_client.h` 顶部仍残留 “pi 1.4 文档”注释，属于待清理的注释漂移，不影响运行时 |
| 凭据传递 | 旧口径是应用通过 `--api-key` 启动 pi，key 出现在 OS 命令行 | 现在 key 按 `key_ref` 解析后只进 `PiClientOptions::childEnv`；argv 不带 key，父进程环境不被修改。私有 Unicode 环境块会剥离父环境里的凭据形态变量，大小写无关合并子项但保留 caller 拼写，并对已知凭据值做诊断脱敏 |
| 资源面 | 只讨论工具白名单 | 额外用 `--no-extensions` + 单个 `-e <providerExtension>`、`--no-mcp --no-skills --no-prompt-templates --no-themes --no-context-files --no-approve --offline` 关闭 ambient 扩展/技能/模板/主题/上下文文件/信任提示。官方 provider 扩展仍是受信任代码 |
| 安全边界 | 容易被读成“只读 = 安全沙箱” | 不是 OS 沙箱：Pi/扩展以当前用户权限运行；私有 env 不阻止子进程读取自己的环境；敏感路径过滤和凭据 fail-closed 是应用层尽力而为；`trust.json.lock` EPERM 仍需普通权限启动。没有形式化安全证明 |
| 状态文件 | 计划表：`assistant.ini` 的 `toolset`、`max_context_tokens`、`pi`；只有 episodes/facts/cost/send log/pi-sessions | 实际键为 `tools`（兼容清空）、`context_tokens`、`pi_command`，另有 `enabled/hotkey/idle_stop_minutes/daily_token_limit/provider/model/key_ref/provider_extension`；新增 `assistant-memory-reset.txt`、`assistant-session.ini`、`assistant-import.ini`、`pi-agent/`、`pi-work/`；MEMORY 卡还声明 `memory.ini` 作配置锚点但无键 |
| 记忆清空 | 计划只要求可清空 | `ClearAll` 先原子推进 reset id/revision，再清 facts、最后清 episodes；多文件有序 best-effort，**非事务、非安全擦除**；`ResetRevision` + digest 让旧 Pi session 失效；清空即使原本为空也通知，作为“忘掉这段对话”的屏障 |
| Pin | 计划只有“可删/可改/可忘” | facts 新增可选 `pinned`；只影响列表排序，不刷新年龄、不改变 Query 语义、不保证免于 cap 淘汰；不是检索权重或永久保留 |
| GUI 测试隔离 | 旧脚本复制 exe 到 testrun 目录，配置随 exe | 助理验收要求有效 `DESK_TEST_INSTANCE` + **已存在、绝对、可写** `DESK_TEST_CONFIG_DIR`；`DESK_TEST_ASSISTANT_ONLY=1` 只加载两张卡；`DESK_TEST_DSH_ROOT` 隔离 DSH；测试实例默认零全局热键/托盘，仅显式 `DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1` 注册 `Ctrl+Alt+F10`；像素证据 PrintWindow 或 own-canvas `WM_APP+11`，不截桌面 |
| 当前验证 | 早期记录有 19/19 全绿 | 最终官方 19/19、五项独立重复、framework/example 均 exit 0，三主 bin 已签名（自签链 UnknownError，不是生产证书链）；最终构建后 C++ 零修改，源清单/diff check/38 文件扫描通过。真实 Pi 策略 `pi-policy/20261108-213605` 19/19；隔离 GUI `offline/20261109-000207` 83/83（作者实跑，reviewer 复核证据）、`probe/20261109-000453` 22/22、`real/20261109-001815` 40/40（独立实跑） |

保留边界（已验收但不得外推）：M1 已按限定隔离口径验收完成，未 commit/merge/tag；用户随后
授权的实际部署已完成启动，等待用户验收，见 [m1-acceptance.md](m1-acceptance.md) §5。
上述测试中的“真实”指真实模型 + 应用桌面，业务资料全部合成、assistant-only，不使用用户真实 DSH/
其他真实卡；测试 idle 为 1 分钟，默认 10 分钟未实等；生产默认物理 `Ctrl+Alt+A`/F10 未按，
隔离验收阶段未操作现网实例。零工具不是 OS 沙箱，Pi/官方 provider 是可信代码，可写安装临时锁/缓存；
活跃生产目录只记元数据变化并排除静态断言；密钥扫描是启发式，Clear 多文件非事务/非安全擦除。
真实 real 的取消/唤醒/资源已跑，但不构成未来 Pi 协议顺序或形式化安全证明。
