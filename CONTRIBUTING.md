# 贡献指南（main / dev 流程）

本仓库用两条长期分支：**`main` = 已验收阶段基线**，**`dev` = 下一阶段开发候选**。日常开发在 `dev`，
开发完成后开 PR，CI 变绿 + 维护者**显式人工验收**后才由维护者 merge 到 `main`。
协作/构建/凭据约定见 [AGENTS.md](AGENTS.md)，用户向说明见 [README.md](README.md)。

> 单人仓库的硬事实：`required_approving_review_count = 0`。GitHub 不允许作者
> approve 自己的 PR，所以**验收不靠 approve 按钮，而靠 PR 评论里的显式记录**。
> 本文里「验收」都指那份记录，不是 CI 绿灯。

## 1. 角色与权限

| 事项 | 谁做 |
|---|---|
| 写代码、本地构建/测试、开 PR、回复评论 | 任何协作者（含 AI agent，在给定写作用域内） |
| push 到 `dev` | 开发者可直接 push（也可以走 PR，二选一，不强制） |
| push 到 `main` | **禁止**，包括维护者；`main` 初始化提交之后只能经 PR 合并 |
| merge PR、打 tag、创建/删除分支、改分支保护、改仓库可见性 | 仅维护者 `@kkiwkK1`（Lead） |
| 人工验收记录 | 仅维护者 `@kkiwkK1` |

AI/子 agent 的 Git 写操作一律由维护者代做：它们只写文件、跑本地验证、汇报文件清单。

## 2. 分支模型

| 分支 | 含义 | 进入方式 |
|---|---|---|
| `main` | 已验收阶段基线：可回滚，未验证能力保留限制 | 只接受 PR；required check `Windows regression` 通过 + 维护者验收记录 |
| `dev` | 下一阶段候选：日常集成 | 直接 push 或 PR；不自动删除；不设「先 PR」的前置要求 |
| `feat/*`、`fix/*` 等 | 可选的一次性工作分支 | 从 `dev` 切出，PR 目标 `dev`，合并后可删 |

`main` 保护（由维护者在 GitHub 设置，协作者不要改）：

- Require a pull request before merging，`required_approving_review_count = 0`
- Require status checks to pass：**`Windows regression`**（必须是最新 commit 上通过）
- Require branches to be up to date before merging（`strict`）：合并前先带上 `main`
  最新提交；用 “Update branch” 合并 `main` 进来，**不要** rebase
- Require conversation resolution before merging
- Do not allow force pushes / Do not allow deletions（`enforce_admins` 同样生效，
  维护者也不能绕过）
- 只允许 **merge commit**（关闭 squash merge 与 rebase merge）
- 关闭 auto-merge

## 3. 日常开发（在 `dev` 上）

```bat
git clone https://github.com/kkiwkK1/desk.git
cd desk
git switch dev
set TOOLROOT=C:\Tools\mingw64          rem 或 MSYS2 UCRT64：set TOOLROOT=C:\msys64\ucrt64
cmd /c build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test
```

- 并行会话不要同时跑 `build.bat`，用 `tools\build-lock.ps1` 串行化。
- 本机 Smart App Control 会拦新构建的 exe：重签用 `tools\sign-local.ps1`
  （当前用户自签证书，不上传；CI 不复用本机证书）。
- 提交前：`git status` 里不应出现 `build\`、`*.exe`、用户 `*.ini`、日志、证书。
- 长期 `dev` 允许普通 push；**不要** force push / reset / rebase 共享分支。

## 4. 开 PR

1. 从 `dev` 切工作分支（或直接在 `dev` 上提交后开 PR，二选一）。
2. 填 `.github/PULL_REQUEST_TEMPLATE.md`：摘要、范围、验证、风险、检查清单。
3. 目标分支规则：
   - **直接在 `dev` 开发 → `dev` 到 `main` 的阶段 PR**；验收前保持 **draft**，
     完成 CI 和维护者明确验收后才转 ready，以 merge commit 发布到 `main`。
   - 可选 `feat/*` / `fix/*` 工作分支的集成 PR 目标是 `dev`；进入 `dev` 不等于获准发布。
   - **`main`** 只接受阶段基线修复及 `dev` 上已验收内容。合并前必须带上验收记录，
     并保留真实网络等尚未验证能力的限制，不因阶段验收就扩大结论。
4. 等 required check **`Windows regression`** 在最新 commit 上变绿；失败就修，
   不要靠重跑碰运气（重跑不改代码只会重复同样的失败）。
5. 不要开 auto-merge，不要自己合并到 `main`。`dev` 的 PR 也建议由维护者合并，
   保持「谁合并、何时合并」有记录。

## 5. CI：Windows regression

文件：`.github/workflows/windows.yml`；job `regression` 的 `name` 固定为
**`Windows regression`**（分支保护的 required check 用的就是这个名字，**不要改名**）。

- 触发：push 到 `main`/`dev`、以 `main`/`dev` 为 base 的 PR、手动 `workflow_dispatch`。
- 权限：`contents: read`；`actions/checkout` 用 `persist-credentials: false`，
  CI 里没有任何写权限和凭据。
- 环境：`windows-latest` + MSYS2 **UCRT64**（`msys2/setup-msys2`，
  `TOOLROOT` 由 setup 输出的实际路径推导，通常 `C:\msys64\ucrt64`；工具链步骤会
  打印 `g++ --version` 并确认接受 `-std=c++20`）。
- 步骤：checkout → 装 GCC → 解析 `TOOLROOT` → `tools\build-lock.ps1 -Target test`
  → 打印证据清单（不上传产物）。
- 时限/并发：job `timeout-minutes: 60`；按 ref 排队，`cancel-in-progress: false`
  （不取消进行中的运行，避免 `main` 上最新一版的验收结果被后到的 push 顶掉）。
- 离线：测试全程不访问外部服务、不 live-probe、不连真实 Pi/SSH、不使用真实凭据。
  因此 CI 红 = 真回归，而不是「没配密钥」。测试个数随分支变化，**不要写死**数量。
- 证据：`build\preview.bmp`、`build\desk-board-preview.bmp` 等渲染产物只出现在
  runner 的 `build\` 里，不打包、不上传、不入库。

本地等价验证（与 CI 同一个入口）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test
```

排错顺序：工具链步骤（找不到 `g++` / 版本太老）→ 编译错误行 → 失败的具体
`*-test.exe` → `tools\check-sources.ps1` 的 `DEAD-CPP` / `UNREF-H`。

## 6. 人工验收（单人边界）

`main` 的合并条件 = **CI 绿 + 维护者显式验收记录**。记录写在 PR 评论里，格式：

```text
验收通过
- 验收人：@kkiwkK1
- commit：<完整 SHA>
- 日期：YYYY-MM-DD
- 方式：本机 build.bat test 全绿；tools\desk-testrun.ps1 <N> 场景 / <M> 检查 / 0 失败
```

- **不要求、也不允许自我 approve**：`review_count = 0` 是单人仓库的既定设置，
  不要为了「流程完整」把 review 数调高（会让所有 PR 永久卡住）。
- 验收必须是**人**的动作：看 diff、跑本机测试、必要时跑隔离实例 GUI 场景并看
  `build\qa\` 截图。PR 模板里的勾选项和 CI 都**无法机器验证**这一点。
- 没有验收记录就不得 merge；未验收的 M2 内容只开 **draft PR**，不要提前合并到
  `main`，也不要开 auto-merge。
- 合并方式：**merge commit**（保历史）。禁 squash、禁 rebase merge。合并后
  `dev` 保留，不删除。

## 7. 合并后同步 `dev`

`main` 每次验收合并后，`dev` 立刻跟进一次（在 `dev` 上）：

```bat
git switch dev
git fetch origin
git merge origin/main
```

禁 `reset --hard`、禁 `rebase`、禁 `force push`：`dev` 是长期分支，重写会让其他人
的本地历史失效。冲突在 `dev` 上正常 merge 解决即可。

## 8. 发布与 tag

- 只在 `main` 上打 tag（`desk-m1.x` 之类），**仅维护者**执行。
- 发布产物是本地签名后的 `build\bin\desk.exe`；构建/安装步骤见
  [README.md](README.md) 与 [AGENTS.md](AGENTS.md) §2。不要把 exe 提交进仓库。

## 9. 提交与隐私

- 一个提交一件事；提交信息说清「为什么改了」。
- 绝不提交：`build\` 生成物、`*.exe`、`*.o`、日志、用户 `*.ini`
  （`aimeter.ini` / `desk.ini` / `ps.ini` / `server.ini` / `assistant*.ini` 等）、
  `*.jsonl`、`trust.json*`、`.env*`、证书/私钥（`*.pfx`/`*.pem`/`*.key`）。
- 凭据只存 Windows 凭据管理器；提交前扫一遍 `sk-`、`Bearer`、`PRIVATE KEY`。
- `.github/dependabot.yml` 只跟踪 GitHub Actions，每周向 `dev` 提一次 PR；
  这些 PR 同样要过 `Windows regression` 并等维护者验收后合并。

## 10. 常见问题

**为什么不能先合后端再补测试？**
required check 只看最新 commit；「先合并、测试以后补」在 `main` 上等于把红的状态
发布出去。请把测试一起放进同一个 PR。

**为什么 `dev` 既允许直推又要求 PR？**
`dev` 是候选集成区，允许快速迭代；`main` 才是必须经 PR + 验收的稳定线。两者要求
不同是刻意的。

**CI 绿了还能不能直接合到 `main`？**
不能。CI 绿只是必要条件；还需要 §6 的验收记录，并且由维护者执行 merge commit。

**PR 卡在「waiting for status to be reported」？**
先看 check 名字是否还是 `Windows regression`、workflow 是否被改名/移动；分支保护里
的 required check 名称和 `jobs.regression.name` 必须一致。
