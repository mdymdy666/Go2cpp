# 有界队列做背压

> 组合：`concurrency`（有缓冲 channel / Select / Context）+ `app`（生产者-消费者服务）。
> 目标：下游变慢时不让内存无限增长，也不让生产者永久阻塞——用「有界队列 + 超时/取消」把
> 压力传回上游。

## 场景

`channel_demo` 只演示无缓冲/有缓冲的语义差别；真实服务里生产速度 > 消费速度是常态。
无界队列会把内存吃光；裸 `send` 又会把生产者永久挂死。正确做法是**有界 + 可取消 + 可观测**。

## 组合方式

```
producer ──send（带超时/ctx）──► [缓冲 N 的 channel] ──recv──► consumer
    │                                   │
    └─ 满：select 超时 → 拒绝/降级         └─ 空：consumer park，不占 CPU
```

| 模块 | 负责 | 不负责 |
| --- | --- | --- |
| `concurrency` | 有界 channel、Select 多路等待、Context 取消 | 背压策略（拒绝/降级/扩容） |
| `app` | 策略选择、指标上报、优雅退出 | 队列语义 |

## 最小实现

参考 [`../modules/concurrency/channel_demo.cpp`](../modules/concurrency/channel_demo.cpp)
（缓冲与关闭语义）、[`../modules/concurrency/context_demo.cpp`](../modules/concurrency/context_demo.cpp)
（取消/超时），以及 [`../modules/concurrency/managed_pipeline_demo.cpp`](../modules/concurrency/managed_pipeline_demo.cpp)
（流水线组合）。

关键骨架（满即超时，把压力传回上游）：

```cpp
auto ch = go2cpp::make_chan<int>(1024);       // 有界缓冲，容量就是背压阈值
go2cpp::go([ch] {
    for (;;) {
        auto v = ch->Recv();                  // 空时 park，不忙等
        if (!v) break;                        // 关闭
        handle(*v);
    }
});

// 生产者侧：满时最多等 50ms，然后走拒绝/降级
if (!ch->TrySend(value, 50ms)) {
    ++rejected;                                // 指标：背压触发次数
}
```

> API 名以 `include/go2cpp/concurrency/channel.hpp` 的实际签名为准；片段是骨架。

## 验证

```sh
# 1) 正常路径：生产/消费同速，rejected == 0
# 2) 背压路径：consumer 里 sleep，观察 rejected 增长、内存不涨
# 3) 关闭路径：close(ch) 后 producer 的 send 立即返回失败，不永久阻塞
```

压测建议：用示例里的看门狗超时 + 计数断言，把「队列长度」「拒绝次数」「消费延迟」
三项打出来；只跑一次冒烟不算验证。

## 常见坑

### 1. 用无界队列「先跑通」
- **现象**：压测几分钟后 OOM，或延迟线性上升。
- **原因**：生产 > 消费时无界队列只会在内存里堆积，不会把压力传回上游。
- **处理**：容量 = 可接受的最大积压；满了就拒绝/降级/限流，并上报指标。

### 2. 关闭时生产者永久阻塞
- **现象**：进程退出卡住，`send` 不返回。
- **原因**：channel 关闭后向它发送会失败，但若生产者阻塞在「队列满」上而消费者已退出，
  没有唤醒源。
- **处理**：发送用 `Select` 同时监听 `ctx.Done()`/关闭信号；关闭顺序固定为
  「先停生产者 → close(ch) → 等消费者退出」。

### 3. 用 `select` 轮询代替阻塞等待
- **现象**：CPU 空转，p99 延迟抖动。
- **原因**：`default` 分支 + `sleep` 轮询，既耗 CPU 又增加延迟。
- **处理**：Fiber 内用阻塞 `Recv`/`Select`（会 park 让出 M），需要超时就带 timeout，
  不要用 `default` 忙轮询。

## 相关文档

- [concurrency 模块手册](../modules/concurrency/README.md) · [deep/01-concurrency.md](../deep/01-concurrency.md)
- [app 模块手册](../modules/app/README.md) · [deep/12-app.md](../deep/12-app.md)
