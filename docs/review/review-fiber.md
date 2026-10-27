# fiber 模块审查报告

- 审查对象：`src/concurrency/fiber.cpp`（3270 行）、`include/go2cpp/concurrency/fiber.hpp`（493 行）、`fiber_local.hpp`
- 基线：`a2fa0df`；本模块文件 md5 与审查快照一致（同期 Bento 工作区有并发改动，不影响本模块行号）
- 审查方式：只读逐段审查 + 子审查交叉验证；覆盖 FiberStack（growable/固定栈）、SIGSEGV handler、resume/suspend、FiberBin、FiberRecord 父链、迁移 API；未运行压测/ASAN
- 统计：**P1×6 / P2×7 / P3×1，共 14 条**

## 1. 问题清单

| ID | 级别 | 位置 | 问题 | 证据 | 建议修复 |
|---|---|---|---|---|---|
| FIBER-01 | P1 | `fiber.cpp:2056-2066,1594` | `debug_stack()` 无锁读取 `m_impl->m_stack.get()` 并连续解引用，与 resume 线程的 `m_stack.reset()` 构成数据竞争，可 UAF；作者注释自认「并发 reset 与读取之间可能出现过渡值」，但 `unique_ptr` 读/写非原子 | 2056 `FiberStack* stack = m_impl->m_stack.get();`，2059-2066 调 `growable()/reserve_size()/usable_size()/growth_base()/growth_top()/growth_committed()`；写端 1594 `m_stack.reset();` | 为 `m_stack` 提供加锁快照或原子化访问（持 record->mutex 时复制必要字段）；禁止在非 `Current()` 线程解引用 `m_stack` |
| FIBER-02 | P1 | `fiber.cpp:1242-1248` | `~Impl()` 用无界 `std::this_thread::yield()` 自旋等待 `m_resume_claim`；若 resume 线程正等析构线程持有的锁（如 Task::run 的 `m_transition_mutex`），双方互等，死锁或烧满一个核 | `while (!m_resume_claim.compare_exchange_weak(expected_claim, true, ...)) { expected_claim = false; std::this_thread::yield(); }`（1243-1248） | 改为有界等待 + 条件变量；或在析构前断言「无并发 resume」并 fail-fast（1235-1237 已有同类先例） |
| FIBER-03 | P1 | `fiber.cpp:1943-1951,1157` | `RequestCancellation()`/`state()` 无锁解引用 `m_record`，与 `reset_for_scheduler` 的 `m_record = std::move(next_record)` 竞争；`RecycleForScheduler` 的 `use_count()!=1` 检查拦不住「持有 Fiber* 的同代调用」 | 1945 `m_impl->m_record->cancellation_requested.store(...)`、1950 `return m_impl->m_record->state.load(...)`；写端 1157 `m_record = std::move(next_record);` | `m_record` 改 `std::atomic<std::shared_ptr>`（C++20）或加锁；Recycle 时清空可观测句柄 |
| FIBER-04 | P1 | `fiber.cpp:1283-1289,1198-1225` | `~Impl()` 对 Suspended 状态强制 `resume_locked(true)` 以展开栈；父级不可恢复时走 `abandon_suspended`，只清 `m_function/m_context` 并置 Failed，**不执行** Fiber 栈上局部对象析构（注释自认）；若局部量是 `sync::Mutex` 的 lock_guard，锁永久不释放 | 1283 `const bool resumed = resume_locked(true);`；1284-1288 失败即 `abandon_suspended(...)`；1199-1201 注释「栈上的 C++ 局部变量无法再展开」 | 文档/析构入口明确「必须在原父 Fiber 上销毁」；abandon 路径至少递增全局计数并输出诊断日志，便于定位泄漏源 |
| FIBER-05 | P1（需人工确认） | `fiber.cpp:1402-1416,1235-1237` | `bind_caller` 在锁内解引用 `caller->m_impl`；若父 Fiber 被另一线程析构则 UAF。当前仅靠「析构者 TLS 不等于目标 Fiber」拦截，跨线程 delete 一个正被当作父级的 Fiber 不会被拦截，resume 侧只对 parent_record 保活、不保活父 Fiber 对象 | 1416 `parent_record = caller == nullptr ? nullptr : caller->m_impl->m_record;`；caller 来自 1493 `bind_caller(s_current_fiber)`；~Impl 1235-1237 只检查 `s_current_fiber == m_owner` | 若契约允许跨线程销毁，resume 侧需持有父 Fiber 的 shared_ptr/生命周期 token；若不允许，在 ~Impl 增加全局注册表校验并 fail-fast |
| FIBER-06 | P1 | `fiber.cpp:383-393,309-325` | 进程级 SIGSEGV handler 直接覆盖宿主已有 handler，不保存/不链式调用；`growable_stack_fatal` 恢复 `SIG_DFL` 后 `raise`，宿主自定义崩溃捕获（Breakpad/Crashpad/sanitizer）被绕过 | `struct sigaction action {}; action.sa_sigaction = &growable_stack_segv_handler; ... if (::sigaction(SIGSEGV, &action, nullptr) != 0)`（383-393）；fatal 路径 `::sigaction(sig, &action, nullptr); ... ::raise(sig);`（309-325） | 安装前保存旧 `sigaction`，非本栈故障时转发旧 handler；或提供显式 opt-in/opt-out 并文档说明 |
| FIBER-07 | P2 | `fiber.cpp:337-364,341-344,1527-1530` | altstack 生命周期两个问题：① 每线程 mmap 后线程退出不释放（注释明确不 munmap），短命线程累计泄漏 256 KiB/线程；② `t_growable_altstack_attempted` 在 mmap/sigaltstack 失败后不回滚，一次瞬时 ENOMEM 会让该线程所有后续 growable Fiber 直接 `Failed` | `void* memory = ::mmap(nullptr, kGrowableStackAltStackSize, ...)`（348）；341-344 `t_growable_altstack_attempted = true;` 在任何失败分支都不回滚；1527-1530 失败即 `state=Failed; return false;` | 用 thread_local RAII 析构 `munmap`；失败时按次数/时间退避重试，或回退固定栈继续运行而非置 Failed |
| FIBER-08 | P2 | `fiber.cpp:1850-1855,239` | `fiber_bin_capacity=0` 无法禁用 FiberBin：1850 先把 0 映射为默认 32，1854 的 `capacity == 0` 分支永不可达，配置序列化后调用方无法表达「禁用」 | `const auto capacity = std::min(kMaxFiberBinCapacity, bin_capacity == 0 ? kDefaultFiberBinCapacity : bin_capacity);`（1850-1852），随后 `if (capacity == 0 || ... ) return;`（1854）；`kDefaultFiberBinCapacity=32`（239） | 用 `std::optional` 或哨兵值区分「未指定」与「禁用」，并为禁用路径加测试 |
| FIBER-09 | P2 | `fiber.cpp:1317-1336` | `entry()` 在 Fiber 栈上析构用户 callable（`self->m_function = {};`），析构抛异常时在 noexcept 中直接 terminate；对比 `Task::run` 特意把 callable 移到调用者侧销毁（954-960），说明作者已知该风险 | 1317 `static void entry(...) noexcept`；1336 `self->m_function = {};`；Task::run 954-960 `completed_function = std::move(m_function);` | 在 entry 中把 `m_function` 移出到调用者可见存储（或包 try/catch），与 Task::run 的销毁策略一致 |
| FIBER-10 | P2 | `fiber.cpp:643-649,579-580` | growable 降级原因被静默吞掉：用户关闭、reserve 不足、handler/altstack 安装失败三种情况统一 `return 0`，`reserve_size()` 只返回 0，诊断快照看不到原因 | `resolve_reserve` 636-637（用户关闭）、643-646（reserve 不足）、647-649（ensure 失败）统一 `return 0` | 记录 `growable_disabled_reason` 枚举/计数并在 `FiberStackDebugSnapshot` 暴露 |
| FIBER-11 | P2 | `fiber.cpp:901-906` | 栈增长只接受 `|fault-rsp| ≤ 256 KiB`；大栈帧、`alloca` 远跳或深调用链一次跨过窗口时直接 `fatal`，而不是继续提交（文档已声明限制） | `if (distance > kGrowableStackFaultWindow) { growable_stack_fatal(sig); }`（904-906） | 在 API/文档中明确「单帧/单次探测距离上限」；错误信息给出触发地址与建议 |
| FIBER-12 | P2 | `fiber.cpp:745-759,694-696` | 全局栈池最多缓存 512 段 growable 栈，每段 reserve 默认 8 MiB → 常驻约 4 GiB 虚拟地址空间；`MAP_NORESERVE` 不占 RSS，但在 `RLIMIT_AS`/地址空间碎片敏感环境会失败 | `if (s_stack_pool.entries.size() < 512U)`（753）；构造 `mmap(nullptr, reserve, PROT_NONE, MAP_NORESERVE)`（694-696） | 池上限做成配置；按 reserve 分桶限制；文档给出虚拟地址预算 |
| FIBER-13 | P2（需人工确认） | `fiber.cpp:2421-2745` | 跨进程迁移（实验特性）导出/导入栈镜像时，对栈上指针、锁状态、TLS、fd 的修正依赖约定；未看到地址空间布局/ASLR/版本兼容校验，错误镜像可能恢复出悬垂指针 | `Fiber::ExportMigrationImage(...)`（2421）、`Fiber::ImportMigrationImage(...)`（2745）；`docs/modules/fiber.md` §6.2 标注实验特性 | 明确「仅同进程/同构建」约束并在导入时校验 magic/版本/栈范围；对锁/句柄类对象拒绝迁移；补损坏镜像 fuzz |
| FIBER-14 | P3 | `fiber.cpp:1881-1893,1957-1960` | `resume_result()` 声明 `noexcept`，内部 `failure()` 会加 `m_failure_mutex`；`std::mutex::lock` 失败抛 `std::system_error`，在 noexcept 内直接 terminate（极罕见） | `FiberResumeResult Fiber::resume_result() noexcept { ... result.failure = failure(); ... }`（1881-1885） | 把 failure 快照改为无锁/原子，或允许 `resume_result` 抛异常 |

## 2. 重点问题说明

### FIBER-01（P1）debug_stack 与栈释放竞争
`debug_stack()` 是诊断接口，但直接取 `unique_ptr<FiberStack>` 的裸指针后多次解引用；`resume_locked` 在任务完成时 `m_stack.reset()`。如果诊断线程在任务结束瞬间读取，`stack` 指针立即悬垂。注释已承认「过渡值」，但这不是数值误差而是 UAF。建议在 `FiberRecord::mutex` 下复制所需字段（`growable/reserve/usable/committed`），不要在锁外解引用。

### FIBER-02（P1）析构自旋
`~Impl()` 无条件等待 `m_resume_claim` 释放。resume 侧在 `Task::run` 的 `m_transition_mutex` 内完成 claim 翻转；若析构线程恰好持有该 transition mutex（例如在 Task 生命周期收尾路径），双方互等。`yield()` 自旋还会把一个核烧满。建议改为条件变量 + 超时，或把「析构时不得并发 resume」写成硬契约并在入口 fail-fast。

### FIBER-06（P1）信号处理器覆盖
作为可嵌入运行时，fiber 在首次创建 growable 栈时对 `SIGSEGV` 做进程级 `sigaction`，不保存旧 handler；fatal 路径恢复 `SIG_DFL` 并 `raise`。宿主进程原本安装的崩溃捕获会被绕过。建议保存旧 handler 并在「非本栈增长」时转发。

### FIBER-13（P2）跨进程迁移
`ExportMigrationImage/ImportMigrationImage` 是实验特性，涉及栈上指针、锁、TLS 恢复。当前未见导入时的地址范围/版本校验（需进一步确认），建议把「仅同构建、同地址空间布局」写成硬约束，并对损坏镜像做 fuzz。

## 3. 亮点

- growable 栈的地址空间不变、只移动 `committed` 的设计正确：增长不复制栈，所有局部变量指针保持有效；`release_to_cache` 在复用前把增长区重新 `PROT_NONE` 并重置 `committed`，避免「复用后仍可写」的静默破坏。
- SIGSEGV handler 只调用 async-signal-safe 函数，使用 `volatile sig_atomic_t` 重入标志、原子增长计数，并保存/恢复 `errno`；不打印日志，细节到位。
- `resume_locked` 对 errno 做 Fiber 级隔离（进入保存调用者、切到目标恢复其自身、返回再恢复调用者），并成对调用 ASAN/TSAN 钩子。
- 嵌套 Fiber 的父链传播、取消传播、`FiberRecord` 生命周期用 shared_ptr 记录；失败路径显式 `abandon_suspended` 而不是 terminate。
- `FiberBin` 用 thread_local 缓存，避免全局锁；回收前检查 `use_count` 防止复用仍被父链引用的对象。

## 4. 维度结论

| 维度 | 结论 |
|---|---|
| 正确性 | 栈增长/切换/errno/ASAN 钩子基本正确；debug_stack、request_cancellation、bind_caller 三处存在与生命周期相关的竞争/UAF；256 KiB 窗口与跨进程迁移是硬边界 |
| 并发 | FiberBin/栈池 thread_local 无锁正确；`~Impl` 自旋等待与 reset 竞争是主要风险；信号 handler 覆盖宿主是集成风险 |
| 资源与错误 | altstack 每线程泄漏 + 失败永久降级；全局栈池 4 GiB 虚拟地址常驻；`resume_result` noexcept 内可能 terminate |
| 风格与可维护性 | 文档与注释质量高（`fiber.md` 对布局、handler 条件、池化键写得很细）；但 `fiber_bin_capacity=0` 语义不可表达、growable 降级原因不可观测 |

## 5. 未覆盖与限制

- 未运行 ASAN/TSAN、未做栈溢出/大帧/迁移损坏镜像实测；FIBER-05/13 标注「需人工确认」。
- 迁移镜像格式、`FiberRecord` 完整状态机、`reset_for_scheduler` 的并发发布顺序未逐行展开，建议后续用专项测试覆盖。
