# network 使用限制

> 依据：`example/network/` 示例 + 实际头/实现；每条标题末尾给出 file:line。

## 1. Hook 只能跑在 Linux —— `src/network/hook.cpp:62-63`
文件顶部直接 `#error "go2cpp hook interposition currently requires Linux"`。
```cpp
#ifndef __linux__
#error "go2cpp hook interposition currently requires Linux"
#endif
```
**后果**：非 Linux 编译直接失败；移植时必须关掉 hook 目标。
## 2. 多 fd 等待只能在受管 Fiber 里 —— `src/network/io.cpp:2250-2252,2417-2422`
`wait` / `WaitAnyFor` / `WaitManyFor` 校验 current_scheduler 与 current_task，否则返回 kError/EPERM。
```cpp
if (Scheduler::current_scheduler() != &m_scheduler || !Scheduler::current_task())
    return {WaitStatus::kError, EPERM};
```
**后果**：普通线程调用立即失败（不会偷偷阻塞）；请用 poll/select 或桥接 Fiber。
## 3. `WaitManyFor` 不是「等全部任务」屏障 —— `example/network/io_wait_many_demo.cpp:110-116`
只返回**当前**已就绪的索引；且必须等注册完成再写 fd，否则丢事件。
```cpp
const IOWaitManyResult r = manager.WaitManyFor(requests, 2s);
// 先完成注册 → 再 send；不要用返回值判断业务任务结束
```
**后果**：把就绪当完成会少收事件；注册未完成就写 fd 会误判丢事件。
## 4. Hook 开关检查不能加锁，关 read/recv 会阻塞 worker —— `include/go2cpp/network/hook.hpp:156,163-166`
开关在每次 syscall 前检查，「绝不能用 mutex」；在 Fiber 内关掉 read/recv/write/send 会让整个 worker 真实阻塞。
```cpp
// 在 Fiber 内：不要用 ScopedThreadHookMode 关 read/recv 去"同步读"
```
**后果**：该 P 上所有 Fiber 全部停摆。
## 5. WebSocket `read_message` 没有超时 —— `example/network/websocket_full.cpp:21,135-137`
签名无超时参数，对端不回帧就无限阻塞；控制帧（Ping/Pong）也不交付给 read_message。
```cpp
socket.set_recv_timeout(2s);   // 必须自己给上界
auto frame = ws.read_message();   // 不要依赖它自己超时
```
**后果**：示例首版在 ping/pong 路径缺上界而挂住；把控制帧当数据帧会一直等。
## 6. Framer 超长帧进入失败态 —— `example/network/network_demo.cpp:57-76`
超 `max_frame` 返回 FrameTooLarge，之后一律 Failed 且缓冲清空；复用前必须 `reset()`。
```cpp
if (framer.try_feed(data) == Framer::FeedError::FrameTooLarge) framer.reset();
```
**后果**：不 reset 则后续所有输入都被拒。
## 7. `FdManager` 不拥有 fd —— `example/network/fd_manager_full.cpp:15-17`
只保存 hook / Socket / IOManager 共享的非阻塞与超时状态，不负责关闭。
```cpp
manager.erase(fd);   // 只清元数据并标记 closed，不 close(fd)
```
**后果**：靠它关 fd 或判断真实描述符状态都会错。
## 8. IOBackend 注册表约束 —— `example/network/io_backend_full.cpp:14-17`
同名不能重复注册；Unregister 后 Create 返回 nullptr；平台默认后端不可注销。
```cpp
RegisterBackend(name, factory);   // 已存在则失败
```
**后果**：注销默认后端会让 IOManager 起不来（台账 D-13）。
## 9. 关闭顺序：先等任务、再 close fd —— `example/network/io_wait_many_demo.cpp:267-270`、`example/README.md:132`
`WaitMany` 结束且 `wait_for()` 返回后才能关 fd。
```cpp
manager.Shutdown();
close_pair(first);   // 顺序反了会 EBADF / 投递到已关闭 fd
```
**后果**：提前 close → 事件投递到已关闭 fd 或 EBADF。
## 10. SSE 半个事件不派发 + 必须设上限 —— `example/network/sse_full.cpp:15`、`example/README.md:216-217`
没有空行结尾时 EOF 不强行派发；`max_event_bytes` 必须设，慢消费者报错后停止生产。
```cpp
options.max_event_bytes = 1 << 20;   // 不设等于允许无限累积
```
**后果**：把截断当完整事件；或缓冲无限增长。
