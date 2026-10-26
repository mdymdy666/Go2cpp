# MCP 框架（mcp/）

`mcp/` 是内置到 Go2Cpp 的 MCP（Model Context Protocol，协议版本 2025-06-18）服务端框架：
六层结构（传输 / 校验 / 工具 / 向量 / 检索 / 应用）+ 插件通知 + 多会话 HTTP/SSE +
LLM agent。默认 stdio 运行，`--http <port>` 切换 HTTP/SSE。

> 这是「怎么跑起来」的最短路径。**完整文档**（分层、API、设计、边界、测试数据）见
> [docs/modules/mcp.md](../docs/modules/mcp.md)；架构原文与 mermaid 时序图见
> [mcp/docs/architecture.md](docs/architecture.md)。

## 快速开始

```bash
# 1) 配置构建（在仓库根；默认不构建 MCP）
cmake -S . -B /UserData/build/go2cpp-mcp-integrated -DGO2CPP_WITH_MCP=ON
cmake --build /UserData/build/go2cpp-mcp-integrated -j8

# 2) stdio 模式（唯一客户端）
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"c","version":"0"}}}' \
  | /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server

# 3) HTTP/SSE 模式：POST /mcp、GET /sse、POST /message?sessionId=...
/UserData/build/go2cpp-mcp-integrated/mcp/mcp_server --http 8765

# 4) 可选：加载兼容 mcp-extened 的 .so 插件；索引 resources/ 下的文档
/UserData/build/go2cpp-mcp-integrated/mcp/mcp_server --plugins <plugin_dir> --resources resources
```

命令行选项（`mcp/src/main.cpp:20-45`）：`--config`、`--name`、`--version`、`--workers`、
`--http`、`--plugins`、`--resources`、`--model`、`--base-url`、`--api-key`、
`--no-retrieval`、`--embedding-dim`、`-v`。

服务端提供 **29 个工具**（22 内置 + 3 检索 + 4 监控）、2 个资源（`bacio:///quote`、
`document:///life-choices.txt`）、1 个提示。初始化后可发 `tools/list` 查看。

## 测试

```bash
# 18 个 mcp ctest（需 -DMCP_BUILD_TESTS=ON；集成构建共 32 个）
ctest --test-dir /UserData/build/go2cpp-mcp-integrated -R 'json_rpc|registry|tools|vector|validation|retrieval' -V

# e2e（stdio 要传 resource_dir，否则 document 资源读不到）
cd mcp
python3 tests/test_stdio_e2e.py /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server \
        /UserData/CodexWorkSpace/Go2cpp/mcp/resources
python3 tests/test_layers_e2e.py /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server resources
python3 tests/test_http_e2e.py  /UserData/build/go2cpp-mcp-integrated/mcp/mcp_server
```

## 配置

配置项完整名为 `mcp.*`（INI 的 section 是第一个点之前的部分）：

```ini
[mcp]
llm.model = deepseek-chat
llm.base_url = https://api.deepseek.com/v1
llm.api_key =            ; 建议用 DEEPSEEK_API_KEY / DASHSCOPE_API_KEY 等环境变量
server.name = go2cpp-mcp
server.workers = 1       ; >1 时每请求一个 Fiber
monitor.base_url = http://127.0.0.1:9101
monitor.timeout_ms = 3000
```

优先级：命令行 > INI > 环境变量 > 代码默认值。示例见 `mcp.ini.example`。

## 限制与已知问题

- **无 TLS**：HTTP/SSE 是明文；HTTPS LLM 端点由系统 `curl` 子进程完成，不是纯 C++ 链路。
- **无持久化**：注册表、向量/文档索引、SSE 会话都是进程内内存态，重启即失效；
  SSE 不支持 `Last-Event-ID` 续传，多实例需要粘滞路由。
- **协议版本固定**：`initialize` 只回 `2025-06-18`；不支持流式响应。
- **插件同进程**：`dlopen` 加载、无隔离，插件崩溃会带崩服务；插件只能发
  `notifications/*` 通知，不能伪造请求/响应。
- **监控只支持 HTTP**：`MonitorClient` 的 `base_url` 只接受 `http://`，Manager 无认证；
  部署在不可信网络需前面加反向代理。
- **eBPF 需要 root**：`os_monitor_*` 的 eBPF 采集器在非 root 下降级（MCP 本身不需要 root）。
- **平台**：只在 Linux x86_64 / WSL2 验证；Windows / macOS 未验证。
- **构建**：顶层默认关闭 `go2cpp_hook` 与运行时测试；当前 HEAD 的 `src/os/dir.cpp`
  存在编译错误，会使 `MCP_BUILD_TESTS=ON` 的测试目标无法构建（详见
  [docs/modules/mcp.md](../docs/modules/mcp.md) §6.4）。

## 文档

- [docs/modules/mcp.md](../docs/modules/mcp.md)：现行模块文档（分层/API/设计/边界/测试）；
- [mcp/docs/architecture.md](docs/architecture.md)：架构原文（mermaid 时序、扩展点、依赖图）；
- [docs/INDEX.md](../docs/INDEX.md)：全部文档索引；
- [docs/modules/os.md](../docs/modules/os.md)：`os::Run` 与监控后端。
