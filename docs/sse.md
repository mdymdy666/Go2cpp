# SSE（Server-Sent Events）

`go2cpp::sse` 提供 HTTP/1.1 服务器推送事件封装。事件编码、增量解析和网络写入分开，底层复用 `net::Socket` 与 `http::ResponseWriter`，普通线程和已接入 `IOManager` 的 Fiber 都可使用。SSE 是单向连接，需要双向通信时使用 WebSocket。

## 发送事件

```cpp
#include "go2cpp/network/sse.hpp"

void serve(go2cpp::net::Socket::Ptr socket) {
    go2cpp::sse::Stream stream(socket);
    if (!stream.start().ok()) return;
    go2cpp::sse::Event event;
    event.id = "order-42";
    event.event = "updated";
    event.data = "status=ready\nversion=3";
    if (!stream.send(event).ok()) {
        stream.close();
        return;
    }
    (void)stream.tick();
    (void)stream.close();
}
```

`start()` 写入 `200 OK`、`Content-Type: text/event-stream`、`Cache-Control: no-cache` 和 chunked 响应头。`send()` 会处理分块编码和短写。多行 data 会编码成多个字段：

```text
id: order-42
event: updated
data: status=ready
data: version=3

```

一个 `Stream` 内部使用 Fiber/线程混合 Mutex 串行化多个线程/Fiber 的写入，保证 chunk 不交错，但不保证并发生产者的业务顺序；需要严格顺序时应在上层排队。模块没有无界内部队列，写入超时或对端读取过慢会返回 `Backpressure`/`Io`，调用方应停止生产并关闭连接，避免慢客户端耗尽内存。`max_event_bytes=0` 表示不设本层大小上限，生产环境应设置有限值。

## 心跳、超时和取消

```cpp
go2cpp::sse::StreamOptions options;
options.heartbeat_interval = std::chrono::seconds(15);
options.write_timeout = std::chrono::seconds(5);
go2cpp::sse::Stream stream(socket, request_context, options);
if (stream.start().ok()) {
    while (stream.active()) {
        // 可由 TimerManager、Scheduler 定时任务或业务循环调用。
        if (stream.heartbeat_due() && !stream.tick().ok()) break;
        // 业务事件到来时调用 send；不要在多个 Fiber 中同时调用。
    }
}
```

`tick()` 在心跳时间未到时不写入；到期后写入 `: heartbeat` 注释。它是显式调度点，适合绑定 `TimerManager`，不会额外占用常驻线程。传入 `ContextPtr` 后，Context 取消或超时会让后续写入快速返回 `Cancelled`/`DeadlineExceeded`，等待中的 Fiber 不应继续生产事件。

## 客户端解析和断线重连

`sse::Parser` 支持任意分片边界、LF/CRLF/CR 换行、UTF-8 BOM、注释、重复 data、`retry` 和有界内存。解析器只在空行处派发事件，未知字段会忽略：

```cpp
go2cpp::sse::Parser parser;
std::vector<go2cpp::sse::Event> events;
for (;;) {
    const std::string bytes = read_from_http_body();
    auto result = parser.feed(bytes, events);
    if (!result.ok()) break; // Parse 或 MessageTooLarge
    for (const auto& event : events) handle_event(event);
    events.clear();
}
```

服务器应为可恢复事件设置 `Event::id`。客户端重连时把 `parser.last_event_id()` 放入 HTTP 的 `Last-Event-ID` 请求头；服务端用 `sse::LastEventId(request)` 获取它并从事件日志恢复。`retry_ms` 只是客户端建议的重连间隔，不构成服务端重试保证。

```cpp
go2cpp::http::Request request = go2cpp::http::NewRequest(
    go2cpp::http::Method::Get, "/events");
request.header.set("Accept", "text/event-stream");
request.header.set("Last-Event-ID", parser.last_event_id());
```

## 错误用法和边界

- `id`、`event` 不能包含 CR、LF 或 NUL；`encode()` 返回 `InvalidEvent`，防止字段注入。
- `data` 的换行会拆成多个 `data:` 行；通过 `StreamOptions::max_event_bytes` 限制事件大小。
- 重复 `start()`、关闭后 `send()`、未 `start()` 直接 `send()` 都会返回错误。
- Context 只控制本流取消，不会回滚已经写入客户端的数据；事务语义应在发送前完成本地校验。取消会使后续发送失败并唤醒正在等待的 Socket 写入；对象仍由调用方持有，建议使用 `shared_ptr` 并在取消后显式 `close()` 释放连接。
- SSE 依赖 HTTP/1.1 长连接；代理可能缓冲响应，默认写入 `X-Accel-Buffering: no`，网关仍需关闭缓冲。
- 当前模块不提供 TLS、HTTP/2/3 推送、跨连接事件存储或无限重放；这些能力应由 Socket 层或 NetService 注入。

## 测试与示例

`tests/network/test_sse.cpp` 覆盖事件编码校验、增量解析、BOM/CRLF、Last-Event-ID、心跳、chunked 关闭和取消边界。`example/network/sse_demo.cpp` 展示普通线程中的流写入、注释心跳、仅更新游标的元数据事件和 Context 取消；在 Scheduler/IOManager 中调用同一接口即可获得 Fiber 级非阻塞等待。Parser 是单消费者状态机，多 Fiber 并发 feed 必须由调用方加锁。
