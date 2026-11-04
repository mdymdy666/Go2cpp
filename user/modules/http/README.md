# HTTP（http 异常处理示例）

> **一句话定位**：本目录只放 **http 的异常处理示例**——客户端超时、服务端被慢
> 客户端拖住、请求体没读满。每个文件都能单独编译、真实运行，输出带预期值。
> 模块正式文档见 [`../../network/README.md`](../../network/README.md)；
> 正常路径示例见 [`../../network/http_demo.cpp`](../../network/http_demo.cpp)。

## 示例一览

| # | 文件 | 真实场景 | 默认行为（file:line） | 可选策略 |
| ---: | --- | --- | --- | --- |
| 01 | [`01-client-timeout.cpp`](01-client-timeout.cpp) | 下游库存服务 1.5s 才回，页面 SLA 只有 300ms | `Client` 默认 **30s** 超时（`include/go2cpp/network/http.hpp:358`），到期返回 `Result::Error::Timeout`（http.hpp:328-329），不抛异常、不重试 | 重试（仅幂等 GET）、降级到本地缓存快照、总预算到期取消 |
| 02 | [`02-server-slowloris.cpp`](02-server-slowloris.cpp) | 慢客户端每 100ms 发 1 字节，几百个半开连接占满 worker | 每请求 `request_timeout` 默认 **10s**（`include/go2cpp/app/gateway.hpp:358`），从第一次 recv 前起算，到期直接关连接并计数（`src/app/gateway.cpp:1301-1302`、`1355-1368`） | 调大（上传/弱网）、调小（内网）、并发限流 503、维护模式 503 |
| 03 | [`03-partial-body.cpp`](03-partial-body.cpp) | 上传 1 MiB 走到一半断网/客户端挂起 | `Parser` 返回 `ParseState::NeedMore`（http.hpp:213、225-229）；douyin_http 短 body 回 **400「请求体不完整」且不进 handler**（`app/douyin/include/douyin_http.hpp:604-616`），超时回 408 | 整体重试、`Content-Range` 续传、审计记录 partial 字节数 |

## 默认语义速查

- **客户端**：`http::Client(timeout)` 的超时是 connect/send/recv 的**绝对
  deadline**（http.hpp:368-369），默认 30s（http.hpp:358）；每次
  `do_request()` 自行建连、不复用连接池（http.hpp:349-350）。
- **服务端核心**：`http::Server::serve_connection()` **不设** recv/write 超时
  （http.hpp:570-577）——要防 slowloris 必须像 `app::Gateway` 一样在连接层加
  单请求总 deadline。
- **解析器**：body 未读满时 `Parser::feed()` 只返回 `NeedMore`，**不会**把半截
  body 标成 `Complete`；调用方必须自己用 EOF/超时终止，且不能进 handler。
- **解析失败**：`ParseState::Error` 不是终态，但调用方应 `reset()` 或丢弃
  Parser（http.hpp:232-244）。

## 编译与运行

前置：项目已构建过一次（产生 `.build-bento-wsl/libgo2cpp_network.so`）。
用 `build/` 目录时把 `-L/-rpath` 换成对应路径即可。

```sh
cd /UserData/CodexWorkSpace/Go2cpp
FLAGS="-std=c++20 -O2 -Wall -Wextra -I include"
LIB="-L .build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl -lgo2cpp_network -lpthread"

g++ $FLAGS user/modules/http/01-client-timeout.cpp  $LIB -o /tmp/http01 && /tmp/http01
g++ $FLAGS user/modules/http/02-server-slowloris.cpp $LIB -o /tmp/http02 && /tmp/http02
g++ $FLAGS user/modules/http/03-partial-body.cpp    $LIB -o /tmp/http03 && /tmp/http03
```

三个示例都不注册 ctest、不监听固定端口（`127.0.0.1:0` 让内核分配），可并行运行。

## 运行结果（本机实测摘要）

```
# 01（约 0.95s）
[默认] 单次请求 error=Timeout message="Connection timed out"（预期 Timeout）
[重试] 第 1 次 error=Timeout
[重试] 第 2 次 error=Timeout
[降级] 使用本地缓存快照: {"stock": 42, "stale": true, "source": "local-cache"}
[取消] 总预算 700ms 已耗尽，停止重试
[client-timeout] 通过

# 02（约 3s）
[默认] 慢客户端 request_sent=0 timeouts=1（300ms 内只发了约 12 字节，被关闭）
[调大] request_timeout=1500ms request_sent=1 status_200=1 timeouts=0
[限流] max_concurrent=2 第三个连接 status_503=1 rejected=1
[维护] status_503=1
[正常] status=200 served=1
[server-slowloris] 通过

# 03（约 1s）
[断开] status=400 received=100 handler=0 file_exists=0
[超时] status=408 received=100 handler=0 client_status=408
[完整] status=200 handler=1 file_size=1024
[重试] first_status=400 second_status=200 second_handler=1
[续传] state=1 content_range="bytes 100-1023/1024"
[partial-body] 通过
```

## 选择建议

- 读接口 + 可短时过期 → **重试 + 降级**；写接口先做幂等键，不要盲目重试。
- 公网入口 → **默认 10s + 并发上限**；只调大超时等于把资源让给 slowloris。
- 大文件/弱网 → 按路由放宽超时并开**续传**；普通接口保持默认、宁可直接 400/408。
- 任何策略都不要破坏默认前提：**请求不完整 = 不提交、不落盘**。
