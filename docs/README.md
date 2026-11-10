# 文档索引

| 文档 | 内容 |
|---|---|
| [architecture.md](architecture.md) | 模块地图、数据流、构建目标、状态文件、线程/刷新模型、新增模块步骤 |
| [assistant-prd.md](assistant-prd.md) | 助理化的需求清单：数据可信、通知、pi/记忆、生成卡片、看板（冻结） |
| [assistant-plan.md](assistant-plan.md) | 实施计划：里程碑、M0a 设计、Git 工作流、验收（冻结） |
| [assistant-m1-plan.md](assistant-m1-plan.md) | M1 计划：pi RPC、长期记忆、召唤、数据文件、验收（现行） |
| [agent-team-handoff.md](agent-team-handoff.md) | M1 接管基线、CommandCode 成员分工、当前任务与验收缺口 |
| [m1-acceptance.md](m1-acceptance.md) | 完整 M1 验收矩阵、自动/实际 Pi/隔离 GUI 证据、部署及边界 |
| [m2-cardspec-plan.md](m2-cardspec-plan.md) | M2.0 CardSpec 严格格式、内联数据卡、分工与阶段验收 |
| [m2-http-plan.md](m2-http-plan.md) | M2.1 用户精确授权、受限 HTTPS、v2 绑定与阶段验收 |
| [m2-gui-acceptance.md](m2-gui-acceptance.md) | M2 实际隔离 GUI、固定合成 transport、安全边界与证据 |
| [refactor-notes.md](refactor-notes.md) | keep/archive/delete 审计、`board.cpp` 拆分计划、技术债 |
| [desk.md](desk.md) | 洞洞板/模块/视觉/配置完整说明 |
| [framework.md](framework.md) | widget 框架契约、尺寸预设、线程约定 |
| [server.md](server.md) | Servers 卡片、共享 SSH 探测、排错 |
| [cli.md](cli.md) | `srvctl` 命令、JSON schema、退出码 |

其他位置：

- 根目录 [AGENTS.md](../AGENTS.md)：构建/测试/签名命令与协作约定；
- 根目录 [README.md](../README.md)：快速开始与项目地图；
- [archive/README.md](../archive/README.md)：旧组件归档清单、归档策略与恢复步骤。

文档改动规则：改构建目标、状态文件、线程/刷新模型、模块契约时更新
`architecture.md`；新增/移动文档时更新本索引；`assistant-prd.md` 与
`assistant-plan.md` 是冻结快照，只读不改。
