# M1 完整验收

> 需求依据：[assistant-m1-plan.md](assistant-m1-plan.md) §1/§7；当前实现见
> [architecture.md](architecture.md) §13；团队与过程见 [agent-team-handoff.md](agent-team-handoff.md)。
> 本文区分实现、自动回归、实际 Pi、隔离 GUI 和未验证边界，不以旧二进制或单次探针替代验收。

## 1. 验收矩阵

| 项 | 交付与判据 | 本轮结果 |
|---|---|---|
| 召唤与交互 | 可配置热键、注册失败可见且有菜单入口；中文输入/流式；Esc；恢复置底 | 离线 83/83、真实模型 GUI 40/40；真实取消后恢复已过，物理组合键未按 |
| 来源 | 合法事实/DSH/episode id 白名单；非法标记剥离；无来源 NO CITATION；普通双括号保留 | 自动回归、离线 GUI、实际模型 probe/real 均通过（仅合成资料） |
| 本地记忆 | 记住/忘掉/清空不依赖网络；错误可见；MEMORY 看/改/Pin/删/清空；重启保留 | 自动回归与离线 GUI 全过，含确认取消/确认清空与两次重启 |
| 遗忘边界 | 事实/episode 过滤；记忆变化与清空使旧 Pi 会话失效；重启不复活 | 自动回归及离线清空后重启通过；未变记忆下实际 Pi 空闲唤醒严格续同一私有会话 |
| BRIEF | 活跃项目、最近结论、卡点；无数据明确“未记录”；聚焦/完成后后台刷新 | 自动回归与合成 DSH 隔离 GUI 通过 |
| 会话/成本/预算 | 正常重启/空闲退出后续用；缺失文件恢复；每日 token/calls/ms；超预算本地摘要 | 自动回归通过；real 4 次发送（3 成功）/2186 tokens，取消后恢复、空闲唤醒续同一 session 通过 |
| 空闲/禁用 | 默认 10 分钟、毫秒 seam 验证退出/唤醒；空闲资源采样；enabled=0 不注册助理 | 自动/离线禁用通过；real 自然退出 60.0 秒，Node 97.1MB/CPU 0ms；board 采样 CPU 0.31%、27MB；未实等默认 10 分钟 |
| 凭据 | 密钥仅子进程私有 Unicode 环境；argv 无 key；父 canary 剥离；问题/Persona/上下文拒当前 key | 进程级/Engine 回归通过；真实应用通过既有引用完成模型 probe，无 key 值入报告 |
| 模型工具 | 私有 Pi agent 目录、显式官方 provider、强制零工具、关闭 ambient 资源；真实 Pi 旧会话续用不能恢复工具 | 最终无 BOM 脚本实跑 19/19 通过，见 `pi-policy/20261108-213605` |
| 回归与集成 | 源清单、全量官方 test 目标、独立关键重复、SDK/example，签名与日志逐项核对 | 官方 19/19、五项独立重复、framework/example 均 exit 0；主产物已重签 |

## 2. 证据入口

- 常规回归：`tools/build-lock.ps1 -Target test`（单一构建者）；必须同时核对测试输出、源清单与最终完成标记。
- 实际 Pi 离线策略：`tools/check-pi-policy.ps1`，本地 faux provider、独立配置目录、无网络/真实凭据；baseline / hard-zero / 未加硬边界对照三阶段。
- 隔离 GUI：`tools/desk-assistant-test.ps1 -Stage offline`。
- 真实通道探针与 GUI 分开：`-Stage probe -AuthorizedNetworkProbe`、`-Stage real -AuthorizedNetworkProbe`。
- 运行证据均由脚本生成到 `build/qa/`，不手改产物、不复制用户配置内容、不保存密钥或完整环境。
- 截图仅隔离窗口 PrintWindow 或板自身画布导出；不回退屏幕截图。

## 3. 边界与非目标

- 零工具限制模型的可调用能力；不是 OS AppContainer，也不防同用户调试器读取内存/环境。安装的 Pi/官方 provider 与用户显式配置属于可信代码。
- 清空覆盖事实与 episodes；两个原子文件重写不是数据库事务。失败不可报成功，恢复为 best effort；重置代号用于使旧模型会话失效，不保证磁盘安全擦除。
- 卡点仅摘录缓存中明确记录的文本，缺失时不推断。项目事实使用导入检查点，避免删除后自动复活；并非持续项目同步器。
- Pi 版本/工具恢复策略升级需重跑实际策略脚本。fake RPC 证明 C++ 行为，不证明任意未来 Pi 协议顺序。
- 隔离实例不占用生产全局热键；可选物理测试只用独立测试组合键，不对正在运行实例发送默认 Ctrl+Alt+A。
- 未经完整证据签收不宣称 M1 完成，不自动修改主工作区、现网程序或启动项。

## 4. 最终签收

**M1 功能实现与限定隔离验收已完成（2026-11-09）**。签收范围为本工作树、当前 Pi 1.5.0、合成业务资料及指定真实模型，不等于生产部署或任意环境的安全证明。

实际证据：

- 实际 Pi 离线策略最终证据：`build/qa/pi-policy/20261108-213605/result.json`，脚本 exit 0、19/19；子退出均为 0，工具 `[read] / [] / [read]`，中文 fork 路径无损且 baseline hash 未变。JSON 无 BOM，Node 直接解析通过。
- 最新官方构建：编译 0 error、19/19、source inventory 与 `[build] ok` 均到达；memory/context/pi/engine/render 五项独立重复、framework/example 均 exit 0；三个主产物已重签。日志在会话临时目录 `m1b3-20261108-230247`，自签链仍为 `UnknownError`，不是生产证书链证明。
- 离线 GUI 最终证据：`build/qa/assistant-m1/offline-20261109-000207/evidence/gui-offline.json`，作者实跑 exit 0、83/83；独立 reviewer 复核 JSON、字节/窗口类前置、前后失败对照及清理。覆盖输入/流式/来源、记忆全部菜单、确认取消/清空、跨重启、Esc 和禁用。早期红记录保留，不作为通过证据。
- 实际应用模型 probe：`build/qa/assistant-m1/probe-20261109-000453/evidence/probe.json`，独立实跑 exit 0、22/22；指定模型给出中文及合法来源，calls=1/tokens=1085，实际 Node 峰值 96.2MB、3 秒 CPU 0ms；board 空闲采样 CPU 0%、WS 26.9MB，均正常收尾无本次子树残留。此为探针，不替代 real 阶段的取消后恢复/实际空闲退出和续用。
- 真实模型 GUI：`build/qa/assistant-m1/real-20261109-001815/evidence/gui-real.json`，独立实跑 exit 0、40/40。真实取消已由 send.log/私有 session 用户轮正向确认，未写成功 episode；恢复提问写入精确新 episode。实际 Node 自然空闲 60.0 秒退出、board 仍存活；唤醒为新 Node 身份，严格续同一私有 session 路径、文件增长且未新增 JSONL。4 次发送、3 次成功、2186 tokens，无重试；Node 峰值 97.1MB、3 秒 CPU 0ms，board 5 秒采样 CPU 0.31%、27MB。旧/新 Node 与本 run 镜像均无残留，HKCU Run 未变。
- 最终只读审计：当前 C++ 均早于新绿构建，之后未改；`check-sources: ok`、`git diff --check` 通过。38 个变更文件的密钥形态命中已按过滤器/测试夹具/标识符归类，无可疑真实 key；这是启发式扫描，不是形式化证明。
- 默认 10 分钟不宣称已实等：实际 GUI 使用 1 分钟配置，默认值及计时逻辑由自动回归覆盖；默认物理 Ctrl+Alt+A 与可选 F10 均未按，只验证本实例的召唤消息路径，保持现网不受影响。
- 活跃生产目录会自行刷新，脚本只记录其元数据变化并排除“静态不变”断言；其余静态目标比较通过。所有控制/截图只针对本次隔离实例，不将这一比较外推为 OS 写入沙箱。
- 代码仍未提交、合并或打 tag。上述为部署前隔离签收；用户后续授权的实际部署见下节。

## 5. 用户授权部署（2026-11-09，待用户验收）

- 按用户“开始部署等我验收”授权，显式指定现用 `%LOCALAPPDATA%\Programs\Desktop Desk Assistant`，不使用安装脚本默认的旧目录。
- 旧实例 PID 3332 经所属主窗口 WM_CLOSE 正常退出，未强制结束；旧可执行文件和安装目录状态备份到 `%LOCALAPPDATA%\Desktop Desk Backups\m1-20261109-160931-8df91d`，不进入仓库。
- 使用 `tools/install-desk.ps1` 仅替换已验收的签名 `desk.exe`；安装后 SHA-256 为 `18F38B7280796ADEADE01C1C2F44113385B6B9E87CA912ED478C7DE4E95BE362`，与验收产物一致。复制期间非可执行文件元数据未变，未覆盖既有配置/记忆；正常启动后应用可继续更新自身状态。
- 通过 Explorer Shell 启动现用安装路径，确认新 PID 31808、生产窗口类 `wg.desk` 可见且响应；启动项全表哈希与部署前一致。未执行用户业务提问或替用户做生产验收。
- **已部署运行，等待用户验收**。如需回滚，先正常退出新版，再以备份中的 `installation-before` 为 `-SourceDir`、现用目录为 `-Destination` 运行同一安装脚本，然后从 Explorer 启动；默认只回滚程序，不覆盖部署后的新配置/记忆。状态备份仅在用户明确要求时恢复。
