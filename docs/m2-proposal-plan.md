# M2.2 提案、预览、确认与回滚

## 1. 阶段基线

用户已在 GitHub 合并 PR #1（merge `b1d581f`），并在会话中确认“验收完成继续推进”。
验收对象为 `f25d49e` 的 CardSpec 界面/离线交互及单动作一次取数；支持证据为命令行回归和
真实隔离 GUI 74 项。Lead 已在 PR 中补录事后验收说明，未重复合并或改写历史，并将 main
正常 fast-forward 同步到 dev。真实公网 HTTPS 正向仍未验证，不能扩大这次验收结论。

完整 M2.2 的目标仍是：模型生成 → 严格校验 → 独立预览 → 用户明确确认 → 可回滚。
当前先做 **M2.2a 原生候选/文件生命周期核心**，不接模型、不接菜单、不改变当前卡片。
因此完成 a 也不能写成“模型生成/预览确认功能已完成”。后续 UI 接线需明确告知用户写盘
和单槽备份的副作用，每次 Apply/Rollback 都要显式确认，不能由模型或刷新自动触发。

## 2. 冻结接口与范围

新增 `src/data/cardspec/store.h/.cpp`，命名空间 `cardspec::store`：

- `Prepare`：只接受现有 v1 inline，严格 Parse/Serialize，生成不可直接篡改的 Candidate；
  纯内存、不写任何文件。失败保留输出，错误不包含输入载荷。
- `ReadActive` / `ReadBackup`：只读原始字节及完整 SHA-256 Snapshot。缺失与已存在空文件
  不同；≤32KiB，调用方提供可信的绝对本地目录，不接受从 JSON/模型来的路径。
- `Apply`：候选再次校验，核 expectedCurrent；当前若存在也必须为合法 v1 inline。
  已检测冲突在任何 temp/backup 写入前退出。不存在目标使用不覆盖 rename；存在时
  用 ReplaceFileW 同步完成替换与旧字节备份。
- `Rollback`：要求 current 与 backup 双 Snapshot，且两者均为合法 v1，恢复 backup 的**原始字节**，
  保留 BOM/空格；当前内容交换进同一个 backup，不增长历史。不存在 backup 时 NoBackup，
  包括首次创建情形；本轮不提供隐式删文件式撤销、缺失/损坏 active 的修复工具。
- `Outcome` 区分 Ok/Conflict/Raced/ChangedDuringWrite/IoError 和本操作的 None/Replaced/
  Unknown target effect。只有 Ok 算成功，不把“写后发现变化”冒充零改或成功。
- `Hooks.beforePromote` 是仅原生测试可注入的竞态窗口；无配置/模型字段能提供回调。

只允许 3 个固定叶子：`cardspec.json`、`cardspec.json.bak`、`cardspec.json.tmp`。
不写 `cardspec.ini`、授权字段或其它数据源；不联网、不 exec、不动态注册模块，不增加模型工具。
已有普通 temp 返回 Busy（只读/不安全 temp 返回 Refused），绝不截断/清除别人或前次运行的文件。
backup 槽也要求普通且≤32KiB，过大或不安全时拒绝，由用户处理而非自动清理。拒相对/UNC/设备/远程目录、
最终目录 reparse、非普通/reparse/多硬链接叶子；祖先目录由可信宿主提供，这不是恶意同用户沙箱。

## 3. 可保证与不可保证

可保证并测试：

1. 合作调用通过会话命名互斥串行；SHA-256 与逐字节预检，已检测冲突不改目标/backup/temp。
2. 写全/FlushFileBuffers 后才提升 temp；普通已存在目标用同目录 ReplaceFileW，读者不会见半段文档。
3. 替换与备份旧字节同一步发生；新建时禁止覆盖，窗口内目标被外部创建则拒绝。
4. temp/backup/active 的类型、大小与共享/只读失败关闭；错误不回显路径或用户字节。
5. 完成后读回目标与备份，异常/漂移不自动 rollback，以免覆盖另一个写者的新改动。

**不能保证强文件系统 CAS**：非协作者可在预检与替换间改目录项。自家互斥不是系统级事务；
写后检测不等于竞态从未发生。也不保证目录项断电持久性、不防同用户抢 mutex 的拒绝服务。
发生不确定替换状态时如实返回 Unknown 并保留可能的恢复数据，不盲删、不声称旧文件一定未动。
本轮 hook 测试必须主动制造该窗口，证明限制真实存在，不能把预检包装成解决了 TOCTOU。

## 4. 分工与验证

| 角色 | 独占范围 |
|---|---|
| Lead | `store.h`、build.bat 接线、docs、Git |
| core fixer | `src/data/cardspec/store.cpp` |
| tests fixer | `tests/cardspec_store.cpp` |
| reviewer / builder | 只读；唯一官方构建生成物 |

回归矩阵包括：Prepare 零 IO/规范往返/非法 v2；missing/empty/SHA256；Apply/单备份/原字节
回滚；current 与 backup 冲突零写；existing temp；只读/共享占用/目录/reparse/硬链接；
过大/非法当前文档；hook 窗口的 Raced/ChangedDuringWrite 与异常清理；所有错误无路径/载荷。
全部在测试 exe 旁的唯一合成目录，不读用户实际配置。不能获得符号链接权限时明确列 skip，
不能把未执行负例记成通过。

当前状态：原生核心与本轮独立回归通过（2026-11-11），UI/模型仍未接线，不沿用旧 GUI 成功
代替本阶段测试。官方 `build-lock -Target test` 实跑 25 项、exit 0、源清单与 `[build] ok` 齐全。
新增 `cardspec-store-test` 为 921 checks / 0 failed / 4 skips；另 store/network/pi-client/
assistant-engine 四项各独立复跑一次均 exit 0，未因 skip 求绿重试。

本地 skip 的确切范围：`symlink file leaf`、`reparse directory`、`apply symlink active`
（缺符号链接权限），以及 `concurrent read Raced branch`（读写共享保护返回 Busy，未观察到
读中变更）。硬链接及 beforePromote 的确定性新建竞争/写后变化全部实际通过。
读侧刻意不共享写入：外部写句柄可导致 Busy；只有真正观察到 size/lastWriteTime 变化才返回
Raced，不能为命中测试放宽共享。这些 skip 是未覆盖分支，不写成已验证。

最终日志：`build/qa/proposal-b3-build-test.{stdout,stderr}.log` 与同前缀 repeat 日志。
首轮编译字段拼写错误、第二轮测试错误要求 writer 写句柄必成功的失败均不记成功；只修相应
拼写/测试口径，保留产品共享保护后完整新运行通过。当前没有产品 GUI/模型调用或真实用户数据
读写，也未部署。

## 5. 后续 UI 与模型接线

后续先明确“预览不是生效值”的独立状态与显式写盘确认；保持正常卡片 shown/授权 epoch
不会被草稿提前替换。菜单遵守宿主只刷新一次，非网络预览/取消不得借宿主刷新自动发请求。
模型阶段另建受控的完整 JSON 捕获通道，保持 M1 零工具，不从聊天显示行拼接产物、
不让模型写路径/授权/生效文件，也不将当前核心阶段冒称模型生成完成。
