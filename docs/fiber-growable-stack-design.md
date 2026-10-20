# Fiber 可变长栈设计方案（2026-10-19）

> 目标：评估 Go2Cpp Fiber 从「固定 128KB 栈」演进到「可变长栈」的可行路线。
> 方法：3 个并行调研 agent（Go runtime / 用户态栈增长 / 本项目代码）+ 主 agent 交叉验证。
> 结论按「风险从低到高」排列，**推荐先做 P0，用数据回答是否真的需要 P1**。

## 0. 结论摘要

| 方案 | 机制 | 栈地址 | 需要编译器支持 | 风险 | 建议 |
|---|---|---|---|---|---|
| **P0 预留 + demand paging** | `mmap(8MB, PROT_NONE)` + 顶部 128KB 提交 | 不变 | 否 | 低 | **先做** |
| **P1 SIGSEGV + mprotect 增长** | fault 时把 guard 下移、提交更多页 | 不变 | 否 | 中 | 可选（仅 release） |
| **P2 `-fsplit-stack` 分段栈** | 编译器序言插桩 + 新段 | 段间跳转 | **是** | 高 | 实验 |
| 共享栈 + 拷栈（coost 式） | 切换时 copy-in/out | 物理栈固定 | 否 | 高（拷栈成本、指针修正） | 不做 |
| 栈复制（Go 式） | 搬家 + 逐指针修正 | **改变** | **精确栈映射** | 不可行 | 不做 |

**核心判断**：C++ 缺「编译器序言插桩 + 精确栈映射 + 安全点」，Go 的复制栈无法照搬；能走的只有「不移动栈」的两条路——预留地址空间（P0/P1）或编译器分段（P2）。

## 1. 当前实现（改造基线）

- 默认栈 `Fiber::DefaultStackSize() = 128KB`（`include/go2cpp/concurrency/fiber.hpp:127`）。
- 分配：`FiberStack` 构造时 `mapping_size = usable + 1 guard page`，`mmap(RW)` + `mprotect(底部页, PROT_NONE)`（`src/concurrency/fiber.cpp:330-420`）。
- 地址语义：`bottom() = mapping + page`、`stack_pointer() = mapping + mapping_size`（栈从高向低增长）。
- 切换：自定义汇编 `go2cpp_context_make(stack_bottom, stack_size, entry)` + `go2cpp_context_jump`（`src/concurrency/context_x64.S`），只换寄存器与 rsp；分配/保护/池化全在 C++。
- 池化：thread-local 8 + 全局 128 个缓存（`src/concurrency/fiber.cpp:380-430`），`FiberBin` 默认 32/线程（`scheduler.hpp:227`）。
- 归一化：`normalize_stack_size` 页对齐、最小 2 页、上限检查（`fiber.cpp:256`）。
- **测试盲区**：`grep -rn 'fiber_stack|stack_limit|stack_overflow' tests/` 零命中——没有任何栈溢出/深递归用例。

## 2. 为什么不能照搬 Go

Go 两代变长栈共用同一触发层：编译器在每个函数序言比较 SP 与 `g->stackguard`，失败调 `runtime.morestack`（`runtime/stack.go` 顶部注释，go1.21 与 go1.2 相同）。

- **分段栈（≤1.2）**：分配新段、链式连接，不移动旧栈 → 不需要指针图；代价是 `lessstack/oldstack` 蹦床与 **hot split**（帧大小卡在段边界时每次调用都分配/释放段）。参数：`StackExtra=2048`、`StackMin=8192`（`runtime/stack.h`，go1.2）。
- **复制栈（1.3+）**：`newstack` 分配 2 倍新栈，`copystack` 用 `memmove` 搬家，并以高地址端为锚点对「所有指向旧栈的指针」统一 `+delta`；依赖编译器生成的 `_FUNCDATA_LocalsPointerMaps` / `_FUNCDATA_ArgsPointerMaps` / `_FUNCDATA_StackObjects` / `_PCDATA_StackMapIndex`（`runtime/stack.go` 的 `adjustframe`/`adjustpointers`/`getStackMap`）。

C++ 无法照搬复制栈的三个硬障碍：

1. **无法在任意 PC 区分指针与整数**（`0x7ffc1234` 可能是指针、`size_t` 或浮点位模式）；保守扫描能「找到」却不能安全「改写」。
2. **无法枚举外部栈引用**：`ucontext_t`、callee-saved 寄存器、`io_uring` SQE/CQE、注册缓冲区、其他线程持有的栈指针，没有统一位置可修正。
3. **Go 用语言契约保证精确性**（指针类型含指针、其他类型不含；`unsafe` 塞整数非法），C++ 没有也不可能有这条契约。

Go 自己还禁止在 syscall 中复制栈（`copystack` 中 `gp.syscallsp != 0` 直接 `throw`），说明「复制点必须无外部引用」是硬约束。

**可借鉴的只有工程细节**：栈池化（`stackpool`/per-P cache）、上限与明确报错（`maxstacksize` + `throw`）、按高水位自适应初始栈、以及「只在安全点扩容、绝不在任意挂起点搬栈」的设计思想。

## 3. 方案 P0：预留虚拟地址 + demand paging（推荐先做）

把「固定 128KB 映射」改为「预留 N 字节、实际按需触页」：

```cpp
// reserve = 8MB（可配），初始只提交顶部 128KB
void* base = mmap(nullptr, reserve, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
mprotect(base, page, PROT_NONE);              // 低端 guard page
char* top = static_cast<char*>(base) + reserve;
```

- 只有实际触碰的页进入 RSS；128KB 用量只占 32 页，其余预留不花物理内存。
- 栈溢出在 guard page 触发 SIGSEGV（行为与现在一致，但 guard 下方是整片未提交区，诊断更干净）。
- 零复制、零插桩、对 `MakeContext/JumpContext` 完全无侵入；改动集中在 `FiberStack` 与 `SchedulerConfig`。
- 代价：每个 Fiber 1~2 个 VMA（`mprotect` 会拆分 VMA）；`vm.max_map_count` 默认 65530，万级 Fiber 需池化复用；虚拟地址 = reserve × fiber 数（8MB × 10k = 80GB 虚拟，x86-64 可接受）。
- 配置建议：`fiber_stack_size` 语义改为「初始提交量」（默认仍 128KB），新增 `fiber_stack_reserve_size`（默认 1~8MB）与 `fiber_stack_max_size`。

**P0 的局限**：它只是把「上限」做大，并不自动增长——超过提交量的深递归仍会在 guard page 崩溃。要真正「长」，需要 P1。

## 4. 方案 P1：SIGSEGV + mprotect 增长（可选）

当 reserve 很大（如 64MB）或需要精确 commit 记账（`overcommit_memory=2`）时：

1. 整段 `PROT_NONE`，初始只把顶部 128KB 提交为 RW；下方紧邻的未提交页天然就是 guard（**不需要移动 guard**，这是最省 syscall 的设计）。
2. 每线程 `sigaltstack`（256KB，`mmap` 而非 `malloc`）+ `SA_SIGINFO | SA_ONSTACK`（不要 `SA_NODEFER`）。
3. handler 内四步判定：
   - 重入保护（`thread_local` 标志）；
   - fault 地址在 `[base, committed)` 且是页边界；
   - 从 `ucontext_t` 取 `REG_RSP` 与 `REG_ERR`：SP 必须在本栈、fault 地址不能比 SP 低太多（仿内核 64KB 窗口，建议 128KB~1MB）；
   - 未到 `limit`；
4. 一次 `mprotect(new_low, committed - new_low, RW)` 把新区域提交，`committed = new_low`，然后 **直接 return**（不 `longjmp`、不换上下文）。
5. 失败路径：恢复 `SIG_DFL`、解除屏蔽、`raise(sig)`，保留 core dump 语义。

与 `swapcontext` 的配合：`tls_stack` 必须在 swap 之前指向目标 fiber、切回后恢复源 fiber；handler 内不切换上下文，因此 `uc_sigmask` 不会把「处理期间屏蔽的 SIGSEGV」带到别的 fiber。

**必须接受的三个硬限制**：

1. **ASAN/TSAN 基本不可用**：ASAN 自己装 SIGSEGV handler 且需要 `__sanitizer_start_switch_fiber`；seastar 的做法是 guard 只在 Debug/Sanitize/Dev 开，ASAN 构建回退固定大栈（`SEASTAR_THREAD_STACK_GUARDS`）。
2. **大栈帧会跳过 guard**（`alloca`/VLA/大数组），其 fault 与野指针在语义上无法区分（内核 v5.0 已因此删除 64KB 窗口检查）。只能放宽窗口 + 业务代码开 `-fstack-clash-protection` + 文档写明「深帧 abort」。
3. **死循环风险**：`mprotect` 失败或判定写错会无限 SIGSEGV（libsigsegv 作者明确警告）。用重入标志 + fault 预算 + 强制检查返回值。

**真实先例**：seastar `thread.cc` 的 `bypass_stack_guard` 就是本方案的最小实现（判断 `REG_ERR & 0x2`、页对齐 `si_addr`、`mprotect` 成 RW），其默认线程栈也正好是 128KB。

## 5. 方案 P2：`-fsplit-stack` 分段栈（实验）

- GCC/Clang `-fsplit-stack`：函数序言比较 SP 与 TCB 中的栈下限，失败调 `__morestack`；新段在返回时进入 `__releasestack` 释放。
- Boost.Context 的 `segmented_stack` 要求 gcc≥4.7 / clang≥3.4，且**只能与 `callcc()`（ucontext 实现）一起用**；本项目已改用自定义 fcontext 汇编，不适用。
- 限制：仅 x86-64 Linux + gold/lld；含第三方库在内的全量重编译；varargs、函数指针、异常展开、调试器都有坑。
- 定位：独立编译配置的 PoC，不做默认路径。

## 6. 方案 D：共享栈 / 栈复制（不做）


### 6.1 主流 C++ 协程库的栈策略（交叉核对）

| 库 | 栈模型 | 配置/默认 | guard page | 是否变长 |
|---|---|---|---|---|
| Boost.Context | fixedsize / protected_fixedsize / **segmented** | `stack_traits`；segmented 需 gcc≥4.7 且只支持 `callcc()`(ucontext) | protected 有 | segmented 是（编译器分段） |
| Boost.Fiber | fixedsize / protected_fixedsize / segmented | `protected_fixedsize_stack` 每 fiber 一次 mprotect，文档称 expensive | 有 | segmented 仅在 ucontext 实现下 |
| Folly Fibers | 固定栈 | `guardPagesPerStack` | 有 | 否 |
| coost | **共享栈 + 拷栈** | 每协程 buffer 按需增长，物理栈固定 | 无 | 缓冲变长，栈地址固定 |
| libaco | 固定栈 | `ACO_STACK_SIZE` | 有 | 否 |
| libco | **共享栈 + 拷栈** | 共享栈区 + 切换 copy-in/out | 无 | 否（物理栈固定） |
| photon | 固定栈 | 配置 | 有 | 否 |

结论：**没有主流 C++ 库实现「栈复制」**（Go 式搬家）；「共享栈」是另一条路线，它省的是每协程物理内存，代价是每次切换拷栈、且指针仍指向固定物理栈。Go2Cpp 当前「独立栈 + 池化」在切换成本上优于共享栈，内存上可通过 P0 预留进一步优化。

- **共享栈（coost 式）**：多个协程共用一段物理栈，切换时把当前栈内容保存到各自 buffer、恢复目标 buffer。等价于每次切换都做一次栈复制，仍然需要修正栈上指针（无栈映射时不可行）；且 coost 的「每协程 buffer 按需增长」只降低每协程内存，不解决栈地址移动问题。
- **栈复制（Go 式）**：见 §2，C++ 无法安全调整指针，除非全量代码经 LLVM statepoint 编译并消除所有外部栈引用——工程上不可行。

## 7. 本项目改造点清单

| 位置 | 现状 | P0 改动 | P1 改动 | 风险 |
|---|---|---|---|---|
| `src/concurrency/fiber.cpp:330-420` `FiberStack` | `mapping_size = usable + guard`；`bottom() = mapping+page`；`stack_pointer() = mapping+mapping_size` | `mmap(reserve)` + 顶部提交 usable；`bottom() = mapping + reserve - usable` | 增加 `committed`/`limit` 字段与增长方法 | 中 |
| `src/concurrency/fiber.cpp:256` `normalize_stack_size` | 0→128KB，最小 2 页，上限检查 | 校验 reserve ≥ usable，新增 reserve 归一化 | 同左 | 低 |
| `src/concurrency/fiber.cpp:380-430` `StackCache` | 按 `size` 缓存 mmap | 缓存键加 `reserve`；可加 `MADV_DONTNEED` | 增长后回收水位 | 中 |
| `include/go2cpp/concurrency/scheduler/scheduler.hpp:206` `SchedulerConfig` | `fiber_stack_size` | 增加 `fiber_stack_reserve_size` / `max_size` | 同左 | 低 |
| `src/concurrency/context_backend.cpp:39` `MakeContext` | 用 `stack_bottom + stack_size` 设 rsp | **不需要改**（栈地址不变） | 不需要改 | 低 |
| `src/concurrency/context_x64.S` | 只换 rsp | **不需要改** | 不需要改 | 低 |
| `tests/` | 无栈溢出/深递归用例 | 增加深递归与边界测试 | 增加增长/指针有效性测试 | 低 |

## 8. 测试矩阵（拒绝冒烟测试）

- **功能**：随机深度递归（1KB~8MB 对数分布，10^5 次）；边界写 `guard_bottom-1/guard_bottom/guard_bottom+1`；读故障 vs 写故障（`REG_ERR & 0x2`）；`alloca(8KB)` 大帧（预期走 fatal）。
- **ucontext 一致性**：增长后从深帧 `swapcontext` 出去再切回，验证栈上局部变量指针仍有效（「不复制栈」的核心断言）。
- **并发/性能**：M 线程 × N fiber 同时增长；`perf trace -e mprotect` 次数应 ≈ 增长次数；`perf stat` 观察 IPI/TLB；四方对比（固定 128KB / 8MB 预留不提交 / SIGSEGV 增长 / `-fsplit-stack`）的切换延迟、创建速率、RSS、VmSize、VMA 数。
- **故障注入**：`LD_PRELOAD` 让 `mprotect` 返回 ENOMEM 验证 fatal 路径；handler 内嵌套 fault 验证 altstack；`ulimit -v` 验证 mmap 失败。
- **真实场景**：深递归 JSON/AST 解析、正则回溯、大帧序列化、接近上限的退化路径、ASAN 构建回退、1 万 fiber 的 VMA 压力。

## 9. 参考链接

- Go：[Go 1.3 Release Notes](https://golang.google.cn/doc/go1.3)、[Go 1.4 Release Notes](https://golang.google.cn/doc/go1.4)、[runtime/stack.go (go1.21)](https://github.com/golang/go/blob/go1.21.0/src/runtime/stack.go)、[runtime/stack.c (go1.2)](https://github.com/golang/go/blob/go1.2/src/runtime/stack.c)、[LLVM Statepoints](https://llvm.org/docs/Statepoints.html)
- 内核/信号：[Overcommit Accounting](https://docs.kernel.org/6.9/mm/overcommit-accounting.html)、[sigaltstack(2)](https://man7.org/linux/man-pages/man2/sigaltstack.2.html)、[getcontext(3)](https://man7.org/linux/man-pages/man3/getcontext.3.html)、[Qualys Stack Clash](https://www.qualys.com/2017/06/19/stack-clash/stack-clash.txt)
- 先例：[seastar bypass_stack_guard](https://github.com/scylladb/seastar/commit/7a3b4b4e4e4a90ce1b161c73dbf2bd14f32b8703)、[Boost.Fiber stack allocation](https://www.boost.org/doc/libs/1_84_0/libs/fiber/doc/html/fiber/stack.html)、[GCC Split Stacks](https://gcc.gnu.org/wiki/SplitStacks)
- 讨论：[SO: MAP_GROWSDOWN 不增长](https://stackoverflow.com/questions/56888725/why-is-map-growsdown-mapping-does-not-grow)、[SO: Coroutines with a growing stack in C](https://stackoverflow.com/questions/5618641/coroutines-with-a-growing-stack-in-c)、[libsigsegv 邮件列表](https://lists.nongnu.org/archive/html/bug-libsigsegv/2019-01/msg00002.html)

## 10. 实现状态（2026-10-19，已落地并复核）

### 10.1 最终配置

| 字段 | 值 | 说明 |
|---|---|---|
| `SchedulerConfig::fiber_stack_size` | **2048** | 页对齐后初始可用 1 页（4096） |
| `SchedulerConfig::fiber_stack_reserve_size` | **8 MiB** | 每 Fiber 预留虚拟地址 |
| `SchedulerConfig::fiber_stack_growable` | **true** | `GO2CPP_GROWABLE_STACK=0` 可关闭 |
| `Fiber::DefaultStackSize()` | 128 KiB（不变） | 直接构造 `Fiber(fn)` 的兼容默认 |
| `Fiber::DefaultStackReserveSize()` | 8 MiB（新增） | 直接构造路径也按 growable 规则预留 |

### 10.2 实现要点

- **布局**：`mmap(reserve, PROT_NONE, MAP_NORESERVE)` + 顶部 `initial_usable` 提交为 RW；`committed` 向下增长，未提交区天然是 guard。
- **handler**：进程级 SIGSEGV；每线程 256KiB `sigaltstack`（mmap 分配）；`SA_SIGINFO | SA_ONSTACK`，不加 `SA_NODEFER`；判定 `fault ∈ [base, committed)`、`rsp ∈ [base, top)`、`|rsp-fault| ≤ 256KiB`、未到上限；从 `min(fault, rsp)` 向下 64KiB `mprotect`；失败恢复 `SIG_DFL` + `raise`。
- **与设计稿的一处偏离**：设计稿要求 `rsp ∈ [committed, top)`，实测函数序言先 `sub rsp` 再写局部变量（probe: fault=committed-8, rsp=committed-0x50），因此放宽为 `rsp ∈ [base, top)` 并按 `min(fault, rsp)` 提交，否则普通深递归会误 fatal。
- **TLS**：`t_current_growable_stack` 在 resume/entry/finish_switch/suspend 维护；Fiber 迁移到新线程后 resume 前补装 altstack。
- **池化**：缓存键 `(initial_usable, reserve)`；复用前把增长过的区间重新 `mprotect(PROT_NONE)` 并重置 `committed`（只重置指针会让后续溢出静默写坏已提交内存）。

### 10.3 独立复核结果（主 agent，2026-10-19 22:40）

| 项目 | 结果 |
|---|---|
| `ctest` | **116/116 通过**（115 既有 + `go2cpp_fiber_growable_stack`），独立跑 60.75s |
| 深递归 | 256KiB-class（1024 帧）、1MiB-class（4096 帧）通过；增长后外层局部变量地址/值不变；4 workers × 32 tasks 并发增长通过 |
| A 场景（200×2 depth1） | QPS **142,491** / cpu **70.3µs** / RSS **16,984kB**（改动前 142,448 / 71.2） |
| s3（8000 rate1, P=4） | QPS **7,895** / cpu **121.9µs** / RSS **161,948kB**（改动前 125.2） |
| 内存布局 | 8000 连接 **VmSize 62.8GB**（8000×8MiB）、VmRSS 207MB（采样时点，稳态 161MB） |
| 增长证据 | strace：初始 `mprotect(4096,RW)`，每次增长 `mprotect(69632,RW)`，释放前 `mprotect(1183744,PROT_NONE)` |

结论：**性能无回退（cpu 还略低），RSS 不变，VmSize 按 8MiB×N 预留，深递归 1MiB 可用。**

### 10.4 已知限制

1. handler 是**进程级 SIGSEGV 覆盖**：与用户自定义 SIGSEGV handler、ASAN/TSAN、JIT 冲突；需要时用 `GO2CPP_GROWABLE_STACK=0` 回退。
2. `Fiber(fn)` 直接构造仍默认 128KiB（兼容），只有 Scheduler 路径默认 2048。
3. 大帧 >256KiB（`alloca`/大数组）按 fatal 处理，不增长。
4. 32 位 / 非 x86_64 / 非原生上下文后端退化为固定栈。
5. `docs/design.md` 等旧文档里「Fiber 栈无自动扩容」的描述尚未同步更新。
