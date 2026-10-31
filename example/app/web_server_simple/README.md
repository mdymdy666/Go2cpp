# web_server_simple —— 单文件 HTTP 服务

一个 `main.cpp` 跑起可用的 HTTP 服务，演示 `bento::HttpServer` +
`Scheduler` + `Channel` 的最小组合。

## 功能

- `GET /`：返回一个可点按钮调用 API 的 HTML 页面；
- `GET /api/echo?name=<名字>&times=<1..5>`：回显参数并附带服务器时间戳（JSON）；
- `GET /health`：`{"status":"ok"}`；
- 非 GET 返回 405，未知路径返回 404（都是纯文本，便于 curl 一眼看出）；
- `SIGINT` / `SIGTERM`：先屏蔽信号再启动服务线程，主线程 `sigwait` 拿到信号后
  通过 Channel 通知后台 Fiber，Fiber 调用 `HttpServer::Stop()` 等待 worker 退出。

## 构建

```sh
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_web_server_simple -j8
```

## 运行

```sh
cd /path/to/Go2cpp
./.build-bento-wsl/example/app/go2cpp_web_server_simple 8080
```

不传端口时默认 `8080`。启动后打印 `[web_simple] 已启动 http://127.0.0.1:8080`。

## 测试（curl）

```sh
# 1) 首页
curl -s -i http://127.0.0.1:8080/ | head -5

# 2) JSON 回显
curl -s 'http://127.0.0.1:8080/api/echo?name=Go2Cpp&times=3'
# {"ok":true,"name":"Go2Cpp","times":3,"server_time_ms":...}

# 3) 参数校验
curl -s -o /dev/null -w '%{http_code}\n' 'http://127.0.0.1:8080/api/echo?times=9'   # 400

# 4) 404 / 405
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/nope              # 404
curl -s -X POST -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/           # 405

# 5) 优雅关闭：另开终端
kill -TERM "$(pgrep -f go2cpp_web_server_simple)"
# 进程输出：收到信号 SIGTERM / 收到 SIGTERM，开始优雅关闭 / 已退出（关闭任务完成=1）
# 退出码 0
```

实测结果（本机 WSL，2026-10-31）：状态码 200/400/404/405 全部符合；发送
`SIGTERM` 后进程正常退出（exit=0），端口立即释放。

## 结构说明

```
web_server_simple/
└── main.cpp   # 全部逻辑：响应构造、路由、API、信号与关闭编排
```

## 边界与取舍

- 只支持 `GET`，没有路由参数、静态文件、SSE、中间件；
- 请求 handler 在 HttpServer 的 worker 线程执行，示例里的状态只有只读常量；
- `HttpServer` 默认 `Blocking` 模型（accept 线程 + 4 个 worker），空闲 keep-alive
  连接在 `Stop()` 时会被关闭，不需要额外处理；
- 信号处理依赖 POSIX `sigwait`；Windows 原生构建不在本示例范围内。

下一步：看 `web_server_full` 如何在不改变“可运行”的前提下补齐 10 项能力。
