# 归档：旧桌面组件（2026-11-05）

这些是 `desk.exe` 洞洞板之前的独立桌面组件和实验。它们已经**停止运行、不再构建**，
只作为参考保留。活跃产物只有：

- `build\bin\desk.exe`：洞洞板 + Account / Servers 卡片
- `build\bin\srvctl.exe`：命令行 SSH 工具

## 内容

| 路径 | 内容 |
|---|---|
| `archive/standalone/aimeter/` | 旧 AiMeter widget（`aimeter.cpp` + Details 弹窗） |
| `archive/standalone/server/` | 旧 Server Status widget（`server.cpp` + Details 弹窗） |
| `archive/standalone/sshpad/` | 旧 SSH Pad（组件内嵌终端） |
| `archive/standalone/clock/` | 旧 Clock 示例 |
| `archive/framework/host/` | 旧 `Widget` / `Host` 框架（`host.*`、`widget.h`） |
| `archive/experiments/ai-mini/` | AI Mini 实验工程 |
| `archive/legacy/app.cpp` | 更早的单文件实现 |
| `archive/tests/server_status.cpp` | 旧 Server Status 的详情/渲染测试 |
| `archive/tools/new-widget.ps1` | 旧 standalone widget 脚手架 |
| `archive/docs/sshpad.md`、`sshpad.png`、`screenshot.png`、`server*.png`、`design/aimeter-*.png` | 旧组件文档与截图 |
| `archive/bin/` | 旧 exe（`aimeter.exe`、`server.exe`、`sshpad.exe`、`clock.exe`、legacy aimeter） |
| `archive/data/aimeter-legacy.ini` | 根目录旧 `aimeter.ini` 的归档副本 |

## 为什么归档

- 旧组件各是一个进程/窗口/托盘/热键，和新板抢资源；
- 新板把共用数据层抽到了 `src/data/aimeter` 和 `src/data/server`，旧 widget 只剩
  业务外壳；
- 旧 `Widget/Host` 只服务这些独立组件，洞洞板使用 `Module/Board`（见
  `src/widget/module.h`、`src/widget/board.h`）。

## 恢复参考

归档代码不是当前构建的一部分。真要把某个旧组件重新编译，至少需要：

1. 把 `archive/framework/host/host.h`、`host.cpp`、`widget.h` 放回 `src/widget/`；
2. 把对应 `archive/standalone/<name>/` 放回 `src/widgets/<name>/`；
3. 数据源直接引用活跃的 `src/data/{aimeter,server}`，不要再复制一份；
4. 在自定义的构建脚本里加回 `src/widgets/*` 的循环（当前 `build.bat` 只构建 desk/srvctl）。

旧 exe 是二进制归档（`build/` 之外的 `archive/bin/`），不参与构建；需要时直接运行，
但不要和 `desk.exe` 同时跑（共享 `aimeter.ini` / `server.ini`）。

## 归档策略（2026-11-07）

- 三类处置：**keep**（参与构建）、**archive**（不构建、留档可恢复）、**delete**
  （只保留 git 历史，不留在工作区）。
- 新增归档放 `archive/<yyyy-mm>-<topic>/`，必须在本文件"内容"表里登记路径、原因与
  恢复步骤；归档目录**不参与构建**，`build.bat` / `check-sources.ps1` 都不扫描它。
- 移动旧代码用 `git mv`（保留历史），不要复制；删除前先打 `archive/<topic>` tag。
- 不允许把"以后可能用"的旧代码留在 `src/` 里：要么进构建与测试，要么进 `archive/`。
- `archive/bin/`、`archive/data/` 是二进制/数据归档，已被 `.gitignore` 忽略；
  只在本机保留，不作为仓库内容分发。

