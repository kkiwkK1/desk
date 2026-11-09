<!--
  目标分支：main 只在“M1 稳定修复 / 发布验收”时使用；日常开发一律 -> dev。
  未验收的 M2 功能只开 draft PR，不要在 CI 变绿后自行 merge。
  本模板的勾选项无法被 CI 机器验证；“CI 绿”不等于“已验收”。
  单人仓库：required review count = 0（自己不能审自己的 PR），
  人工验收由 @kkiwkK1 在评论里显式记录，见下方“人工验收”一节。
-->

## 摘要

<!-- 一句话说明这个 PR 解决什么。关联 issue / 任务 id： -->

## 目标分支

- [ ] `dev`（日常开发 / M2 候选）
- [ ] `main`（仅稳定 M1 修复或发布验收，必须已有验收授权）

## 变更范围

<!-- 列出改动的文件/模块；越界改动要在这里说明理由。 -->

| 文件 / 目录 | 改了什么 |
|---|---|
|  |  |

## 验证

- 本地：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test`
  - [ ] 通过（`build.bat test` 全绿，含 `tools\check-sources.ps1` 源清单自检）
  - 未跑的原因（如有）：
- CI：required check **Windows regression**（workflow `.github/workflows/windows.yml`）
  - [ ] 本 PR 最新 commit 上为绿色
- GUI / 场景证据（需要时）：`tools\desk-testrun.ps1`、`tools\desk-ui-test.ps1`、
  `tools\desk-perf.ps1`，证据留在 `build\qa\`（不入库、不上传 CI）。

## 人工验收（必填，无法机器验证）

> 单人仓库没有第二个人能 approve，所以 PR 的 review 结论由维护者在评论里记录。
> **没有这段记录不得合并到 `main`。**

- 验收人：@kkiwkK1
- 验收 commit（完整 SHA）：
- 验收日期：
- 验收方式（勾选并补充）：
  - [ ] 本机 `build.bat test` 全绿
  - [ ] 隔离实例 GUI 场景（`tools\desk-testrun.ps1`），场景数 / 检查项 / 失败数：
  - [ ] 其它（说明）：
- 结论：<!-- 通过 / 需修改；通过时请写明“验收通过 + 日期 + SHA” -->

## 风险与回滚

<!-- 行为变化、配置/状态文件迁移、性能影响；如何回滚（revert 哪个 commit）。 -->

## 检查清单

- [ ] 改动只覆盖本 PR 声明的内容，没有夹带无关重构
- [ ] 提交粒度单一，提交信息说明了“为什么”
- [ ] 新增/移动 `.cpp` 已进入 `build.bat` 的对应清单（`FW` / data / tests），
      `tools\check-sources.ps1` 无 `DEAD-CPP` / `UNREF-H`
- [ ] 没有提交 `build\` 生成物、`*.exe`、`*.o`、日志
- [ ] 没有提交凭据：`*.pfx` / `*.pem` / `*.key` / `.env*` / 用户 `*.ini` /
      `*.session*` / `trust.json` / 本机证书；测试全程离线，不 live-probe、
      不连真实 Pi、不使用真实凭据
- [ ] 文档已同步：`docs\architecture.md`（构建目标/状态文件/线程模型/模块契约）、
      `docs\README.md` 索引；冻结文档 `docs\assistant-prd.md`、`docs\assistant-plan.md`
      未被修改
- [ ] 目标分支正确（日常 -> `dev`，`main` 仅验收/发布）
- [ ] 目标 `main` 时已同步：`main` 开启 strict check，合并前用 “Update branch”
      把 `main` 合入本分支（不 rebase）
- [ ] 合并方式确认为 **merge commit**（禁 squash / rebase），合并后 `dev` 不会被删除
- [ ] 已获得 @kkiwkK1 的显式验收授权；未开 auto-merge、未自行合并
