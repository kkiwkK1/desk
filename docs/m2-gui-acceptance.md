# M2 隔离板上 GUI 验收

## 1. 范围与状态

本轮在正式仓库 `dev` 继续，PR #1 保持草稿，不合并、不部署、不停止现用 M1。
已补齐 CardSpec 的实际 Win32 板上交互证据：2026-11-10 本轮新候选官方回归通过，
第二次完整 GUI 运行 74/74、exit 0；不是沿用无窗口渲染测试或 GitHub CI 代替实际 GUI。
这些自动化证据也不等于用户对整个 M2 的明确人工验收。

网络策略仍采用 [m2-http-plan.md](m2-http-plan.md) 的公网 IPv4 直连边界。198.18/15
保留地址继续拒绝，不新增 IP 覆盖、系统代理、Fake-IP 放行或 DoH。真实 TLS/SNI/
165 固定/peer/响应头正向链路仍是独立待验项，不阻塞本轮离线 GUI 工作。

## 2. 唯一离线测试模式

`DESK_TEST_CARDSPEC_ONLY=1` 是原生测试入口，不是 CardSpec 的字段或网络能力：

- 必须同时有合法 `DESK_TEST_INSTANCE`、显式 `DESK_TEST_CONFIG_DIR`，且配置目录
  通过宿主的绝对路径/存在/可写校验。缺失或无效时返回 2，不建普通板，不弹阻塞对话框。
- 模式只注册 CardSpec；不加载 Account/Servers/PowerShell/Assistant/Memory，不注册
  全局热键/托盘/自启。优先于助理卡测试模式和物理热键探测。
- 固定注入原生 fake fetch，绝不调用真实 DNS/WinHTTP，即使点击批准后刷新也不会出网。
  仍使用产品自己的授权检查、实际 MessageBox 确认、保存、刷新、Commit 与渲染路径。
- `cardspec-test-fetch-count.txt` 在合成配置目录初始化为 0，每次 fake 回调加 1。
  初始化/写入失败关闭；正常应用不写该文件。读取计数应容忍短暂的文件共享/写入窗口。
- fake 成功响应固定如下，不读取任何用户数据或可选数据文件：

```json
{"items":[{"name":"HTTP_FIXTURE_ROW","text":"SYNTHETIC_RESPONSE","value":3,"max":5}]}
```

这是“实际 GUI + 合成 transport”证据，**不是实际 HTTPS 成功**。批准成功由宿主菜单路径
触发一次刷新；取消不请求；已批准配置的初次 Load 仍不请求。CardSpec 不再在菜单内重复请求
刷新，批准/每次手动 Reload 的调用增量必须精确为 1，不能容忍 2 次取数。普通传输不变。

## 3. 脚本与证据要求

入口为 `tools/desk-cardspec-gui-test.ps1`，重用 `desk-testlib.ps1`。候选需先通过
官方串行回归并签名，脚本复制到自己拥有的隔离目录后运行，环境变量只传子进程。

覆盖：

1. 缺 suffix、缺 cfg、无效 cfg 的失败关闭负控制；有效实例的注册/窗口/计数正控制。
2. 实际 Add module 菜单仅含 CardSpec，实际点击上板及可见状态持久化。
3. 中文 rows、内容高度/重排、无定时刷新、实际 Reload；bar 的非零/零条形差异。
4. 非法 JSON、缺文件均清除旧值并显示错误；修复后无需重启即可恢复。
5. HTTP 未批准与取消确认的零 fake 调用；实际批准、一次 fake 成功及 HTTP/as-of；
   手动刷新调用数增长；实际撤销后清授权/旧值、刷新不再调用；重启不自动取数。
6. HKCU Run 不变、自有窗口/子进程清理，不污染父环境和真实用户配置。

截图只允许自有窗口 PrintWindow / 自有画布导出；不截桌面，不触生产窗口或热键。
像素检查针对卡片内容区域，不能用整块大板稀释差异。缺动作、MSAA 失败、白图、
未到达预期状态必须失败或明确 blocked，不能把缺证据记为通过。

结果/JSON/截图只由测试入口生成在 `build/qa/`，不复制到源码或提交。所有成功结论
必须来自当前运行的 exit、每项检查和实际证据，不能拼接失败轮次。

## 4. 本轮分工

| 角色 | 独占范围 | 任务 |
|---|---|---|
| Lead | `src/apps/desk/main.cpp`、docs、Git | 隔离模式/fake、文档、验后 dev 提交 |
| GUI fixer | `tools/desk-cardspec-gui-test.ps1` | 脚本/合成数据/自有窗口/证据 |
| builder | 只读 + 官方 build 产物 | 唯一串行回归与签名，不自行 GUI |
| reviewer | 只读 | 隔离与安全边界、独立审计证据 |

任何范围扩大先报 Lead；不修改 main，不把“继续推进”当人工验收或合并许可。

## 5. 最终运行证据（2026-11-10）

- b3 官方 `test`：所有 24 项实际运行通过，源清单与 `[build] ok`、真实 exit 0；
  CardSpec network/render 独立复跑通过。日志位于会话临时目录 `cardspec-gui-b3/`。
- 签名候选 SHA-256：`D4CB4AF902B9F652113690F2565CA16AA83073E1CBA63F27258670C354A5C339`。
- 真实 GUI 最终完整轮：`build/qa/cardspec-gui/20261110-050330/evidence/gui.json`，
  `name=cardspec-gui`、`pass=true`，74 pass / 0 fail / 0 blocked，真实进程 exit 0，约 45 秒。
  12 张 PNG 全来自这次自有画布，不复用此前失败轮或 static 图。
- 实际 Add 菜单仅 CardSpec、上板、中文 rows/高度重排、无自动刷新、bar 非零/零、
  非法/缺文件清旧值与修复恢复、持久化重启全部到达。
- 两次真实授权对话框均核标题/OK/Cancel/默认 Cancel。批准的 fake 调用 0→1，
  手动 Reload 1→2，重启后 Reload 0→1；取消/初次 Load/未授权/撤销后均为 0 次新增。
  宿主的菜单刷新保留，模块冗余请求已移除；没有以允许 2 次来放宽验收。
- AS OF 专用 footer ROI 正控 ink=136、未批准同 ROI 阴性 ink=0；可见而非最小高度裁切。
  自有进程 TCP 快照为空（仅特定 NoMatching 视为空，其他查询错误仍 blocked）。
- 缺 suffix/缺 cfg/无效 cfg 三次负控均 exit 2 且不建普通窗口；测试实例全部结束，
  HKCU Run/生产配置元数据不变，没有现用进程被结束，没有系统网络设置变更。

首轮 `20261110-045229` exit 1，68 pass/2 fail/1 blocked，**不计成功**：PS5 generic
List 包装导致收尾及 JSON 缺失；单元素数组展开与粗 ROI 误判 AS OF；无 TCP 匹配被误判
为查询不可用。只修脚本的类型/几何/错误分类与异常安全后完整重跑，不改产品图像凑绿。

验收强度：实际 GUI + 原生固定 fake transport；未证明真实 DNS/TLS/SNI/165 pin/peer/
响应头正向成功。M2.1 的网络环境限制仍单列，PR 保持草稿、未合并、候选未部署。
