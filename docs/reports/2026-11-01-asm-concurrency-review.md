# 汇编级并发审查：Scheduler / Task / 并发结构热路径

- 审查对象：`/UserData/CodexWorkSpace/Go2cpp`，构建 `.build-bento-wsl`（Release, `-O3 -DNDEBUG`，GCC 13.3，x86-64）
- 方法：`objdump -d --no-show-raw-insn -C` 反汇编 `libgo2cpp_scheduler.so` / `libgo2cpp_sync.so` / `libgo2cpp_channel.so` / `bento_channel_thread_stress`，逐函数数指令、定位原子/调用/分支；`go tool objdump` 反汇编 Go 1.x `runtime.newproc/newproc1` 做对照
- 边界：全程只读，未修改任何仓库文件；本报告与中间反汇编产物仅落在 `/tmp`
- 基线（来自任务背景）：任务 656–764 ns（Go 135 ns，4.2×）；Channel 10.73 ns（第一）；Mutex 3.75 ns；Fiber 切换 23.5 ns（物理下限 21 ns，不在本次范围）

---

## 0. 量化底数（先把"贵"的证明摆出来）

| 对象 | 指令数（含冷分支） | 栈帧 | canary | GOT 间接调用 |
|---|---|---|---|---|
| `Scheduler::spawn(std::function, TaskOptions)` | 194 | 0x58 | 有 | 3 |
| `Scheduler::enqueue(shared_ptr const&)` | **789** | **0x2c8（712B）** | 有 | 多处 |
| `Scheduler::wake_registered` | **294** | — | 无 | 6+ |
| `Scheduler::park_wait` | 77 | 0x8 | 无 | 2 |
| `Task::Task`（单独实例化） | ~90 | 0x8 | 无 | 1 |
| `Task::~Task` | ~100 | 0x8 | 无 | 6 |
| `Task::wait()` | **170** | 0x78 | 有 | 6 |
| `Task::wait_for()` | ~170 | 0x78 | 有 | 6 |
| `Task::run()` | **441** | 0x2f8（760B） | 有 | 8+ |
| `core::ParkingCondition::WaitOnce` | **1583**（冷热混合） | 0xb8 | 有 | 5 |
| `sync::Mutex::lock()`（快路径） | 26 | 0x28 | 有 | 2 |
| `sync::ConditionVariable::NotifyOne`（无等待者） | 5 | 0 | 无 | 0 |
| `Channel<int>::SendUntil`（缓冲快路径） | 前端 40+ | 0x138 | 有 | 0（同 TU 直调） |
| Go `runtime.newproc` | **19** | 0x28 | 无（仅 stack guard） | 0 |
| Go `runtime.newproc1` | **233**（首次创建路径，含 G 分配） | 0x50 | 无 | 0 |

库级计数（`objdump` 全文 grep）：

| 库 | canary 访问数 | `call *GOT` 数 | `lock` 前缀指令数 | `__tls_get_addr` |
|---|---|---|---|---|
| scheduler.so | 296（≈148 个函数） | **2180** | 588 | **78** |
| sync.so | 131 | 711 | 175 | — |
| channel.so | 98 | 615 | 63 | — |

编译事实：`CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG`，无 `-fno-semantic-interposition`、无 `-fvisibility=hidden`、无 `-fno-plt`、无 `-Wl,-Bsymbolic-functions`；GCC 13 Ubuntu 默认 `-fstack-protector-strong [enabled]`（实测 `gcc -Q --help=common`）。这三条是下面一半浪费的根因。

---

## 1. 按收益排序的浪费清单

> 收益为单次操作估计（单核热缓存，3 GHz 量级），"可省指令"按执行路径计。

### #1 `Task::wait()/wait_for()` 用 `std::mutex + 条件变量 + seq/waiters` 三件套完成通知
- **位置**：`scheduler.so` `Task::wait` `0x17720`：`pthread_mutex_lock` @ `0x17824`、`lock addl $0x1,0x244(%rbx)`（waiters++）@ `0x17857`、`ParkingCondition::WaitOnce` 间接调用 @ `0x178fd`、`lock subl` @ `0x1787f`、`pthread_mutex_unlock` @ `0x178a0`。`wait_for` 同构 @ `0x17b24/0x17b37/0x17c34/0x17bc3`。
- **浪费类型**：不必要的锁（mutex 只保护 `m_completion_seq`/`m_completion_waiters` 两个标量）+ 2 次 PLT 调用 + 2 次原子 RMW。而 `ParkingCondition` 本身已经是 futex（`syscall $0xca`）+ 自旋实现的，等于同一条等待路径上叠了两套同步机制。
- **可省**：约 45–55 条/次 + 2 次间接调用（glibc `pthread_mutex_lock` 无竞争快路径 ~20 条/次，lock+unlock 40 条）。
- **预期收益**：15–25 ns/任务（join 路径固定成本）。
- **难度**：中。语义要求"无等待者时 notifier 不加锁直接返回、且不能丢唤醒"——直接把 `m_completion_seq` 当作 futex word、`m_completion_waiters` 用 relaxed 原子即可满足（notifier 侧 `fetch_add` + `futex_wake` 仅在计数非零时发生）。

### #2 `Task::wait()` 的自等待检测复制了 `shared_ptr`
- **位置**：`0x17745`–`0x177bf`（`__tls_get_addr` → guard → `mov %fs:0x0(%rbp)` → `__shared_count` 拷贝 → `_Sp_counted_base` 释放）。`wait_for` 完全重复 @ `0x17a38`–`0x17ab5`。
- **证据片段**：`call *__tls_get_addr`、`cmpb $0x0,0x10(%rax)`（guard）、`addl $0x1,0x8(%rbp)`（+1）、`movabs $0x100000001,%rcx; cmp`（libstdc++ 单线程优化检查）、`mov 0x8(%rbp),%eax; lea -0x1(%rax); mov`（−1）、`cmp $0x1`。
- **浪费类型**：值语义 shared_ptr 拷贝 + 两次原子 RMW + TLS 查找，只为了做一次**指针比较**。源码 `scheduler.cpp:646/666` 是 `if (Scheduler::current_task().get() == this)`。
- **可省**：约 26–30 条/次 + 1 次 `__tls_get_addr`。
- **预期收益**：8–12 ns/任务。
- **难度**：**低**。仓库里已经有 `current_task_ptr()`（`scheduler.cpp:4783`，直接 `return t_task.get();`），改成 `current_task_ptr() == this` 即可，零语义变化。

### #3 非平凡 `thread_local` 触发 guard + `__tls_get_addr`（全库 78 处）
- **位置**：`Task::wait` @ `0x1774c`、`park_wait` @ `0x14792`、`wake_registered` @ `0x1efa3`、`WaitOnce` @ `0x2d3e6/0x2d3ff/0x2d432/0x2d44b`、`enqueue`（读 `t_incoming_lock_depth`）。
- **证据**：`lea GOT; call *__tls_get_addr; cmpb $0x0,0x10(%rax)`（guard）——**即使 `t_scheduler/t_task` 已标 `tls_model("initial-exec")`（`scheduler.cpp:217`），因为 `std::shared_ptr` 是非平凡类型，guard 地址仍要经 `__tls_get_addr` 获取**。initial-exec 只省了"取偏移"的部分，没省掉 guard 判断与函数调用本身。
- **可省**：每次访问约 4 条 + 1 次函数调用（`__tls_get_addr` 快路径 ~10–20 cycle）。park/wake 往返平均触达 6–10 次。
- **预期收益**：10–20 ns/park 往返。
- **难度**：中。引入 `thread_local Task* t_task_raw`（POD、零初始化、IE 模型，访问退化为一条 `mov %fs:off`），热路径只读裸指针；`std::shared_ptr t_task` 保留给需要保命的诊断路径。

### #4 `wake_registered` 内联了 4 份 `std::deque::size()` 魔法乘法，外加一次 64 位除法
- **位置**：`0x1ef16`–`0x1ef77`、`0x1f035`–`0x1f087`、`0x1f150`–`0x1f1b6`、`0x1f234`–`0x1f28c`；除法在 `0x1f289`（`div %rcx`）。
- **证据**：每份都是同一段 18 条指令：读 3 个 deque 的 `_M_finish/_M_start`、`sar $3`、`adc $-1`、`sar $8`、`imul $0x8f5c28f5c28f5c29`（除 24/除 16 的魔数乘法）再相加。同一个"可运行任务总数"在函数里被算了 4 次。
- **可省**：约 60 条/次 wake；`div` 本身在 Skylake 上 35–90 cycle。
- **预期收益**：10–20 ns/wake。
- **难度**：低。维护一个 `std::size_t runnable_total`（入队/出队时增减），或至少把 size 计算提取成 `__attribute__((always_inline))` 的局部变量只算一次；选 P 用 `& (n-1)`（P 数取 2 的幂）或乘法哈希替代 `div`。

### #5 头文件 inline 的 `detail::HotSpinLock::lock()` 通过 GOT 间接调用
- **位置**：`wake_registered` @ `0x1eea9`、`0x1efc9`；`Task::run` @ `0x16d67`、`0x16e36`；`enqueue` 有两处**内联展开**（`0x1b579`、`0x1b61b`）说明同一函数在不同 TU 有不同命运。
- **证据**：`call *0x230d1(%rip) # HotSpinLock::lock()@@Base+0x1dab0`；该符号在 `nm` 里是 `W`（weak，可被插入）。
- **浪费类型**：语义插入（`-fPIC` 默认 `-fsemantic-interposition`）阻止同 DSO 内联；`lock()` 快路径只有 `lock xchg` + `jne` 三条。
- **可省**：每次 1 条 `call` + 1 次栈帧 + 1 条 GOT load，实测量级 5–8 cycle；park/wake 往返 2–4 次。
- **预期收益**：3–6 ns/wake。
- **难度**：**低**。加 `-fno-semantic-interposition`（GCC 支持，配合 `-fvisibility-inlines-hidden`）或给 `lock()` 加 `__attribute__((always_inline))`。

### #6 全库 2180 处 GOT 间接调用——每次多一次 load + 间接跳转
- **样例**：`Task::cancellation_requested()`（函数体只有一条 `movzbl` 的原子 load）在 `ConditionVariable::Wait` @ `sync 0xec79`、`WaitOnce` @ `0x2d2e0` 是 `call *GOT`；`Task::id()` @ `0x2d4ef` 同理；`Fiber::SuspendForScheduler` @ `park_wait 0x147fc`；`ParkingCondition::WaitOnce` @ `Task::wait 0x178fd`。
- **浪费类型**：跨 TU/跨库小函数未内联；每条间接调用还带来分支预测器压力（间接跳转目标多变）。
- **可省**：每次 4–6 cycle；单任务热路径 8–12 次。
- **预期收益**：10–20 ns/任务。
- **难度**：**低**。链接期 `-Wl,-Bsymbolic-functions`（同 DSO 内定义不经过 PLT），把单字段访问器（`cancellation_requested`/`id`/`state`）移到头文件内联。

### #7 栈保护 canary 遍布 148 个函数（296 处 TLS 访问）
- **位置**：`Task::wait` `0x17735`/`0x177e0`、`wait_for` `0x17a28`、`spawn` `0x1f37b`、`enqueue` `0x1b445`、`Task::run` `0x16cf5`、`WaitOnce` `0x2d299`、`Mutex::lock` `0xe65d`、`worker_loop` `0x1feac`。
- **浪费类型**：每个函数 2 次 `%fs:0x28` 访问 + `sub`/`jne` + 一个栈槽 + 冷调用点；更隐性的代价是把本可全寄存器化的局部变量逼上栈（`Mutex::lock` 26 条里有 4 条纯 canary）。
- **可省**：4 条/进出 + 减少 spill。
- **预期收益**：2–5 ns/任务；`Mutex::lock` 上约 0.4 ns/次。
- **难度**：**低**（`-fno-stack-protector`，或只对 `scheduler.cpp/sync.cpp` 两个热 TU 关闭；需先过安全评审——这两个 TU 没有用户数据拷贝，风险低）。

### #8 `Task` 里 5 个 `shared_ptr/weak_ptr` 成员：构造/析构各付一次控制块原子
- **位置**：`Task::Task` `0x17d40`–`0x17e9b`（`__shared_count` 拷贝 @ `0x17f...`，`_M_release` 序列）；`~Task` `0x16150`–`0x16379` 里 **5 段** 相同的 12–16 条释放序列（`mov 0x8(%rbp); lea; movabs $0x100000001; cmp; ...; __libc_single_threaded; mov; lea -1; mov; cmp 1`）。
- **成员**：`m_cancellation_gate`(weak, 0x120)、`m_emergency_next`(0xf0)、`m_completion_notification_next`(0x108)、`enable_shared_from_this::m_weak_this`、以及 owner 侧的强引用槽。
- **可省**：40–50 条/任务 + 3–4 次原子 RMW（单线程时 libstdc++ 用 `addl` 非 lock，但 `__libc_single_threaded` 检查本身就是 2 条 load + 分支）。
- **预期收益**：15–25 ns/任务。
- **难度**：中。改裸指针 + 显式生命周期（emergency/notification 链由 Scheduler 互斥锁独占，本就满足"只在锁下修改"的注释约束）。

### #9 `spawn` 的 `operator new(0x2c0, align_val_t(0x40))` 与 ~40 条逐字段零初始化
- **位置**：`spawn` @ `0x1f3cf`/`0x1f3e6`（`mov $0x2c0,%edi; mov $0x40,%esi; call operator new(unsigned long, std::align_val_t)`），紧随其后 `0x1f4df`–`0x1f5b8` 约 40 条清 0 store（`movq $0x0`、`movl $0x0`、`movb $0x0`、`movaps xmm`）。
- **浪费类型**：over-aligned 分配走 `aligned_alloc` 路径（比普通 `malloc` 多一次对齐修正）；704 字节对象里大部分字段在随后 1 µs 内会被覆盖（`m_state_word`、`m_queued`、`m_registered` 等）。
- **可省**：约 20–30 条 store（合并 + 延迟初始化）+ 对齐分配的开销。
- **预期收益**：10–15 ns/任务。
- **难度**：低–中（先确认 64B 对齐是否真为假共享优化所需；若只是习惯性对齐，直接去掉）。

### #10 全局 GID / node generation 用 `lock xadd`
- **位置**：`Task::Task` @ `0x17d59`（`lock xadd %rax,s_next_g_id`）、`spawn` @ `0x1f470`、`WaitOnce` @ `0x2d51f`（`lock xadd g_next_node_generation`）。
- **浪费类型**：每次任务创建、每次 park 各一次跨核缓存行独占写。单线程时也至少 ~20 cycle。
- **可省**：改为线程本地批量预分配（一次 `fetch_add(256)` 摊薄 1/256），或让 node generation 用线程内计数器 + 高位编码线程号。
- **预期收益**：单线程 2–5 ns/任务；多核并发 spawn 场景 10–30 ns/任务（消除缓存行弹跳）。
- **难度**：**低**。

### #11 `sync::Mutex::lock()` 每次都读全局 `g_held_park_detect`
- **位置**：`sync 0xe650`：`mov 0x98a2(%rip),%rax; mov (%rax),%eax; test`。
- **证据**：`g_held_park_detect` 是 `std::atomic<int>`（`sync.cpp:135`），位于另一 TU，故必须 GOT 取值再解引用 = 2 次依赖 load + 1 分支。
- **可省**：约 3–5 条/次 lock（canary 另计在 #7）。
- **预期收益**：Mutex 3.75 ns 里约 0.3–0.6 ns；锁密度高的场景线性放大。
- **难度**：**低**。若该检测默认关闭：把开关做成 `static const bool`（编译期关闭时整段消失），或用 `__builtin_expect` + 每线程缓存值。

### #12 `ConditionVariable::NotifyOne` 无等待者时仍执行 `xchg`（隐式 lock）
- **位置**：`sync 0xc5d0`–`0xc5e2`：`mov (%rdi),%rdx; xor %eax,%eax; xchg %rax,0x40(%rdx); test %rax,%rax; je ret`。
- **浪费类型**：无条件的 RMW（`xchg` 带隐式 lock，即使无竞争也 ~20 cycle，且独占缓存行）。常见场景恰恰是"通知时没人等"。
- **可省**：改为先 `mov` 读一次再判空返回（省 ~15–20 cycle），仅在非空时 `xchg`。
- **收益**：约 5 ns/次空通知。
- **难度**：低–中。**必须验证内存序**：需要保证 waiter 的发布（对 `0x40` 的写）与 notifier 的读之间不丢失；若队列头用 release store 发布节点，则 acquire load 判空是安全的，否则保留 `xchg`。

### #13 `ConditionVariable::Wait` 的 `CurrentTarget()` 非内联 + 3 次全局标志读 + 多次 shared_ptr 操作
- **位置**：`sync 0xec36`（`call CurrentTarget()`，结果经隐藏返回指针写到 `0x60(%rsp)`，再用 `movzbl 0x78(%rsp)` 读出"是否构造"标志）；`0xed58`、`0xedd0`、`0xf220`、`0xf260` 四处 `mov GOT; mov (%rax),%eax` 读 `g_held_park_detect`；`0xec79` `Task::cancellation_requested()` 的 GOT 调用。
- **可省**：约 20 条/次 wait（含一次函数调用与隐藏返回区的内存往返）。
- **预期收益**：5–8 ns/wait。
- **难度**：中（`CurrentTarget` 改成返回平凡结构或直接内联）。

### #14 `Channel<T>::SendUntil` 每次 send 两次非内联调用；`ChannelSpinLock::lock` 未内联
- **位置**：`bento_channel_thread_stress` `0x1404a`（`ContextSendStatus`，返回含 `shared_ptr<Error>` 的结构，随后 `_M_release` @ `0x1406d`）、`0x1408d`（`unique_lock<ChannelSpinLock>::lock()`）、`0x14122`（`SelectProbe::SetValue<int&>`）；栈帧 `0x138`，canary @ `0x14037`。
- **可省**：`ChannelSpinLock::lock` 快路径就是 `lock xchg`+`je`，内联可省一次 call/ret + 栈帧操作（约 6–10 cycle）；`ContextSendStatus` 无 context 时应直接返回 `kReady`（一次比较），可内联。
- **预期收益**：10.73 ns 上省 1.5–3 ns（约 15–25%）。
- **难度**：**低**（把 spin 慢路径拆成 `noinline` 的 `lock_slow()`，快路径强制 inline）。

### #15 `ParkingCondition::WaitOnce` 内 4 次 `__tls_get_addr`（WaitNode 对象池）
- **位置**：`0x2d3e6`、`0x2d3ff`（`t_pool` 的 guard + 取 pool）、`0x2d432`、`0x2d44b`（同一 pool 再一次）。
- **浪费类型**：thread_local 池是带析构的容器 → 每次访问都 guard 检查 + `__tls_get_addr`；其中两次只是为了读 `pool.end()` / 写 `pool.end()`。
- **可省**：约 30–40 条 + 3 次函数调用/park。
- **预期收益**：10–15 ns/park。
- **难度**：低（换成 POD 自由链表 `thread_local WaitNode* t_free_head`，零 guard、零 `__tls_get_addr`）。

### #16 `worker_loop` 每轮重复 4 次空指针判 + 3 次状态位读
- **位置**：`0x200c9`、`0x200ee`、`0x20110`（同一 `cmpq $0x0,0x90(%rsp)`），`0x200d8`/`0x200df`、`0x200f7`/`0x20101`（同一 `movzbl 0xa69(%rdi); test $0x2`）。
- **浪费类型**：因中间存在可能写内存的调用，编译器无法 CSE，同一条件重复求值。
- **可省**：10–14 条/迭代。
- **预期收益**：3–5 ns/任务。
- **难度**：**低**（在循环头缓存 `bool has_task` / `bool stopping` 到局部变量）。

### #17 栈帧过大：`enqueue` 712B、`Task::run` 760B、`worker_loop` 1032B
- **位置**：`0x1b43e`（`sub $0x2c8`）、`0x16cee`（`sub $0x2f8`）、`0x1fea5`（`sub $0x408`），另有每轮迭代 `0x200a1`–`0x200b1` 三条 `movaps` 清 48 字节。
- **浪费类型**：Fiber 栈默认 4KB 量级，深调用链里 1KB 栈帧直接提高栈溢出/扩容概率，也让 `-fstack-clash-protection`/页触碰更痛。
- **可省**：把 `enqueue`/`run` 的冷分支拆成独立 `noinline` 函数，热路径栈帧可降到 ~0x80。
- **预期收益**：2–5 ns + 降低 Fiber 栈增长概率。
- **难度**：中（纯重构，不改语义）。

### #18 `LocalRunQueue` 索引用 `lea+shl` 算 ×24
- **位置**：`push 0x15b11`–`0x15b19`（`movzbl %cl,%eax; lea (%rax,%rax,2),%rdx; shl $0x3,%rdx`）、`pop 0x11f4e`–`0x11f55`。
- **可省**：条目改成 32 字节对齐后 `shl $5` 一条搞定（省 1–2 条，且避免跨缓存行）。
- **预期收益**：0.5–1 ns/次 push/pop。
- **难度**：**低**。

### #19 `park_wait` 里重复解引用 `shared_ptr` 与重复读同一状态字节
- **位置**：`0x147bf`、`0x147cd`、`0x147e9`、`0x14806`、`0x14809`：`mov (%rbx),%rax; movzbl 0x8c(%rax)` 共出现 5 次（`shared_ptr const&` 每次重新取裸指针）；`0x147d9`/`0x147e9` 与 `0x14802`/`0x14809` 读同一取消位。
- **可省**：6–8 条/park。
- **预期收益**：2 ns。
- **难度**：**低**（进入函数时一次性 `Task* t = task.get();`）。

### #20 `wake_registered` 反复重载 `Impl*`
- **位置**：`0x1ee90`、`0x1efba`、`0x1efdc`、`0x1f0bb`、`0x1f0c8`、`0x1f0e5`、`0x1f0f7`、`0x1f11a` 等 6–8 次 `mov 0x10(%rbp),%rax`。
- **可省**：4–5 条/wake。
- **预期收益**：1–2 ns。
- **难度**：**低**。

### #21 `spawn` 里 `std::function` 的移动/判空/销毁三条路径
- **位置**：`0x1f437`–`0x1f45b`（判空 + `movdqu` 16B + 清源）、`0x1f5c3`–`0x1f5da`（`test; lea; mov $3,%edx; call *%rax` 调 manager 销毁）。
- **可省**：约 8–10 条/任务（若 `spawn` 接收可平凡移动的 `Function&&` 并保证不析构空壳，可省掉 manager 调用）。
- **预期收益**：2–3 ns。
- **难度**：低。

---

## 2. 与 Go 的对照（为什么不只是"常数因子"）

| 维度 | Go | go2cpp 现状 | 差距来源 |
|---|---|---|---|
| `go f()` 入口 | `runtime.newproc` **19 条**（仅 stack guard + 打包参数 + `systemstack`） | `Scheduler::spawn` **194 条** + `enqueue` 热路径 120–200 条 | `std::function` 搬运、`make_shared` 控制块、40 条零初始化、canary、参数经栈传递 |
| G 分配 | `gfget` 从 P 的空闲链表复用，**通常无 malloc** | 每任务 `operator new(0x2c0, align 64)` + `operator delete` | 无对象池；`Task` 无复用 |
| 生命周期 | 无引用计数（GC） | 5 个 `shared_ptr/weak_ptr` 成员 + `enable_shared_from_this` | 构造/析构各 3–5 次原子 RMW |
| 等待/唤醒 | 直接操作 G 状态 + `notesleep`/futex，无 mutex | `m_completion_mutex` + `ParkingCondition` 两套叠加 | 同一条路径两次同步 |
| 栈保护 | 无 canary（仅 stack guard，必需） | 148 个函数带 canary | 编译器默认开关 |
| 跨 TU 调用 | 单二进制，静态链接，全内联 | 26 个 `.so`，2180 处 GOT 间接调用 | 库拆分 + 语义插入 |

结论：go2cpp 的差距**不是** Fiber 切换（23.5 ns 已近下限），而是"**每个任务要付一次堆分配、5 次引用计数、2 套同步原语、以及 148 个函数的栈保护**"。要逼近 Go 必须动引用计数模型（#8）与对象池，单纯指令级微调只能吃掉其中一部分。

---

## 3. 收益汇总与路线建议

| 批次 | 包含项 | 累计可省 | 预估任务耗时 |
|---|---|---|---|
| 现状 | — | — | 656–764 ns |
| A：零/低风险（纯编译选项 + 局部改写） | #2 #5 #6 #7 #10 #11 #14 #16 #18 #19 #20 #21 | ~120–160 条指令 + 8–10 次间接调用 | **560–600 ns** |
| B：中风险（结构小改） | #1 #3 #4 #9 #15 #17 | 再加 ~120–180 条 + 4–6 次原子 | **470–520 ns** |
| C：架构级（引用计数/对象池） | #8 + Task 池化 | 再加 ~60–80 条 + 3 次堆操作 | **380–430 ns** |

> 保守口径：A 批约 −8%~−15%，B 批累计约 −25%~−30%，C 批才可能进入"2× Go"区间。**不建议**把 #12（NotifyOne 去 xchg）与 #8 之外的改动当成"能到 135 ns"的依据。

### 建议实施顺序
1. 先做**编译选项包**（`-fno-semantic-interposition`、`-Wl,-Bsymbolic-functions`、热点 TU `-fno-stack-protector`）：零代码改动、可整体回滚、直接覆盖 #5/#6/#7 三项。
2. 再做**纯局部改写**：#2（一行）、#10（批次 ID）、#14（spin lock 拆快慢路径）、#16/#19/#20（局部变量缓存）——全部可单测验证。
3. 然后**去锁**：#1（完成通知合并为 futex 字）、#11（开关编译期化）。
4. 最后评估 **Task 对象池**（复用而非 `new/delete`，一次性解决 #8/#9）。

---

## 4. 复现命令（只读）

```bash
cd /UserData/CodexWorkSpace/Go2cpp
objdump -d --no-show-raw-insn -C .build-bento-wsl/libgo2cpp_scheduler.so > /tmp/asmrev/scheduler.asm
objdump -d --no-show-raw-insn -C .build-bento-wsl/libgo2cpp_sync.so      > /tmp/asmrev/sync.asm
objdump -d --no-show-raw-insn -C .build-bento-wsl/libgo2cpp_channel.so   > /tmp/asmrev/channel.asm

# 关键计数
grep -c 'fs:0x28'            /tmp/asmrev/scheduler.asm   # 296 canary 访问
grep -c 'call   \*0x'        /tmp/asmrev/scheduler.asm   # 2180 GOT 间接调用
grep -c '__tls_get_addr'     /tmp/asmrev/scheduler.asm   # 78
grep -c 'div    %'           /tmp/asmrev/scheduler.asm   # 20（其中 1 处在 wake 选 P 路径）

# Go 对照
go tool objdump -s 'runtime\.newproc' /tmp/asmrev/goprobe
```

中间产物：`/tmp/asmrev/{scheduler,sync,channel}.asm`、`/tmp/asmrev/newproc.txt`（可随时删除，不属于仓库）。
