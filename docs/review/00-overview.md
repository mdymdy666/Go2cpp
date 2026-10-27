# 00 模块阅读顺序（摘要）

> 完整版见 `docs/code-review-guide.md` §2/§5。这里只留索引，避免两份文档漂移。

1. 地基：`context_backend.hpp` → `context_x64.S` → `fiber.cpp`
2. 调度：`scheduler.hpp` → `scheduler.cpp`
3. 同步：`parking_condition.cpp` → `sync.cpp` → `rwmutex/recursive_mutex/spin_mutex/futex_mutex`
4. channel/context/timer/future：`channel.hpp` → `channel.cpp` → `context.cpp` → `timer.cpp` → `future.cpp`
5. IO/hook：`io.cpp`/`io_backend.cpp`/`fd_manager.cpp` → `hook.cpp` → `socket.cpp`
6. 网络基础：`address.cpp`、`byte_array.cpp`、`http.cpp`
7. HTTP2/TLS：`frame.cpp` → `hpack.cpp` → `stream_state.cpp` → `connection.cpp` → `tls_transport.cpp`
8. HTTP3/QUIC：`http3/client.cpp` → `quic/picoquic_connection.cpp`
9. SSE/WS：`sse.cpp`、`websocket.cpp`
10. 外围：`config/`、`os/`、`logging/`、`metrics/`、`tools/`

当前用户指定顺序：**error → config → log → context**，其余待定。
