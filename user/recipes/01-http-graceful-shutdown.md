# HTTP 服务 + 优雅关闭

> 组合：`network`（HTTP 解析/连接）+ `concurrency`（Scheduler/Fiber）+ `app`（服务骨架）。
> 目标：收到 `SIGTERM` 后**停止接受新连接 → 等在途请求 → 关闭调度器**，不丢请求、不永久 park。

## 场景

单模块示例里，`web_server_simple` 只演示「起服务、回响应」；`scheduler_full` 只演示
「启动/关闭调度器」。真实服务必须把两者接起来：关闭顺序错了，要么在途请求被截断，
要么 Scheduler 先停、Fiber 永久 park 在 epoll 上。

## 组合方式

```
SIGTERM ──► signal handler（只置 atomic 标志，不做任何阻塞操作）
                 │
                 ▼
        主循环（非 Fiber 线程）
           ├─ 1. listener.Close()      停止 accept，新连接被拒
           ├─ 2. 等待 in-flight == 0   在途请求处理完
           └─ 3. Scheduler::Shutdown() 最后关调度器
```

职责划分：

| 模块 | 负责 | 不负责 |
| --- | --- | --- |
| `network` | 监听、accept、HTTP 解析、连接生命周期 | 进程信号、关闭顺序 |
| `concurrency` | Fiber 调度、channel、超时/取消 | 业务 drain 逻辑 |
| `app` | 把上面两层组装成可运行服务 | 通用性（按业务改） |

## 最小实现

参考 [`../modules/app/web_server_simple/main.cpp`](../modules/app/web_server_simple/main.cpp)
（单文件服务）与 [`../modules/app/web_server_full/app_server.cpp`](../modules/app/web_server_full/app_server.cpp)
（分模块版，含 middleware/router/static/sse）。

关键骨架（信号与关闭顺序）：

```cpp
std::atomic<bool> stopping{false};
std::signal(SIGTERM, [](int) { stopping.store(true); });   // 只置标志

while (!stopping.load()) {
    // 1) 停止接受新连接
    listener.Close();
    // 2) 等在途请求结束（计数由连接回调维护）
    while (in_flight.load() > 0) { go2cpp::time::Sleep(10ms); }
    // 3) 最后关调度器
    scheduler.Shutdown();
}
```

> 片段是骨架，不是可直接编译的完整程序；完整可运行版本见上面的示例文件。

## 验证

```sh
# 正常路径
./build/user/app/go2cpp_web_server_simple 28080 &
curl -s http://127.0.0.1:28080/health     # {"status":"ok"}

# 关闭路径：发 SIGTERM，观察是否等完在途请求再退出
kill -TERM %1; wait %1; echo "exit=$?"
```

失败路径：把 `in_flight` 的递减删掉，服务会在 `SIGTERM` 后卡住（第 2 步永远不结束）；
把 `Scheduler::Shutdown()` 提到第 1 步之前，在途请求会拿到空响应/连接重置。

## 常见坑

### 1. 在信号处理函数里做关闭/加锁/日志
- **现象**：偶发死锁，或信号处理后进程不退出。
- **原因**：信号处理函数运行在任意线程上下文，只能调用 async-signal-safe 函数。
- **处理**：handler 只 `store` 一个 `atomic<bool>`（或写 self-pipe），关闭逻辑放主循环。

### 2. Scheduler 先关，Fiber 永久 park
- **现象**：进程不退出，`ps` 看线程还在，CPU 为 0。
- **原因**：关闭调度器时仍有 Fiber 阻塞在 channel/epoll 上，唤醒源已销毁。
- **处理**：先取消 context / 关闭 listener / 关闭 channel，让所有等待者被唤醒，再关调度器。

### 3. 只关 listener，不 drain 在途请求
- **现象**：客户端收到 `connection reset`，日志里请求处理到一半。
- **原因**：`Close()` 只停止 accept，不等待已 accept 的连接。
- **处理**：维护 in-flight 计数（或连接表），关闭前等待计数归零，并给一个上限超时兜底。

## 相关文档

- [network 模块手册](../modules/network/README.md) · [deep/02-network.md](../deep/02-network.md)
- [concurrency 模块手册](../modules/concurrency/README.md) · [deep/01-concurrency.md](../deep/01-concurrency.md)
- [app 模块手册](../modules/app/README.md) · [deep/12-app.md](../deep/12-app.md)
