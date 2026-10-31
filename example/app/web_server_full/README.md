# web_server_full —— 多模块 HTTP/SSE 服务

在 `web_server_simple` 之上补齐一个“像样”的 Web 服务该有的东西：路由参数、
中间件、静态文件、SSE、JSON API、配置、日志、错误处理、健康检查、优雅关闭。

## 功能

1. **路由**：`Router` 支持方法 + 路径段匹配，`:id` 形式的路径参数
   （`GET /api/items/42`）；
2. **中间件**：洋葱模型 `Recover → RequestId → AccessLog → CORS`；
3. **静态文件**：`www/index.html`、`style.css`、`app.js`，带百分号解码、
   `..` 拒绝、根目录前缀校验、MIME 映射与 8 MiB 上限；
4. **SSE**：`GET /events`，每个订阅者一个 `Channel<SseEvent>`（容量 16），
   Fiber 每 1 秒广播 tick，handler 每 500 ms 心跳注释；慢客户端丢事件不拖慢别人；
5. **JSON API**：`/api/echo`、`/api/items/:id`、`/api/stats`、`/api/routes`、
   `/api/time`、`/api/broadcast`（POST，向 SSE 广播）；
6. **配置**：`server.ini` 的 `[server]` / `[logging]`，命令行可覆盖端口与
   `--web-root`；
7. **日志**：分级（debug/info/warn/error）+ stdout + 文件，访问日志含
   方法/路径/状态/耗时与 `X-Request-Id`；
8. **错误处理**：`/api/boom` 故意抛异常，`Recover` 中间件转成 500 JSON；
   未知路径返回 JSON 404；非 GET/HEAD 的静态路径走 404 而不是越权读文件；
9. **优雅关闭**：`SIGINT/SIGTERM` → `SseHub::Stop()` → `HttpServer::Stop()` →
   `Scheduler::shutdown()`，顺序固定，避免在途 Fiber 访问已销毁对象；
10. **健康检查**：`/health`、`/healthz` 返回状态、uptime、累计请求数与版本。

## 文件结构

```
web_server_full/
├── main.cpp          # 参数解析、信号、启动/关闭
├── config.h/.cpp     # INI → Config（先组装副本，全部成功才提交）
├── logger.h/.cpp     # 线程安全日志：stdout + 文件
├── router.h/.cpp     # 方法 + 路径段匹配（:param）
├── middleware.h/.cpp # 中间件链 + Recover/RequestId/AccessLog/CORS
├── static_files.h/.cpp # 静态文件安全读取 + MIME
├── api.h/.cpp        # JSON API 与健康检查
├── sse_hub.h/.cpp    # SSE 订阅表 + 定时 Fiber + Channel 背压
├── app_server.h/.cpp # 组装：配置/路由/中间件/SSE/HttpServer
├── server.ini        # 示例配置
└── www/              # index.html / style.css / app.js
```

## 构建与运行

```sh
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_web_server_full -j8

# 必须在仓库根目录运行（配置与 www 都是相对路径）
./.build-bento-wsl/example/app/go2cpp_web_server_full
# 或指定配置/端口/静态根目录
./.build-bento-wsl/example/app/go2cpp_web_server_full \
    --config example/app/web_server_full/server.ini --port 18081
```

默认监听 `127.0.0.1:8081`。启动日志会打印生效配置与完整路由表。

## 测试

以下命令按顺序执行（仓库根目录、另开终端）：

```sh
# 1) 健康检查 + 请求 ID/耗时头
curl -s -i http://127.0.0.1:8081/health

# 2) 静态页面与 MIME
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' http://127.0.0.1:8081/
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' http://127.0.0.1:8081/style.css

# 3) 路由参数
curl -s http://127.0.0.1:8081/api/items/42
# {"ok":true,"id":"42","name":"item-42"}

# 4) 异常恢复
curl -s -i http://127.0.0.1:8081/api/boom
# HTTP/1.1 500 Internal Server Error + {"ok":false,...}

# 5) JSON 404 与 OPTIONS 预检
curl -s -i http://127.0.0.1:8081/no-such-path
curl -s -i -X OPTIONS http://127.0.0.1:8081/api/echo

# 6) 目录穿越必须被拒（两个都应 403）
curl -s -o /dev/null -w '%{http_code}\n' --path-as-is 'http://127.0.0.1:8081/../CMakeLists.txt'
curl -s -o /dev/null -w '%{http_code}\n' --path-as-is 'http://127.0.0.1:8081/%2e%2e/CMakeLists.txt'

# 7) SSE：终端 A 保持订阅
curl -sN http://127.0.0.1:8081/events
# 终端 B 触发广播
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"event":"demo","data":"from-test"}' http://127.0.0.1:8081/api/broadcast

# 8) 并发：200 个请求 / 8 并发，随后看 stats
seq 1 200 | xargs -P 8 -I{} curl -s -o /dev/null http://127.0.0.1:8081/api/time
curl -s http://127.0.0.1:8081/api/stats

# 9) 优雅关闭
pkill -TERM -f go2cpp_web_server_full
```

实测（2026-10-31，WSL，Release，4 worker）：

- `/health` 200，`/api/items/42` 200，`/api/boom` 500，未知路径 404，
  `OPTIONS` 204；两个目录穿越都 403；
- SSE 订阅期间收到 `welcome`、3 个 `tick`、`demo` 与 `: heartbeat`；
- 200 个并发请求全部成功，`/api/stats` 显示累计 217 请求；
- `SIGTERM` 后退出码 0，日志按 `SSE hub 已停止 → 已关闭` 顺序收尾。

## 边界

- 路由只支持精确段与 `:param`，没有通配符/正则；匹配是线性扫描（示例规模）；
- 静态文件无 `Range`、`ETag`、预压缩；每次请求读整个文件（上限 8 MiB）；
- SSE 队列满时丢弃该订阅者的事件（记录 warn），不做阻塞式背压；
- HTTP 明文，没有 TLS；`HttpServer` 的 `NonBlocking` 模型尚未实现；
- 日志同步写文件，高频压测下会成为瓶颈（示例取舍）。
