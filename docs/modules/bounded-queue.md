# 模块文档：有界队列 / 背压（REQ-004）

> **状态：现行**（2026-11-01）。基线工作树 `/UserData/CodexWorkSpace/Go2cpp`，
> 构建 `.build-bento-wsl`（Release，`GO2CPP_WITH_BENTO=ON`）。
> 源码：`include/go2cpp/concurrency/channel.hpp`（`Channel<T>`）、
> `include/go2cpp/concurrency/fast_channel.hpp`（`FastChannel` / `FastSpscChannel`）。
> 本文只回答三件事：**容量语义是什么、`TrySend` 怎么用、什么时候该换 `FastChannel`**；
> 逐接口细节见 [`channel.md`](channel.md)，迁移风格见
> [`concurrency-usage.md`](concurrency-usage.md)。

---

## 0. 30 秒结论

| 问题 | 答案 |
|---|---|
| 队列有界吗？ | 有。`MakeChannel<T>(cap)` 的 `cap` 就是硬上限，缓冲最多 `cap` 条，**永不增长** |
| 满了 `Send` 会怎样？ | **阻塞**（在受管 G 上 park，不占 worker；普通线程退化为 futex 等待） |
| 满了 `TrySend` 会怎样？ | **立即返回 `kWouldBlock`**，不阻塞、不排队、不扩容——这就是「满返回 false」 |
| 怎么背压？ | 用 `Send` 阻塞，把压力反传给生产者；或观测 `TrySend` 的失败率自行丢/降级 |
| 内存会随消息数涨吗？ | 不会。实测 1e7 条 RSS 增长 0.00–0.12 MiB（见 §5） |
| 什么时候用 `FastChannel`？ | 固定容量 + 数据面 + 不需要 Select/超时/取消时；否则用 `Channel<T>` |

---

## 1. 容量语义

### 1.1 两种容量

| `cap` | 语义 | 典型用途 |
|---|---|---|
| `0` | **rendezvous**：发送方必须等到接收方就位才返回，缓冲不存任何元素 | 严格同步、握手、信号 |
| `N > 0` | **有界 FIFO**：最多缓冲 `N` 条，先进先出 | 任务队列、管道、限流令牌桶 |

`Capacity()` 返回创建时的固定容量，不随后续操作变化；`Len()` 返回**当前**
缓冲内元素数（不含正在交接的等待者），是瞬时值，不能当流控账本用。

### 1.2 各操作在「满 / 空 / 关闭」下的行为

| 操作 | 成功 | 满（发送侧） | 空（接收侧） | 已关闭 |
|---|---|---|---|---|
| `Send(v, ctx={})` | `kReady`，阻塞直到成功 | **阻塞**，直到有空间或 ctx 取消 | — | `kClosed` |
| `SendFor(v, timeout, ctx={})` | `kReady` | 阻塞至多 `timeout`，超时 `kTimedOut` | — | `kClosed` |
| `TrySend(v)` | `kReady` | **`kWouldBlock`**（立即返回） | — | `kClosed` |
| `TrySendBatch(vals, n)` | 返回实际写入条数（0..n） | 写入尽可能多，返回实际条数（可能 0） | — | 返回 0 |
| `Recv(ctx={})` | `kReady` + `value` | — | **阻塞**，直到有值 | 先排空缓冲，排空后 `kClosed` |
| `RecvFor(timeout, ctx={})` | `kReady` | — | 阻塞至多 `timeout`，超时 `kTimedOut` | 同上 |
| `TryRecv()` | `kReady` + `value` | — | **`kWouldBlock`**（立即返回） | 排空后 `kClosed` |
| `TryRecvBatch(vals, n)` | 返回实际取出条数 | — | 返回 0 | 返回缓冲中剩余条数 |
| `Close()` | `kReady` | — | — | 幂等；重复 `kAlreadyClosed` |

**关键点**：`kWouldBlock` **不是错误**。它是「当前无法立即完成」的正常状态，
与 `kClosed`（生命周期结束）、`kCancelled` / `kDeadlineExceeded`（上下文结束）
是三种不同原因，必须分开处理。

### 1.3 关闭后的排空顺序

`Close()` 只禁止**新的发送**；**缓冲中已有的值仍会被 `Recv` 取走**，取空之后
才返回 `kClosed`。所以消费者循环必须以 `result.ok` 为终止条件，而不是以
`IsClosed()` 为终止条件：

```cpp
while (true) {
  const auto r = jobs->Recv();
  if (!r.ok) break;      // 只有「关闭 + 排空」才会走到这里
  Handle(*r.value);
}
```

---

## 2. `TrySend`：背压信号与丢弃策略

### 2.1 语义

```cpp
go2cpp::ChannelSendResult TrySend(T value);
```

- **绝不阻塞**（不 park、不取时间戳）；
- 成功 → `status == kReady`，`Ok() == true`；
- 队列满（或有未就绪的并发接收者）→ `status == kWouldBlock`，`Ok() == false`；
- 通道已关闭 → `status == kClosed`，`Closed() == true`；
- 值移动构造失败 → `status == kInvalid`。

用 `Ok()` 判断成功；用 `status` 区分「满」与「关闭」：

```cpp
const auto r = jobs->TrySend(job);
if (r.Ok()) {
  ++sent;
} else if (r.status == go2cpp::ChannelStatus::kWouldBlock) {
  ++dropped;                 // 消费者跟不上：背压信号
} else if (r.Closed()) {
  break;                     // 通道已关闭：停止生产
} else {
  LogError(r.error);         // kInvalid 等真实错误
}
```

### 2.2 三种用法

#### 用法 A：丢弃（可丢的实时数据）

监控指标、日志采样、视频帧这类「旧数据没有价值」的流，满就丢，最多保留最新。
丢弃率本身就是背压指标，应打进监控：

```cpp
if (!ch->TrySend(frame).Ok()) {
  ++dropped_frames;          // 消费端跟上之前，新帧直接丢
}
```

#### 用法 B：退避重试（有界等待）

不能丢、但也不想无限阻塞时，用短退避把 `TrySend` 变成「有界等待」。**必须**
带睡眠或 `yield`，否则就是忙等烧 CPU：

```cpp
// 不推荐：空转会把一个 P 吃满
// while (!ch->TrySend(v).Ok()) {}

// 推荐：显式退避，总预算由 deadline 控制
const auto deadline = Clock::now() + 50ms;
while (!ch->TrySend(v).Ok()) {
  if (Clock::now() >= deadline) { ++dropped; break; }
  std::this_thread::sleep_for(50us);   // 或在 G 上用 go2cpp::time::Sleep
}
```

更好的写法是直接用带超时的 `SendFor(v, 50ms)`：语义一样，但通道内部完成等待，
不会反复抢锁。

#### 用法 C：阻塞背压（不能丢）

管道、任务队列、代理转发这类「一条都不能少」的场景，直接用 `Send`。生产者被
消费者限速，这是**期望行为**，不是性能问题：

```cpp
const auto r = ch->Send(item, ctx);   // 满则 park，M 让给别人
if (!r.Ok()) return HandleStop(r.status);
```

实测对照（`examples/bounded_queue`，cap=256，慢消费者）：

| 策略 | 2e6 条耗时 | 丢弃 | 说明 |
|---|---|---|---|
| `TrySend` + 丢弃 | 0.051 s | 1,968,512（98.4%） | 生产者完全不被阻塞，压力由丢弃吸收 |
| 阻塞 `Send` | 3.16 s | 0 | 生产者被消费者限速，零丢失 |

### 2.3 常见错误

| 错误 | 后果 | 正确做法 |
|---|---|---|
| `while (!ch->TrySend(v).Ok()) {}` | 忙等，吃满一个 P | 加退避或改用 `SendFor` |
| 把 `kWouldBlock` 当错误抛出 | 正常背压变成异常风暴 | 它是状态，不是错误 |
| 用 `Len()` 做精确流控 | 瞬时值，多线程下立刻过期 | 以 `TrySend` 结果为准 |
| 持有 `sync::Mutex` 时阻塞 `Send` | 与消费者形成锁环，死锁 | 先放锁再发 |
| 用无界 `std::queue` 模拟通道 | 内存先于逻辑爆炸 | 用 `cap` 有界，满了背压 |
| 关闭后继续 `TrySend` | 返回 `kClosed`，数据静默丢失 | 以 `Closed()` 为停止信号 |

---

## 3. `Channel<T>` vs `FastChannel`：什么时候用哪个

### 3.1 两者是什么

| | `Channel<T>` | `FastChannel<T, Cap>` | `FastSpscChannel<T, Cap>` |
|---|---|---|---|
| 头文件 | `go2cpp/channel.hpp` | `go2cpp/concurrency/fast_channel.hpp` | 同左 |
| 容量 | **运行期** `MakeChannel<T>(cap)` | **编译期** `Cap` | 编译期 `Cap` |
| 热路径 | 自旋锁 + 环形缓冲 | Vyukov 有界 MPMC ring（无锁） | SPSC ring（无锁） |
| 满/空 | 阻塞或 `kWouldBlock` | 无锁快路径失败后落锁 park | 无锁重试 / park |
| 多生产者多消费者 | ✅ | ✅ | ❌ 仅 1P1C |
| `Select` 多路复用 | ✅ | ❌ | ❌ |
| `Context` / 超时 / 取消 | ✅ `SendFor` / `RecvFor` / `DoneCase` | ❌ | ❌ |
| 关闭后 `Recv` | 返回 `kClosed`（`optional` 无值） | 写**零值**并返回 `kClosed` | 同左 |
| 对 `T` 的要求 | nothrow move + nothrow 析构 | 另需**默认构造** | 另需默认构造 |
| 定向视图 / `DebugSnapshot` | ✅ | ❌ | ❌ |
| 成熟度 | **现行** | 实验性（头注释标注） | 实验性 |

### 3.2 选型判据

1. **需要配置容量、超时、取消、Select、关闭排空语义** → `Channel<T>`。
   控制面、业务队列、跨模块接口默认选它。
2. **容量固定、数据面、追求极限吞吐、1P1C** → `FastSpscChannel<T, Cap>`。
   例如网络收包线程 → 协议解析 Fiber 的单向流水线。
3. **容量固定、数据面、MPMC** → `FastChannel<T, Cap>`。
   例如多个 worker 抢同一个任务环。
4. **不确定** → 先用 `Channel<T>`。`cap` 在运行期可配，压测后再决定是否值得
   换成编译期容量的无锁版本。

一句话：**`Channel<T>` 是语义完整的默认选择，`FastChannel` 是固定布局下的性能选项。**
两者的「有界 + 满时拒绝/阻塞」语义一致，差的是可配置性与额外能力，不是正确性。

---

## 4. 最小可运行示例

`examples/bounded_queue/bounded_queue_demo.cpp` 演示了快生产者 → 有界队列 →
慢消费者的三种策略（`TrySend` 丢弃 / 阻塞背压 / RSS 对比）。核心 20 行：

```cpp
#include "go2cpp/channel.hpp"

auto jobs = go2cpp::MakeChannel<Job>(1024);   // 有界：容量即硬上限

// 生产者（快）：满则立即失败，由业务决定丢/降级
for (;;) {
  const Job job = NextJob();
  const auto r = jobs->TrySend(job);
  if (r.Ok()) {
    continue;
  }
  if (r.status == go2cpp::ChannelStatus::kWouldBlock) {
    ++dropped;                 // 背压信号
    continue;
  }
  break;                       // kClosed
}
jobs->Close();

// 消费者（慢）：队列空则 park，关闭且排空后退出
while (true) {
  const auto r = jobs->Recv();
  if (!r.ok) break;
  Handle(*r.value);
}
```

运行：

```bash
examples/bounded_queue/build_and_run.sh --items 2000000
```

---

## 5. 验收：1e8 条 send/recv 无丢失 + RSS 稳定

### 5.1 测试入口

```bash
# 默认 1e8 条（4 条独立流水线，每通道 1 生产者 + 1 消费者，阻塞 Send/Recv）
tests/concurrency/run_bounded_queue_test.sh

# 快速自检（1e7，约 1 秒）
tests/concurrency/run_bounded_queue_test.sh --items 10000000
```

脚本不注册 ctest、不改现有 CMake，直接 `g++ -O2` 链接 `.build-bento-wsl`
（与 `tests/concurrency/build_and_run_tests.sh` 同一惯例）。可用
`BUILD_DIR=/path/to/build` 指定其他构建目录。

### 5.2 判定标准（四条全部满足才算通过）

1. **数量**：`sent == received == 1e8`（每个 lane 精确等于 `1e8 / lanes`）；
2. **无丢失/无重复/不乱序**：消费者逐条校验 `seq` 从 0 连续递增，且
   `checksum == n*(n-1)/2`（每个 lane 独立校验）；
3. **有界**：`TrySend` 满时返回 `kWouldBlock`、阻塞 `Send` 在满时挂起
   （`DebugSnapshot().send_waiters >= 1`）；`peak_len <= capacity`；
4. **RSS 稳定**：1e7 条长跑中 RSS 峰值增长 `< 32 MiB`（实测 ≤ 0.12 MiB）。

### 5.3 实测结果（2026-11-01，WSL2 / g++ 13 / Release）

落盘验证时宿主机 load average ≈ 4.3（另有构建任务并行），1e8 用时 3.88 s；
空载首次运行是 2.15 s / 46.57 Mmsg/s。**耗时随负载波动，判定只看数量与校验和**。

```text
$ tests/concurrency/run_bounded_queue_test.sh
[bounded_queue] items=100000000
[test] bounded_queue: capacity semantics
[test] bounded_queue: TrySend drop accounting
  total=2000000 sent=29256 dropped=1970744 received=29256 elapsed=0.023s
[test] bounded_queue: multi-producer + close drain
[test] bounded_queue: 1e8-style no-loss blocking
  lanes=4 capacity=1024 total=100000000 sent=100000000 received=100000000
  elapsed=3.881s rate=25.77 Mmsg/s
[test] bounded_queue: RSS stability
  items=10000000 capacity=1024 consumed=10000000 peak_len=961
  baseline=5.88 MiB peak_rss=5.88 MiB growth=0.00 MiB elapsed=3.196s
[bounded_queue] PASSED
```

重复三次（同一二进制、load ≈ 4.3）：`3.97 s / 25.21 Mmsg/s`、
`4.13 s / 24.22 Mmsg/s`、`3.92 s / 25.51 Mmsg/s`，每次
`sent == received == 100000000`，无一次失败。

### 5.4 RSS 怎么量

测试与示例都读 `/proc/self/statm` 的 `resident` 字段乘以 `sysconf(_SC_PAGESIZE)`：

```cpp
unsigned long long size = 0, resident = 0;
FILE* f = fopen("/proc/self/statm", "r");
fscanf(f, "%llu %llu", &size, &resident);   // 单位：页
fclose(f);
const auto rss = resident * static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
```

采样点：生产者每 1,000,000 条一次，记录峰值；基线取通道创建之后、生产开始之前。
注意：

- 必须用 **Release**（`-O2 -DNDEBUG`）构建；ASan/TSan 会显著放大 RSS 与耗时；
- 首次运行会因动态库加载抬高基线，**看增长量**而不是绝对值；
- 宿主机内存压力会导致页回收，RSS 可能小幅回落，属正常。

### 5.5 为什么要 4 条流水线

`Channel<T>` 的容量与语义与流水线数无关。测试把 1e8 平摊到 4 条独立通道，
每条只有 1 生产者 + 1 消费者，目的是：

- 让总耗时落在秒级（单条 1e8 约 11 s，4 条约 2–3 s）；
- 避免多生产者竞争掩盖「单条链路是否丢数据」；
- 每 lane 独立校验 checksum，任何一条有问题都会单独报错。

多生产者/多消费者共享同一通道的语义由用例 5 覆盖（4 生产者 + 单消费者 +
关闭排空，校验每个 producer 的序号连续）。

---

## 6. 相关文件

| 文件 | 作用 |
|---|---|
| `include/go2cpp/concurrency/channel.hpp` | `Channel<T>` / `TrySend` / `Select` 实现 |
| `include/go2cpp/concurrency/fast_channel.hpp` | `FastChannel` / `FastSpscChannel`（实验性） |
| `docs/modules/channel.md` | Channel/Select 的完整接口与实现细节 |
| `docs/modules/concurrency-usage.md` | Thread → Fiber 迁移手册（含通道选型） |
| `docs/modules/bounded-queue.md` | 本文 |
| `examples/bounded_queue/` | 可编译运行示例（丢弃 / 背压 / RSS 对比） |
| `tests/concurrency/test_bounded_queue.cpp` | 本需求专项测试（1e8 无丢失 + RSS） |
| `tests/concurrency/run_bounded_queue_test.sh` | 独立编译运行入口（不注册 ctest） |
| `collab/acceptance.md` | REQ-004 验收记录 |
