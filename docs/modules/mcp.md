# 模块文档：MCP 框架（mcp/）

> **适用 HEAD**：`1b46de8`（2026-10-26，「MCP 框架 + os 多进程/IPC + eBPF 监控内置」）。
> **构建**：`/UserData/build/go2cpp-mcp-integrated`（`-DGO2CPP_WITH_MCP=ON`，C++17，Linux/WSL2 x86_64）。
> **状态**：现行。本文是 `mcp/` 的集中文档：分层、API、设计、边界、测试。
>
> **合并说明**：本文合并 `mcp/docs/architecture.md`（964 行，2026-10-25）的六层架构、
> API 参考、时序与限制章节；旧文顶部已标注合并去向，**正文保留未删**（历史细节与
> mermaid 时序图仍以旧文为准）。本文补充了旧文缺少的 `file:line` 引用、当前工具清单
> 实测、以及 2026-10-26 的 e2e 复跑结果。

---

## 1. 概述

`mcp/` 是一个内置到 Go2Cpp 的 **MCP（Model Context Protocol，协议版本 2025-06-18）
服务端框架**，同时提供一个可独立运行的 agent 循环。它把「协议 / 传输 / 校验 / 工具 /
向量 / 检索 / 应用」拆成互不反向依赖的层，默认以 stdio 运行，`--http <port>` 时切换为
HTTP/SSE 多会话模式。

- 命名空间：所有公开 API 位于 `go2cpp::mcp`（`app/` 为 `go2cpp::mcp::app`，`tools/` 为
  `go2cpp::mcp::tools`，余同）；插件 ABI 是 `extern "C"`，不在任何 C++ 命名空间内
  （`mcp/include/mcp/plugin/plugin_api.h:86-101`）。
- 无第三方 JSON 依赖之外的库：`nlohmann/json` 在 `mcp/third_party/` 内置。
- 链接：`mcp_core` 静态库，`PUBLIC go2cpp_runtime`（INTERFACE 目标），私有链接 `dl`
  （`mcp/CMakeLists.txt:7-61`）。
- 运行时依赖只通过 `go2cpp/...` 头文件取用：scheduler/fiber、channel、future、
  fiber_local、http/socket、os::proc、config（依赖图见 `mcp/docs/architecture.md:870-940`）。

### 1.1 最小运行

```bash
# 构建（集成构建目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /UserData/build/go2cpp-mcp-integrated \
      -DGO2CPP_WITH_MCP=ON
cmake --build /UserData/build/go2cpp-mcp-integrated -j8

# stdio（唯一客户端）
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"c","version":"0"}}}' \
  | /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server

# HTTP/SSE：POST /mcp、GET /sse、POST /message?sessionId=...
/UserData/build/go2cpp-mcp-integrated/mcp/mcp_server --http 8765

# 加载兼容 mcp-extened 的 .so 插件；--resources 指定文档索引目录
/UserData/build/go2cpp-mcp-integrated/mcp/mcp_server --plugins <dir> --resources <dir>
```

---

## 2. 分层与目录

| 层 | 公开头文件 | 实现 | 职责 | 不负责 |
| --- | --- | --- | --- | --- |
| 应用层 `app/` | `mcp/include/mcp/app/application.hpp` | `src/app/application.cpp`、`src/main.cpp` | `Builder`/`Application` 装配、配置合并、`RunServer`/`RunAgent` | 协议细节、工具实现 |
| 检索层 `retrieval/` | `retrieval/tool_retriever.hpp`、`retrieval/document_store.hpp` | `src/retrieval/*.cpp` | 工具 RAG 选择、文档分块与检索、注册表事件增量索引 | 调用 LLM、协议渲染 |
| 向量层 `vector/` | `vector/embedding.hpp`、`vector/vector_index.hpp` | `src/vector/*.cpp` | 文本编码（Hash/HTTP）、余弦 top-k、LRU 缓存、持久化 | 工具语义、MCP 协议 |
| 工具层 `tools/` + `server/registry` | `tools/builtin_tools.hpp`、`tools/tool_builder.hpp`、`server/registry.hpp` | `src/tools/*.cpp`、`src/server/registry.cpp` | 工具/提示/资源定义与注册、内置实现、注册表事件 | 选工具、向量化 |
| 校验层 `validation/` | `validation/schema.hpp`、`validation/validators.hpp` | `src/validation/*.cpp` | JSON Schema 子集、参数/URI/工具定义校验 | 执行工具、IO |
| 传输层 `transport/` + `net/` | `transport/transport.hpp`、`mcp/net/http_post.hpp` | `src/transport/*.cpp`、`src/net/http_post.cpp` | 消息边界、stdio、HTTP/SSE 多会话、一次性 POST | 解析 JSON-RPC、知道工具 |

辅助包：`protocol/`（JSON-RPC 与领域类型，全层可用）、`server/`（命令分发）、
`plugin/`（C ABI 加载与通知）、`config/`（`go2cpp::config` 接线）、`util/`（进程执行）。
`mcp/tests/` 是测试与 e2e 脚本，`mcp/examples/` 是 agent/服务端示例。

依赖方向（只允许向下）：
`app → retrieval → vector`、`app → tools → validation`、`app → transport`、`app → protocol`。
禁止反向依赖：传输层不认识工具，向量层不认识 MCP，校验层只接收 JSON。

---

## 3. API 一览

### 3.1 协议层（`protocol/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `enum class RpcError` | `json_rpc.hpp:40` | `-32700`/`-32600`/`-32601`/`-32602`/`-32603`/`-32000`/`-32001` |
| `struct Request` | `json_rpc.hpp:56` | `id`/`method`/`params`/`notification`（原始帧无 id） |
| `struct ParseOutcome` | `json_rpc.hpp:70` | 解析成功/失败（失败带 error 帧） |
| `ParseRequest` / `MakeResult` / `MakeError` / `MakeNotification` | `json_rpc.hpp:88-102` | 帧构造 |
| `Content` / `ToolResult` / `ToolContext` | `types.hpp:29`/`:42`/`:76` | 内容块、`isError` 结果、工具上下文（参数/meta/notify/progress） |
| `Tool` / `Prompt` / `Resource` | `types.hpp:89`/`:120`/`:139` | 注册表领域对象；`ToJson()` 对齐 MCP 字段 |

### 3.2 传输层（`transport/`、`net/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `class ITransport` | `transport.hpp:30` | `Start`/`Stop`/`ReadMessage`/`WriteMessage`/`Name`；只负责消息边界 |
| `StdioTransport` | `src/transport/stdio_transport.hpp` | stdin 按行读、stdout 写一行；写加锁 |
| `HttpTransport` | `src/transport/http_transport.hpp` | `Start(port&, error)`/`Run`/`Stop`/`BroadcastNotification`；连接级 Fiber |
| `HttpTransport::Options` | `src/transport/http_transport.hpp:58-70` | host/port（0=内核分配）/workers/心跳/轮询/空闲超时/会话队列容量 |
| `HttpPostJson` | `mcp/include/mcp/net/http_post.hpp` | 一次性 POST；`http://` 走 Go2Cpp，`https://` 走 curl 子进程 |

HTTP 路由：`POST /mcp`（无会话请求-响应，`http_transport.cpp:355`）、`GET /sse`
（分配 sessionId 并推 `endpoint`，`:350`）、`POST /message?sessionId=...`
（`/messages` 兼容别名，`:360`、`:393`）。

### 3.3 校验层（`validation/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct Issue` / `struct Result` | `schema.hpp:39`/`:45` | 问题路径 + 消息、`Summary()` |
| `Validate(schema, value, root_path)` | `schema.hpp:59` | 子集校验器 |
| `ValidateToolArguments` | `validators.hpp:22` | `tools/call` 前按 `inputSchema` 校验 |
| `ValidateToolDefinition` | `validators.hpp:29` | 注册时校验工具 schema 本身 |
| `ValidateResourceUri` | `validators.hpp:36` | `resources/read` 的 URI 校验 |

支持的关键字：`type/enum/const/required/properties/additionalProperties/items/
minimum/maximum/minLength/maxLength/pattern/minItems/maxItems/anyOf/oneOf`
（`schema.hpp:8-22`）。`$ref/allOf/format` 等**忽略而不报错**。

### 3.4 工具层与注册表（`tools/`、`server/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `class ToolBuilder` | `tool_builder.hpp:32` | 链式 `AddTool`/`AddPrompt`/`AddResource`，样板收进门面 |
| `class Registry` | `registry.hpp:52` | 工具/提示/资源容器；同名覆盖；全部公开方法加锁 |
| `ToolChangeKind` / `ToolChangeListener` | `registry.hpp:34`/`:45` | `kAdded`/`kUpdated`/`kRemoved`，锁外回调 |
| `Registry::RegisterTool` / `UnregisterTool` | `registry.cpp:71`/`:99` | 注册/覆盖/注销并通知监听器 |
| `Registry::AddToolListener` / `RemoveToolListener` | `registry.cpp:120`/`:127` | 监听器 id；回调可再次注册（快照后锁外执行，`:137`） |
| `Registry::InvokeTool` | `registry.cpp:186` | 调用工具；不存在返回 nullopt |
| `Registry::ToolList` | `registry.cpp:146` | `tools/list` 的 result |
| `RegisterBuiltinTools` | `src/tools/builtin_tools.cpp:10` | 注册 22 个内置工具 + 提示 + 资源 |
| `RegisterRetrievalTools` | `src/tools/retrieval_tools.cpp:68` | 注册 `search_tools`/`knowledge_search`/`server_stats`（3 个） |
| `RegisterMonitorTools` | `src/tools/monitor_tools.cpp:264`、`:320` | 注册 4 个 `os_monitor_*` 工具（客户端注入/配置重载） |

### 3.5 向量层（`vector/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `class IEmbeddingProvider` | `embedding.hpp:36` | `Name`/`Dimension`/`Embed`/`EmbedBatch`（默认逐个） |
| `HashEmbeddingProvider` | `embedding.hpp:69` | FNV-1a n-gram，离线、确定性，默认 512 维 |
| `HttpEmbeddingProvider` | `embedding.hpp:92` | OpenAI 兼容 `/embeddings` |
| `class EmbeddingCache` | `embedding.hpp:128` | 线程安全 LRU（默认 256），命中/未命中统计 |
| `class VectorIndex` | `vector_index.hpp:42` | `Add/Remove/Contains/Get/Search/Save/Load`；`shared_mutex` |
| `VectorIndex::Search` | `vector_index.hpp:65` | 暴力余弦 top-k，跳过维度不一致条目 |

### 3.6 检索层（`retrieval/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `class ToolRetriever` | `tool_retriever.hpp:69` | 构造时订阅注册表（`subscribe=true`），析构退订 |
| `ToolRetriever::ContextFor` | `tool_retriever.hpp:98`、`tool_retriever.cpp:226` | 返回 OpenAI tools 形态的 top-k 工具 |
| `ToolRetriever::OnToolChanged` | `tool_retriever.cpp:171` | 增量 upsert/erase；文本未变复用向量 |
| `class DocumentStore` | `document_store.hpp:50` | 文档按 uri 整体替换、块级向量复用、`AddDirectory` |
| `DocumentStore::Search` | `document_store.hpp:77` | 文档块 top-k |

锁顺序固定：`Registry::m_mutex → ToolRetriever::m_mutex → VectorIndex::m_mutex`
（`mcp/docs/architecture.md:182-183`）。embedding 在锁外计算，注册与检索可并发。

### 3.7 服务端与应用层（`server/`、`app/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct ServerOptions` | `server.hpp:54` | name/version/protocol_version/verbose/workers |
| `class Server` | `server.hpp:72` | 持有 `Registry&`（不拥有）；`Serve`/`Stop`/`HandleMessage`/`SendNotification`/`SendBroadcastNotification`/`Set*Sink` |
| `Server::Serve` | `server.cpp:36` | 读-处理-写循环；`workers>1` 时每请求一个 Fiber |
| `Server::HandleMessage` | `server.cpp:114` | 纯函数：解析+分发+编码，不碰 IO（单测入口） |
| `Server::Dispatch` | `server.cpp:202` | 方法路由（`initialize`/`tools/*`/`prompts/*`/`resources/*`/`logging/*`/`completion/*`/`roots/*`） |
| `Server::HandleToolsCall` | `server.cpp:266` | 校验 → `InvokeTool` → `ToolResult`；异常转 `isError=true` |
| `Server::HandleResourcesRead` | `server.cpp:341` | URI 校验 → 读取；未知资源 `-32000` |
| `struct AppOptions` | `application.hpp:40` | server/llm/monitor 设置、config/plugins/resources、http_port、检索参数 |
| `class Application` | `application.hpp:62` | `Create`/`RunServer`/`RunAgent`/`RegisterTool`/`UnregisterTool` |
| `Application::Create` | `application.cpp:48` | 构造向量/检索/文档/工具的顺序见 §4.7 |
| `Application::RunServer` | `application.cpp:124` | 插件加载、HTTP/SSE 或 stdio 选择、sink 注册 |
| `class Builder` | `application.hpp:114` | 链式配置 + `Build()`；命令行覆盖 ini |

### 3.8 插件 ABI（`plugin/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `ClientNotificationCallback` | `plugin_api.h:38` | `(pluginName, notification)`；插件→客户端通知 |
| `PluginType` | `plugin_api.h:48` | TOOLS/PROMPTS/RESOURCES |
| `PluginAPI` | `plugin_api.h:86` | `GetName/GetVersion/GetType/Initialize/HandleRequest/Shutdown/GetTool*` + `notifications` |
| `CreatePlugin` / `DestroyPlugin` | `plugin_api.h:103-104` | 导出符号 |
| `class PluginLoader` | `src/plugin/plugin_loader.hpp` | `LoadDirectory`/`LoadFile`/`UnloadAll`；`dlopen` |
| `class NotificationHub` | `src/plugin/plugin_loader.hpp` | `Deliver`/`Detach`/`forwarded`/`dropped`/`rejected` |

ABI 与 `mcp-extened` 逐字段一致，参考项目已编译的 `.so` 可直接加载
（`plugin_api.h:1-12`）。

### 3.9 LLM 与 Agent（`llm/`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct LlmConfig` | `llm_client.hpp:33` | base_url/model/api_key/timeout_ms |
| `class LlmClient` | `llm_client.hpp:61` | 非流式 `POST {base_url}/chat/completions` |
| `LlmClient::Chat` | `llm_client.cpp:50` | 发消息 + tools，解析 `tool_calls` |
| `LlmClient::HttpPost` | `llm_client.cpp:41` | 统一出口：https→curl、http→Go2Cpp |
| `ApiKeyFromEnv` | `llm_client.cpp:37-40` | `DASHSCOPE_*`→`QWEN_*`→`DEEPSEEK_*`→`OPENAI_*` |
| `struct AgentConfig` | `agent.hpp:32` | system_prompt/max_iterations（默认 8）/parallel_tool_calls |
| `class Agent` | `agent.hpp:54` | `Run`/`SetToolRetriever`；多 tool_calls 用 `go()`+Future 并行 |

### 3.10 监控客户端（`tools/monitor_client.hpp`）

| API | 位置 | 说明 |
| --- | --- | --- |
| `MonitorErrorKind` / `MonitorError` | `monitor_client.hpp:31`/`:42` | InvalidEndpoint/Connect/Timeout/Parse/HttpStatus/ResponseFormat |
| `Result<T>` | `monitor_client.hpp:52` | `Ok`/`Fail`，不抛异常 |
| `MonitorQuery` | `monitor_client.hpp:78` | limit（1–256，默认 10）、timeout_ms（100–60000）、base_url |
| `class MonitorClient` | `monitor_client.hpp:92` | `Latest`/`Stats`/`Kernel`/`Health` |
| `class HttpMonitorClient` | `monitor_client.hpp:132` | 只接受 `http://`；地址规范化与错误归类 |
| `MockMonitorClient` | `monitor_client.hpp:165` | 测试替身：可编程数据/错误、调用记录、零网络 |

工具层（`monitor_tools.cpp`）不拼 URL、不 include 网络头文件，只依赖
`MonitorClient`；`Application::Create` 负责注入。

---

## 4. 设计

### 4.1 JSON-RPC 分发

`Serve`（`server.cpp:36`）循环 `ReadMessage` → `HandleMessage` → `WriteMessage`；
`HandleMessage`（`:114`）解析后调用 `Dispatch`（`:202`），用 `RpcException` 统一
把错误路径收敛到一处编码。错误码：

| 场景 | 返回 |
| --- | --- |
| JSON 解析失败 | `-32700` |
| 缺 `name` / 参数类型不符 / schema 不满足 / URI 非法 | `-32602`（message 带 JSON 路径） |
| 未知方法 | `-32601` |
| 未知工具 | `-32001` |
| 工具业务失败（除零、负数开方等） | `result.isError=true`（协议成功，`server.cpp:300-312`） |
| 未知资源 | `-32000` |
| `POST /message` 缺 sessionId | 400 + `-32600` |
| 未知/过期 sessionId | 404 + `-32000` |
| 请求处理期间会话断开 | 404（响应无处投递） |

`tools/call` 的顺序是：取 `name`/`arguments` → 按 `inputSchema` 校验
（`server.cpp:292-300`）→ `InvokeTool` → `ToolResult::ToJson`。校验失败不会进入
工具内部，这是与「旧实现不校验」的关键行为差异。

### 4.2 工具注册与事件驱动的增量索引

```
RegisterTool / UnregisterTool（任意线程）
  │  Registry 锁内 upsert/erase；锁外快照监听器并通知
  ▼
ToolChangeListener(kAdded/kUpdated/kRemoved)
  │  ToolRetriever::OnToolChanged（tool_retriever.cpp:171）
  ▼
AddTool（只编码 1 条向量）｜UpdateTool（文本未变复用向量）｜RemoveTool（erase）
  ▼
VectorIndex::Add/Remove → Retrieve(query, top_k) 返回 id + 余弦分
```

`Application::Create` **先构造检索器再注册内置工具**（`application.cpp:56-68`），因此
29 个工具逐个增量入索引；启动末尾只做一次「索引条目数 == 注册表工具数」自检，
不一致才回退 `Refresh()`（`application.cpp:106`）。注册表事件在锁外投递，监听器内
可以安全地再次注册工具。

### 4.3 校验层

`schema.cpp` 实现表中的关键字；`validators.cpp` 在其上做三件事：
`ValidateToolArguments`（`tools/call` 前）、`ValidateToolDefinition`（注册时）、
`ValidateResourceUri`（`resources/read` 前）。`additionalProperties:false` 让
多余字段直接得到 `-32602`（监控工具 schema 就依赖这一点）。`$ref/allOf/format`
被忽略，避免工具 schema 因未知关键字整体失败（`schema.hpp:20-22`）。

### 4.4 向量检索与文档索引

- 工具：`ToolRetriever` 对「名字 + 描述」编码，`ContextFor` 返回 OpenAI tools 形态的
  top-k（默认 5，min_score 0.05），并**始终追加** `search_tools`/`knowledge_search`
  两个检索工具，避免模型「找不到检索入口」。
- 文档：`DocumentStore::AddDocument` 按 uri 整体替换，按块文本复用旧向量；
  `AddDirectory` 逐文件调用，重复加载同一文件是零向量化开销（`document_store.cpp`）。
- 规模：`VectorIndex` 是 O(n) 暴力余弦，适合几十到几千条；上万条应换 HNSW 等近似
  索引，扩展点保持 `Add/Search/Remove/Get` 不变。

### 4.5 SSE 多会话

```
GET /sse
  ├─ 写 text/event-stream 头
  ├─ 分配 sessionId（进程内递增 + 单调时钟，http_transport.cpp:588）
  ├─ 注册 Session{queue, closed, last_active}
  └─ 推 event: endpoint / data: /message?sessionId=<id>
        │
        ├─ POST /message?sessionId=<id>（/messages 别名）
        │    ├─ 缺 id → 400；未知/过期 → 404（http_transport.cpp:393-406）
        │    ├─ 当前 Fiber 绑定 sessionId（FiberLocal，:412 ScopedSessionBinding）
        │    ├─ HandleMessage：响应 + 工具通知
        │    ├─ 响应 → 本会话 queue（SendFor，5s 背压）
        │    └─ 通知 sink → 按 FiberLocal 定向 TrySend
        │
        └─ SSE 写循环
             ├─ queue → event: message
             ├─ 每 15s 写 : ping
             ├─ 每 500ms 探测 FIN/RST → 清理
             └─ 空闲超时 → 清理
```

两个出口互不混用：`RouteNotification`（`http_transport.cpp:563`）按 FiberLocal 定向；
`BroadcastNotification`（`:574`）发给全部活跃会话。`POST /mcp` 没有会话绑定，其通知
走广播。会话表由 `m_sessions_mutex` 保护，投递时只在锁内取 `shared_ptr`，发送/等待
在锁外完成，避免持锁 park。调参环境变量：`MCP_SSE_HEARTBEAT_MS`、
`MCP_SSE_POLL_MS`、`MCP_SSE_IDLE_TIMEOUT_MS`（`http_transport.cpp:221-227`）。

### 4.6 插件通知广播

```
插件（任意线程）
  └─ PluginAPI::notifications->SendToClient(pluginName, json)   [C ABI]
       ▼
  PluginNotificationTrampoline（宿主注入，plugin_loader.cpp:69）
       ▼
  NotificationHub::Deliver（:168）
       │ 解析 JSON、只放行 notifications/*、1 MiB 上限（:22/:25）
       ├─ 非法/越权/超限 → rejected()++ 并丢弃
       └─ 合法 → Server::SendBroadcastNotification
                  ├─ HTTP/SSE：BroadcastNotification → 所有活跃会话
                  └─ stdio：无广播 sink → 退回写传输（唯一客户端）
```

宿主在 `Initialize()` **之前**注入 `NotificationSystem`（`plugin_loader.cpp:302-305`）；
投递表以插件自报名字为键，同名插件后加载者覆盖前者。卸载顺序：
`Hub::Detach()` → `Shutdown()`（此间通知计入 `dropped()`）→ 删投递表 →
`DestroyPlugin()` → `dlclose()`（`plugin_loader.cpp:386`）。插件只能发通知，不能伪造
请求/响应；无客户端时通知**丢弃不缓存**（`Server::notifications_dropped()`）。

### 4.7 应用装配与配置

```
Builder（链式 + ini + 命令行覆盖）
  └─ Application::Create（application.cpp:48）
       ├─ 向量层：HashEmbeddingProvider（application.cpp:54）
       ├─ 检索层：ToolRetriever（先订阅）+ DocumentStore（resources 索引）
       ├─ 工具层：RegisterBuiltinTools → RegisterRetrievalTools → RegisterMonitorTools
       ├─ 插件：RunServer 内 LoadDirectory（application.cpp:149）
       └─ 校验：Server 在 tools/call 前调用
  ├─ RunServer（:124）：--http >0 → HttpTransport，否则 StdioTransport
  └─ RunAgent（:183）：LLM + 检索子集工具
```

配置项完整名 `mcp.*`（`[mcp]` 段 + 点后键）：`mcp.llm.{model,base_url,api_key,
timeout_ms}`、`mcp.server.{name,version,workers}`、`mcp.monitor.{base_url,timeout_ms}`；
INI 的 section 是完整名第一个点之前的部分（`mcp/config/config.hpp:35-81`、
`mcp/docs/architecture.md:292-320`）。优先级：命令行 > INI > 环境变量 > 代码默认值。

### 4.8 监控工具（`os_monitor_*`）

工具层只依赖 `MonitorClient`（`MonitorQuery` 统一承载 limit/timeout/base_url）；
`HttpMonitorClient` 负责地址规范化、HTTP 状态与 JSON 解析，把失败收敛成
`MonitorError{kind, message, http_status}`。工具把这些错误原样放进
`result.isError=true` 的文本（附 URL 与排查提示），**不**让 server 退出。Manager
未启动时其它工具照常工作，只有监控工具返回连接错误。`os_monitor_health` 的 schema
不接受 `limit`（`additionalProperties:false`，传了在协议层得到 `-32602`）。

---

## 5. 边界与陷阱

### 5.1 没有 TLS（传输层）

HTTP/SSE 是明文 HTTP；`HttpTransport` 不做 TLS 终止。部署在不可信网络时必须前置
反向代理/隧道。LLM 的 `https://` 端点由系统 `curl` 子进程完成 TLS，**不是纯 C++
链路**（`llm_client.hpp:10-18`、`llm_client.cpp:41-47`），这也是 `http_post` 的已知取舍。

### 5.2 没有持久化

- 注册表、向量索引、文档索引都是进程内内存态；重启后需要重新注册/索引。
- SSE 会话是单进程内存态：多实例部署要粘滞路由或共享会话总线；进程重启后旧
  `sessionId` 全部失效，且不支持 `Last-Event-ID` 断点续传。
- 监控 Manager 默认内存环形缓冲 256 条，重启即清空（`MonitorClient` 只是查询方）。
- `VectorIndex::Save/Load` 是显式持久化扩展点（`MCPVEC1` 文本格式），默认不启用。

### 5.3 协议版本固定

`initialize` 只回 `2025-06-18`（`server.cpp:247-264`），没有版本协商矩阵；不支持
流式响应（`LlmClient` 也是非流式）。旧客户端调用 `resources/subscribe`/
`resources/unsubscribe` 得到空对象（`server.cpp:229-233`），不会真正订阅。

### 5.4 插件同进程、无隔离

插件用 `dlopen` 加载到服务进程：崩溃会带崩服务，插件可以访问服务地址空间。
`NotificationHub` 只做「方法前缀 + 1 MiB + JSON 对象」的白名单，不是安全沙箱。
ABI 兼容 `mcp-extened`，参考项目的 8 个插件可直接用。

### 5.5 HTTPS 依赖 curl、eBPF 依赖 root

- `https://` LLM 端点需要系统安装 `curl`；没有 curl 时 `HttpPost` 返回失败。
- `os_monitor_*` 的后端 eBPF 采集器需要 root 与内核 BTF，非 root 降级为 `/proc`
  采集器；`ctest` 的 3 个 `*_root` 用例会 SKIP。MCP 服务本身不需要 root。

### 5.6 构建开关与测试

- `mcp/` 默认不构建，必须 `-DGO2CPP_WITH_MCP=ON`（顶层 `CMakeLists.txt:27-29`）；
  `MCP_BUILD_TESTS` 打开后才注册 `mcp/tests/`。
- 顶层默认关闭 `go2cpp_hook` 与运行时自带测试；MCP 的测试依赖 runtime 的
  `go2cpp_os`，当前 HEAD 的 `src/os/dir.cpp` 编译错误会阻断测试目标（见 §6.4）。
- 平台：只在 Linux x86_64/WSL2 验证；Windows/macOS 未验证。

---

## 6. 测试

### 6.1 ctest 用例（mcp 目录，18 个）

`MCP_BUILD_TESTS=ON` 后 `mcp/tests/CMakeLists.txt` 注册 18 个用例：

| # | 用例 | 覆盖 |
| --- | --- | --- |
| 1 | `json_rpc` | 解析/编码、通知、错误码 |
| 2 | `registry` | 注册/覆盖/注销、监听器、锁外回调 |
| 3 | `tools` | 内置工具与计算结果 |
| 4 | `vector` | 哈希向量、余弦、LRU、Save/Load |
| 5 | `validation` | schema 子集与三类 validator |
| 6 | `retrieval` | 增量注册/覆盖/注销、增量 vs 全量、随机 200 步、并发检索、文档块复用 |
| 7 | `process` | `go2cpp::os::Run` 适配：启动/输出/超时/退出码/并发/泄漏 |
| 8 | `robot_control` | 假 `ros2` 脚本：成功/超时/错误码/命令缺失/白名单 |
| 9 | `plugin_notify` | 假插件 .so：转发/非法帧/工作线程/无客户端/卸载/并发 |
| 10 | `monitor_client` | Mock 客户端注入、四个工具、参数透传、零网络 |
| 11 | `monitor_tools` | 真实 Manager：注册/schema/未启动/真实查询 |
| 12 | `monitor_boundaries` | 500/404/超时/畸形响应/空客户端 |
| 13 | `monitor_capacity` | 256 默认容量、1/0 极端容量、非法 push、4×100 并发 |
| 14 | `monitor_concurrent_tools` | 100 并发工具调用、响应隔离、并发注册/注销 |
| 15 | `monitor_args_fuzz` | limit/timeout 边界矩阵 + 1000 组随机 fuzz |
| 16 | `monitor_e2e` | 真实 Manager + 真实 stdio server 全链路 |
| 17 | `monitor_http_faults` | HTTP 500/404/超时/截断/非 JSON/连接拒绝，断言进程存活 |
| 18 | `docs_consistency` | README/testing 声明的 ctest/工具数与实际一致 |

集成构建（`GO2CPP_WITH_MCP=ON` + `MCP_BUILD_TESTS=ON`）共 **32** 个 ctest：
mcp 18 个 + os monitor 12 个 + bufio 2 个。`docs_consistency` 读的是**仓库根**
`README.md` 与 `docs/testing.md` 的数字；当前 HEAD 的根 README 正被并行文档任务重写，
该用例在数字声明缺失时会失败，属于文档同步问题，不是代码缺陷。

### 6.2 端到端脚本（未注册进 ctest，直接对真实 server 运行）

| 脚本 | 覆盖 | 用法 |
| --- | --- | --- |
| `test_stdio_e2e.py` | 工具/提示/资源、错误路径、ROS 工具、未知方法 | `<server> [resource_dir]` |
| `test_layers_e2e.py` | 检索/校验/配置/文档检索 + 轻量压测 | `<server> [resource_dir]` |
| `test_http_e2e.py` | `/mcp`、`/sse`、3 客户端隔离、通知双出口、404/400、断开重连、10×100 压测、空闲超时、插件广播 | `<server>` |
| `test_plugin_e2e.py` | 参考插件（8 个 .so）的提示/资源加载 | `<server> <plugin_dir>` |
| `plugin_notify_e2e.py` | 插件通知 stdio + HTTP 广播、非法帧拒绝、无会话丢弃 | `<server> <plugin_dir>` |
| `test_random_input.py` / `test_bench.py` / `test_agent_offline.py` / `test_concurrency.py` | 随机帧、LLM 基准、离线 agent、并发 | 各自 `--help` |

### 6.3 本次复跑数据（2026-10-26，旧二进制 + 当前源码）

| 项 | 结果 |
| --- | --- |
| `tools/list` | **29** 个 = 22 内置 + 3 检索 + 4 监控；`resources/list` 2 个、`prompts/list` 1 个 |
| `test_stdio_e2e.py`（带 `mcp/resources`） | **32 项断言，0 失败**；启动日志「已索引 1 个文档文件（201 个片段）」 |
| `test_layers_e2e.py` | **20 项断言，0 失败**；`search_tools` 1000 次 **5829 req/s**，P50 0.156ms、P95 0.274ms、P99 0.317ms |
| `test_http_e2e.py` | 94 项断言，**4 失败**：10×100 压测中 5/10 客户端 `Broken pipe`，只收到 500/1000 响应（约 1722 req/s）；3 客户端隔离、通知定向、插件广播、404/400 均通过；第二次复跑在通知隔离阶段断连中止 |
| 检索压测（历史记录） | 400 条 × 512 维、8 线程约 3.7–5.3 万 qps；单线程 P50 约 0.15ms；增量注册 P50 约 6µs（`mcp/docs/architecture.md:182-183`，`test_retrieval.cpp:576-636` 只打印不设阈值） |
| 工具数自检 | 22+3+4=29 与 `tools/list` 实测一致 |

> HTTP/SSE 压测的失败是**本机旧二进制复跑**的结果（连接被重置），不能据此声称
> 「10×100 已通过」。旧架构文档写的是当时构建的通过记录；当前以获得
> `stdio`/`layers` 全绿与 HTTP 单会话/多会话隔离通过为准，压测结论需在新构建上重跑。

### 6.4 复现命令

```bash
# 构建 MCP（集成目录已有 mcp_core；开启测试需 MCP_BUILD_TESTS=ON）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /UserData/build/go2cpp-mcp-integrated \
      -DGO2CPP_WITH_MCP=ON -DMCP_BUILD_TESTS=ON
cmake --build /UserData/build/go2cpp-mcp-integrated -j8

# 单测
ctest --test-dir /UserData/build/go2cpp-mcp-integrated -R 'json_rpc|registry|tools|vector|validation|retrieval' -V

# e2e（stdio 需要 resource_dir，否则 document 资源读不到）
cd /UserData/CodexWorkSpace/Go2cpp/mcp
python3 tests/test_stdio_e2e.py /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server \
        /UserData/CodexWorkSpace/Go2cpp/mcp/resources
python3 tests/test_layers_e2e.py /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server resources
python3 tests/test_http_e2e.py  /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server
```

**当前构建阻塞（如实记录）**：开启 `MCP_BUILD_TESTS=ON` 后 `cmake --build` 在
`src/os/dir.cpp` 的 7 处 `detail::ContainsNul` 未声明处失败（`dir.cpp:118`/`:196`/
`:267`/`:376`/`:412`/`:432`/`:463`），并有 `libgo2cpp_network` 未定义
`go2cpp::hook::mark_system_nonblocking` 的链接错误；因此 §6.1 的 18 个测试二进制
本次未能生成。`mcp_server` 本身是既有产物，§6.3 的 e2e 数据用它复跑得到。

---

## 7. 旧文档合并与相关文档

| 旧文档 | 处理 | 说明 |
| --- | --- | --- |
| `mcp/docs/architecture.md`（964 行） | 六层、API、时序、限制并入本文 §1–§5；原文保留并在顶部标注合并 | 本文补 `file:line` 与实测；mermaid 时序图、扩展点步骤、依赖图仍看旧文 |
| `mcp/README.md` | **本次新建**（断链修复） | 旧 `architecture.md` 顶部 `../README.md` 断链；新 README 指向本文 |
| `docs/INDEX.md` | 本次追加两行（mcp、os） | 索引唯一入口 |
| `docs/modules.md` | MCP 作为可选子目录（`GO2CPP_WITH_MCP`）的说明 | 未改；模块 target 列表已含 24 项 |

相关文档：

- [MCP 架构原文](../../mcp/docs/architecture.md)：mermaid 时序图、扩展点步骤、依赖图；
- [MCP README](../../mcp/README.md)：构建/运行/限制的最短路径；
- [docs/INDEX.md](../INDEX.md)：全部文档入口；
- [docs/modules.md](../modules.md)：模块目录与 CMake target；
- [docs/modules/os.md](os.md)：`os::Run` 与监控后端；
- [docs/testing.md](../testing.md)：测试约定（MCP 数字以本文为准）。

---

## 8. 变更记录

| 日期 | 变更 |
| --- | --- |
| 2026-10-26 | 首版：合并 `mcp/docs/architecture.md`，补全六层 API 与设计（带 `file:line`）、29 工具实测、stdio/layers 复跑数据；记录 HTTP 压测失败与当前 HEAD 构建阻塞 |
