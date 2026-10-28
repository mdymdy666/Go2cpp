# 模块文档：fiber（栈式 Fiber）

> **状态：现行**。基线 `HEAD 1b46de8`（2026-10-26 核对）。
> 本文是 `fiber` 模块的**唯一现行文档**。合并来源：`docs/fiber_guide.md`、
> `docs/fiber-growable-stack-design.md`、`docs/fiber-migration.md`、
> `docs/coost-comparison.md`（四篇顶部均已加合并标注，原文保留作追溯）。
>
> ⚠️ **两件容易混淆的事**：本模块有两种「迁移」——
> **§6.1 同进程跨 OS 线程迁移**（运行时常规能力，G 在 M 之间搬）与
> **§6.2 跨进程快照迁移**（实验特性，`GO2CPP_MIGRATION_API`）。二者机制、API、
> 限制完全不同，旧文档常混写，阅读时先分清。
>
> 旧文档的行号已全面漂移（`fiber.cpp` 从 1,502 行涨到 3,270 行，`fiber.hpp` 从
> 228 行到 493 行）。本文所有 `file:line` 都是 2026-10-26 重新核对的；**不要**从旧
> 文档抄行号。

---

## 1. 定位与边界

Fiber 是 G 的**执行载体**：一份独立栈 + 一份保存的寄存器上下文。
`Scheduler` 决定「下一个跑谁」，`Fiber` 负责「怎么把 CPU 交给它 / 从它拿回来」
（`include/go2cpp/concurrency/fiber.hpp:5-10`）。

| 维度 | 契约 |
|---|---|
| 依赖方向 | Fiber **单向**依赖上下文后端；**不依赖也不拥有** `Scheduler`（`fiber.hpp:269-275`） |
| resume 串行 | 同一 Fiber 的 `resume()` 必须由调用方串行；并发 resume 返回 false（`m_resume_claim`，`fiber.cpp:1777`） |
| 谁负责迁移 | 跨 OS 线程恢复由调度器保证；Fiber 本身不挑线程 |
| 栈的所有权 | Fiber 自有独立栈（不是 OS 线程栈），因此可以换线程继续跑 |
| 异常边界 | 用户异常不穿过 Fiber 边界；存进 `failure()` 并置 `Failed`（`fiber.cpp:1324-1335`） |
| 谁不该直接用 | 在 managed G 里裸调 `Fiber::resume()` 会绕过调度器；普通用户代码应使用 Scheduler 提交任务 |

`include/go2cpp/fiber.hpp` 是 4 行转发头，真身是
`include/go2cpp/concurrency/fiber.hpp`（493 行）。

---

## 2. 文件布局

| 文件 | 内容 |
|---|---|
| `include/go2cpp/concurrency/fiber.hpp` | `FiberState`/`SuspendReason`/`FiberExecutionBinding`/`FiberContextFrame`/`FiberStackDebugSnapshot`/`Fiber`，以及迁移 API（493 行） |
| `src/concurrency/fiber.cpp` | `FiberStack`、SIGSEGV 增长 handler、栈缓存、`FiberBin`、`FiberRecord`、`Fiber::Impl`、迁移实现（3,270 行） |
| `include/go2cpp/concurrency/detail/context_backend.hpp` | `MakeContext` / `JumpContext` 的 ABI 声明（`Context`、`Transfer`） |
| `src/concurrency/context_x64.S` | x86_64 换栈汇编（83 行） |
| `include/go2cpp/concurrency/fiber_local.hpp` | `FiberLocalCache<T>`（逻辑 G 局部存储，见 §8） |
| `include/go2cpp/concurrency/thread_policy.hpp` | 线程策略作用域（只写 TLS 元数据，见 §8） |
| `example/concurrency/fiber_sync_demo.cpp` | 可运行示例 |

---

## 3. 快速上手

```cpp
#include "go2cpp/fiber.hpp"

using go2cpp::Fiber;
using go2cpp::SuspendReason;

Fiber f([] {
    int local = 40;
    ++local;                       // 41
    Fiber::Suspend(SuspendReason::Yield);   // 回到 resume 方
    ++local;                       // 42：从挂起点继续，栈上局部变量仍在
});

f.resume();                        // 跑到 Suspend，返回 caller
f.resume();                        // 从 Suspend 之后继续，直到主体返回
// f.state() == FiberState::Completed
```

- 首次 `resume()` 才分配栈并创建上下文（`initialize_context`，`fiber.cpp:1708-1740`）。
- `Fiber::Suspend()` 在**非 Fiber 线程**返回 false；因此
  `GO2CPP_CHECK(!Fiber::Suspend())` 可以作为前置断言（`tests/concurrency/test_fiber.cpp:44`）。
- 需要显式失败信息时用 `resume_result()`（`fiber.hpp:348`），返回
  `FiberResumeResult{accepted, state, failure, context_snapshot}`（`fiber.hpp:255-265`）。
- 挂起原因会记录在 `suspend_reason()`：`None/Yield/Park/Io/Timer/Synchronization`
  （`fiber.hpp:173-180`），**只用于观测**，不改变调度决策。

---

## 4. 状态机与生命周期

`FiberState`（`fiber.hpp:164-170`）：`Ready → Running → Suspended → Running → ...
→ Completed | Failed`。

| 操作 | 前置状态 | 结果 |
|---|---|---|
| `resume()` | `Ready`（首次）或 `Suspended` | 切进去；返回时挂起或终态 |
| `Suspend(reason)` | 当前线程正在执行本 Fiber（`Running`） | `Suspended`，控制权回到 caller |
| 主体正常返回 | `Running` | `Completed`，`m_context`/`m_stack` 置空（`fiber.cpp:1585-1595`） |
| 主体抛异常 | `Running` | `Failed`，异常存进 `failure()` |
| 析构 `Ready` Fiber | `Ready` | 跳过主体，直接 `Completed`（`fiber.cpp:1256-1265`） |
| 析构 `Suspended` Fiber | `Suspended` | 置取消 + 由固定父级 resume，等它自然结束（`fiber.cpp:1267-1290`） |
| 析构时父级不可恢复 | `Suspended` | `abandon_suspended()` → `Failed`，**不伪造 Completed**（`fiber.cpp:1198-1230`） |

关键约束：

1. **不能在 Fiber 自己的栈上析构它**：`Impl::~Impl()` 检测
   `s_current_fiber == m_owner` 时 `std::terminate()`（`fiber.cpp:1232-1237`）。
2. **析构可能阻塞**：主体若忽略取消、永不返回，析构/`join`/`shutdown` 会一直等——
   这保证挂起栈上的 RAII 不被跳过（`fiber.hpp:322-328`）。
3. **生命周期闸门是 `m_resume_claim`**：析构先取得 claim 再观察终态，避免
   「刚发布 Completed、resume 还没释放 claim」的窗口里释放上下文导致 UAF
   （`fiber.cpp:1239-1248`）。
4. **错误父级路径不 terminate**：`abandon_suspended()` 标 `Failed`、清理
   `fiber_local` 值、清空上下文；代价是**无法展开挂起栈上的局部 RAII**
   （`fiber.cpp:1198-1230`），所以规范用法始终由固定父级收尾。

---

## 5. 栈管理

### 5.1 默认值与归一化

| 项 | 值 | 位置 |
|---|---|---|
| `Fiber::DefaultStackSize()` | 128 KiB | `fiber.hpp:281-283` |
| `Fiber::DefaultStackReserveSize()` | 8 MiB | `fiber.hpp:288-290` |
| Scheduler 默认初始提交 | `fiber_stack_size = 2048` → 页对齐 4096（**1 页**） | `scheduler.hpp:227`、`fiber.cpp:436-460` |
| Scheduler 默认预留 | 8 MiB | `scheduler.hpp:231` |
| 归一化下限 | `max(requested, page_size)`，即**至少 1 页** | `fiber.cpp:449` |
| 上限检查 | `size + page` 加法在构造期检查，溢出抛 `std::length_error` | `fiber.cpp:409-417`、`:450-459` |

> **旧文档的「最小 2 页」已失效**：当前归一化是 1 页，测试断言也改成
> `stack_size() == page_size`（`tests/concurrency/test_fiber.cpp:54`）。
> 直接构造 `Fiber(fn)` 仍默认 128 KiB；只有 Scheduler 路径默认 2048→1 页。

### 5.2 固定栈布局（回退路径）

```text
mapping: [ guard page (PROT_NONE) ][ usable ... ][ top ]
                                     ^ bottom()   ^ stack_pointer()
```

- `mmap(usable + page, RW)` 后把最低页 `mprotect(PROT_NONE)`（`fiber.cpp:707-720`）；
- `bottom() = mapping + page`、`stack_pointer() = mapping + mapping_size`（`fiber.cpp:592-602`）；
- 栈从高地址向低地址增长，越界立即 SIGSEGV。

### 5.3 可变长栈：布局与增长（当前默认）

```text
reserve = 8 MiB, 整段 mmap(PROT_NONE | MAP_NORESERVE)
[ base ............ committed .................. top ]
  ^ 未提交区（天然 guard）  ^ 已提交 RW（初始 1 页）  ^ 栈顶
                            ^ 每次 fault 向下扩 64 KiB
```

实现要点（`fiber.cpp:286-407`、`:533-765`、`:854-941`）：

1. **分配**：`mmap(reserve, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`，
   只把顶部 `initial_usable` 改成 RW（`fiber.cpp:690-706`）。未提交区天然是 guard，
   **不移动 guard**，这是最省 syscall 的设计。
2. **handler**：进程级 `sigaction(SIGSEGV)`，`SA_SIGINFO | SA_ONSTACK`，**不加
   `SA_NODEFER`**（`fiber.cpp:383-393`）；每线程 256 KiB `sigaltstack`，必须用
   `mmap` 而不是 `malloc`（栈溢出时堆元数据可能已被踩坏，`fiber.cpp:337-364`）。
3. **判定四步**（`fiber.cpp:855-941`）：
   - TLS 重入标志（`t_growable_in_handler`）→ 二次 fault 直接 fatal；
   - `fault ∈ [base, committed)`，且不是取指故障（`REG_ERR & 0x10`）；
   - `rsp ∈ [base, top)`，且 `|rsp - fault| ≤ 256 KiB`；
   - `committed - base >= 64 KiB`（还有增长空间）。
4. **增长**：从 `min(fault, rsp)` 向下对齐/退 64 KiB，`mprotect(new_low,
   committed - new_low, RW)`，更新 `committed`，**直接 return**——内核重试触发 fault 的
   指令。不 `longjmp`、不换上下文，栈地址与所有局部变量指针保持有效
   （`fiber.cpp:912-939`）。
5. **失败**：恢复 `SIG_DFL`、解除屏蔽、`raise(sig)`，保留 core dump 语义
   （`growable_stack_fatal`，`fiber.cpp:309-322`）。
6. **复用重置**：栈回缓存前，把增长过的区间重新 `mprotect(PROT_NONE)` 并重置
   `committed`（`fiber.cpp:728-765`）。只重置指针会让后续溢出静默写坏已提交内存。

> **与设计稿的一处偏离（重要）**：设计稿要求 `rsp ∈ [committed, top)`，但实测函数
> 序言先 `sub rsp` 再写局部变量（probe：`fault=committed-8, rsp=committed-0x50`），
> 因此放宽为 `rsp ∈ [base, top)` 并按 `min(fault, rsp)` 提交，否则普通深递归会误 fatal。

### 5.4 退化条件（何时不是 growable）

`resolve_reserve()`（`fiber.cpp:623-657`）在任一条件成立时返回 0（固定栈），
**不安装 SIGSEGV handler、不分配 sigaltstack**：

| 条件 | 说明 |
|---|---|
| `stack_growable = false` | 调用方显式关闭 |
| `GO2CPP_GROWABLE_STACK=0`（或 `false`/`off`） | 环境变量关闭（`fiber.cpp:328-335`） |
| `reserve <= usable` 或剩余不足 64 KiB | 预留区没有增长空间 |
| 备用栈/handler 安装失败 | 退化而不是让 Fiber 创建失败 |
| 非 Linux / 非 x86_64 / 非原生上下文后端 | 走 `boost::context` 分支 |
| ASan/TSan 构建 | 强制固定栈（`fiber.cpp:627-634`），sanitizer 自己管理栈边界 |

### 5.5 栈缓存

| 缓存 | 位置 | 上限 | 键 |
|---|---|---|---|
| 线程本地 | `s_thread_stack_pool`（`fiber.cpp:228`） | 32（`fiber.cpp:745`） | `(initial_usable, reserve)` |
| 全局兜底 | `s_stack_pool` + `s_stack_pool_mutex`（`fiber.cpp:226-227`） | 512（`fiber.cpp:753`） | 同上 |

- 缓存的是**已解除映射语义的整段映射**（growable）或 `boost.stack_context`（回退）；
  缓存满则 `munmap`/`deallocate`；
- 栈在 Fiber **仍可恢复时绝不能复用**：只有「已完成/失败且上下文已清空」的栈才回池
  （`fiber.cpp:164-206`）；
- 旧文档写的「thread 8 + global 128」是历史值，当前是 **32 + 512**。

### 5.6 工具链与平台

| 构建 | 处理 |
|---|---|
| ASan | `__sanitizer_start_switch_fiber` / `finish_switch_fiber` 成对调用；强制固定栈 |
| TSan | `__tsan_create_fiber` / `__tsan_switch_to_fiber`；强制固定栈 |
| Valgrind | `VALGRIND_STACK_REGISTER` / `DEREGISTER`（`fiber.cpp:552-565`） |
| 非 Linux x86_64 | `GO2CPP_USE_NATIVE_CONTEXT` 未定义 → `boost::context` 后端 |

---

## 6. 上下文切换

### 6.1 两个汇编原语

`src/concurrency/context_x64.S`（83 行）只做寄存器与 `rsp` 的交换，分配/保护/池化全在 C++：

| 原语 | 位置 | 做什么 |
|---|---|---|
| `go2cpp_context_make(stack_bottom, stack_size, entry)` | `context_x64.S:17-48` | 在栈顶搭初始帧：`rbx=entry`、`rbp=end`、`rip=trampoline`，返回上下文指针 |
| `go2cpp_context_jump(target, data)` | `context_x64.S:50-75` | 压入 6 个 callee-saved（`rbp/rbx/r15/r14/r13/r12`）+ 返回地址，切 `rsp`，跳转；返回 `Transfer{saved_context, data}` |

保存区布局（`context_x64.S:63-73`）：`+0 r12、+8 r13、+16 r14、+24 r15、
+32 rbx、+40 rbp、+48 rip`。**caller-saved 寄存器不保存**：挂起点一定在函数调用内部，
SysV ABI 保证活跃值已落栈。`errno` 也不在保存区，由 `resume_locked` 单独双向保存。

### 6.2 resume 的执行路径

`Fiber::Impl::resume_locked()`（`fiber.cpp:1491-1636`）是**整个 Fiber 机制的唯一切换点**：

1. 可选校验调用者（`bind_caller`，`fiber.cpp:1402-1449`）——父子关系不符直接返回 false，
   防止「A 的父级被写成 B」的栈链损坏；
2. 记住最初的 caller，登记 `FiberTlsRestore`（本函数可能沿父链循环多轮）；
3. 状态必须是 `Ready`/`Suspended`；`Ready` 且无上下文时先 `initialize_context()`；
4. 置 `Running`、清 `reason`；
5. 保存 caller 的 errno → 切 TLS 当前 Fiber → 恢复本 Fiber 的 errno
   （`load_errno`/`store_errno` 用 `GO2CPP_FIBER_NOINLINE`，`fiber.cpp:259-261`，
   防止编译器跨线程缓存 `__errno_location()`）；
6. `JumpContext`；返回后按状态清空 `m_context`/`m_stack`（终态）并恢复 TLS/errno。

调度器路径 `resume_from_scheduler()`（`fiber.cpp:1376-1396`）额外做两件事：
父级只绑一次（`m_scheduler_parent_bound` 缓存），并走 `scheduler_fast`——跳过
`last_thread` 的加锁写回（只在跨线程时用哈希比较更新，`fiber.cpp:1543-1553`）。

### 6.3 切换成本（实测数据，注明来源与前提）

| 指标 | 数值 | 前提/来源 |
|---|---|---|
| Fiber 切换（含调度器 park/wake + runqueue） | **~500 ns** | WSL2 6.6.87.2 / x86_64 / 12 CPU / n=100000 ping-pong（`docs/switch-benchmark.md:23`，三轮 466/514/523 ns） |
| 线程切换（同一基线的对照） | **~28.6 µs** | 同上，三轮 27,657/29,015/29,253 ns；**裸机通常 2–5 µs**，28 µs 是 WSL2 放大（`switch-benchmark.md:37-38`） |
| 加速比 | **~57×** | 同上 |
| 纯 `resume/suspend`（无调度器） | ~100–200 ns | 同上，作者估算 |

> 任务描述里的「线程 ~27µs」是这台机器的三轮最小值；本文以 `switch-benchmark.md`
> 的 28.6 µs 均值为准，并保留 2–5 µs 的裸机区间。
> 另有一条旧口径「0.73–0.80 µs/往返」来自 `gmp-perf-analysis` 的 park/wake 计时
> （`docs/gmp-perf-analysis.md:53`），与上表不是同一测量对象，不要混用。

### 6.4 FiberBin（调度器专用复用池）

- `thread_local FiberBin { std::vector<std::unique_ptr<Fiber>> }`（`fiber.cpp:234-238`）：
  一个 M 只访问自己的池，**不跨线程转移仍挂起的 Fiber**。
- 容量：API 默认参数 **32**（`fiber.hpp:314`、`fiber.cpp:239`），
  调度器配置 `fiber_bin_capacity` 默认 **128**（`scheduler.hpp:256`、`:280`），
  硬上限 **4096**（`fiber.cpp:242`）。
- 回收条件（`RecycleForScheduler`，`fiber.cpp:1845-1871`）：容量非 0；状态
  `Completed/Failed`；不是当前 Fiber；`m_record.use_count() == 1`（没有子 Fiber 仍持有
  父 record）；否则**静默丢弃**，由 `unique_ptr` 正常析构（栈仍回栈缓存）。
- 重绑定前 `reset_for_scheduler()` 会校验「终态 + 上下文已清空 + 无 resume claim」，
  违反则抛 `std::logic_error`（`fiber.cpp:1133-1144`）。

---

## 7. 两种迁移

### 7.1 同进程跨 OS 线程迁移（运行时能力）

物理基础：Fiber 的栈是自己 `mmap` 的普通内存，上下文里保存的是**栈指针**而不是
「某线程的栈」；换线程继续跑只是换一个 `rsp`（`fiber.hpp:269-275`）。

- 调度器把同一个 G 换到另一个 M 上 resume 即可；`test_fiber.cpp` 用两个真实
  `std::thread` + `gettid` 双重确认迁移（`tests/concurrency/test_fiber.cpp` 迁移用例）。
- 迁移钩子不是公开 API，而是**记账**：
  - `FiberExecutionBinding{scheduler_token, task_id, machine_id, processor_id, managed}`
    只保存不拥有对象的标识，避免 Fiber 依赖 scheduler 模块（`fiber.hpp:182-198`）；
  - `BindCurrentExecution()` / `bind_execution()` 记录最近的绑定（`fiber.hpp:376`、`:420`）；
  - `FiberRecord::last_thread` 在跨线程恢复时更新（`fiber.cpp:1543-1553`）。
- growable 栈迁移后的补充动作：新线程可能还没装 `sigaltstack`，
  `resume_locked()` 在真正 jump 前确认并补装（`fiber.cpp:1523-1531`）。
- 谁拿到 `Fiber` 对象并串行 `resume`，谁就能推进它——这也是「Fiber 不依赖 Scheduler
  生命周期」的物理基础。

### 7.2 跨进程快照迁移（实验特性）

构建开关：`GO2CPP_MIGRATION_API` 默认 **OFF**；`-DGO2CPP_MIGRATION_API=ON` 打开，
`GO2CPP_BUILD_ENHANCED=ON` 预设会**强制打开**（`cmake/Go2CppOptions.cmake:12`、`:66-70`）。
关闭时类型布局、热路径与行为都不变（`fiber.hpp:28-32`）。

| 能力 | API | 位置 |
|---|---|---|
| 开启本 Fiber 的迁移等待路径 | `EnableMigration()` | `fiber.hpp:435` |
| 导出挂起 Fiber 的快照 | `ExportMigrationImage(path, options, error)` | `fiber.hpp:453` |
| 在目标进程重建 | `ImportMigrationImage(path, options, error)` | `fiber.hpp:469` |
| 通道等待状态 | `ChannelMigrationBase::ExportWaitState/AdoptWaitState` | `fiber.hpp:107-118` |

设计要点（合并自 `fiber-migration.md`）：

- 只支持**无缓冲、单生产者单消费者、无 select、无 deadline/context** 的通道接收等待；
  `Channel<T>::ExportWaitState` 对复杂场景**明确拒绝**而不是给一个语义不同的快照
  （`fiber.hpp:52-72`）。
- 快照 = 栈 + 上下文保存区 + 等待状态；格式 v3（magic `G2CMIG03`、header 224 B、
  段表 48 B/项、末尾 64 B HMAC-SHA256 签名），全小端定宽逐字段写字节，不 `memcpy(struct)`。
- 指针审计：未登记的堆指针默认拒绝；`allow_dead_heap_slots=true` 只用于受控 demo，
  被清 0 的槽在恢复后一旦使用会立即崩溃而不是悬空执行（`fiber.hpp:148-158`）。
- 导入侧默认校验 build-id 与签名（`FiberMigrationOptions::verify_build_id`/
  `verify_signature`，`fiber.hpp:141-159`）；重建后是 `Suspended`，**不自动 resume**。
- 实测（demo）：栈快照 65,816 字节、8 个符号、22 条重定位、6 帧；10 轮 dump+restore
  约 25 ms/轮；`tools/migration/tests/run_tests.sh` 53/53 通过
  （`docs/fiber-migration.md` §7）。

**限制**：跨机器/跨架构未验证；栈帧布局必须跨版本稳定（可迁移 TU 需 `noinline`、
关 IPA 克隆、`-fno-omit-frame-pointer`、`-fno-stack-protector`）；异常展开中/持锁挂起
点未验证；性能 O(栈使用量)。

---

## 8. FiberLocal 与 thread_policy

`FiberLocalCache<T>`（`fiber_local.hpp`）：

- 值按 Fiber 的**逻辑身份**（`FiberRecord`）索引，而不是按线程；
- 值的所有权属于 trampoline：主体返回后、交回 caller 前，`fiber_local::detail::Cleanup`
  统一析构（`fiber.cpp:1337-1339`）；key 句柄先死不影响已有值；
- 跨线程迁移保持；Fiber 结束（含失败/放弃路径）时销毁（`fiber.cpp:1225`）。

`thread_policy`（`ScopedThreadParticipation`、`ScopedThreadHookMode`）：

- **只写 TLS 元数据**，不创建 M、不绑定 P、不改变调度（`fiber_guide.md` §9 的结论仍成立）；
- `thread_policy::detail::EnterRuntimeWorker/LeaveRuntimeWorker` 是源码级 Hook，
  不是 ABI，第三方库不能依赖。

---

## 9. 诊断与记账（FiberRecord）

- `FiberRecord` 是 `shared_ptr` 共享元数据（`fiber.cpp:952-982`）：`id`、`parent`、
  `alive`、`state`、`reason`、`cancellation_requested`、`last_thread`、`execution`；
- 父链**只存元数据**，不存父栈/owner 所有权：父对象销毁后仍能生成
  `alive=false` 的**墓碑帧**，但真正的恢复仍要求父 Fiber 对象存活
  （`fiber.cpp:943-946`）；
- `context_snapshot()` 逐级锁 record → 拷 `shared_ptr` → reverse → 插入 main 帧
  （`fiber.hpp:409-413`、`fiber.cpp:1985`）；
- `CancellationRequested()` **沿父链向上查**（子能感知父取消），
  `RequestCancellation()` 只置自己（`fiber.cpp:1909-1923`）；
- `debug_stack()` 返回 `FiberStackDebugSnapshot`：栈范围、reserve、committed、当前 SP；
  `current_sp` 只在当前线程正在执行该 Fiber 时非 0，不伪造（`fiber.hpp:225-250`）；
- 调试元数据表有上限（默认 4096 条，超限批量淘汰 512，`fiber.cpp:1025-1027`）。

---

## 10. 与 Scheduler / P 的关系

- Scheduler 把 Fiber 当 G 在 M/P 上排队；Fiber 不知道 P 的存在。
- `TaskOptions::stack_size` / `stack_reserve_size` / `stack_growable` 传入 Fiber
  （`scheduler.hpp:275-288`）；`SchedulerConfig::fiber_stack_growable=false` 时
  `spawn()` 会强制关闭（`scheduler.hpp:285-287`）。
- Fiber 用 `AcquireForScheduler` / `RecycleForScheduler` 与 M 的 FiberBin 对接；
  普通用户 Fiber 不经过此路径，行为不因池化改变（`fiber.hpp:304-321`）。
- `resume_from_scheduler()` 与 `resume()` 的分工见 §6.2：前者信任 Task 的 execution
  claim 已串行化，后者每次校验调用者。
- `FiberExecutionBinding::scheduler_token` 是 Scheduler 地址的整数形式，**不拥有**
  Scheduler，不会延长其生命周期（`fiber.hpp:182-185`）。

---

## 11. 性能与内存数据（含前提，勿跨场景横比）

| 数据 | 前提 | 出处 |
|---|---|---|
| Fiber 切换 ~500 ns vs 线程 ~28.6 µs（~57×） | WSL2 12 核，n=100000 ping-pong | `docs/switch-benchmark.md:23` |
| 每单位虚拟内存：Fiber 68.5 KB vs 线程 6,294 KB（**≈92×**） | n=1000，Fiber 64 KB 栈，独立进程采样 | `docs/memory-benchmark.md:49` |
| 每单位 RSS：Fiber 60.6 KB vs 线程 65.8 KB（只省 ~8%） | 同上 | `docs/memory-benchmark.md:49` |
| growable 落地前后：A 场景 QPS **142,491** / cpu **70.3 µs**（原 142,448 / 71.2） | 200×2 depth1 | `docs/fiber-growable-stack-design.md` §10.3 |
| s3（8,000 连接）：QPS **7,895** / cpu **121.9 µs**（原 125.2） | P=4，rate=1 | 同上 |
| 8,000 连接 VmSize **62.8 GB**（8000×8 MiB）、VmRSS 稳态 **161 MB**（采样 207 MB） | 同上 | 同上 |
| 增长足迹（strace） | 初始 `mprotect(4096,RW)` → 每次增长 `mprotect(69632,RW)` → 释放前 `mprotect(1183744,PROT_NONE)` | 同上 |
| 深递归通过 | 256 KiB 级（1024 帧）、1 MiB 级（4096 帧）、4 workers × 32 tasks 并发增长 | 同上 |
| `go()` 创建成本 17.6 µs → 14.5 µs | 去掉两次冗余 `mprotect`（栈缓存复用） | `src/concurrency/fiber.cpp:186-188` 注释 |
| 迁移快照 65,816 B / 22 重定位 / 6 帧；dump+restore ~25 ms/轮 | demo，PIE+ASLR | `docs/fiber-migration.md` §7 |

> 92× 与 500 ns/28.6 µs **不在**四篇合并来源里，是 2026-10-21 的独立基准
> （`switch-benchmark.md`、`memory-benchmark.md`）；此处按任务要求并入，并标明来源。
> 所有数字都是 WSL2 结果，**不可**直接当裸机/生产容量规划。

---

## 12. 测试

### 12.1 当前构建的 `ctest -N`

`/UserData/build/go2cpp-mcp-integrated` 的 `ctest -N` 只有 14 条（12 条 `go2cpp_os_monitor*`、
`go2cpp_bufio_test`、`go2cpp_bufio_stress`），**不含 fiber 用例**。fiber 用例注册在
`cmake/Go2CppTests.cmake`，默认构建才可见。

### 12.2 fiber 相关 ctest / 测试模块

| 用例 | 注册位置 | 说明 |
|---|---|---|
| `go2cpp_fiber_growable_stack` | `cmake/Go2CppTests.cmake:304`（TIMEOUT 120） | 复用 `go2cpp_tests`，`GO2CPP_TEST_FILTER=fiber_growable`，PASS 正则 `fiber growable stack` |
| `go2cpp_tests` | `:216`（TIMEOUT 120） | 全量；fiber 模块 filter：`fiber`、`fiber_growable`（`tests/test_main.cpp:91-92`）、`fiber_local`（`:105`） |
| `go2cpp_unknown_filter_rejected` / `go2cpp_known_filter_runs` | `:289` / `:295` | 过滤器本身的自检 |

测试文件：

| 文件 | 覆盖 |
|---|---|
| `tests/concurrency/test_fiber.cpp` | 挂起/恢复、errno 双向、跨线程迁移（gettid 双确认）、嵌套与墓碑、析构三态、错误父级、异常边界、FiberLocal、取消、栈归一化（18 类边界） |
| `tests/concurrency/test_fiber_growable_stack.cpp` | `test_default_config_and_normal_fiber`、`test_deep_recursion_256k`、`test_deep_recursion_1m`、`test_stack_address_stability`、`test_migrated_growth`、`test_concurrent_growth`、`test_fixed_fallback`（7 个用例） |
| `tests/concurrency/test_fiber_local.cpp` | FiberLocal 生命周期 |
| `tests/concurrency/test_thread_policy.cpp` | thread_policy 作用域 |
| `tools/migration/tests/run_tests.sh` | 迁移 53/53（含位翻转拒绝、句柄重定位、签名负例） |

历史 ctest 计数（113/113 → 116/116）是 2026-10-18～20 的快照，**不要**写进验收结论；
当前构建的实际清单以 §12.1 为准。

---

## 13. 边界与降级矩阵

| 场景 | 行为 | 依据 |
|---|---|---|
| ASan / TSan 构建 | 强制固定栈，不装 SIGSEGV handler | `fiber.cpp:627-634` |
| 用户自定义 SIGSEGV handler / JIT | 冲突：本模块安装的是**进程级** handler；用 `GO2CPP_GROWABLE_STACK=0` 回退 | `fiber.hpp:232-235`、`fiber.cpp:366-398` |
| 大帧（`alloca`/大数组）> 256 KiB | fatal（不增长），恢复默认动作并 core | `fiber.cpp:292`、`:904-906` |
| 32 位 / 非 x86_64 / 非原生后端 | 退化为固定栈或 `boost::context` | `fiber.cpp:623-657` |
| 并发 resume 同一 Fiber | 第二个调用返回 false，不破坏状态 | `fiber.cpp:1364-1374` |
| 在错误父级析构挂起 Fiber | 标 `Failed`、丢弃上下文；**不**展开局部 RAII | `fiber.cpp:1198-1230` |
| 在自身栈上析构 | `std::terminate()` | `fiber.cpp:1232-1237` |
| 迁移未开启 / 构建未开 API | `MigrationApiEnabled()` 返回 false，普通路径行为不变 | `fiber.hpp:483-486` |
| 迁移遇复杂通道场景 | 明确拒绝并给出原因，不退化成语义不同的快照 | `fiber.hpp:52-72` |
| FiberBin 复用条件不满足 | 静默丢弃，正常析构；语义不变 | `fiber.cpp:1845-1871` |

---

## 14. 旧文档合并对照

| 旧文档 | 并入本文 | 已作废/不再保留 |
|---|---|---|
| `docs/fiber_guide.md`（862 行） | 定位、errno、跨线程迁移、嵌套与墓碑、析构三态、异常边界、FiberLocal、thread_policy、FiberRecord、测试评述 | 「最小 2 页」、把 128 KiB 当唯一默认、`fiber.cpp` 旧行号（1,502 行版本） |
| `docs/fiber-growable-stack-design.md`（183 行） | 为什么不能照搬 Go、growable 布局/判定/退化/池化重置、落地实测数据、已知限制 | P1/P2 的未采纳方案细节（保留原文作参考）、设计稿的估算值（80 GB/10k） |
| `docs/fiber-migration.md`（857 行） | §7.2 的格式/签名/safepoint/句柄表/限制、demo 实测 | 设计稿 API（§11.1 的 `vector<byte>` 版本）与落地签名不同，本文只写落地签名 |
| `docs/coost-comparison.md`（542 行） | 「独立栈 + 零拷贝 vs coost 共享栈拷栈」的结论 | 「GMP 切换 = `swapcontext`」（实际是自定义汇编）、中间轮次的作废数字 |

---

## 15. 已知限制与待办

1. **growable 与 sanitizer 互斥**：ASan/TSan 下强制固定栈，深递归能力下降。
2. **进程级 SIGSEGV 是独占资源**：与用户 handler、JIT、其他协程库冲突，只能通过
   `GO2CPP_GROWABLE_STACK=0` 规避。
3. **大帧不增长**：>256 KiB 的 `alloca`/VLA 按 fatal 处理。
4. **直接构造 `Fiber(fn)` 仍默认 128 KiB**：只有 Scheduler 路径默认 1 页 + 8 MiB 预留。
5. **跨进程迁移是实验特性**：默认构建不含；只支持无缓冲单等待者；栈帧布局必须跨版本
   稳定；跨机器/跨架构未验证。
6. **析构可能长时间阻塞**：忽略取消的主体无法被强制终止（这是为保证 C++ RAII 语义）。
7. **本文未在本机构建重跑 fiber 用例**：`ctest -N` 清单与注册表已分开记录；
   需要实测时用默认构建 + `GO2CPP_TEST_FILTER=fiber_growable`。
