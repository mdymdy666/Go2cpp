# Worker 调度链优化设计（P0-4 resume_claim / P1 runnext·栈池·自旋配额）

日期：2026-10-30
工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL）
定位：**只读设计文档**。供 `a8e07736`（P0 调度链改造）参考；本文件不修改任何源码。
前置阅读：`docs/research/go-scheduler-fiber-analysis.md`（下称「分析文档」）。

> 口径说明：本文的「实测」数据来自本工作树 2026-10-30 20:00–20:20 的基准，
> 环境为 WSL2（Ryzen 9 8945HX，12 vCPU，`-O3 -DNDEBUG`，gcc 13.3）。
> 主机当时有其他 agent 的并行负载，所有数字均为**同轮交替 A/B、多轮取最小值**，
> 绝对值会随机器状态漂移，**只看相对差值**。

---

## 0. 结论先行

1. 分析文档 §5.2 的估算（「6 次 mutex 60~90ns」）**偏保守**：callgrind 实测
   `pthread_mutex_lock+unlock` 占 park/wake 往返**指令数的 26%**，但每次无争用
   `std::mutex` 的 lock+unlock 只有约 **145 条指令 ≈ 12ns**；真正被低估的是
   **TLS 访问**——共享库默认 general-dynamic TLS，`__tls_get_addr` 占 **10.5%** 指令，
   一次 park/wake 往返要付 10+ 次。
2. 在**不改动 P0 结构**的前提下，只做「TLS 模型 + 锁类型 + 3 处热路径去锁」，
   实测就能把调度链 **123.8ns → 79.8ns（-35.5%）**、pingpong **-30.9%**、
   channel cap0 **-19.3%**。这说明分析文档 P0 的 55~75ns 目标里，
   **有一半不需要动队列/状态机协议**，属于低风险收益。
3. P0-4（resume_claim）不能「直接删」：`Fiber::Impl::~Impl()` 的
   `try_claim_for_destroy()`（FIBER-02 安全契约）依赖它做「析构前必须独占
   resume 权」的判定。正确做法是**把热路径的 CAS 降级为 store，保留 claim
   作为析构互斥**，并补一条 debug 断言（§3）。
4. 推荐的落地顺序（与 P0-1/P0-2/P0-3 的耦合最小 → 最大）：
   **TLS 模型 → 锁类型 → P1-3 runnext → P0-2 环形队列 → P0-1 CAS 状态机 →
   P0-3 dequeue 即 claim → P0-4 resume claim → P1-2 栈池 → P1-4 自旋配额**。

---

## 1. 实测基线（为什么先做 TLS 和锁类型）

### 1.1 callgrind（pingpong 20000 往返，`Ir` 口径）

| 项 | 基线占比 | 说明 |
|---|---:|---|
| `pthread_mutex_lock` + `pthread_mutex_unlock` | **26.0%** | 6 次/往返（4 × `m_transition_mutex` + 2 × `processor.mutex`） |
| `__tls_get_addr` | **10.5%** | 共享库 general-dynamic TLS；一次往返 10+ 次 |
| `worker_loop` 本体 | 14.5% | 取任务 + M/P 记账 + 事件 |
| `requeue_from_worker` | 5.5% | 队列发布 |
| `Task::run` / `resume_locked` / `jump` | 3.3% / 2.8% / 3.5% | Fiber 层 |
| `wake_registered` / `park_for_scheduler` / `try_mark_running` | 2.9% / 1.8% / 1.3% | 状态机 |

### 1.2 单项实测（同一轮交替，min-of-20，单位 ns/op）

| 场景 | 基线 | 仅 TLS initial-exec | 再叠加自旋锁 | 再叠加 P 队列自旋锁 |
|---|---:|---:|---:|---:|
| pingpong（1 park + 1 wake 往返） | 159.38 | 128.27 | 123.30 | **110.11** |
| yield（同 M 换出往返） | 146.86 | 127.58 | 122.81 | **116.66** |
| channel cap0（一次阻塞收发） | 286.84 | 243.70 | 241.01 | **231.46** |
| fiber 裸切换（1 次） | 15.4 | — | — | 15.4（不受影响） |

拆解：`pingpong - 2×fiber_switch = 110.11 - 30.6 = 79.8ns` 调度链；
`yield` 为 86.4ns。**分析文档的 55~75ns 目标已进入射程，且尚未动队列结构。**

### 1.3 一条被证伪的假设

「worker_loop 的 M/P 原子记账（`running_workers`/`running_machines`/`PState`）
是热路径大头」——**不成立**。把 4 个 RMW + 2 个 store 全部删掉的实验版本，
yield 只快 0.7%（噪声内），但 channel 反而劣化 **+153%**（`notify_worker`
判定失去 `running_machines == 0` 的依据，每次 wake 都唤醒空闲 M）。
**结论：记账要保留，别把它当优化目标；它同时承担唤醒抑制职责。**

---

## 2. 已实测有效的低风险改动（建议 a8e07736 直接吸收或保留）

### 2.1 initial-exec TLS 模型（-20ns/往返，收益最大）

**现状**：`scheduler.cpp` 顶部 7 个 `thread_local`（`t_scheduler`、`t_task`、
`t_machine_id`、`t_processor_id`、`t_blocking_machine_id`、`t_in_idle_poll`、
`t_incoming_lock_depth`）。共享库编译默认 general-dynamic，每次访问 =
`__tls_get_addr` 调用（约 25~40 cycle）。

**改法**：

```cpp
#if defined(__GNUC__) || defined(__clang__)
#define GO2CPP_SCHED_HOT_TLS __attribute__((tls_model("initial-exec")))
#else
#define GO2CPP_SCHED_HOT_TLS
#endif
thread_local Scheduler* t_scheduler GO2CPP_SCHED_HOT_TLS = nullptr;
thread_local std::shared_ptr<Task> t_task GO2CPP_SCHED_HOT_TLS;
// ... 其余 5 个同理
```

**风险（必须写进 PR 说明）**：
- 这是 **TLS 模型级 ABI 变化**：`initial-exec` 要求库在进程启动时加载。
  **任何在改动前链接好的二进制（含第三方 .so）加载新库会因静态 TLS 块不足
  而崩溃**——必须全量重建；提供 `dlopen` 插件的场景不要用。
- 命中概率不低：本工作树里未重链接的 `go2cpp_tests` 就这样崩过一次
  （SIGSEGV 在 ld.so `_dl_fixup`）。
- 若不能接受，可退化为「把 7 个标量打包进一个 `thread_local` 结构体」，
  一次取地址后复用（收益约为 initial-exec 的 60~70%，无 ABI 风险）。

**预期**：-16%（约 -20ns/往返）。

### 2.2 `detail::HotSpinLock` 替换两类 mutex（-8~-14ns）

**现状**：`Task::m_transition_mutex`（4 次/往返）与 `Impl::Processor::mutex`
（2 次/往返）都是 `std::mutex`；无争用时每次 lock+unlock ≈145 条指令。

**改法**：`include/go2cpp/concurrency/scheduler/scheduler.hpp` 内新增
`namespace detail { class HotSpinLock; }`（TTAS：一次 `exchange(acquire)`；
失败先 `pause` 只读自旋 128 次，再 `yield()`；`unlock` 为 `store(release)`），
成员类型替换，cpp 中 38 处 `lock_guard` 类型同步替换。

**风险**：
- 临界区必须短（Task 状态锁是 3~6 条原子；P 队列锁含 `deque::push_back`，
  可能分配——高并发下自旋会烧 CPU，但不会死锁）。
- 不可重入（与 `std::mutex` 相同）。
- `Task` 对象由 40B 锁缩到 1B → **布局变化，必须全量重建**（这次段错误的
  头号嫌疑就是「旧 .o 未重编」）。

**预期**：transition 部分 -8~-10ns，P 队列部分 -5~-6ns。

### 2.3 wake 临界区内预留入队权（-3~-4ns）

**现状**：`wake_registered` → `wake_for_wait()`（锁 1）→ `requeue_from_worker`
→ `try_mark_queued_from_worker()`（锁 2）。两次临界区保护的是同一组字段
（`m_state`/`m_wake_pending`/`m_queued`）。

**改法**：新增 `Task::wake_for_wait_reserve()`（内部 `wake_for_wait_impl(bool)`），
在 `kWaiting → kRunnable` 且 `!m_execution_claim` 分支里**同一临界区内**
`m_queued.store(true)`；新增 `Scheduler::requeue_from_worker_reserved()`
跳过 `try_mark_queued_from_worker`，失败出口先 `clear_queued()` 再回退到
既有 `requeue_from_worker`/`requeue_to_processor`/`enqueue` 链。
仅当 `current_scheduler()==this && registered()`（本 M 唤醒）时启用。

**风险**：预留后若走 `enqueue()` 回退，`prepare_enqueue()` 会看到
`m_queued==true` 而拒绝发布 → 所有回退出口必须 `clear_queued()`（已在实现中
逐个覆盖）。`wake_for_io`/`wake_or_cancel` 不走预留，语义不变。

**预期**：-3~-4ns/往返；channel cap0 同量级。

### 2.4 三处零语义热路径清理（合计 -3~-5ns）

1. `recover_orphan_running()`：先 `m_state.load(acquire) != kRunning` 直接
   返回 false，进锁后再复核（`run()` 正常返回时永远不是 kRunning）；
2. `run()` 取 Fiber：新增 `std::atomic<Fiber*> m_fiber_ptr`（写侧持
   `m_transition_mutex` release 发布；`run()` 侧 acquire 读），删掉每次
   resume 前的 transition 锁；
3. `worker_loop`：`t_task = std::move(task)` / `task = std::move(t_task)`
   替代拷贝 + `reset()`，消除每轮 2 次 shared_ptr 原子引用计数。

**风险**：`m_fiber_ptr` 的写点只有「首次创建」与「终态回收」，都在
`m_run_claim` 持有者的临界区内，其它线程只读 → 安全；但新增原子成员同样是
布局变化。

---

## 3. P0-4：删掉 `resume_claim` 的两次 CAS（设计）

### 3.1 现状（`src/concurrency/fiber.cpp`）

```cpp
// resume_from_scheduler()
bool expected = false;
if (!m_resume_claim.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
...
const bool result = resume_locked(false, true);
m_resume_claim.store(false, std::memory_order_release);   // 第二次写
return result;
```

`m_resume_claim` 还有第二个用途：`~Fiber` 的 `try_claim_for_destroy()`
（FIBER-02）用它保证「析构时没有线程正在 resume」。`~Impl()` 里
`if (!m_resume_claim.load(acquire)) std::terminate();`。

### 3.2 方案 A（推荐，风险低）：热路径 CAS → store，保留 claim 语义

```cpp
bool resume_from_scheduler() noexcept {
    // 调度器所有权协议保证：同一 G 同一时刻只有一个执行者
    // （Task::m_execution_claim + m_run_claim），因此这里不需要 CAS。
    // 只在 debug 构建保留并发检测。
#if !defined(NDEBUG)
    bool expected = false;
    if (!m_resume_claim.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
#else
    m_resume_claim.store(true, std::memory_order_release);
#endif
    ...
    m_resume_claim.store(false, std::memory_order_release);
    return result;
}
```

前提条件（必须写进注释并由测试守护）：
- 只允许 `Task::run()`（持有 `m_run_claim`）调用 `resume_from_scheduler`；
- `~Fiber` 只会在 Task 终态、registry 释放后发生（此时没有执行权），
  所以 `try_claim_for_destroy()` 仍能安全取得 claim；
- **公开的 `Fiber::resume()` 保持 CAS 不变**（用户可能跨线程 resume）。

收益：每次切换省 1 次 `lock cmpxchg`（约 5~7ns），往返 2 次切换 → **-6~10ns**。

风险：低，但需要一条「并发 resume_from_scheduler 会破坏状态」的 debug
断言；release 下每切换约省 5ns。

### 3.3 方案 B（更彻底，风险中高）：把所有权完全交给队列节点

P0-2/P0-3 落地后，dequeue 即 claim，节点所有权已经表达「谁在跑这个 G」。
此时可以：
- `resume_from_scheduler` 的 claim 只在 debug 构建存在；
- `~Fiber` 的 FIBER-02 契约改为「Task 终态 + registry 已摘除」的断言。

不建议与 P0-2/P0-3 同一 PR 提交——FIBER-02 是安全兜底，先留一个版本观察。

---

## 4. P1-3：runnext 快槽（设计）

### 4.1 Go 的语义（`proc.go:7529` `runqput`）

```go
// 伪码
if randomize || (runqhead == runqtail && ...) {
    // 放到 runnext，旧的 runnext 挤回队尾
}
```

`runnext` 的价值在 ping-pong：G1 park 后 G2 唤醒 G1，G1 直接进
runnext；G2 让出后下一个 `runqget` 先取 runnext，**跳过整个队列**。

### 4.2 Bento 的落点

- `Impl::Processor` 增加 `std::shared_ptr<Task> runnext`（或 P1-1 落地后
  用裸指针 + registry 保活）；
- `wake_registered` 的本地快路径：当
  `current_scheduler()==this && task->last_processor_id()==t_processor_id`
  且 `runnext` 为空时，直接把 G 放进 `runnext`（**在同一个 P 锁内**）；
- `worker_loop` 取任务：先看 `own_processor.runnext`（一次 load），命中则
  清空并直接 claim，**不再碰 deque**；
- `runnext` 被抢占时（已有值）退回原 `queue.push_back`。

### 4.3 风险与约束

1. `runnext` 里的 G 仍是「已 queued」语义：`m_queued` 必须为 true，
   `runnable` 计数必须加 1（与 deque 相同），否则 shutdown 的存活计数错乱；
2. 窃取者看不到 runnext → 若 P 的 worker 阻塞，该 G 不会被偷走。
   Go 的缓解是 `runqsteal` 也会偷 runnext；Bento 目前窃取路径必须**同时
   检查 victim.runnext**，否则 park 后可能饿死；
3. `runnext` 与 `local_queue_limit` 的溢出逻辑要一致（满时转全局队列）。

**预期**：ping-pong 场景 -5~-10ns；对吞吐无收益。

---

## 5. P1-2：Fiber 栈分档池（设计）

现状（分析文档 P1-2）：`Fiber::AcquireForScheduler` 走 growable/mmap，
池化后单次 `go()` 仍约 14.5µs。

方案：
- `Fiber::Impl` 旁增加进程级 `StackPool`：按 8/16/32/64 KiB 四档维护
  free-list（纯指针数组 + 自旋锁，无系统调用）；
- `AcquireForScheduler(stack_size)`：≤64KiB 且 `!stack_growable` 时从对应档
  取；取不到再 mmap；释放时按 `m_stack_size` 还回；
- guard page：池化栈在创建时一次性 `mprotect(PROT_NONE)` 底部页，复用时
  不需要重设；栈内容不做清零（安全：Fiber 入口不依赖零初始化）；
- 迁移/跨线程复用安全：池本身用自旋锁保护，栈对象与 Fiber 解耦。

风险：低（纯分配策略）。验收：`go()` 路径 P99、RSS 不增长、ASAN 长稳。
预期：`go()` 14.5µs → 1~2µs。

---

## 6. P1-4：自旋配额（设计）

现状：`worker_loop` 空闲时直接 `condition.wait`（`m_impl->mutex`），没有
「正在自旋找活的 M」计数，因此 `notify_worker` 无法判断「是否已有 M 在找活」。

方案（对齐 `proc.c:1190`）：
- `Impl` 增加 `std::atomic<std::size_t> spinning_workers`；
- worker 在「取任务前快速尝试」阶段 `fetch_add(1)`，进入阻塞等待前
  `fetch_sub(1)`；
- `notify_worker()` 只在
  `2 * spinning_workers < processors.size() - idle_workers` 时唤醒新 M；
- 与现有 `running_machines` 判定合并：`running == 0 && quota 允许` 才 notify。

风险：低（只影响唤醒决策，不影响任务执行）；需要 `scheduler_stress` 的长
稳验证「唤醒抑制不会导致 G 饿死」。

预期：省 CPU（非延迟）；高 GOMAXPROCS、低并行度场景避免所有 M 空转。

---

## 7. 与分析文档 P0 的衔接顺序（给 a8e07736 的建议）

| 顺序 | 改动 | 与其它项的耦合 | 预期累计 |
|---|---|---|---|
| 1 | §2.1 TLS 模型 | 无 | 123.8 → ~104ns |
| 2 | §2.2 自旋锁 | 无（但布局变化，需全量重建） | → ~90ns |
| 3 | §2.3/§2.4 去锁清理 | 与 P0-1 冲突（P0-1 会重写同函数） | → ~80ns |
| 4 | P1-3 runnext | 依赖 P0-2 的队列结构；可先于 P0-2 用 deque 实现 | → ~75ns |
| 5 | P0-2 环形队列 | 与 P1-3 同 PR 更省事 | → ~65ns |
| 6 | P0-1 CAS 状态机 | 与 §2.3 冲突（worker 唤醒路径重写） | → ~55ns |
| 7 | P0-3 dequeue 即 claim | 与 §2.2 的自旋锁共存 | → ~50ns |
| 8 | P0-4 resume claim | 依赖 P0-3 的所有权 | → ~45ns |
| 9 | P1-2 栈池 / P1-4 自旋配额 | 无 | 与延迟无关（吞吐/CPU） |

**冲突提醒**：第 3 步与第 6 步改的是同一批函数
（`wake_registered` / `wake_for_wait` / `requeue_from_worker`）。
建议 P0-1 直接吸收 §2.3 的 reserve 思路（在同一 CAS 里顺带置 `m_queued`
位），而不是先应用我的版本再重写。

---

## 8. 验收矩阵（每一步都要过）

| 项 | 方法 |
|---|---|
| 功能 | `GO2CPP_TEST_FILTER=scheduler` + `scheduler_stress` + `channel` + `parking_condition` + `fiber` + `future` |
| 本改动回归 | `tests/concurrency/test_worker_chain_opt.cpp`（7 场景：pingpong / MPMC channel / ParkingCondition / yield+Mutex / 4P work stealing / shutdown 竞态 / 嵌套 Fiber）——本工作树实测全 PASS |
| 竞态 | TSan（线程级，不启动 Scheduler 的约定不变）+ 3 轮随机 + 5 场景 |
| 长稳 | `stress_mixed_billion` ≥ 1e8 次 park/wake |
| 性能 | 同轮交替 A/B、min-of-N；perf 不可用时用 **callgrind 指令数**（对频率/抢占不敏感，本机实测比 wall-clock 稳定得多） |

### 8.1 本次踩过的坑（供参考）

1. **部分 .o 未按新布局重编 → 堆损坏**：`Task` 布局一变（锁 40B→1B），
   必须 `ninja -t clean` 或删构建目录；症状是 ld.so `_dl_fixup` 段错误，
   极难定位。
2. **构建脚本坏掉**：`cmake/Go2CppTests.cmake` 的
   `set_tests_properties(go2cpp_os_hook_conflict_test ...)` 在
   `if (TARGET go2cpp_hook)` 之外，`GO2CPP_WITH_HOOK=OFF` 时 CMake 直接失败
   （已在本工作树修复，见 diff）。
3. **主机噪声**：共享主机上 load 会到 100+，wall-clock 微基准的绝对值
   可漂移 2~3 倍；必须同轮交替 + min-of-N，或改用 callgrind 指令数。
4. **`std::mutex` 不是瓶颈的全部**：先去量 `__tls_get_addr`，再决定要不要
   动状态机——TLS 一项就值 20ns。

---

## 9. 交付物

- 本文件（只读设计）。
- 独立回归：`tests/concurrency/test_worker_chain_opt.cpp`（已加入工作树，
  未注册 ctest；`g++ -std=c++17 -O2 -Iinclude -pthread` 可直接编译）。
- 实测脚本与数据：`/tmp/wc_opt/`（`ab_fine.sh`、`bench_scheduler_park`
  编译产物、callgrind 输出），临时目录可删。
