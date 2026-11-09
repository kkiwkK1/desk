# AGENTS.md — Desktop Desk 协作约定

本文件写给在**本仓库**工作的 AI/人类协作者：构建/测试/签名的唯一入口、目录与职责
分层、写作用域、凭据与隐私边界，以及 `main` / `dev` 的 Git 权限。分支流程与验收
细节见 [CONTRIBUTING.md](CONTRIBUTING.md)，模块细节见
[docs/architecture.md](docs/architecture.md)，文档索引见 [docs/README.md](docs/README.md)，
用户向说明见 [README.md](README.md)。

## 0. 一句话

Windows 桌面洞洞板 `desk.exe`（C++20 + Win32，MinGW-w64）：一个进程、一块板、
若干卡片（Account / Servers / PowerShell / Assistant / Memory）；`srvctl.exe` 是
共享 `src/data/server` 的 SSH 命令行工具。

## 1. 分支模型与 Git 权限（先读这一节）

| 分支 | 角色 | 谁能写 | 规则 |
|---|---|---|---|
| `main` | **稳定 M1**，可交付/可回滚 | 只经由 PR 合并 | 保护开启：必须有 PR、required check `Windows regression` 绿、禁 force push、禁删除分支、要求对话解决；`required_approving_review_count = 0`（单人仓库不能自我 approve） |
| `dev` | **M2 候选**，日常集成分支 | 开发者可直接 push | 不要求先开 PR；禁 force push；不设自动删除；验收合并后从 `main` 同步 |

- **`main` 初始化提交之后严禁直接 push**（对 Lead 也一样）：所有进入 `main` 的改动
  必须走 PR，并且有 `kkiwkK1` 的显式人工验收记录（PR 评论里的「验收通过 + 日期 +
  commit SHA」）。`review_count = 0` 不是「不用验收」，而是「不靠 GitHub approve
  按钮验收」。
- **合并方式只用 merge commit**：禁 squash、禁 rebase merge，保留每个提交的历史。
  合并后 `dev` 不删除。
- **不自动合并**：不开启 auto-merge，不在没有验收记录时 merge；未验收的 M2 功能
  只开 **draft PR**，等验收后再转 ready。
- **Git 写操作只由 Lead 执行**：创建/删除分支、打 tag、merge、push 到 `main`、
  修改分支保护、改仓库可见性 —— 都由 `@kkiwkK1` 做。人类开发者可以把日常提交
  push 到 `dev`；AI/子 agent 只写文件、跑本地验证并汇报文件清单（它们不碰分支与 tag）。
- **每轮 `main` 验收合并后同步 `dev`**：`git fetch origin` 然后
  `git merge origin/main`（在 `dev` 上）。禁 `reset --hard`、禁 `rebase`、禁
  `force push` 到任何共享分支。
- 不 amend 已推送的提交，不重写 `dev`，不引入 `.gitattributes` 改行尾规范化。

## 2. 构建、测试、签名（只在这些命令上花时间）

工具链：**MinGW-w64 GCC**（C++20，UCRT + POSIX 线程）。`build.bat` 默认用
`C:\Tools\mingw64`，用 `set TOOLROOT=<路径>`（目录下应有 `bin\g++.exe`）覆盖；
CI 用 MSYS2 **UCRT64**（`C:\msys64\ucrt64`）。所有中间文件都在 `build\` 下，路径
保持相对 —— MinGW 汇编器打不开含非 ASCII 字符的临时路径。

| 目的 | 命令 | 产物 |
|---|---|---|
| 全量构建 | `cmd /c build.bat` | `build\bin\desk.exe`、`build\bin\srvctl.exe` |
| 只构建框架 SDK | `cmd /c build.bat framework` | `build\sdk\include\widget\*.h`、`build\sdk\lib\libwg.a` |
| 只用 SDK 构建示例 | `cmd /c build.bat example` | `build\bin\minimal-board.exe` |
| 构建并跑回归测试 | `cmd /c build.bat test` | `build\*-test.exe`、`build\preview.bmp`、`build\desk-board-preview.bmp` |
| 串行化构建（并行会话） | `powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test` | 同上；会话级互斥体 `Local\wg-build-lock` |
| 签名（SAC 机器必做） | `powershell -ExecutionPolicy Bypass -File tools\sign-local.ps1` | 重签 `build\bin\*.exe`、`build\*-test.exe` |
| 安装到用户目录 | `powershell -ExecutionPolicy Bypass -File tools\install-desk.ps1` | `%LOCALAPPDATA%\Programs\Desktop Desk\`（不改进程/启动项） |
| 源清单自检 | `powershell -ExecutionPolicy Bypass -File tools\check-sources.ps1` | 打印 `check-sources: ok` 或问题清单 |
| 清理构建垃圾 | `powershell -ExecutionPolicy Bypass -File tools\clean.ps1` | 默认 dry-run；加 `-Artifacts -Force` 才删，保留 `build\qa` |

要点：

- **并行会话不要同时跑 `build.bat`**：它写共享的 `build\obj`。串行入口是
  `tools\build-lock.ps1`。
- `build.bat test` 在运行前用 `tools\sign-local.ps1` 给测试 exe 自签（当前用户证书，
  不上传、不需要管理员）；`desk.exe` / `srvctl.exe` 仍需构建后手动签名，否则
  Windows 11 Smart App Control 会拦 `An Application Control policy has blocked this file`。
  **CI 里的签名是 runner 现场的临时自签**，绝不复用、不复制本机证书或 `.pfx`。
- `build.bat test` 最后会跑 `tools\check-sources.ps1`：新增 `.cpp` 没进对应清单会
  以 `DEAD-CPP` 失败。
- 测试数量随分支/PR 变化（`main` 与 `dev` 不同），文档和 CI 都**不写死**测试个数；
  只说「全部测试通过」。
- GUI/验收脚本一律使用隔离实例，不要碰正在运行的 desk：
  `tools\desk-testrun.ps1`（场景 + 截图 + `build\qa\` 证据）、
  `tools\desk-ui-test.ps1`（PS 卡）、`tools\desk-perf.ps1`（性能）、
  `tools\bench-desktop.ps1`（资源采样）。隔离靠 `DESK_TEST_INSTANCE=<后缀>`
  （id/窗口类/互斥体加后缀、不迁自启）与 `DESK_TEST_DISPLAY_FILE`
  （模拟工作区/DPI 的 ASCII 脚本）。

## 3. 目录约定

```text
src\apps\desk\main.cpp     应用入口：创建模块、设置 BoardOptions（build.bat 自动收集本目录 *.cpp）
src\panels\<id>\           卡片模块（build.bat 自动发现 src\panels\*，每个目录名 = 模块 id）
src\data\<domain>\         业务数据源：aimeter / server / ps / assistant（build.bat 里逐个列出 .cpp，不自动发现）
src\widget\                框架：公共头 + Windows 宿主实现（build.bat 的 FW 列表逐个列出 .cpp）
src\cli\                   srvctl（build.bat 自动收集本目录 *.cpp）
tests\                     无窗口测试源码（build.bat 显式构建，不自动发现）
examples\minimal-board\    仅用 SDK 的独立示例（build.bat example）
tools\                     构建/签名/安装/验收/自检脚本
docs\                      文档（索引见 docs\README.md）
.github\                   CI（workflows\windows.yml = Windows regression）、PR 模板、dependabot
archive\                   历史归档，不参与构建（见 archive\README.md）
build\                     生成物（被 .gitignore 忽略，禁止手改）
```

职责分层：`src/apps`、`src/panels`、`src/data` 依赖 `src/widget`，框架**不**反向
依赖它们；`srvctl` 是独立应用，复用 `src/data/server` 与 `src/widget/proc`。

## 4. 写作用域与职责分层

- **只改任务说明给出的路径范围**；开工前列出「预计要碰的文件」，收尾必须汇报
  文件清单与「验证过什么、没验证什么」。范围需要扩大时先停下来问，不要用
  「顺手改了」代替授权。
- 一个任务只做一件事；**提交前必须能编译**。
- 构建入口（`build.bat`、`tools\` 下的构建/签名脚本）、框架源清单、`.github\`
  工作流和 `docs\` 的既有文件各有明确归属：不在自己的范围里就不要改；需要别人
  改时提出请求，由负责方落地。
- 并发协作者不要同时构建；正在构建时不要改 `build.bat` 或框架源清单。

## 5. 不要编辑 `build\` 产物

`build\` 全部是生成物，已被 `.gitignore` 忽略：

- 不要手改 `build\bin\*.exe`、`build\*.exe`、`build\obj\`、`build\sdk\`、
  `build\qa\`、`build\testrun\`；需要重建就 `build.bat`，需要清理就
  `tools\clean.ps1`（默认只列清单）。
- 不要把 `build\` 里的文件复制进 `src\`、`docs\` 或提交进仓库；
  截图/预览只作为 `build\qa\` 证据，正文文档用 `docs\*.png`。
- 不做「补签名」之外的任何二进制手术；签名统一走 `tools\sign-local.ps1`。
- 构建脚本是唯一生成入口：新增 `.cpp` 时必须进入 `build.bat` 的对应清单
  （`FW` 列表 / data 列表 / tests），否则 `tools\check-sources.ps1` 会报
  `DEAD-CPP`。

## 6. 新增模块三件套

```bat
rem 1) 生成骨架（自动创建 src\panels\weather\weather.h + weather.cpp）
powershell -ExecutionPolicy Bypass -File tools\new-panel.ps1 -Name weather -Title WEATHER

rem 2) 编辑 src\panels\weather\weather.cpp：
rem    Describe() 写 id/title/configFile/sizePreset/refreshSeconds/hasDetails
rem    LoadData() / RefreshData()（worker，可阻塞）/ BodyHeightOf() / PaintBodyOf()

rem 3) 在 src\apps\desk\main.cpp 注册工厂（一行 include + 一行 push_back）
rem   #include "panels/weather/weather.h"
rem   modules.push_back(CreateWeatherModule());

cmd /c build.bat        rem 编译
cmd /c build.bat test   rem 回归
```

约定：

- 模块只画 body，不建窗口、不画外框；高度只声明**内容高度**，框架加 48px 外框、
  按 8px 向上取整并校验预设范围（`Compact` 256 / `Standard` 340 / `Wide` 568 /
  `Full` = 卡片区宽度）。
- `RefreshData` 只写 staging，`PaintBodyOf` 只读 shown，互不共享可变状态；
  后台线程用 `RequestRepaint()` 请求重绘，不要直接碰 Canvas/窗口。
- 新模块自动出现在 **Add module** 菜单；位置/展开状态由板持久化。
- 新增数据源要放到 `src\data\<domain>\`，并进入 `build.bat` 的显式列表；
  新增框架单元要同时更新 `FW` 列表。
- 行为/配置/状态文件变化时，同步更新 `docs\desk.md`（或对应文档）与
  `docs\architecture.md`。

## 7. 文档规则

- 文档用中文；链接用相对路径（如 `desk.md`、`../README.md`），不写本机绝对路径。
- 改动构建目标、状态文件、线程/刷新模型、模块契约时，必须更新
  `docs\architecture.md`；新增/移动文档时更新 `docs\README.md` 索引。
- `docs\assistant-prd.md`、`docs\assistant-plan.md` 是冻结的需求/计划快照，
  只读不改；与代码冲突时以代码 + `docs\architecture.md` 为准，并在
  `docs\refactor-notes.md` 记录差异。
- 不手改归档文件（`archive\` 只增不改）；不重排既有 README 截图/设计稿。

## 8. 凭据、隐私与离线测试

- 提交前扫描密钥：`sk-`、`Bearer`、`PRIVATE KEY`、`.credentials`、`*.pem`、
  `*.pfx`、`*.session`。凭据只存 Windows 凭据管理器（或 DSH 凭据库），
  不落明文文件、不进日志、不进提交。
- 永不提交：用户态 `*.ini`（`aimeter.ini` / `desk.ini` / `ps.ini` / `server.ini` /
  `assistant*.ini` 等）、`*.jsonl`（会话/记忆）、`trust.json*`、`.env*`、证书/私钥、
  `build\` 生成物。`.gitignore` 已覆盖这些名字，但**忽略规则不是安全边界**：
  提交前仍要自己看一眼 `git status`。
- 自动化测试必须**全离线**：不 live-probe 第三方接口、不连真实 Pi/SSH、不读取用户
  数据目录、不使用真实凭据；需要外部服务的用例要么用 fake/local，要么默认跳过。
- 不改仓库的可见性/权限设置，不新增超出需要的权限（CI 只读）。

## 9. 提交、CI 与汇报

- 提交粒度单一，提交信息说清「为什么」；不夹带无关格式化。
- 每个 PR 的 required check 是 **Windows regression**（`.github/workflows/windows.yml`，
  job `regression` 的 `name`）。本地等价验证：
  `powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test`。
- CI 绿 ≠ 已验收：`main` 的合并还需要 `@kkiwkK1` 在 PR 里写明「验收通过 + 日期 +
  commit SHA」（PR 模板里有对应小节）。
- 向 Lead 报告：task id / 改了哪些文件 / 关键结论 / 未决问题 / 是否跑过构建或测试。
  不要只贴命令输出，说明「验证了什么、没验证什么」。
