# design · 06 接口签名汇总清单（第 10 轮，深化）

> 把散落在 `design/01`–`05` 与 `scenarios/` 各文档里的接口设计**汇总成一份清单**，
> 每个接口标明：**归属层 / 前置条件 / 失败语义 / 线程安全 / 默认开关 / 依据**。
> **本文件是设计清单，不是实现承诺**；签名是形状示意。

## 0. 分四组

| 组 | 数量 | 默认 | 触碰核心 |
|---|---:|---|---|
| **R. 只读接口** | 6 | 常开 | 需给 `Scheduler`/`Task`/`Mutex` 加方法 |
| **C. 受控接口** | 5 | **默认关闭** | 需加受控方法 + 前置校验 |
| **P. 插件接口** | 4（2 已有 + 2 新增） | 观察者可选；控制类默认空 | 观察者已有；选择器需加接入点 |
| **T. 传输/序列化接口** | 3 | 常开 | 否（独立库） |

---

## 1. R 组：只读接口（P0，风险最低，建议先做）

### R1 · 枚举全部 G

```cpp
// 设计形状
struct TaskSnapshot {
    GId id{0};
    GState state{GState::kNew};
    bool queued{false};
    bool started{false};
    bool cancellation_requested{false};
    PId last_processor_id{0};
    TaskClassId task_class{0};
    bool failed{false};
    // Fiber 侧（若该 G 已有 Fiber）
    bool has_fiber{false};
    FiberState fiber_state{FiberState::Ready};
    SuspendReason suspend_reason{SuspendReason::None};
    std::uint64_t fiber_id{0};
    FiberExecutionBinding binding{};
    std::size_t nesting_depth{0};
};
std::vector<TaskSnapshot> Scheduler::snapshot_tasks() const;
```

| 项 | 内容 |
|---|---|
| 前置 | 无（任意时刻可调用） |
| 失败语义 | 不抛；分配失败返回空或部分结果（需在实现中明确） |
| 线程安全 | 内部逐分片加锁（`registry_mutexes[16]`），**不一次持 16 把锁** |
| **一致性** | **分片级一致、全局非原子** —— 必须在调用方文档与输出中标注 |
| 依据 | `scheduler.cpp:1529`（注册表强持有非终态 G）、`1792-1805`、遍历范式 `1694-1705` |
| 注意 | 只复制**只读字段**；不长期持有 `shared_ptr` 之外的引用 |

### R2 · 某 P 的本地队列明细

```cpp
std::vector<GId> Scheduler::processor_queue(PId processor_id) const;
```

| 项 | 内容 |
|---|---|
| 前置 | `processor_id < processor_count()` |
| 失败语义 | 越界返回空 vector（或提供 `bool` 重载，需定案） |
| 线程安全 | 持 `Processor::mutex` 复制 id 列表 |
| 依据 | `Processor::queue`，`scheduler.cpp:1007-1021` |
| 缺口 | 现有 `ProcessorSnapshot` 只导出 `queued` 长度 |

### R3 · 某 G 的 Fiber 调用链

```cpp
FiberContextSnapshot Task::context_snapshot() const;
```

| 项 | 内容 |
|---|---|
| 前置 | 该 G 已有 Fiber（`m_fiber != nullptr`）；否则返回空 |
| 线程安全 | 转发 `Fiber::context_snapshot()`（`fiber.hpp:210`），内部自持锁 |
| 依据 | `fiber.hpp:206-211`（含墓碑帧语义） |

### R4 · 某 G 的执行绑定

```cpp
FiberExecutionBinding Task::execution_binding() const;
```

| 项 | 内容 |
|---|---|
| 注意 | 返回值是"**不拥有的标识**"（`fiber.hpp:32-34`），**使用前需校验调度器仍存活** |
| 依据 | `fiber.hpp:215` |

### R5 · 元数据查询

```cpp
struct TaskMetadata {
    std::string name;         // 例如 s_main_task
    std::string description;
    std::string function;     // 例如 void worker_loop(int)
    std::string file;
    std::uint32_t line{0};
    bool present{false};      // 区分「未注册」与「无来源」
};
std::optional<TaskMetadata> Scheduler::metadata(GId id) const;
// 注册侧（路线待定案，见 FINAL-REPORT §7 第 1 项）
void Scheduler::register_metadata(GId id, TaskMetadata metadata);
```

| 项 | 内容 |
|---|---|
| **前置** | 元数据机制必须**先定案**（转译期注入 / 运行期注册 / 共存） |
| 失败语义 | 未注册返回 `nullopt`；**输出层必须把"未注册"与"字段无来源"分开呈现** |
| 依据 | `Task` 只有私有 `m_function`（`scheduler.hpp:362`）；`context.cpp` 无位置 API（grep 计数 0） |

### R6 · 锁的只读快照

```cpp
struct MutexDebugSnapshot {
    bool held{false};
    bool has_waiters{false};
    std::size_t waiters{0};
    std::size_t native_waiters{0};
    std::size_t fiber_waiters{0};
    // 可选且默认关闭（需 owner 追踪；开启后与生产行为不同）：
    // GId owner_task{0}; std::thread::id owner_thread{};
};
MutexDebugSnapshot Mutex::DebugSnapshot() const;
```

| 项 | 内容 |
|---|---|
| 线程安全 | `m_state`/`m_native_waiters` 原子读；`m_waiters.size()` **需持队列锁** |
| **做不到** | **持有者身份不可知**（`m_state` 不记录 owner）—— 必须在文档与输出中声明 |
| 依据 | `sync.cpp:665-686` |

---

## 2. C 组：受控接口（P4/P5，**默认关闭 + 前置校验**）

> 统一约定：**编译期默认关闭 + 运行期显式开启**（双重）；失败**不做任何改动**并返回 `false`
> 或错误；所有接口**必须在 dump 输出中标注"调试控制面已启用"**。

### C1 · 跨 M/P 迁移

```cpp
bool Scheduler::migrate(GId id, PId target_processor);
```

| 项 | 内容 |
|---|---|
| 前置 | G 存在、非终态、**未在执行**（`m_execution_claim == false`）；`target_processor` 合法 |
| 语义收窄 | ⚠️ 现有 `requeue_to_processor` 要求 `try_mark_queued()` 成功 → **只接受"当前未入队"的 G**；已在队列中的需**新增"从原 P 摘除"**能力 |
| 依据 | `scheduler.cpp:2527-2574`（含失败回滚与克制的唤醒策略） |
| 风险 | 中（队列归属变更，需与 work stealing 正确协同） |

### C2 · 插队

```cpp
bool Scheduler::enqueue_before(GId moving, GId target);
```

| 项 | 内容 |
|---|---|
| 前置 | 二者都在**同一个 P 的队列**中（否则报错而非静默失败） |
| 实现位置 | 唯一本地入队点是 `processor.queue.push_back`（`scheduler.cpp:2547`）；`deque` 支持中间插入 |
| 必须持锁 | `Processor::mutex` |
| **默认关闭原因** | 破坏 FIFO 公平性（现有设计的隐含约定） |
| 风险 | 中高（影响延迟分布，测试需覆盖顺序性与饥饿） |

### C3 · 替换函数 / 延迟替换

```cpp
// 语义 A：仅对「尚未首次运行」的 G 有效
bool Task::replace_before_first_run(Task::Function function);
// 语义 B（推荐）：当前主体返回后，再入队执行一次 —— 用现有 spawn 即可，无需改 Task
//   由 debug/control 层封装，不属于核心接口
```

| 项 | 内容 |
|---|---|
| **硬约束** | `m_function` 在 Fiber **首次进入时被 `std::move` 移出**（`scheduler.cpp:830-905`）→ 对已运行的 G 替换**无效** |
| 禁止 | ❌ 对**已挂起但栈存活**的 G 替换函数体（会破坏栈上 RAII 不变量） |
| 依据 | `release_callable()`（`126-130`）及其 6 个"放弃执行"调用点 |

### C4 · 协作式断点

```cpp
// 设计形状
enum class BreakpointKind { kOnFiber, kOnState, kOnLockWait };
bool DebugController::SetBreakpoint(GId id, BreakpointKind kind);
// 命中后：G 在**安全检查点**停下（不是任意行）
```

| 项 | 内容 |
|---|---|
| **硬约束** | 项目明确「不会从另一个线程强行切断任意 C++ 调用栈」→ 只能停在协作安全点 |
| 安全点候选 | `Fiber::Suspend`/`SuspendForScheduler`、`Task::park_for_scheduler`、`mark_yielded*`、`Scheduler::yield_current`/`park_current` |
| 标志机制 | 可复用与取消标记同款（Fiber 本地标记 + `CancellationRequested()` 同构） |
| **必须写进 CLI 帮助** | 「本工具只能停在安全点，不能停在任意行；任意行断点请用 gdb」 |
| 未确认 | 安全点分布与数量**未统计** → 精度无法承诺 |
| 依据 | `scheduler.hpp:399-400,409-412,511-524`；`fiber.hpp:156,161-166` |

### C5 · 锁争用选择钩子

```cpp
// 设计形状（在持队列锁时调用；不得阻塞、不得抛异常）
class WaitSelector {
public:
    virtual ~WaitSelector() = default;
    virtual WaitNode* Select(std::deque<std::shared_ptr<WaitNode>>& waiters) noexcept = 0;
};
// 接入：Mutex::Impl 增加 std::shared_ptr<WaitSelector> selector（默认空 = 现有 FIFO 行为）
bool sync::SetWaitSelectorFactory(WaitSelectorFactory factory);   // 形状参考 WaitBackend
```

| 项 | 内容 |
|---|---|
| **必须遵守** | ① 在持队列锁时选择；② 选中后仍要 `TryFinish(kNotified)` 认领（失败回退 FIFO）；③ **不得**让未选中者重新竞争；④ **必须处理饥饿**（建议"被跳过次数上限，超限强制 FIFO"） |
| **绝不能做** | ❌ 「先释放锁再让所有人重新竞争」—— 会破坏 `Unlock` 的「所有权无缝转移、`TryLock` 不能插队」语义 |
| 正交性 | 与 `HybridMutex`（自旋次数）、`WaitBackend`（park/wake）**互不重叠**，可同时启用 |
| 依据 | `sync.cpp:955-1005`（出队逻辑）、`sync.hpp:13-66`（WaitBackend 只管 park/wake） |
| 风险 | **高**（改公平性与尾延迟，必须默认关闭） |

---

## 3. P 组：插件接口

| # | 接口 | 状态 | 用途 | 关键约束 |
|---:|---|---|---|---|
| P1 | `SchedulerObserver::OnEvent` | ✅ **已存在**（`scheduler.hpp:140-183`） | 事件流 | **不能控制执行权**；回调在**锁外**执行；异常被隔离 |
| P2 | `WaitBackend`（`Park`/`ParkForCleanup`/`Wake`） | ✅ **已存在**（`sync.hpp:13-66`） | 换 park/wake 机制 | 只影响**之后创建**的等待节点 |
| P3 | `DebugSink`（`Write`/`Flush`） | 🆕 新增 | 输出后端（stdio/文件/缓冲/topic） | `Write` **必须非阻塞**（回调在 worker 线程） |
| P4 | `DebugSource`（`Poll`） | 🆕 新增 | 记录来源（事件/周期快照/手动） | 消费在独立线程 |

---

## 4. T 组：传输与序列化

| # | 接口 | 用途 | 约束 |
|---:|---|---|---|
| T1 | `Serialize(snapshot, Format) -> std::string` | text/ini/json | json 复用项目 `json.hpp` |
| T2 | `Publish(topic, message)` | topic 传输（Stratum 式适配点） | **发布端绝不阻塞**；有界队列 + 丢弃计数 |
| T3 | `Record` / `Replay` | 录制回放 | 回放**只重建视图**，不能重建运行时 |

---

## 5. 接口间的依赖顺序

```
R1,R2 (枚举) ──> R3,R4 (per-G 详情) ──> T1 (序列化) ──> P3 (sink) ──> CLI
R5 (元数据) ────┘（需先定案注入方式）
R6 (锁快照) ──> C5 (选择钩子，需 R6 的观测能力配合验证)
C1,C2 (迁移/插队) ──> C3 (替换) ──> C4 (断点)
T2 (topic) ──> 多机（需时钟方案定案）
```

**关键路径**：`R1/R2 → T1 → P3` 即可产出可用的 dump；其余为增强。

---

## 6. 本清单的自我约束

- 所有"依据"列都是本会话实际读过的源码位置；**未读到的实现细节不写结论**；
- 签名是**形状示意**，参数与返回类型可能随实现调整；
- 标 ⚠️ 的语义收窄（C1）与标 ❌ 的禁止做法（C3 改栈、C5 重新竞争）**不是建议而是硬约束**，
  依据均来自项目自身注释或实现；
- C 组全部要求"默认关闭 + 显式启用 + 前置校验 + 输出标注"，这是设计红线。
