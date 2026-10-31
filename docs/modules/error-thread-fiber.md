# 模块文档：Error 工作状态与线程 / Fiber 对齐

> 适用基线：`feat/isolated` 工作树 `/UserData/CodexWorkSpace/Go2cpp`（2026-10-31）。
> 源码：`include/go2cpp/error/error_local.hpp`、`include/go2cpp/error/error_fiber.hpp`。
> 状态：**现行**。本文替代此前散落在各模块注释里的 thread_local 约定。

## 1. 为什么需要 ErrorLocal

本项目是 M:N 调度：一个逻辑执行体 G（Fiber）会在多个 OS 线程 M 之间迁移。
C++ 的 `thread_local` 绑定的是 **M**，不是 G。把「每执行体独立」的状态放在
`thread_local` 上有两种故障模式：

1. **状态丢失**：G 在 M1 写入，挂起后迁移到 M2，M2 读到的是空的 / 默认值；
2. **状态串线**：G1 在 M1 写入，挂起后 G2 在同一个 M1 上跑，读到了 G1 的值。

`ErrorLocal<T>` 把「存储在哪」收敛成一个运行期后端：

- 默认后端 = 线程局部（与裸 `thread_local` 等价，零 Fiber 依赖）；
- Fiber 后端 = `FiberLocal<T>`，值绑定 G、随迁移走、G 结束回收；在 Fiber 外
  自动退化为线程局部。

错误体系的**工作状态**（而非不可变错误对象）都走这一层：深链析构工作表、
最近错误槽、defer 回调边界与异常记录。错误对象本身仍然是不可变
`shared_ptr<const Error>`，可以自由跨线程/Fiber 共享，不需要 ErrorLocal。

## 2. 设施与不变量

### 2.1 `ErrorLocal<T>`

| 操作 | 语义 | 异常 |
|---|---|---|
| `GetOrCreate(args...)` | 当前上下文没有值时用参数构造并保存 | 可能抛（`make_shared`） |
| `TryGet()` | 只读，不存在返回 `nullptr` | `noexcept` |
| `Reset()` | 清除当前上下文的值（先摘出容器再析构） | `noexcept` |
| `SlotId()` | 诊断用槽位 ID（永不复用） | `noexcept` |

槽对象（`ErrorLocal<T>` 实例）必须**活得比使用它的值更久**，通常用函数局部
静态或成员持有。槽位 ID 进程内单调递增、永不复用：复用会让新槽读到旧值。

### 2.2 生命周期

- 线程后端：值由 `ThreadLocalValues` 持有，线程退出时按「波次」清空——某个值
  的析构函数若又创建了新的 `ErrorLocal` 值，会落进已清空的表，由下一轮收走；
- Fiber 后端：值由 G 的映射表持有，G 的 trampoline 结束时 `Cleanup` 回收；即使
  还有别处引用，`shared_ptr` 语义也保证不会泄漏。

### 2.3 安装与 DSO

`error_fiber.hpp` 在静态初始化阶段自动安装 Fiber 后端（可用
`GO2CPP_ERROR_FIBER_NO_AUTO_INSTALL` 关闭后手动调用
`InstallFiberErrorLocalStorage()`）。安装是单向的：已安装 Fiber 后端后再装其他
后端返回 `false`，避免运行中把状态分裂到两个存储。

Linux/GCC 构建下，`error_local.hpp` 的后端指针与 `kThreadStorage` 都是
`STB_GNU_UNIQUE` 符号，动态链接器会把多个 DSO 的副本合并成同一份；`inline`
函数局部静态同样被合并。因此**主程序（或任一 DSO）安装一次 Fiber 后端，所有
共享库里的 `ErrorLocal` 都能看到**（实测见 §6.3）。这条性质依赖 GCC 默认的
`-fgnu-unique`；换 clang 或 `-fno-gnu-unique` 时，每个 DSO 需要各自 include
`error_fiber.hpp`，否则回退为线程后端。

安装必须发生在**使用 ErrorLocal 值之前**。安装前写入的值留在旧后端，不会迁移：
这是有意的，避免在静态初始化阶段引入锁与分配。

## 3. A/B/C 分类

判定问题（按顺序）：

1. 这个值描述的是「当前逻辑执行流」吗（是否在 defer 回调内、最近错误、
   重入深度……）？G 迁移后它还必须成立吗？→ **A 类**，迁 `ErrorLocal`；
2. 它描述的是进程、M 或线程级配置/运行时环境吗（调度器、线程策略、M 的
   errno 槽）？→ **B 类**，保留 `thread_local`，由执行体切换代码显式维护；
3. 它只是缓存/池/采样器，且只在同步调用内使用（不跨挂起点持有语义）？→
   **C 类**，保留；但要确认它按 M 采样或在同一调用栈内成对使用。

### 3.1 A 类：每执行体独立，迁移后必须保留

| 文件 | 符号 | 状态 |
|---|---|---|
| `src/tools/control_flow.cpp` | `s_defer_depth` / `s_defer_exception` → `DeferState` | **本次迁移**到 `ErrorLocal<DeferState>` |
| `include/go2cpp/error/error.hpp` | `detail::DestroyTable()` | 已迁移（`ErrorLocal<DestroyWorkTable>`） |
| `include/go2cpp/error/error.hpp` | `CurrentErrorSlot()` | 已迁移（`ErrorLocal<ErrorPtr>`） |
| `src/logging/log.cpp` | `t_submit_depth` | A 类观察项；不在本任务边界，建议后续迁移 |
| `src/network/hook.cpp` | `s_real_call_depth` / `s_resolving` / `s_bound_manager` | A 类观察项；不在本任务边界，建议后续迁移 |
| `include/go2cpp/bento/net/log_hook.hpp` | `t_emit_depth` | A 类观察项；不在本任务边界，建议后续迁移 |

### 3.2 B 类：进程 / M / 线程级，保留

| 文件 | 符号 | 保留理由 |
|---|---|---|
| `src/concurrency/scheduler.cpp` | `t_scheduler`、`t_task`、`t_machine_id`、`t_processor_id`、`t_blocking_machine_id`、`t_in_idle_poll`、`t_incoming_lock_depth`、`t_incoming_stripe` | M 的调度环境；G 切换时由调度器显式设置/恢复，不能跟随 G |
| `src/concurrency/fiber.cpp` | `s_current_fiber`、`s_main_execution`、`t_errno_slot`、Fiber id 计数器、`t_next_fiber_name` | Fiber 运行时自身的「当前执行体」指针与 M 级缓存；切换代码维护 |
| `src/concurrency/thread_policy.cpp` | `s_policy_scheduler`、`s_policy_participation`、`s_policy_hook`、`s_runtime_worker` | 线程级参与策略，属于 M |
| `src/concurrency/fiber_local.cpp` | `s_thread_values` | FiberLocal 在 Fiber 外的线程后备实现 |
| `src/network/io.cpp` | `t_inside_poll` | 每 M 的 poll 重入标记，属于事件循环线程 |
| `include/go2cpp/error/error_local.hpp` | `ThreadValues()::values` | 默认线程后端的实现本体 |
| `src/bento/net/token_pairing.cpp` | `random_device device` | 每线程随机源（`random_device` 不保证线程安全） |

### 3.3 C 类：纯优化缓存 / 池 / 采样，保留

| 文件 | 符号 | Fiber 安全依据 |
|---|---|---|
| `src/concurrency/fiber.cpp` | `StackCache`、`FiberBin`、growable altstack 标记 | 按 M 复用栈/Fiber，切换代码成对使用；不承载 G 语义 |
| `src/concurrency/timer.cpp` | `ThreadTimerTag tag` | 仅给当前 M 的定时器打标，注册/取消在同一调用内 |
| `src/concurrency/sync.cpp` | `t_wait_node_cache` | 等待节点在同一同步调用内借出/归还 |
| `src/concurrency/parking_condition.cpp` | `LocalPool pool`、`t_wake_list` | 同上；park/wake 在同一 M 的调用栈内成对 |
| `src/concurrency/channel.cpp` | 每线程可复用 `state` | Select 轮询的临时状态，不跨挂起 |
| `src/concurrency/context.cpp` | 分配器 `head/count`、`t_draining_state_releases` 及待释放表 | 析构重入保护/分配缓存；只在同步析构期间使用 |
| `src/debug/debug_event_ring.cpp` | `t_ring`、`t_thread_id`、`t_local_seq`、`t_processor_id`、`t_bound_task`、hint 槽、采样计数器、`t_registration`、`t_totals` | 事件环按 M 采样；G 迁移后按新 M 采样是正确的诊断语义 |
| `src/bento/error/error.cpp` | `g_release_state` | 同步释放队列：入队后立即 `DrainReleaseQueue()`，不跨挂起点 |
| `src/tools/error.cpp` | `t_error_release_work` | 同上：旧 `tools/error.hpp` 的析构工作表是同步迭代释放 |
| `src/os/proc.cpp` | `unknown[32]` | 格式化未知状态的静态缓冲，纯优化 |
| `include/go2cpp/concurrency/channel.hpp` | `budget`、`pool`、`generation` | 每 M 的 Select 轮询预算/节点池 |
| `include/go2cpp/concurrency/recursive_mutex.hpp` | `token` | 递归深度标记，同一调用栈内成对 |
| `include/go2cpp/debug/debug_events.hpp` | `StagingSlot slot` | 事件暂存，写完即提交 |

> 注意：C 类「保留」的前提是**不跨挂起点持有语义**。如果将来某个缓存需要在
> Fiber 挂起后继续使用（例如把等待节点缓存在 G 上），必须重新按 A 类处理。

## 4. 本次迁移：defer 边界与异常

### 4.1 问题

`src/tools/control_flow.cpp` 原来：

```cpp
thread_local unsigned int s_defer_depth = 0;
thread_local std::exception_ptr s_defer_exception;
```

`defer` 的析构在 G 的栈上执行回调；如果回调里调用 `Fiber::Suspend`（清理逻辑
里等待 I/O 是常见写法），G 可能在 M2 恢复：

- `recover::take()` 在 M2 检查 `s_defer_depth( M2 ) == 0`，误判「不在 defer
  边界」，panic 无法恢复；
- `LastDeferException()` 在 M2 读到另一个线程的异常（或空）。

### 4.2 实现

两个字段合并成 `DeferState { unsigned int depth; std::exception_ptr exception; }`，
由一个 `ErrorLocal<DeferState>` 槽承载：

- `InDeferCallback()`：`TryGet()` 后判断 `depth != 0`，不分配；
- `DeferCallbackScope`：构造时 `GetOrCreate()` 并把 `depth` 加 1，析构时恢复
  旧值；作用域对象在 Fiber 栈上，只保存槽内状态的裸指针（Fiber 后端下状态
  属于 G，迁移不改变地址）；
- `RecordDeferException()` / `LastDeferException()` / `ClearDeferException()`：
  读写同一槽的 `exception` 字段。

`defer` 的析构路径是 `noexcept`，而 `GetOrCreate()` 可能因分配失败抛异常：
`DeferCallbackScope` 与 `RecordDeferException` 都捕获并吞掉分配失败。退化为
「本次不标记 defer 边界 / 不记录异常」，而不是让异常穿过 noexcept 边界。

### 4.3 影响

- 普通线程：默认后端仍是线程局部，行为与原实现一致；
- Fiber：安装 Fiber 后端后，边界与异常记录随 G 迁移；
- 性能：`InDeferCallback()` 从一次 TLS 读变成一次后端函数指针调用 + 哈希查找；
  defer 本身不在热路径（每次析构执行一次用户回调），可接受；
- 嵌套 defer：`depth` 计数保留，恢复旧值语义不变。

## 5. 审计结论：errno / LastError / 当前 Context

### 5.1 Socket 的 errno

`src/network/socket.cpp` **没有自建 `thread_local` errno**。它直接使用 libc 的
`errno`（每线程 TLS），并且：

- 每个可能返回错误的点都在**当前 M** 重新写 `errno`（socket.cpp 注释明确说明
  Fiber 挂起后 errno 会换线程副本，不能沿用 park 前的值）；
- 公开 API 通过 `SocketResult.error` / 返回值显式传错误码，不要求调用方跨
  挂起点读 `errno`；
- Fiber 运行时在 `suspend()` 保存 `m_saved_errno`、`resume()` 时 `swap_errno`
  恢复，G 迁移后 errno 仍属于该 G（前提是访问方没有缓存 `__errno_location()`
  的地址；见 §6.1 的测试注记）。

因此**不需要**给 Socket 增加 `ErrorLocal<int>`：那会与 libc 的 errno 语义重复，
且无法替代 syscall 返回值。需要「跨挂起点的最近错误」时用
`error::SetCurrentError()` / `CurrentError()`（已基于 ErrorLocal）。

### 5.2 LastError

`include/go2cpp/error/registry.hpp` 的 `LastErrorCode()` / `LastErrorDetail()`
是 `Registry` 单例的**实例成员 + mutex**（进程级诊断状态），不是 per-execution
状态，属于 B 类，保留。

新错误体系的「最近错误」是 `CurrentErrorSlot()`，已经是
`ErrorLocal<ErrorPtr>`，本次不需要再改。

### 5.3 当前 Context

`Fiber::CurrentContext()` / `CurrentContextSnapshot()` 读取 `s_current_fiber`，
这是 Fiber 运行时的「当前执行体」TLS 指针，由 `resume()`/`suspend()` 显式设置
和恢复（`previous_fiber` 机制），属于 B 类。项目里没有独立的
`thread_local Context*`；`Context` 对象由调用方持有，不放在 TLS 上。

## 6. 验证

### 6.1 新增测试：`tests/error/test_control_flow_fiber.cpp`

7 个用例、68 个断言，全部通过：

| 用例 | 覆盖 |
|---|---|
| `defer_boundary_follows_migration` | M1 的 defer 回调里挂起，M2 恢复后仍在边界内 |
| `defer_exception_follows_migration` | M1 记录异常，M2 读到同一个 `exception_ptr`；Clear 生效 |
| `defer_state_isolated_between_fibers` | A 在 defer 回调里挂起时，B 与主线程看不到 |
| `defer_state_reclaimed_after_fiber` | G 结束后状态回收，新 G 不可见 |
| `defer_exception_thread_isolation` | 普通线程之间互不可见（线程后端不退化） |
| `errno_follows_migration` | errno 在 M1 设置、M2 恢复后仍为原值 |
| `nested_defer_follows_migration` | 嵌套 defer 的深度跨迁移保持 |

另有一个不 include `error_fiber.hpp`、不链接 Fiber 运行时的
`tests/error/test_error_local_thread.cpp`：验证默认后端仍是 `kThread`、
线程之间互不可见、`Reset` 只影响当前线程（线程后端不退化）。

> errno 用例通过 noinline 的 `ReadErrno()/WriteErrno()` 访问：编译器可能把
> `__errno_location()` 的结果缓存到寄存器并跨 `Fiber::Suspend` 复用，直接读
> `errno` 会读到旧 M 的槽（实测 M2 读到 M1 的 55）。这不是运行时缺陷，而是
> C++ 编译器不知道上下文切换的固有边界；业务代码不要依赖 errno 跨挂起点。

构建与运行：

```bash
cmake -S tests/error -B /tmp/error-align-build -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -C /tmp/error-align-build
ctest --test-dir /tmp/error-align-build --output-on-failure
```

### 6.2 回归

- `tests/error` 全量 4 个 ctest 用例通过：`error_enhanced`、
  `error_local_thread`、`error_fiber`、`error_control_flow_fiber`；
- 主构建 `.build-bento-wsl` 的 `go2cpp_tests` 全量运行：control_flow 相关用例
  通过；全量失败数与改动前基线一致（仅 `test_spin_mutex` 的直方图计数断言
  5 处，属既有失败，与本次改动无关）。

### 6.3 DSO 场景

主构建是 `BUILD_SHARED_LIBS=ON`，`defer` 的实现位于
`libgo2cpp_control_flow.so`，而 Fiber 后端由用户 TU（或主程序）安装。用探针
程序链接主构建的 `.so` 验证：

```text
backend=1 (1=fiber)
backend=1 tid_before=78450 tid_after=78451 in_before=1 in_after=1
DSO_PASS
```

即：可执行里安装的后端，`control_flow.so` 内的 `InDeferCallback()` 能看到，
跨 M 迁移后仍为真（依赖 GCC `STB_GNU_UNIQUE` 合并，见 §2.3）。

## 7. 使用规范

1. **A 类状态一律用 `ErrorLocal`**：判断标准是「G 迁移后它还必须成立吗」。
   典型：defer 边界、最近错误、析构工作表。
2. **槽对象放静态或成员**：`static ErrorLocal<T> slot;`（函数局部静态）或类
   成员；不要放在栈上临时构造，否则槽 ID 随对象销毁而失效（ID 不复用，但
   值会留在后端直到上下文结束）。
3. **安装时机**：Fiber 程序在创建任何 ErrorLocal 值之前 include
   `go2cpp/error/error_fiber.hpp`（或手动 `InstallFiberErrorLocalStorage()`）。
   不要把安装放在会与静态初始化竞争的早初始化路径里。
4. **noexcept 函数内先 `TryGet()`，需要创建时捕获分配失败**：`GetOrCreate()`
   会分配，不能让它穿过 `noexcept` 边界。
5. **不要在 ErrorLocal 值里放「跨 G 共享的可变状态」**：值属于当前 G，其他 G
   看不到；需要共享时用显式参数/共享对象。
6. **保留 thread_local 的三条理由**：M/线程级运行时状态（B 类）、纯缓存/池
   （C 类）、只在同步调用内成对使用（C 类）。写新代码时如果拿不准，先问
   「G 迁移后这个值还成立吗」。
7. **errno 不要跨挂起点读**：Socket 等 I/O 封装应显式返回错误码；需要「最近
   错误」语义时用 `CurrentErrorSlot`。
8. **DSO**：主程序统一安装后端时，GCC `-fgnu-unique` 下所有 DSO 共享；若用
   clang 或关闭 unique，每个使用 ErrorLocal 的 DSO 都要各自 include
   `error_fiber.hpp`。

## 8. 遗留与建议

- `src/logging/log.cpp` 的 `t_submit_depth`、`src/network/hook.cpp` 的
  `s_real_call_depth` / `s_resolving` / `s_bound_manager`、
  `include/go2cpp/bento/net/log_hook.hpp` 的 `t_emit_depth` 都属于 A 类，但不在
  本次任务的改动边界（`src/tools/control_flow.cpp`、`include/go2cpp/tools/**`、
  `src/network/socket.cpp`、`include/go2cpp/error/**`、`tests/**`）。建议后续按
  同样模式迁移；hook 的 `s_bound_manager` 还需要明确「绑定 IOManager 是 M 级
  还是 G 级」的设计问题。
- `src/tools/error.cpp` 与 `src/bento/error/error.cpp` 的同步释放工作表保持
  `thread_local`（C 类），不要为了「统一」把它们迁到 ErrorLocal：它们不跨
  挂起点，迁移只会增加分配与查找开销。
