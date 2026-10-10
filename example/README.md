# 可运行示例

第一次接触项目建议按以下顺序运行：

1. `beginner_demo.cpp`：最短的 `go()`、Scheduler 和 Channel 示例；
2. `runtime_demo.cpp`：Context、错误链和 Select 的组合用法；
3. `fiber_sync_demo.cpp`：Fiber 与普通线程共用 Mutex、ConditionVariable 和
   WaitGroup；
4. `io_wait_many_demo.cpp`：多个 FD 的 epoll 等待、超时和取消；
5. `context_rollback_demo.cpp`：Context 局部回滚和补偿动作。

上面只是入门主线，**完整示例清单与场景说明见下方「示例场景速查」表**。

详细的环境准备、生命周期约束和故障排查请先阅读
[`docs/getting_started.md`](../docs/getting_started.md)。示例都要求先构建
`GO2CPP_BUILD_EXAMPLES=ON` 的构建目录；示例返回前会等待任务并关闭自己创建的
Scheduler，便于复制到新项目。

`example/concurrency/runtime_demo.cpp` 是端到端示例。它创建 G/M/P 调度器，通过类型化
channel 传递值，绑定 timeout context，演示 `WithValue` 和显式子节点取消，
使用仅发送/仅接收视图执行 select，并格式化和检查包装错误的身份。显式控制流
示例见 control_flow_demo.cpp；它展示 RAII defer、状态式 panic/recover 和
SelectCaster，不伪造 Go 的隐式栈跳转。
构建命令：

```sh
cmake -S . -B build -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build --target go2cpp_runtime_demo
./build/go2cpp_runtime_demo
```

## 示例场景速查

每个示例都至少包含一个成功路径、一个等待/取消或资源生命周期路径，以及一个
可观察的错误或边界路径。运行时先看带有 `[模块][场景]` 前缀的日志，再看最后
一行的布尔结果；日志中的失败状态是教学内容，不代表进程测试失败。

| 示例 | 成功场景 | 等待/并发场景 | 错误或边界场景 |
| --- | --- | --- | --- |
| `beginner_demo` | `go(scheduler, fn)` 与 Channel 收发 | `SelectLoop` 等待发送者 | Select 超时、nil Channel、默认 Scheduler |
| `runtime_demo` | Context.Value、Channel、错误包装 | 两个 Fiber 发送/接收并等待完成 | Select 超时、关闭后发送 |
| `fiber_sync_demo` | Mutex/ConditionVariable/WaitGroup | P=1 下 Fiber park 后由生产者通知 | ConditionVariable 超时、手动 Fiber 恢复 |
| `managed_pipeline_demo` | 无缓冲生产者/消费者 | Context 绑定的 Channel 等待 | 超时取消、关闭后发送 |
| `dynamic_gmp_demo` | P/M 扩展和任务亲和 | 空闲 Worker 回收到最小值 | 取消请求与启动竞争，最终收敛为 `Cancelled` |
| `mixed_runtime_demo` | 普通线程与 Fiber 交接混合锁 | 条件变量通知、BlockingRegion | Lock/Wait 失败立即终止，避免未持锁解锁 |
| `control_flow_demo` | defer LIFO 与参数求值 | defer 内 recover | move-only 内建 Select 返回 `kInvalid` |
| `timer_task_demo` | 一次性任务；固定速率周期执行三次后自动结束 | 动态重排间隔、到期回调投递为 Fiber | 取消竞态、weak_ptr 资源消失、回调异常不杀死定时器线程 |
| `network_demo` | Framer 粘包分帧、HTTP 完整请求解析 | 半包：分隔符跨两次输入；HTTP 逐字节 NeedMore | 超长帧进入失败态并丢弃缓冲、非法请求行报错 |
| `io_wait_many_demo` | WaitMany 收集多个 socket | socket→channel→Select 多源等待 | DefaultCase、Context 取消 |
| `io_hook_demo` | Hook socket 收发 | Fiber 让出 M 等待 readiness | `SO_RCVTIMEO` 转换为 `ETIMEDOUT` |
| `websocket_demo` | RFC 6455 握手与文本消息 echo | 服务端线程 recv 循环等待请求 | 握手失败直接返回非零；两端显式关闭 |
| `sse_demo` | 事件编码与解析、socketpair 上的流生命周期 | 半包增量解析、心跳写入 | Context 取消后拒绝新事件 |
| `context_rollback_demo` | commit 保留已提交资源 | 父取消/deadline 驱动补偿 | undo 异常、失效 savepoint |
| `log_config_demo` | 自定义 Formatter Item | 配置加载后应用日志 | 非法 INI 保留旧快照 |

一次构建后可以批量运行全部示例（每个命令都有 20 秒看门狗）：

```sh
for demo in beginner runtime fiber_sync managed_pipeline dynamic_gmp \
  mixed_runtime control_flow timer_task network io_wait_many websocket sse \
  context_rollback log_config io_hook; do
  timeout 20s ./build/go2cpp_${demo}_demo || exit $?
done
```

普通线程与 managed Fiber 的边界场景请运行 `example/concurrency/mixed_runtime_demo.cpp`：

```sh
cmake --build build --target go2cpp_mixed_runtime_demo
./build/go2cpp_mixed_runtime_demo
```

该示例让普通线程和 Fiber 共同使用 `sync::Mutex`、`ConditionVariable`、
`WaitGroup` 的等待队列，检查 `FiberLocal` 状态跨协作式 yield 保持，按普通
线程设置 Hook 参与策略，并声明短生命周期 `BlockingRegion` 让替代 M 服务
队列任务。外部 `ScopedThreadParticipation` 只是策略/资格作用域；它不会把
普通线程附加成调度器 worker。


## 新手入口

`example/concurrency/beginner_demo.cpp` 展示最短的 `go(scheduler, callback)`、
`go2cpp::fiber`、`Scheduler::add()` 和 `SelectLoop` 用法；不想显式
管理调度器时可以直接 `go2cpp::go(callback)`，程序结束前调用
`shutdown_default_scheduler()`。

## Select、socket 和 channel 多源等待

`example/network/io_wait_many_demo.cpp` 是一个完整的多源事件示例，建议新手按照日志中的
“场景一”到“场景四”逐段阅读：

1. `WaitManyFor()` 同时注册两个 socket FD，一次收集已经就绪的索引；它不是
   等待所有业务任务完成的屏障。两个字节都被读走后，示例才进入下一场景。
2. `IOManager` 中的 socket 等待 Fiber 在 epoll 触发后，把 `SocketEvent` 发送到
   `socket_events` channel；另一个 Fiber 将业务整数发送到
   `business_events`。同一个 `Select({RecvCase(business_events),
   RecvCase(socket_events)})` 先后接收两类事件，展示了“阻塞函数注册后由事件
   唤醒，再统一分发”的用法。普通线程只负责写 socket，不直接触碰 Fiber 状态。
3. `DefaultCase` 对空 channel 立即返回 `kReady`，适合非阻塞探测；没有
   `DefaultCase` 时，Select 才会等待 channel 事件或超时。
4. 一个永远没有发送者的 channel 配合可取消 Context，取消函数执行后 Select
   返回 `kCancelled`。这条路径是错误处理示例，不能把取消误判成正常超时。

运行并查看 stdout 和 `log/select_io_channel_demo.log`：

```sh
cmake --build build --target go2cpp_io_wait_many_demo
./build/go2cpp_io_wait_many_demo
```

`WaitAnyFor/WaitManyFor` 只能在当前 IOManager 管理的 Fiber 中等待；普通线程请
使用系统 `poll/select`，或者把等待封装在一个 Fiber 后用 channel 把结果转交给
普通线程。所有示例等待都有超时，任务完成并确认 `wait_for()` 返回后才能关闭 fd。

## Context 局部回滚

`example/concurrency/context_rollback_demo.cpp` 是可直接运行的完整示例，不只演示一个成功
分支，而是用日志把六种结果串起来：

1. 成功提交：`commit()` 丢弃 undo，已提交资源仍由业务持有；
2. 多步骤流程：`savepoint()` 只撤销后半段，随后 `rollback()` 撤销剩余步骤；
3. 父取消：父 `Context` 取消后，child 的 undo 自动执行，并可等待
   `rollback_done()` 确认补偿确实完成；
4. 截止时间：父 Context 到期后自动取消 child，验证 `DeadlineExceeded` 和补偿；
5. 错误补偿：undo 抛异常时，其他动作仍继续执行，事务进入 `kFailed`，调用方
   必须检查 `had_failure()`；
6. 错误用法：重复使用已经消费的 savepoint 会返回 false，不能把旧 token 当成
   新的保存点。

运行时默认把 Info/Warn 日志同时写到 stdout 和 `log/context_rollback_demo.log`，
这样可以对照“业务步骤完成、回滚完成、最终资源数”检查结果。undo 只适合登记
幂等、短小、非阻塞的本地补偿；它不能撤销已经发出的网络请求或恢复父 Context。
构建并运行：

```sh
cmake --build build --target go2cpp_context_rollback_demo
./build/go2cpp_context_rollback_demo
```

最短的新手写法如下。必须先完成业务动作，再登记对应的逆操作；作用域离开时若
没有提交，析构函数会兜底执行完整回滚：

```cpp
go2cpp::ContextRollback scope(go2cpp::Background());
resource.acquire();
scope.record_undo([&] { resource.release(); });
if (business_ok) {
    scope.commit();
}
// business_ok=false 时，scope 析构自动释放 resource。
```

当流程需要多个阶段时，在阶段边界保存 token；一次 `rollback_to()` 会消费该
token，成功后必须重新调用 `savepoint()`，否则返回 false。显式回滚后要同时检查
`rollback_done()` 和 `had_failure()`，不能只看 `rollback()` 的返回值，因为返回
true 只表示已经领取了回滚请求，不代表每个 undo 都成功。

## 控制流与 Caster

    cmake --build build --target go2cpp_control_flow_demo
    ./build/go2cpp_control_flow_demo

SelectValue/SelectCaster 的完整测试在 test_control_flow 中；普通 std::any
自定义事件可用 SelectProbe::SetAny() 接入同一转换路径。独立 SelectValue 支持
move-only；Channel Select/EventBatch 的内建 case 要求可复制值，违反时返回
kInvalid 而不是永久等待。

## 定时任务示例

`example/concurrency/timer_task_demo.cpp` 按顺序演示普通线程一次性任务、取消竞态、固定速率周期、动态重排、weak_ptr 资源消失、异常隔离以及 Scheduler 把到期回调投递为 Fiber。关键日志会标明每次执行和最终状态：

```sh
cmake --build build --target go2cpp_timer_task_demo
./build/go2cpp_timer_task_demo
```

TimerService 的完整接口和限制见 `docs/timer_usage.md`。需要做线程/Fiber/调度器对比时，使用独立基准：

```sh
cmake -S . -B build-bench -DGO2CPP_BUILD_BENCHMARKS=ON -DGO2CPP_BUILD_TESTS=OFF -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-bench --target go2cpp_timer_task_benchmark
GO2CPP_TIMER_BENCH_TASKS=2000 ./build-bench/go2cpp_timer_task_benchmark
```

## SSE 推送示例

`example/network/sse_demo.cpp` 先演示事件编码和半包解析，再用 `socketpair` 模拟一个
普通线程中的长连接。真实服务端把已接受的 `net::Socket` 传给 `go2cpp::SseStream`，在
IOManager Fiber 中使用同一接口即可；`docs/sse.md` 还给出心跳、Context 取消、重连的
Last-Event-ID 和错误用法：

```sh
cmake --build build --target go2cpp_sse_demo
./build/go2cpp_sse_demo
```

业务循环应由 TimerService 定期调用 `stream.tick()`，不要忙等；事件必须设置合理的
`StreamOptions::max_event_bytes`，慢消费者返回错误后停止生产并关闭连接。