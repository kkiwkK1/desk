# M2 CardSpec 实施与验收

> 用户在 M1 部署后授权继续推进。冻结需求见 [assistant-prd.md](assistant-prd.md) D1/D2/D4；
> 本文为现行分阶段实施决策，不修改冻结需求、不把局部交付写成完整 M2。

## 1. M2.0：安全的声明式渲染内核

首条闭环：手写 `cardspec.json` → 严格解析校验 → 单张 CARDSPEC 卡 → 修改后手动刷新，
不重建、不重启即可更新内容；非法或缺失 spec 在卡内显示可读错误，修复后刷新可恢复。

- 应用层实现放 `src/data/cardspec` 与 `src/panels/cardspec`，不污染框架 SDK。
- 一型一卡：固定模块 id/title 为 `cardspec`/`CARDSPEC`，Wide 568，默认隐藏，菜单添加；
  spec 自身标题画在 body 内。手动刷新（refreshSeconds=0），不自动轮询。
- 仅加载当前配置目录的固定文件 `cardspec.json`；不支持 spec 自选输入路径。
- v1 仅 `source={"type":"inline"}`；无网络、任意文件数据源、exec、LLM 调用或生成代码执行。
- M1 仍强制零工具，不改 assistant/memory/engine/RPC 或正在运行的已部署实例。
- M1 未提交改动完整保留；本轮不自动 commit/merge/tag/部署。新增实现独立分区，main/build 仅接线。

## 2. v1 格式（M2.0）

```json
{"version":1,"title":"今日目标","source":{"type":"inline"},"layout":"rows","rows":[{"label":"学习","text":"阅读三十分钟"}]}
```

```json
{"version":1,"title":"目标进度","source":{"type":"inline"},"layout":"bar","rows":[{"label":"学习","value":3,"max":5}]}
```

- 根字段仅 version/title/source/layout/rows，全部必填；source 仅必填 type；所有层拒绝重复/未知字段。
- version 必须数值 1；layout 为 rows 或 bar；source.type 仅 inline。
- rows 布局每项仅 label/text；bar 布局每项仅 label/value/max，字段全部必填。
- 输入最多 32KiB（包含可选文件首 UTF-8 BOM）；有效 UTF-8；title 1..64、label 1..32、text 1..96 个 UTF-16 code unit；
  禁 C0 控制字符和 DEL，禁止换行；行数 1..8。bar 数值有限且 max>0、0<=value<=max。
- 校验失败输出对象不变；错误说明只含字段路径/限制，不回显原始输入。
- 序列化仅产生该格式；数据类型不携带 URL、路径、凭据或代码执行能力。

## 3. 分工与冻结接口

- Lead：`cardspec.h` 接口、main/build 接线、docs、唯一构建授权。
- m2-core：`src/data/cardspec/cardspec.cpp`、`tests/cardspec.cpp`。
- m2-ui：`src/panels/cardspec/*`、`tests/cardspec_render.cpp`。
- verifier：只读审计与授权的串行构建/测试；不操作已安装实例。

冻结 C++ 接口位于 `src/data/cardspec/cardspec.h`：`Layout`、`Row`、`Spec`、
`Validate`、`Parse`、`Serialize`。UI 可在 core 实现完成前按此接口独立开发。

## 4. M2.0 done

1. 两种合法格式渲染，中文/条形比例正确，体高遵守宿主契约。
2. 错误矩阵覆盖尺寸/字段/类型/重复键/UTF-8/控制字符/非有限数/越界与零 IO 权限边界。
3. 固定配置文件载入、缺失/非法状态、手动 Refresh/Commit 更新与恢复、staging/shown 分离有测试。
4. 新核心与渲染测试进入标准构建，原 M1 十九项不得退化；源清单、diffcheck 和完整 test 通过。
5. 先用无窗口渲染与合成目录验证；需要 GUI 时另授权隔离实例，不部署 M2 候选到现网。

## 5. 后续阶段（本轮未实现）

- M2.1：受限只读 HTTP 的代码与离线 24/24 已通过；本机保留地址 DNS 映射使公开探针被拒，
  真实 HTTPS 正向验收未完成。权限与证据见 [m2-http-plan.md](m2-http-plan.md)，不是完整 M2 已完成。
- M2.2：模型生成 spec → 校验 → 预览 → 用户确认/撤销。M1 的模型工具权限保持关闭。
- 多卡/卡片库/运行时注册、任意文件源与 exec 不在 M2.0，另行决策，不借阶段名扩大权限。

## 6. 当前状态

**M2.0 实现与无窗口渲染验收已完成（2026-11-09），完整 M2 尚未完成。**

- 核心：严格扫描器构造现有 `JsonValue`，不改 M1 宽松解析器，以保证重复键/非法 UTF-8 不被吞掉；
  使用现有值类型、转码和序列化。typed Validate/Parse/Serialize 及错误矩阵通过。
- 面板：固定文件 bounded 只读、目录/reparse 拒绝、大小复检失败关闭；缺失/非法可读错误、手动
  Reload、修复恢复与 staging/shown 分离通过。Wide 下两布局 1..8 行可表示；极窄 body 外零像素。
- 最终官方 `tools/build-lock.ps1 -Target test`：21/21 实跑、exit 0，源清单与末尾 `[build] ok` 均齐；
  包含原 M1 十九项与新增 `cardspec-test`、`cardspec-render-test`。日志在会话临时目录
  `m2b3-20261109-165404-build.{out,err}.log`，编译 0 error，仅三条既有未用函数 warning。
- 独立重复：新两项及 memory/pi-client/engine/desk-render 六项均 exit 0；framework/example
  构建均 exit 0；候选已按现有脚本重签，自签链 UnknownError 不等于发布证书链证明。
- 预览：测试自绘 `build/cardspec-preview.bmp`（600×216），Lead 目视本轮中文与 3/5、0/5 条形比例正常；
  不是实际板上 GUI 截图。此前两轮编译/生命周期夹具红不作通过记录；未为通过删除覆盖或改框架。
- 未验证：实际板上 Add module/交互、SAC 候选启动、reparse/权限及读取期间文件变化的可制造故障场景；
  文件大小复检失败分支仅代码审查。数值读写依赖当前默认 C locale，未来改全局 locale 需复核。
- M1 已部署版未触碰；M2 候选**未部署**，没有执行真实 Pi/HTTP 或读取用户业务数据。
  未提交/合并/打 tag。下一阶段先设计受限 HTTP，再做模型生成/预览/确认，不能把本阶段写成全 M2。
