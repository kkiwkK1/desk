# M2.1：受限只读 HTTP 数据源

> 用户授权继续推进；M2.0 见 [m2-cardspec-plan.md](m2-cardspec-plan.md)。
> 本阶段不部署到现用 M1，不开放模型工具、鉴权、任意文件、exec 或模型自动上板。

## 1. 决策与边界

- 不修改 `src/widget/http.*` 或 M1 数据层：现有封装的自动代理/重定向/重试不适合本阶段。
- 新传输只允许精确授权的 HTTPS DNS 主机/443，禁 userinfo、IP 字面量、显式端口、fragment、
  控制符/空白/反斜线，URL 最长 2048；只允许已编码 ASCII 目标，拒明显鉴权查询参数。
- 只有用户配置的精确 URL，spec 只引用配置档 id，不能自行给 URL 或授网络权限。
- 只读 GET、无请求体/自定义 header/引用密钥；固定 Accept/User-Agent/identity 编码。
  NO_PROXY、禁重定向/Cookie/自动鉴权，保持 TLS 证书校验，不提供客户端证书，不重试。
- 仅 IPv4：解析结果全部必须为公网地址，私网/loopback/link-local/CGNAT/保留/文档/组播地址拒绝。
  每请求独立 session，HTTP/1.x（显式不启用 H2/H3），禁全局连接池，解析目标用 WinHTTP option 165 固定到所选公网 IP；
  body 前再核对实际 peer。不支持所需安全选项即失败关闭，不退回不固定的请求。
- 网络逻辑预算 10000ms（DNS 子预算 2000ms），每个阻塞阶段更新剩余预算；响应头阶段同时设
  RECEIVE_TIMEOUT(6) 与 RECEIVE_RESPONSE_TIMEOUT(7)，不能遗留默认 90 秒头部超时。
  WinHTTP 到期通知有惰性，实际返回可能延后，事后 wall clock 必须拒收超预算结果；
  不宣称严格硬实时 10 秒返回。响应最多 32KiB，
  只收 200/application-json/UTF-8（允许 UTF-8 BOM），压缩/超大/重定向都失败而非截断成功。
- 手动刷新、单飞，无新 timer。初次 Load 只读本地 spec/配置，不出网。失败清除旧值显示错误，
  不保留伪装 LIVE 的旧数据；成功显示 HTTP 与 as-of。
- 这是应用级能力边界，不是 OS 网络沙箱。WinHTTP 165 对数字解析目标和 SNI 行为仍需当前系统
  真实正向探针；peer 复核是额外防线，不能声称形式化防重绑定证明。

## 2. v1 兼容与 v2 schema

现有 `cardspec::Parse/Validate/Serialize` 完整保留 v1 inline-only 语义与错误矩阵。
新 `ParseSourceDocument` 支持 v1（委托原入口）及 HTTP-only v2，所有对象仍是闭集，重复键拒绝。

```json
{"version":2,"title":"公开仓库","source":{"type":"http","profile":"demo"},"layout":"rows","rows":[{"label":"名称","text_path":"/full_name"},{"label":"星标","text_path":"/stargazers_count"}]}
```

```json
{"version":2,"title":"进度","source":{"type":"http","profile":"demo"},"layout":"bar","rows":[{"label":"任务","value_path":"/done","max_path":"/total"}]}
```

- 根仍为 version/title/source/layout/rows。v2 source 仅必填 type=http、profile（ASCII slug 1..32）。
- rows 项仅 label/text_path；bar 项仅 label/value_path 与 max 或 max_path（两者恰有一个）。
- 路径仅 RFC 6901 JSON Pointer：最多 256 字节/8 段，字面对象键与规范十进制数组下标，
  不支持 wildcard、JSONPath、脚本、regex 或 LLM 抽取；空路径选择根。
- 文本行可显示字符串/有限数值/布尔，拒 null/数组/对象；bar 严格 JSON 数值，不把字符串转数值。
  绑定后再走 v1 Validate，沿用标题/行数/单行文本/控制字符/进度范围限制。
- 32KiB 响应通过同一严格 JSON 扫描器：不改 M1 原解析器，不吞重复键，不回显响应原文/路径 token。

## 3. 单端点用户授权

`cardspec.ini` 是模块 Store（UTF-16LE，单 BOM；板布局仍在 `desk.ini`），新增四个独立于 spec 的键：

```text
http.profile=demo
http.url=https://api.github.com/repos/microsoft/Windows-classic-samples
http.approved_profile=
http.approved_url=
```

前两键由用户在配置文件中设置，重启候选以加载；URL 必须是公开、无密钥端点。
后两键由卡片菜单“批准当前 HTTPS 端点”确认后保存；精确绑定配置档与规范化完整 URL。
用户也可手工管理授权，但模型生成器无权写这些键。修改 URL/配置档使旧授权失效。

- 菜单确认显示从用户配置解析的精确 URL 与无代理/无鉴权限制，取消零修改/零请求。
- 保存批准失败不得内存放行；撤销失败仍在本进程禁用出网并显示错误。
- 已获准进入在飞的请求不保证能在发包前撤回，但返回结果要因权限 epoch 变化被丢弃，后续请求禁止。
  epoch 不仅检查 transport/decode，也随 ViewState 盖捕获戳，在测量/Status/Paint 实时核对，
  防止 Refresh 完成后、Commit 前撤销仍闪现旧数据；重新批准也不能复活旧结果，必须重新取数。
- 批准成功由宿主菜单路径触发一次刷新，每次只发一条 GET。CardSpec 的 `OnMenu` 不再重复
  调用 `RequestRefresh()`，避免与宿主刷新叠加成 `again` 两次取数；异步外部事件仍可使用该接口。
  没有批准或初次 Load 时 fake fetch 计数必须为 0。

## 4. 独占范围与冻结接口

Lead 写并冻结 `strict_json.h`、`http_source.h`、`http.h` 与公共工厂选项；main 不变。

| lane | 写作用域 | 交付 |
|---|---|---|
| source-core | `cardspec.cpp`、`http_source.cpp`、`tests/cardspec_source.cpp` | 严格通用 JSON facade、v2 schema/Pointer/绑定，v1 不退化 |
| network | `http_policy.cpp`、`http_winhttp.cpp`、`tests/cardspec_http.cpp` | 授权/URL/公网策略、单请求传输、fake 依赖矩阵与可选固定公开探针 |
| UI | `src/panels/cardspec/*`、`tests/cardspec_network.cpp` | Load 零出网/Refresh 授权/确认撤销/epoch/as-of；不改旧渲染测试除必要 API 兼容 |
| Lead | headers、build、docs/AGENTS | 接线/唯一构建授权/验收 |

不并发改同文件。新增三测试进入官方 test，预期 24 项（原 21 不删除）；作者不自行构建。

## 5. done 与验证顺序

1. 纯 v2/Pointer/绑定矩阵；v1 的 21 项旧测试不退化。
2. fake resolver/exchange：无批准零调用、地址混合/保留拒绝、超时/不支持 pin/peer 不符、
   非 200/重定向/错误类型/压缩/超限/坏 JSON，无网络测试不能冒充真实 TLS。
3. UI 合成 Store + fake fetch/confirm：取消/写失败/撤销/epoch/无缓存假 fresh，全部无窗口。
4. 独立静态审计 + 唯一串行官方 24 项/关键重复/SDK/example。
5. 另明确授权一次固定公开 GitHub endpoint 探针（不带任何用户信息/凭据、不读用户配置），
   验证当前 Win11 上 pin/TLS/peer/JSON 绑定。默认 test 不出网。实际板上 GUI 与部署另行授权。

## 6. 当前状态

**M2.1 实现与离线验收通过；真实 HTTPS 正向验收尚未完成（2026-11-09）。**

- 官方新 `test` 已实跑 24/24、exit 0，源清单与 `[build] ok` 齐全；保留原 21 项，新增
  source/http/network。日志在会话临时目录 `m21b4-20261109-183805-build.{out,err}.log`。
- 七项独立重复、framework/example、候选签名均 exit 0。HTTP fake 矩阵实际运行 517 检查，
  fake-resolve=73、fake-exchange=50；默认测试没有真实 DNS/HTTP/模型/凭据访问。
- 独立审查定位并复核了 DNS hEvent/输入 lifetime、响应头 InsufficientBuffer 契约、安全 option
  request-scope、分阶段 timeout 与 Content-Length 完整性；UI publicationEpoch 对测量/Status/Paint
  实时重判，覆盖 Refresh→Revoke→Commit 与重新批准不复活旧值。测试引号/状态前置错误均修夹具，
  不放宽生产 schema 或权限。
- 唯一授权公开探针 `cardspec-http-test.exe --live-probe`（固定 GitHub 公共仓库元数据、无凭据）
  实际 exit 1、约 60ms、failure=AddressBlocked、HTTP status=0；日志
  `m21live-20261109-184403.{out,err}.log` 位于会话临时目录。没有重试或降级请求。
- 另一次仅解析诊断确认该域在本机返回 198.18.0.0/15 保留基准测试段（常见 Fake-IP 映射）；
  因公网过滤，未进入 HTTPS 请求。未修改 DNS/代理/全局网络配置，也未放开该地址段。
- 因此仅获得“DNS/保留地址被安全拒绝”的实际证据，**没有**真实 TLS/SNI/165 pin/peer/
  响应头/JSON 正向链路证据，不将离线绿或 fail-closed 当作公网功能已验收。
- 实际板上 GUI/确认对话框已在新显式隔离模式中通过 74 项（固定 native fake，非真实 HTTPS），
  含菜单单取数修复及真实复制候选启动；证据见 [m2-gui-acceptance.md](m2-gui-acceptance.md)。
  未部署/合并/tag，现用 M1 不变；候选在 dev/草稿 PR，仍需用户对整个 M2 明确验收。
  后续需在正常公网 DNS/直连环境复验，或另行设计用户显式授权的代理能力；不能自动用代理兜底。

## 7. 当前网络裁决与独立 GUI 验收

当前继续采用现有公网直连路径：198.18/15 仍拒绝，不新增 `pin_ip`、代理或 DoH。
这不阻塞离线/实际隔离 GUI 验收，但阻塞“真实公网 HTTPS 功能已验收”的结论。
GUI 使用显式隔离模式及固定 native fake，见 [m2-gui-acceptance.md](m2-gui-acceptance.md)。

未来若需要适配网络环境，应先明确授权变化，而非静默兜底：

- 可信配置中手工指定公网 IP 会扩展授权元组与撤销 epoch，需单独设计/授权；并非本轮能力。
- 显式 proxy/Fake-IP 模式改变 socket peer 的含义，不能沿用“真实目标公网已核验”的表述。
- DoH 引入额外第三方请求与信任关系，不能凭已有端点授权默认启用。

应用的无显式代理/TLS/peer 检查也不是 OS 沙箱或物理链路拓扑证明。
