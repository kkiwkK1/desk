# M1 接管与 Agent Team 任务板（2026-11-08—09）

> **当前 GitHub 流程**：正式仓库为 `kkiwkK1/desk`。`main` 的 `a035f02` 是已重新通过
> 19/19、SDK/example 的稳定 M1 导入；本分支为独立新仓库的 `dev`，仅承载 M2 开发候选。
> 原 aimeter 仓库历史/工作树没有被改写。后续按 [CONTRIBUTING.md](../CONTRIBUTING.md)
> 在 dev 开发并提 PR，CI 绿且用户明确验收后才能 merge commit 进 main，未验收只开草稿 PR。
> 下文旧工作区、feat 分支与会话记录为历史，不是后续操作指令。M2.1 的真实 HTTPS 正向验收仍未完成。

## 接管基线

- 工作区：`_worktrees/assistant-m1`，分支 `feat/assistant-m1`，起点 `5f6770a`，接管时工作树干净。
- Lead：当前 Pi 会话；主工作区只读，不停止或覆盖已安装的 desk，不自动合并/tag/push。
- 已实现：Account 多账号与历史、服务器卡、终端卡；M1 ASSISTANT/MEMORY、Pi RPC、上下文、召唤和成本记录。
- 尚未验收闭环：真实渠道启动、异常/取消/记忆写入一致性、engine 测试进入标准构建、配置与诊断文档漂移。
- 本轮优先完成 M1 稳定性和验收，不扩展 M2/M3，不拆 board。

## 模型与调度

成员使用 CommandCode 的精确模型 `deepseek/deepseek-v4.1-flash-fast`，经 Pi CLI 独立子进程执行。
当前目录未发现独立 `latest` 型号；不臆造别名。联网最小探针已返回 `TEAM_MODEL_OK`。
本机该 provider 由扩展提供：`--no-extensions` 会导致 `Unknown provider`，不能用于这些成员。
凭据使用现有 DSH 引用，仅在启动进程内存/环境中解析，不写入任务、配置、报告或命令行。
首轮成员通信采用 CLI 任务提示与完成报告。用户安装 `pi-agents-team` 后，后续改用扩展的 delegate/wait/message/result 管理，不再另建手工团队进程。
默认 worker-minimal 会禁用 ambient 扩展，首次 CommandCode worker 启动失败且未留下活动成员。已新增用户级 `~/.pi/agent/agents-team.json`，七个角色显式加载官方 CommandCode provider，模型固定为上述型号，Windows 工具使用 read/powershell（仅 fixer 加 edit/write）；禁用递归、写任务必须显式提供作用域。
新增用户级 `models.json` 与 `commandcode-dsh-key.ps1`：运行时由 Pi 私有凭据命令读取既有 DSH ACC2 引用；配置和脚本均不含密钥值，不修改 auth 文件，不改变主会话模型。
官方 provider 最小启动探针返回 `TEAM_EXTENSION_CONFIG_OK`；provider + team 扩展联合启动返回 `TEAM_NATIVE_CONFIG_OK`，两者 exit 0。直接在普通 Node 中导入 team loader 因缺少宿主映射的 typebox 失败，未为此安装依赖或修改扩展；Pi 宿主联合启动正常。
重载后原生 `delegate_task` 已成功启动 explorer/reviewer，`wait_for_agents` 与 `agent_result` 收到完整设计/审查；现在用相同扩展管理实现与验收，不再手工启动团队进程。上一轮 19 项通过记录仅适用于当时基线，新变更必须重新串行构建。
本轮冻结接口：`OpenSharedMemory(dir,error)` 返回 `shared_ptr<MemoryStore>`；`Fact::pinned=false`；`SetPinned(id,bool,error)`；`Subscribe(std::function<void()>)` 返回 `uint64_t` token，`Unsubscribe(token)`；成功事实写盘后在 store 锁外通知，UI 回调须独立处理在飞通知的析构安全。

## 前轮稳定性任务（历史结果，不代表完整 M1 当前基线）

| task | 成员 | 状态 | 范围与交付 |
|---|---|---|---|
| T01 | Lead | 首轮修复完成，全量测试通过 | 梳理交接；`engine.*` 取消/失败不落记忆；新增 `tests/assistant_engine.cpp`（自带合成 RPC peer）；集成、文档与构建清单 |
| T02 | m1-pi | 复审交付，全量测试通过 | `src/data/assistant/{json,pi_client}.*`、`tests/{fake_pi_rpc,pi_client}.cpp`：RPC 生命周期与诊断隐私，修复可复现高优先级问题并补测试 |
| T03 | m1-memory | 已交付，全量测试通过 | `src/data/assistant/{memory,context}.*`、`tests/assistant_{memory,context}.cpp`：上下文/记忆敏感过滤、预算和持久化审计，修复与回归 |
| T04 | m1-ui | 已交付，全量测试通过 | `src/panels/assistant/*`、`src/panels/memory/*`、`tests/desk_render.cpp`：错误/流式/键盘与记忆卡一致性，修复与回归 |
| T05 | verifier | 静态审查及关键 5 项独立复跑通过 | 审阅 engine/测试与交叉契约；全量构建由 Lead 独占执行 |
| T10 | verifier / reviewer | 新基线 19/19 + 4 项独立重复全绿 | 唯一构建者走 build-lock，实际输出核对，不运行旧二进制替代新测试 |
| T09 | Lead | 原生 delegate/wait/result 成功启用 | 用户要求切换 pi-agents-team；仅新增用户 Pi 的团队/provider 凭据引用配置，不修改安装包、不写密钥明文、不改当前主会话模型；新配置需 `/reload` |
| T06a | m1-memory / fixer | 已交付，新全量及独立重复通过 | `memory.{h,cpp}`、`tests/assistant_memory.cpp`：按目录共享实例、锁外变更通知、Pin 持久化及回归 |
| T06b | m1-ui / fixer | 已交付，新全量及独立重复通过 | `src/panels/memory/*`、`tests/desk_render.cpp`：同一实例订阅、Delete/Pin/Refresh、析构安全与回归 |
| T06c | Lead | 共享接线/静默Stop/握手中断回归通过 | `engine.*`、`tests/assistant_engine.cpp`：复用共享实例；Stop 静默复位；早退回调移到锁外；集成与文档 |
| T07 | Lead + m1-pi | 已由本轮 M1-S 接续，待完整验收 | 当前实现已改私有子环境与强制零工具，不再用 argv key；仍不是 OS 沙箱，当前 key 问题/Persona 门禁还在修复回归 |
| T08 | m1-pi / fixer | 本轮修复全量及独立重复通过；保留真实协议顺序风险 | `pi_client.*`、`tests/{pi_client,fake_pi_rpc}.cpp`：超时后未决 run 隔离、abort 关联、stdinWrite 锁纪律、restart/Stop 有界回归 |

成员不得 git 写操作、不得构建、不得修改其他作用域/文档/安装配置；如需越界，在报告中请求 Lead。
所有源码变更通过 Lead 审阅后统一构建；不把旧 build harness 当成标准测试已覆盖。

## 完整 M1 闭环（用户授权本轮）

用户要求完成整个 M1，按原计划验收，不把前轮稳定性绿等同于 M1 完成。

| task | owner | 独占写作用域 | 本轮目标 |
|---|---|---|---|
| M1-S | m1-pi / fixer | `src/data/assistant/pi_client.*`、`tests/{pi_client,fake_pi_rpc}.cpp` | 私有 Unicode childEnv、去 argv key、私有 agent dir/显式 provider/零工具与资源关闭、fake GUI 模式 |
| M1-E | engine / fixer（Lead 授权） | `src/data/assistant/{engine,context}.*`、`tests/assistant_{engine,context}.cpp` | 本地确定性记忆命令、BRIEF/项目事实/episodes、citation/idle/session/预算及测试 |
| M1-U | ui-memory / fixer | `src/data/assistant/memory.*`、`tests/assistant_memory.cpp`、`src/panels/{assistant,memory}/*`、`tests/desk_render.cpp` | 事实编辑/清空、错误可见、brief 刷新与对应卡级回归 |
| M1-Q | host-qa / fixer（Lead 授权） | `src/widget/{appenv,board}.*`、`src/widget/module.h`、`src/apps/desk/main.cpp`、`tests/board_host.cpp`、`tools/desk-assistant-test.ps1` | 配置隔离、test 无热键/托盘污染、own-buffer 截图、禁用/热键诊断、离线与真实 GUI 验收脚本 |
| M1-P | policy / fixer（Lead 授权） | `tests/pi_policy_provider.ts`、`tools/check-pi-policy.ps1`（新增） | 实际 Pi 的离线 provider 抓包验证零工具续会话，含灵敏度对照，不加入常规 fake 构建依赖 |
| M1-L | Lead | `build.bat`、docs；`tools/desk-testlib.ps1` 仅消息入口 Unicode 修复 | 接口裁决、集成、唯一串行构建授权、真实网络/隔离 GUI 验收与证据签收；独立 HWND_MESSAGE 实测证实 WM_CHAR 的 A→W thunk 后，限定修正 SendMessage/PostMessage 的 W 绑定 |

本轮接口约定：`PiClientOptions::childEnv` 为 `vector<pair<wstring,wstring>>`、`providerExtension` 为显式已安装官方 provider 路径；M1 Pi 子进程强制零工具与私有 agent 配置，不继承模型可调用工具/用户扩展。`MemoryStore::ClearAll(error)` 清事实与 episodes；UI 的 Module 新增线程安全 `RequestRefresh()`，宿主经 PostMessage 转 UI 调度；engine 测试可用 `EngineOptions` 指定合成 DSH 根/毫秒级 idle，正常配置默认 10 分钟。

安全前置探针：独立临时 `PI_CODING_AGENT_DIR`，显式 `--no-extensions -e <官方provider>`、`--no-tools --exclude-tools *`、禁 MCP/skills/prompts/themes/context/项目配置，通过已有 DSH 凭据的进程内环境调用精确模型返回 `M1_PRIVATE_ZERO_TOOLS_OK`，exit 0；不修改 Pi 安装包/全局配置、不写密钥值。此处仅证明启动与真实通道，不等于应用已实现或 GUI 已验收。

## 当前执行快照（完整 M1，本轮）

- 旧会话成员已失效；本轮由原生新成员接续，不将旧成员历史输出当作当前执行。
- w17 已修正环境块测试：大小写无关替换但保留 caller 拼写；唯一性、凭据剥离、Unicode、双 NUL 断言不减。
- w18 完成策略脚本 PS5 UTF-8/BOM 收尾；w20 最终实跑 `build/qa/pi-policy/20261108-213605`，19/19、exit 0、JSON 无 BOM 可直接解析，真实 Pi 工具 `[read] / [] / [read]`，两个 fork 指向未改写的 baseline，中文路径无损。
- w19 是唯一构建者。最新官方 test 编译 0 error、19/19、source inventory 和 `[build] ok` 均齐全；五关键独立重复、framework/example 也均 exit 0，三个主 bin 产物已重签。日志位于会话临时目录 `m1b3-20261108-230247`。未启动现网 GUI。
- w21 修正 engine fixture 的自动项目导入、历史问答关键词串扰、配置 Start 时加载和 canary 假覆盖，未改生产 engine；w25 修 host fixture 的尾斜杠契约，未改生产 appenv。两者均已由上述新官方全量验证。
- w22 完成 `docs/{architecture,desk,refactor-notes}.md` 当前契约同步，Lead 后续更新验收状态。
- w23 首轮 GUI 因 provider 流中断失去报告，w26 恢复早期失败现场；后续定位为测试工具 PS5 标量/返回值/句柄/类名/焦点/双 BOM 等夹具问题，以及 Lead 用独立消息窗实证后修正的 WM_CHAR ANSI 入口。未因此修改生产 UI 或放宽阈值。
- w27 最终离线自测 `offline-20261109-000207/evidence/gui-offline.json` 为 exit 0、83/83，w26 已独立核对证据和三处最终修复；两个重启、确认取消/清空、Esc 与禁用均过。本次镜像/子树全部安全收尾。
- w26 单次授权模型 probe `probe-20261109-000453/evidence/probe.json` 为 exit 0、22/22；真实指定模型、中文/引用/成本、Node 非零身份资源样本（96.2MB、CPU 0ms）与收尾均过，board 采样 CPU 0%。只用合成 DSH，不读取真实业务资料。
- w27 补齐 real 非 Minimal 分支，w26 单次授权实跑 `real-20261109-001815/evidence/gui-real.json` 为 exit 0、40/40。取消有远端提交正控且无成功 episode，随后恢复成功；60.0 秒自然 idle 退出、board 仍活，wake 为新 Node 且严格续同一私有 session 文件，无额外 JSONL；4 次发送/3 成功/2186 tokens，Node 峰 97.1MB、CPU 0ms，board 采样 0.31%/27MB，本 run 与新旧子树均无残留。
- w19 最终只读审计：当前 CPP 全部早于绿色构建，之后未改；源清单与 diffcheck 通过，38 文件密钥形态命中均为夹具/过滤器/标识符，无可疑真实 key（非形式化证明）。主工作区只读、当前干净，未触碰现网或启动项。
- **M1 功能与限定隔离验收已完成**，最终证据与边界见 [m1-acceptance.md](m1-acceptance.md)。默认 10 分钟由配置/ms 回归覆盖，GUI 实等的是 1 分钟；默认物理 Ctrl+Alt+A/F10 均未按。所有业务资料合成，仅模型通道与桌面交互为真实；不外推为生产语料质量或 OS 沙箱。
- 不修改主工作区、已安装实例或启动项；不 commit/merge/tag/push/部署。

## 验收门禁

1. 汇总每个成员的文件清单、问题证据、补测内容和未决项。
2. `tools/check-sources.ps1`，串行 `tools/build-lock.ps1 -Target test`。
3. 诊断/日志不泄露凭据；失败与取消不写入虚假成功记忆。
4. 真实模型探针与 GUI 验收分开记录；不把探针成功等同于桌面助理全链路成功。
5. 更新本文结果及 `architecture.md`；未达标不标 M1 完成、不合并。

## 前轮稳定性验证记录（当前新代码须重新验收）

- 真实渠道最小探针：CommandCode 指定精确模型返回 `TEAM_MODEL_OK`，exit 0。
- 源清单：`check-sources: ok`；`git diff --check` 无空白错误（存在仓库既有 LF/CRLF 提示，不进行规范化）。
- 全量回归：所有写代码成员退出后，Lead 通过 `tools/build-lock.ps1 -Target test` 获取锁，完成全量编译与 19 项测试，exit 0；`pi-test`、`assistant-engine-test` 都是 0 failures，CLI 为 94 checks / 0 failed。
- 构建签名脚本对本机自签链报告预期的 `UnknownError`，脚本正常退出且所有测试 exe 实际运行成功；不把这当作生产证书链验证。
- 独立 verifier 在唯一执行时段复跑 `assistant-memory-test`、`assistant-context-test`、`pi-client-test`、`assistant-engine-test`、`desk-render-test`，五项 exit 均为 0；未重新编译、未运行现网 GUI。
- 变更文件密钥形态扫描未发现长 key/Bearer 或私钥块；过滤器和合成短串仍是代码的一部分，不宣称形式化安全证明。
- 尚未运行真实桌面 GUI 验收，未部署、未改自启、未操作主工作区和现网进程。

## 前轮稳定性修复与边界（历史）

- T01：引擎区分成功/失败/取消；仅成功执行记忆命令并写 episode/成功调用成本；完成前持久化，避免下一请求覆盖 question；pi wrapper 首次 Ask 懒初始化、同步发布，空闲 Stop 与 worker 启动串行。新增标准 engine 合成 RPC 回归。
- T02：修复崩溃耗尽后再次 Start 的 joinable 线程终止；复审改为逐流有状态诊断脱敏，覆盖长 token、跨 chunk、命令行已知 key；修正测试等待首个 PROTOCOL 事件而非最终 EXIT 的竞态。只承诺已测诊断形态，不把日志脱敏当文件/凭据访问硬隔离。
- T05：未发现构建阻断项；要求补启动阶段取消/成功成本测试、终态 settling 守卫与迟到 delta 守卫，Lead 已采纳；独立复跑关键五项通过（没有第二次全量编译）。
- T03：修复 90 天降权反向加分；事实写盘失败回滚内存；增加 legacy 敏感事实不进入上下文/日志的回归。成员另做过无产物语法检查，不能替代标准构建。
- T04：失败时优先显示错误而非陈旧部分回答；Esc 后拒绝迟到 delta/error/done，下一提交重新接收；增加渲染/交互回归。
- 首轮结束时 MEMORY 共享接线未完成；本轮 T06 已补实现，须以新回归结果验收。真实桌面网络问答、文件读取硬隔离仍未完成；应用仍通过 `--api-key` 启动 pi，未解决 OS 命令行可见性。

## 原生团队推进记录（T06/T08）

- 原生工具管理的 explorer/reviewer 完成只读调查后，三个 fixer 各自负责共享记忆、MEMORY UI、RPC；Lead 独占 engine 与文档，两个 reviewer 独立复审。
- 数据层按目录共享且失败返回 nullptr；成功事实写盘后锁外通知；Pin 为可选 JSON 布尔字段并跨重启保存；删除墓碑不参与合并，显式重新记住相同文本创建新 id。
- MEMORY 卡订阅同一实例，通知弱引用卡片状态，不持有裸 this；析构清重绘入口并退订；菜单错误回滚，排序后按 id 选中。
- Engine Stop 静默禁止新 Ask、清请求/回调，再 RequestStop 中断握手并 join；后续 Start 可重新提问。早退回调移到锁外，问题由值快照供 worker 使用。
- RPC 新增 Idle/Active/Unsettled：本地超时只发布一次终态，远端 run 未确认结束前拒绝新 Ask，丢弃无主流事件；abort 关联 id，stdinWrite 统一锁；RequestStop 为 wrapper 的永久关闭标志。
- 独立复审未发现本轮新增 P0/P1 阻断（旧墓碑合并问题已修正）。保留 fail-closed 策略：任何 PiEvent Error 都使该问答不写记忆，不采用“错误后仍有文本就算成功”的不可靠启发式；未来若需 advisory/fatal 分类应单独设计协议。
- 审查曾称握手哨兵会写仓库 CWD；实际 peer 工作目录为该用例的 configDir，且仅当目录名 handshake-stop 才写相对哨兵，清理范围仍在 fixture 内。新回归需核对确实无仓库散落文件。
- 未证实边界：真实 Pi 的 abort 成功响应与 settled 的严格输出顺序、重复 settled 的防御；不加可能吞掉真终态的猜测计数。协议顺序需单独真实联调验证，自动 fake 不等于协议证明。
- T10 首次验收编译红：SupervisorMain 缺 `}` 导致级联错误，测试未运行；已由 owner 定点补括号。独立探针确认本机部分块内失败路径会返回 0（不是所有 `exit /b` 都如此），Lead 在 build.bat 增加外层显式完成标记。隔离探针失败1/7→外1、成功0→外0，实际非法目标返回1。
- 第二次验收编译/链接全部通过，签名脚本执行，前9项通过（含新共享记忆/Pin/回滚/墓碑回归）；第10项 pi-client-test 报5条 crash/reap 失败，剩余9项未执行。新门禁实际返回1，未误判绿。
- 首因已定位：ReportExit 将旧 run 置 Unsettled，自动重启握手成功后未恢复 Idle，只清 exitPending，故下一 Ask 始终被拒。owner 仅修新子进程握手后的状态复位，再由唯一 verifier 重跑；不放宽断言、不用旧exe。
- 第三次验收编译通过、RPC/engine 转绿，15/19 通过后 desk-render 的真实共享卡片 fixture 被旧敏感过滤 `sk-` 子串误伤（desk-render-test）。这不是本轮引入的 pattern；已修为 token 起始边界，保留真 key 与中文紧邻的拒绝回归，没有改 fixture 掩盖问题。
- 最终验收：官方 `tools/build-lock.ps1 -Target test` 实际 exit 0、编译 0 error，19/19 测试全部运行通过，签名阶段、source inventory 和末尾 `[build] ok` 均到达。独立串行重复 assistant-memory / pi-client / assistant-engine / desk-render 四项，各 exit 0。
- 最终日志在会话系统临时目录：`t10r4-build.out`、`t10r4-build.err`、`t10r4-rerun-{assistant-memory,pi-client,assistant-engine,desk-render}.log`；预览仍为构建生成的 `build/preview.bmp` 与 `build/desk-board-preview.bmp`。未把临时日志手工复制到 build 产物区。
- 签名脚本执行完成，仍显示自签名链的 `UnknownError`（不是生产证书链证明）；测试 exe 已实际执行成功。未运行真实 desk/网络问答、SAC 专项或 TSan/ASan，未测试 gate 改动后的独立 framework/example 命令。
- 新回归后仓库没有散落的 handshake-entered.txt，源码 git 状态未被 verifier 改动；主工作区、现网进程与启动项未改。Lead 收尾 `git diff --check` 无空白错误，变更文件长 key/Bearer 与私钥块形态扫描 0 命中（不构成完整安全证明）。
- 下一验收顺序：先在独立配置目录做真实通道与 GUI 验收；T07 的凭据命令行可见性/读取硬隔离仍单独保留，不能仅因构建绿就宣称 M1 完整安全或自动合并。

## 文件清单

- 协调/构建：`../AGENTS.md`、`../build.bat`。
- 文档：`README.md`、`architecture.md`、`assistant-m1-plan.md`、`desk.md`、`refactor-notes.md`、[m1-acceptance.md](m1-acceptance.md)（新增）、本文（新增）。
- 引擎/RPC/上下文/记忆：`../src/data/assistant/{engine,pi_client,memory,context}.{h,cpp}`。
- 两卡：`../src/panels/assistant/assistant.{h,cpp}`、`../src/panels/memory/memory.{h,cpp}`。
- 宿主：`../src/apps/desk/main.cpp`、`../src/widget/{appenv,board}.{h,cpp}`、`../src/widget/module.h`。
- C++ 回归：`../tests/assistant_engine.cpp`（新增）、`assistant_memory.cpp`、`assistant_context.cpp`、`pi_client.cpp`、`fake_pi_rpc.cpp`、`desk_render.cpp`、`board_host.cpp`。
- 验收工具：`../tests/pi_policy_provider.ts`（新增）、`../tools/check-pi-policy.ps1`（新增）、`../tools/desk-assistant-test.ps1`（新增）、`../tools/desk-testlib.ps1`（仅两个 Win32 消息入口显式 Unicode）。

源码与文档尚未提交；未做 git 写操作、合并、tag 或 push。成员原始事件/报告与本轮构建日志保留在会话临时目录，不混入源码或 build 生成入口。

## 用户授权部署（2026-11-09）

用户在隔离签收后明确要求“开始部署等我验收”。Lead 经只读部署核对后，备份现用安装目录，正常关闭旧 PID 3332，使用正式安装脚本显式部署到 `%LOCALAPPDATA%\Programs\Desktop Desk Assistant`，未覆盖配置/记忆；通过 Explorer 启动新版 PID 31808，生产板窗口可见、响应正常，启动项哈希未变。

备份位于 `%LOCALAPPDATA%\Desktop Desk Backups\m1-20261109-160931-8df91d`；可执行文件哈希与最终验收产物一致。**当前已部署运行，等待用户验收，不代表用户生产验收已通过**。部署与回滚说明见 [m1-acceptance.md](m1-acceptance.md) §5；该部署步骤未提交/合并/打 tag，也未启动 M2。

## 用户授权继续：M2.0

用户随后回复“ok继续推进”。Lead 开始 M2.0；不据此补写用户逐项手工验收结果，不再次操作现用 M1。
完整阶段边界与冻结 schema/API 见 [m2-cardspec-plan.md](m2-cardspec-plan.md)。

| lane | owner | 独占写作用域 | 交付 |
|---|---|---|---|
| M2.0-core | fixer | `src/data/cardspec/cardspec.cpp`、`tests/cardspec.cpp` | 严格无 IO 的 inline-only schema/序列化与错误矩阵 |
| M2.0-ui | fixer | `src/panels/cardspec/*`、`tests/cardspec_render.cpp` | 单张默认隐藏 CARDSPEC 卡，固定 JSON 文件、手动刷新与恢复、渲染回归 |
| M2.0-lead | Lead | `src/data/cardspec/cardspec.h`、main/build、docs/AGENTS | 冻结接口、接线、唯一构建授权与签收 |

M2.0 由 w31 实现 core+错误矩阵，w32 实现 panel/view+无窗口渲染，Lead 冻结头并接 main/build；
w34 只读审计，w33 是唯一构建者。首次两项编译错误和随后一项错误生命周期断言均定点修复，
没有修改框架/M1 组件、删除覆盖或放宽阈值。

最终官方 `test` 21/21、六项关键独立重复、framework/example 均 exit 0，源清单和 `[build] ok` 齐全；
日志 `m2b3-20261109-165404-build.{out,err}.log` 位于会话临时目录。候选签名已执行，测试自绘
`build/cardspec-preview.bmp` 中文/3/5 与 0/5 比例已目视。M2.0 可按无窗口渲染范围签收，
不是完整 M2 或实际板上 GUI 验收；最终明细见 [m2-cardspec-plan.md](m2-cardspec-plan.md) §6。

M1 三十八文件未提交基线全部保留，不自动 git 写；M2 核心/UI 新文件分离，不修改现有
assistant/memory/RPC 或框架，不恢复模型工具、不执行生成代码。已部署 M1 未触碰，M2 候选未部署。

## 用户授权继续：M2.1 HTTP

用户再次要求“继续”，Lead 按 [m2-http-plan.md](m2-http-plan.md) 推进受限 HTTP。
已冻结 `strict_json.h`、`http_source.h`、`http.h`、面板工厂选项与 ViewState/LoadOptions；
旧 v1 Parse 仍严格只接 inline，v2 由新入口处理。初次 Load 零出网、精确用户授权与响应不缓存旧值。

| lane | 独占写作用域 | 交付 |
|---|---|---|
| M2.1-source | `src/data/cardspec/{cardspec,http_source}.cpp`、`tests/cardspec_source.cpp` | 通用严格 JSON facade、v2 与 Pointer 绑定 |
| M2.1-network | `src/data/cardspec/{http_policy,http_winhttp}.cpp`、`tests/cardspec_http.cpp` | 用户授权/公网策略/独立 WinHTTP、fake 矩阵与固定公开探针 |
| M2.1-ui | `src/panels/cardspec/*`、`tests/cardspec_network.cpp` | 手动 Fetch、确认/撤销、权限 epoch、as-of 与错误恢复 |
| M2.1-lead | 新公共 headers、build、docs/AGENTS；`tests/cardspec_source.cpp` 仅剩余 fixture 字符串引号 | 接口裁决/接线/唯一构建授权；原作者 idle 后接手单点夹具修复 |

本阶段最终已完成新的官方 24/24、七项独立重复、framework/example 与候选签名（日志
`m21b4-20261109-183805-build.{out,err}.log` 在会话临时目录）；HTTP fake 517 checks/73 resolve/50 exchange。
w38/w39/w32 分别交付 source、网络、UI；w34 与 w40 审计并复核权限发布 epoch/DNS lifetime/headers/
选项 scope 等修复，w33 唯一构建。三处 Source fixture 引号及未批准应 Blocked 的测试前置均修正，
没有放宽 schema/权限，不修改 M1 或框架。

w33 唯一授权固定公开 GitHub 探针实际 exit 1（60ms，AddressBlocked/status0）；只读解析诊断确认
本机返回保留 198.18/15 映射，过滤生效、未进入 HTTPS，也未重试/代理兜底/改变系统设置。
因此 **M2.1 离线候选通过，真实 HTTPS 正向验收未完成**，不能以安全拒绝当所有联网功能已验收。
详情见 [m2-http-plan.md](m2-http-plan.md) §6；需要正常公网 DNS/直连复验或另行设计显式代理授权。

现用 M1/凭据/启动项未触碰，M2 候选未部署，无 git 写。
