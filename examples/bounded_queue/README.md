# 有界队列 / 背压示例（REQ-004）

对应文档：[`docs/modules/bounded-queue.md`](../../docs/modules/bounded-queue.md)。

本目录演示三件事：

1. **`TrySend` 满时返回 `kWouldBlock`**（不阻塞），由业务决定丢弃；
2. **阻塞 `Send` 的背压**：生产者被慢消费者限速，零丢失；
3. **RSS 稳定**：有界 `Channel` 的内存在长跑中不增长，无界 `std::deque` 会。

## 编译运行

```bash
# 使用 .build-bento-wsl（默认）
examples/bounded_queue/build_and_run.sh

# 指定规模与场景
examples/bounded_queue/build_and_run.sh --items 2000000
examples/bounded_queue/build_and_run.sh --scenario a          # 只跑 TrySend 丢弃
examples/bounded_queue/build_and_run.sh --scenario b          # 只跑阻塞背压
examples/bounded_queue/build_and_run.sh --scenario c          # 只跑 RSS 对比

# 指定其他构建目录
BUILD_DIR=/path/to/build examples/bounded_queue/build_and_run.sh
```

脚本用 `g++ -std=c++17 -O2` 直接链接 `.build-bento-wsl` 下的 `libgo2cpp_*`，
**不改任何现有构建文件**，也不注册 ctest。

## 输出解读（`--items 2000000` 实测）

```text
== A. TrySend + 慢消费者：满时丢弃（不阻塞） ==
  capacity=256 items=2000000 sent=31488 dropped=1968512 consumed=31488
  elapsed=0.051s drop_rate=98.43% 生产者从未被阻塞

== B. 阻塞 Send + 慢消费者：背压（零丢弃） ==
  capacity=256 items=2000000 sent=2000000 consumed=2000000
  elapsed=3.163s Send累计耗时=3.093s（被消费者限速）

== C. RSS 对比：有界 Channel vs 无界 std::deque ==
  items=2000000
  有界 Channel(capacity=1024): baseline=5.62 MiB peak=5.62 MiB growth=+0.00 MiB
  无界 deque            : baseline=5.62 MiB peak=30.45 MiB growth=+24.82 MiB
```

- **A** 说明 `TrySend` 永不阻塞：生产者在 0.051 s 内发完 2e6 条，消费者只来得及
  取走 3 万条，其余按策略丢弃。丢弃率就是背压信号。
- **B** 说明阻塞 `Send` 把压力反传给生产者：总耗时从 0.051 s 变成 3.16 s，
  与被限速的消费者同速，**零丢弃**。
- **C** 说明有界队列的内存只与 `capacity` 有关，与消息总量无关。

## 相关

- 测试（1e8 无丢失 + RSS）：`tests/concurrency/test_bounded_queue.cpp`，
  入口 `tests/concurrency/run_bounded_queue_test.sh`
- 验收记录：`collab/acceptance.md` 的 REQ-004 条目
