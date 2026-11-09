# M1 实施计划：可召唤的助理 + 长期记忆（pi RPC）

> 依据 [assistant-prd.md](assistant-prd.md) 与 [assistant-plan.md](assistant-plan.md)。
> 默认决策（未经另行反对即按此执行）：
> 1. 召唤 = 临时把整块板抬到最前 + 聚焦助理卡（不做单独悬浮窗）；
> 2. 工具 = 只读 `read,grep,find,ls`；
> 3. 记忆 = 自动写入 + 可删/可改/可忘（不做"候选确认"流程）；
> 4. pi 会话 = 单一助理会话长期续用；
> 5. 上下文 = 先用 `session_projcache` 摘要，不碰 zstd 会话正文；
> 6. 模型 = pi 默认 DeepSeek 模型，先不做 Router/Worker 拆分。

## 1. 目标与验收

目标：`Ctrl+Alt+A` 唤出助理，问项目/学习/额度；中文流式回答带来源；能记住目标、偏好、约束、里程碑，可查可改可忘；不主动推送（通知属于 M0b）。

验收：

1. `Ctrl+Alt+A` 唤出/隐藏；输入问题；中文流式；Esc 取消；
2. 回答引用真实来源（DSH 会话/项目/记忆），无来源时标 `NO CITATION`；
3. "记住：…"落盘，重启仍在；MEMORY 卡可看/删/改；"忘掉…"立即生效；
4. BRIEF 区块列出 DSH 活跃项目 + 最近结论 + 卡点（来自 projcache `turnOutline`）；
5. `build.bat test` 全绿；fake RPC、记忆、上下文、渲染测试覆盖；
6. 空闲 0 CPU；pi 空闲 10 分钟自动退出；密钥不进 prompt/日志。

不在 M1：主动通知/托盘气泡（M0b）、CardSpec 生成卡片（M2）、看板/任务执行（M3）、多助理并行、DSH 会话正文解析。

## 2. 架构

```text
desk.exe（UI 线程）
  └─ AssistantModule（Wide 568：输入行、流式回答、来源、BRIEF、状态）
       │ worker 线程
  └─ AssistantEngine
       ├─ ContextBuilder：画像 + 活跃项目 + 近期 episodes + 检索，预算内装配 prompt
       ├─ MemoryStore：assistant-episodes.jsonl / assistant-facts.jsonl（UTF-8，原子写）
       └─ PiClient：子进程 `pi --mode rpc`（JSONL stdin/stdout）
            └─ pi session（长期续用）+ persona.md + 只读工具
```

- pi 只当 Worker：不 fork、不装插件；人格用 `--append-system-prompt persona.md`；
  硬约束（只读、超时、预算、空闲退出、密钥不出现）都在 C++。
- 工具白名单 `--tools read,grep,find,ls`；不开放 bash/edit/write。
- 上下文由 C++ 提供：`~/.dsh/storages/workspace.json`、`session_projcache` 的
  `turnOutline`、记忆事实、当前时间；pi 不直接读凭据。
- 启动：`pi --mode rpc --session-dir <配置目录>\pi-sessions [--session <上次路径>]`；
  `get_state` 后保存 session 路径；空闲 10 分钟 `Stop()`，下次唤醒重启并续会话。
- 取消：Esc → `{"type":"abort"}`。

### 接口（先冻结，UI/引擎据此并行）

```cpp
namespace assistant {

struct PiEvent {
    enum class Kind { TextDelta, Tool, Usage, Done, Error };
    Kind kind;
    std::wstring text;       // TextDelta/Tool/Error
    long long inputTokens;   // Usage
    long long outputTokens;
    bool aborted;            // Done
};

class PiClient {
public:
    bool Start();
    void Stop();
    void Ask(std::wstring_view prompt);
    void Cancel();
    bool Running() const;
    std::wstring SessionPath() const;
    void SetEventHandler(std::function<void(const PiEvent&)>);  // worker 线程
};

struct Fact {
    std::wstring id, kind, text, sourceRef;
    long long createdMs, updatedMs, expiresMs;
    int confidence;   // 0..100
    std::wstring status;  // active / deleted
};

class MemoryStore {
public:
    bool Load(const std::wstring& configDir, std::wstring* error);
    bool AppendEpisode(const std::wstring& question, const std::wstring& answer,
                       const std::wstring& sources, std::wstring* error);
    bool AddFact(const Fact& fact, std::wstring* error);
    bool UpdateFact(const std::wstring& id, const std::wstring& text, std::wstring* error);
    bool DeleteFact(const std::wstring& id, std::wstring* error);
    std::vector<Fact> Facts() const;
    // keyword + recency + kind 过滤；用于 context 与 MEMORY 卡
    std::vector<Fact> Query(std::wstring_view text, int limit) const;
};

struct ContextBundle {
    std::wstring prompt;                 // 注入 pi 的完整上下文
    std::vector<std::wstring> sourceIds; // 允许被引用的 id/ref
};

class ContextBuilder {
public:
    ContextBundle Build(long long nowMs, int budgetTokens);
};

} // namespace assistant
```

## 3. 数据与状态文件

| 文件 | 内容 |
|---|---|
| `assistant.ini` | `enabled`、`hotkey`、`pi` 命令覆盖、`model`、`toolset`、`idle_stop_minutes`、`max_context_tokens` |
| `persona.md` | 助理人格与规则（首次生成，可编辑） |
| `assistant-episodes.jsonl` | 只追加：问、答、时间、来源、会话路径 |
| `assistant-facts.jsonl` | 长期事实：`id/kind/text/source/confidence/status/created/updated/expires` |
| `assistant-cost.ini` | 每日 token/调用次数/耗时 |
| `assistant-send.log` | 每次发送的上下文摘要（引用 id/文件，不含密钥） |
| `pi-sessions/` | pi 自己的会话文件（不由我方解析） |

记忆三层：episodes → facts → working context。事实必须带来源；90 天未用降权；
敏感路径（`.ssh`、`.credentials`、`*.pem`、凭据名）永不进入上下文与发送日志。

## 4. 阶段与交付

### M1.0 运行时（4–5 天）

| 交付 | 文件 |
|---|---|
| pi 子进程 + JSONL 客户端（启动/重启/超时/abort/事件队列） | `src/data/assistant/pi_client.{h,cpp}` |
| JSON 解析/序列化（事件、facts、prompt 结构） | `src/data/assistant/json.{h,cpp}` |
| fake RPC 子进程 + 客户端测试 | `tests/fake_pi_rpc.cpp`、`tests/pi_client.cpp` |
| 助理卡（输入/流式/来源/BRIEF/状态） | `src/panels/assistant/assistant.{h,cpp}` |
| 召唤：`Ctrl+Alt+A` 抬前 + 聚焦；失焦/隐藏恢复置底 | `src/widget/board.{h,cpp}` |
| 模块注册、persona 默认、配置 | `src/apps/desk/main.cpp` |

### M1.1 记忆与上下文（4–5 天）

| 交付 | 文件 |
|---|---|
| 记忆存储（追加/原子改写/查询/纠正/遗忘/去重/裁剪） | `src/data/assistant/memory.{h,cpp}` |
| DSH 上下文 + 预算装配 + 发送日志 | `src/data/assistant/context.{h,cpp}` |
| 引擎（提问→上下文→pi→流式→来源校验→episode/成本） | `src/data/assistant/engine.{h,cpp}` |
| MEMORY 卡（事实列表/来源/时间；Pin/Delete/Refresh） | `src/panels/memory/memory.{h,cpp}` |
| 测试 | `tests/assistant_memory.cpp`、`tests/assistant_context.cpp` |

来源规则：模型输出 `[[mem:<id>]]` / `[[src:<ref>]]`；引擎只接受真实存在的 id，
非法引用剥离；一条都没有 → 页脚 `NO CITATION`（黄色）。

### M1.2 项目 / BRIEF / 成本 / 隐私（3–4 天）

- 项目：`workspace.json` 标题/路径/最后活动 → `kind=project` 事实；"记住：X 的目标是 Y" → `kind=milestone`；
- BRIEF：召唤时展示今日/本周各项目"最后活动 + 一句结论 + 卡点"，可展开来源；
- 成本：每次问答记 tokens/耗时，页脚 `TOKENS n · 12s`，日累计写 `assistant-cost.ini`；
- 隐私：发送记录、禁用开关、清空记忆、`persona.md` 可编辑；
- 文档：`docs/architecture.md`、`README.md`、`docs/README.md`、`AGENTS.md` 同步。

### M1.3 验收（2 天）

- Lead 集成 + verifier 独立复跑；打 tag `m1-assistant`，合并 `feat/assistant-m1` → `dev`。

## 5. Agent Team

| 角色 | 负责 | 写作用域 |
|---|---|---|
| Lead | 召唤/board、模块注册、build.bat、引擎接线、git/合并 | `board.*`、`main.cpp`、`build.bat`、`engine.*`、`tests/assistant_engine.cpp`、docs |
| m1-pi | JSON + PiClient + fake RPC + 测试 | `src/data/assistant/{json,pi_client}.*`、`tests/fake_pi_rpc.cpp`、`tests/pi_client.cpp` |
| m1-memory | 记忆/上下文 + 测试 | `src/data/assistant/{memory,context}.*`、`tests/assistant_memory.cpp`、`tests/assistant_context.cpp` |
| m1-ui | 助理卡 + MEMORY 卡 + 渲染测试 | `src/panels/assistant/*`、`src/panels/memory/*`、`tests/desk_render.cpp` |
| verifier | 独立构建/测试/审计 | 只读 + `build/` |

规则：只在 `_worktrees/assistant-m1` 内工作；不并发构建；只有 Lead 做 git 写操作；
写作用域互不重叠；越界先停并通知 Lead。

## 6. Git

- 分支 `feat/assistant-m1`，worktree `_worktrees/assistant-m1`（从 `dev` @ `m0a-final` 之后）；
- 提交序列：`docs(m1): plan` → `feat(assistant): pi rpc client` → `feat(assistant): memory + context` → `feat(assistant): ask card + summon` → `feat(assistant): memory card` → `test/docs`；
- tag `m1-assistant`；合并回 `dev` 后保留分支；
- 回滚：worktree 内 `git reset --hard <tag>`；单点 `git revert`；不重写 `dev`；提交前扫描密钥。

## 7. 测试与验收

| 层 | 用例 |
|---|---|
| 单元 | JSON、PiClient 帧/超时/崩溃/取消（fake）、记忆 CRUD/纠正/遗忘/去重/裁剪、上下文预算、引用校验、敏感路径排除 |
| 渲染 | 助理卡中文流式换行/光标/高度；MEMORY 卡行布局；`desk_render` 增加快照 |
| 集成 | fake RPC 全链路（问→流式→done→episode）；重启记忆仍在；pi 空闲退出/唤醒续会话 |
| 手工 | 真实 pi 回答带来源；记住/忘掉；Esc 取消；无密钥；空闲 0 CPU；pi 运行时内存 < 150MB |

门禁：`build.bat test` 全绿、`check-sources.ps1` 通过、verifier 签收；真实 pi 网络问答由用户在正常会话补验（DSH 沙箱 WinHTTP 不可用）。

## 8. 风险

| 风险 | 应对 |
|---|---|
| pi RPC 版本漂移 | 锁定 1.4.0；`assistant.ini` 可覆盖命令；fake 测试不依赖网络；升级前跑兼容测试 |
| 启动器 stdout 污染 | 优先 `pi.cmd`；失败回退 `node <release>/dist/bundle/cli.js`；stderr 单独 drain |
| 记忆编造/污染 | facts 带来源；回答只引用真实 id；纠正/遗忘可见；候选可删 |
| 隐私 | 只读工具 + 敏感路径排除 + 发送日志；`enabled=0` 完全停用 |
| 成本 | 空闲停进程、上下文预算、每日 token 记录；超预算降级为本地摘要 |
| 召唤与置底冲突 | 临时抬前 + 聚焦，隐藏/失焦 `PinToDesktop()`；平时仍置底 |
| 中文渲染 | 复用 `cjk::TextTop` / `Wrap`；渲染测试覆盖首行 |

## 9. 未决项（默认已选，随时可改）

1. 召唤形态：整板抬前（默认） vs 独立悬浮窗；
2. 工具权限：只读（默认） vs bash/写；
3. 记忆写入：自动 + 可删（默认） vs 候选确认；
4. 会话：单会话续用（默认） vs 每问新会话；
5. 上下文：projcache 摘要（默认） vs 会话正文（需 zstd）；
6. 模型：pi 默认（初始决策） vs Router/Worker 拆分。

## 10. 当前执行修正与签收（2026-11-09）

- 本文前部为原实施方案；当前实现以 [architecture.md](architecture.md) §13 为准，
  进度以 [agent-team-handoff.md](agent-team-handoff.md) 为准，不把阶段计划当完成证明。
- 当前默认渠道是 CommandCode 的 `deepseek/deepseek-v4.1-flash-fast`，配置通过
  `assistant.ini` 的 provider/model/key_ref；当前 M1 强制零工具与私有 Pi agent 目录，
  只显式加载官方 provider。旧 `tools` 白名单配置不再放开模型工具，不是 OS 沙箱。
- `tests/assistant_engine.cpp` 已进入标准 test 目标，不再依赖 build 目录里的临时 harness。
- MEMORY 卡共享接线、锁外变更通知与持久化 Pin 已补实现，当前验收以任务板的新回归结果为准。
  Pin 仅影响列表排序，不刷新检索年龄，也不保证免于事实数上限淘汰；这是本轮明确的最小语义。
- **功能实现与限定隔离验收已完成**：官方 test 19/19、关键五项独立重复、SDK/example，
  实际 Pi 策略 19/19、离线 GUI 83/83、真实 probe 22/22、真实模型 GUI 40/40 均通过。
  取消后恢复、配置 1 分钟自然退出和新 Node 续同一 session 已实际验证，证据见
  [m1-acceptance.md](m1-acceptance.md)。
- 业务资料仍为合成 DSH、只启助理两卡；未操作用户真实资料/现网或默认物理热键，
  未实等默认 10 分钟。密钥通过子进程私有环境而非 argv；清空非事务/安全擦除。
- 修改尚未提交、合并或打 tag。用户随后明确授权部署，现用安装已更新并正常启动，
  等待用户验收；备份、切换与回滚见 [m1-acceptance.md](m1-acceptance.md) §5。
