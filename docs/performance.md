# 性能整改与 Coost 对比

## 2026-09-30 调度器与混合锁专项收尾

本节是本轮代码变更后的最新结果，优先级高于本文早期采样。测试使用
`build-release-current`，WSL2/Linux，Release 编译；宿主调度会造成明显抖动，
因此只报告重复运行区间，不把单次最好成绩当作保证。

### 调度器热路径

本轮的实际执行链路为：

1. 外部生产者先完成 G 的 owner、取消门、注册表 admission 和 queued 状态；
   已注册的 IO/定时器唤醒跳过 registry 分片锁。
2. 新任务进入 16 条 incoming 条带，生产者通过 TLS 固定条带；非空位图让
   worker 不必为明显为空的条带逐一加锁。
3. worker 先取当前 P 本地队列，再按亲和性预算处理 incoming，最后才进入全局
   admission、窃取和救援队列。当前 worker 唤醒本地 G 时直接回本地 P，避免
   再次走外部 admission。
4. Fiber 返回后只在仍有 runnable G 时通知其他 M；维护周期中的 M 扩容、空闲
   M 回收和 sysmon 观察不进入每次恢复的全局锁路径。

公平调度基准为 50,000 个 G、每个 G 4 次 yield、8 个生产线程、8 个 P。当前
Go2Cpp 的 `spawn_ms + elapsed_ms` 为 `84～103 ms`（5 次：84、92、101、103、
95 ms）；同一份本机 coost 对照为 `18～36 ms`。中位数约为 coost 的 `3.0x`，
尚未达到 1.25x 目标。关闭 `collect_metrics` 只改变几个百分点，说明主要成本
不是计时器，而是每个 G 的 `shared_ptr`/注册表生命周期、Boost.Context 受保护
栈以及 G 状态转换和跨 M admission。

继续把这些状态改成无锁裸指针会破坏 shutdown、取消和跨线程最后一个引用的
所有权；把受保护栈替换为 coost 的快照栈也会失去 guard page，并且要求禁止栈
地址逃逸。当前没有在未证明安全前做这两类危险优化。

### 混合 Mutex

`go2cpp::sync::Mutex` 采用一个原子锁位加一条受 `m_mutex` 保护的 FIFO 等待队列。
Fiber 和普通线程共用队列；Fiber 等待节点在调用栈上，带 Context 的节点使用
线程本地小缓存。Unlock 始终在队列锁下完成线性化，并把锁位直接交给队头，避免
“先清锁、后来者抢到、旧 owner 再误清锁”的竞态。已有 waiter 时禁止新的
自旋者越过队头。

专项混合基准（2,000 Fibers、32 普通线程、206,400 次临界区）当前为
`8.5～8.8 s`；coost 为 `1.6～5.7 s`（一次宿主抖动离群），线程
`std::mutex` 基准为 `39～40 ms`。因此该场景仍不适合高争用共享锁，不能宣称
达到线程池或 coost 性能。原因是 Fiber 阻塞后必须经过 park、状态发布、跨 M
唤醒和安全栈恢复；一次 CAS 优化无法消除这条链路。

规范使用方式：

- 短临界区、低到中等争用时可直接使用 `sync::Mutex`，普通线程和 managed Fiber
  可以同时访问同一实例。
- 不要在持锁期间执行 socket、文件、定时器等待或主动 park；先复制所需状态，
  解锁后再做阻塞操作。
- 高争用计数器优先使用原子变量或按 P/任务类别分片；普通线程独占的热点使用
  `std::mutex`，不要为了统一接口把所有锁都换成混合锁。
- `TryLock` 在存在 FIFO waiter 时返回 false，这是公平性约束，不是异常。

### 栈后端结论

本机 coost HEAD `c1cc11b32d5208912675a98fe091686848e60bc3` 使用固定约 1 MiB
执行栈槽，挂起时把栈内容复制到可增长的 Buffer；Buffer 增长不等于执行栈自动
扩容，深栈仍可能越过固定槽。它没有 Go 式 `morestack`/`copystack` 的保护页、
完整 C++ RAII 展开和 ASan/TSan Fiber 标注。Boost 的 `segmented_stack` 又依赖
`-fsplit-stack`/libgcc，工具链、异常和 sanitizer 兼容性受限。因此本项目继续
使用 Boost.Context 受保护固定栈，自动动态栈列为未完成能力；不能以 coost 的
快照机制冒充已解决。

### 其他高负载结果

同一轮 `go2cpp_high_load_stress` 三次：计算 Fiber `1415～1522 ms`、线程池
`1372～1414 ms`；IO 多路等待 Fiber `646～786 ms`、线程 poll
`1785～1919 ms`；Channel Fiber `21～27 ms`、线程 `67～88 ms`。这些结果说明
IO、Channel 和低争用计算场景仍有优势，但不能推导混合锁和公平调度已经全面
超过 coost。

本文记录本轮调度器和混合锁优化的实际结果。所有数据来自同一台
WSL2/Linux 主机、同一份源码和同一条 `go2cpp_high_load_stress` 命令；
时间单位为毫秒。

## 本轮实现的优化

- 调度器在本地 P 队列成功取到 G 后不再获取调度器全局 admission 锁。
- G/M/P 运行计数和任务亲和性记录统一放到两条取任务路径之后，避免本地
  快路径遗漏 M/P 计数。
- `maybe_grow()` 从每次 Fiber 恢复改成有界维护周期；当活跃 M 少于 P 且
  仍有 runnable G 时立即补齐初始 worker，保留 P=1/并发 shutdown 的语义。
- Scheduler 恢复 Fiber 时首次绑定父级，后续同一 G 的恢复跳过重复父链元数据
  锁；发生 M/P 迁移时仍更新调试绑定。
- 混合锁在短临界区竞争时先进行最多 16 次 Fiber 协作让出，只有持续竞争才
  创建等待节点并进入 FIFO handoff。已发布等待者存在时禁止无序插入，避免
  新的 TryLock/Lock 越过 FIFO 队列。

这些改动没有把普通线程锁伪装成 Fiber 锁，也没有用异步强行切断 C++ 栈；
Fiber 等待仍通过 Scheduler park，普通线程仍通过条件变量等待。

## 高负载结果

测试参数固定为：调度器 50,000 个任务、每个任务 4 次 yield；计算负载
8,000 个任务、每个任务 100,000 次循环；混合锁为 2,000 个 Fiber 和
32 个普通线程；Channel 为 32,000 条消息；IO 为 4,096 个 reader、25
轮 readiness。代表性运行结果如下：

| 场景 | Fiber/Go2Cpp | 线程基准 | 比例 |
|---|---:|---:|---:|
| 计算任务 | 1454 ms | 1415 ms | 1.03x |
| 调度与 yield | 474 ms | 65 ms | 7.29x |
| 混合 Mutex | 103 ms | 37 ms | 2.78x |
| Channel | 58 ms | 80 ms | 0.73x（Fiber 更快） |
| IO 等待 | 1748 ms | 1812 ms | 0.96x（Fiber 更快） |

旧实现同一基准的调度约 6.2 秒、混合 Mutex 约 13.9 秒；本轮分别降到
约 0.47 秒和 0.10 秒。重复运行存在宿主调度抖动：调度通常为
0.45～0.57 秒，混合 Mutex 在稳定运行时约 0.10～0.20 秒。高负载测试
连续完成全部校验，没有发现重复执行、丢任务、锁失败或消息丢失。

调度基准中的线程版本调用 `std::this_thread::yield()`，它不保存用户栈，
所以不能把 7.29x 直接解释为 Fiber 上下文切换的纯成本。当前指标中
`fiber_resume_ns` 可用于测量真实 Fiber 恢复累计时间；本轮代表性运行是
约 0.79 秒/250,000 次恢复，即约 3.2 微秒/次，其中包含安全状态、errno、
调度边界和统计开销。

## 与 Coost 的可比性

本轮直接使用本机已有的 Coost checkout：
`/mnt/e/CodexWorkspace/Go2Cpp/coost`，构建时的 HEAD 为 `c1cc11b`。
该 checkout 在开始前已经是 dirty 状态，因此下面的数字代表这份本地快照，
不代表某个干净发布版；没有修改 Coost 源码。参见
[Coost 官方仓库](https://github.com/idealvin/coost)。

两个项目都在同一台 WSL2/Linux 主机上以 Release 构建。调度测试使用 50,000
个任务、每个任务 4 次让出、8 个生产线程；Coost 使用 `co::sleep(0)` 让任务
被定时器重新放回 runnable 队列，因而只比较“自动重新调度”这条路径，不能把
Coost 的 `co::yield()`（需要调用者显式再次 resume）当成同一个 API。混合锁
测试使用 2,000 个协程、32 个普通线程，共 206,400 次加解锁；IO 测试使用
4,096 个 socketpair reader、25 轮 readiness。

| 场景 | Coost | Go2Cpp（本轮） | 结果 |
|---|---:|---:|---|
| 自动调度与 yield | 13～52 ms | 436～497 ms | Go2Cpp 慢约 8～38 倍 |
| 混合 Fiber/线程 Mutex | 1,097～1,766 ms | 99～107 ms | Go2Cpp 快约 10～18 倍 |
| 多 fd IO 等待 | 486～569 ms | 1,666～2,098 ms | Go2Cpp 慢约 3.0～4.3 倍 |

调度差距主要来自实现模型：Coost 使用少量共享栈，切换时保存/恢复栈内容；
Go2Cpp 当前使用 Boost.Context 受保护固定栈，并在安全性优先的路径中维护
G/M/P、Fiber 父链和任务状态。共享栈可以显著减少栈分配和切换成本，但必须
补齐 C++ 栈对象生命周期、栈地址失效和异常边界验证，不能直接替换成未验证
的 memcpy 方案。

混合锁结果已经反超 Coost。本轮将无 Context 的竞争等待节点改为调用栈上的
节点，队列只保存裸指针并保持 FIFO handoff；带 Context 的等待仍由缓存的
拥有节点保证取消期间的生命周期。这样去除了短等待的 shared_ptr 控制块和
分配开销，同时没有改变普通线程阻塞语义。

IO 仍是明确的性能缺口。Go2Cpp 的 DescriptorToken、fd generation 校验、
取消/超时竞争和 epoll interest 更新增加了锁与生命周期管理成本；Coost 的
实现使用更轻量的嵌入式等待节点。下一步可以在保持 generation 校验的前提下
增加 Fiber 私有 IO 等待节点和批量 interest 更新，但当前不宣称已经解决。

上述 Go2Cpp 调度对照临时将 Fiber 栈设为 32 KiB；生产默认值仍由
`SchedulerConfig` 控制，没有为了单个基准改变公开默认值。Coost 的调度和锁
数据来自三次连续运行，IO 数据也来自三次连续运行；宿主调度抖动会影响区间。

## 复现实验

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-engineering-wsl --target go2cpp_high_load_stress -j2
timeout 90s ./build-engineering-wsl/go2cpp_high_load_stress
```

完整功能测试仍需单独运行 `go2cpp_tests`；性能测试不是内存安全或竞态
证明，ASan、UBSan、TSan 和 Valgrind 仍按 `docs/testing.md` 的独立命令执行。

## 2026-09-30 当前工作区复测

以下数字来自当前工作区最后一次 Release 构建，均在同一台 WSL2 主机上重复运行，
用于判断本轮改动是否引入回归，不代表所有机器的固定性能承诺。

| 场景 | Go2Cpp | 对照 | 结论 |
|---|---:|---:|---|
| 公平调度（50,000 个任务） | 90～114 ms | Coost 14～48 ms | 中位数约 4.0 倍，仍未达到 1.25 倍目标 |
| 高负载计算 | 1.45～1.50 s | 线程池 1.41～1.50 s | 约 1.0～1.06 倍 |
| 混合 Fiber/线程 Mutex | 约 2.0 s | 线程基准约 47 ms | 仍是明显缺口；不能宣称已解决 |
| IO 多路等待 | 869～909 ms | Coost 1,409～1,529 ms | 约为 Coost 的 0.59 倍，本场景已超过 Coost |

工程化热路径的独立测量为：8 个线程并发写入启用的 CallbackSink 约
2.67～4.71M 条记录/秒；8 个线程并发读取配置快照约 17.9～19.2M 次/秒。
这两个测试没有与 Coost 同 API 的直接对照，因此不能推导“达到 Coost 1.25 倍”。

本轮再次评估了自研 Fiber Context。之前的自定义汇编上下文在嵌套 Fiber 父链恢复
场景出现栈上下文破坏并已撤回；生产实现仍是 Boost.Context 的受保护固定栈。
因此当前没有 Go 式 `morestack`/`copystack` 自动扩容，也没有把固定栈替换成未经
验证的共享栈。该限制是已知兼容性边界，不能按“自动扩栈已完成”交付。

随后针对 IO 取消竞态增加了 WaitNode 的回调和完成操作屏障：移除 Context 回调或
回收节点前，会等待正在执行的取消/完成回调退出；epoll pending readiness 在下一次
消费时用零超时原生 `poll` 再验证，避免 FD 已被读空后返回伪 ready。该修复后的高负载
样例为：计算 1,544 ms（线程 1,490 ms）、调度 123 ms（线程 88 ms）、混合 Mutex
8,931 ms（线程 39 ms）、Channel 22 ms（线程 72 ms）、IO 955 ms（线程 2,035 ms）。
这说明 IO 场景仍明显受益，但混合锁和调度提交开销仍未达到 Coost 的 1.25 倍目标。

## 2026-09-30 Context 与跨 M 唤醒专项

Linux x86_64 已增加 Go2Cpp 自有上下文后端：汇编只保存 callee-saved 寄存器、
RSP/RIP 和 caller transfer，C++ `FiberStack` 使用带 `PROT_NONE` guard page 的
mmap，并按栈大小使用 TLS/全局有界缓存。这样不再链接 Boost.Context；ASan/TSan
切换标记、父链校验、`m_resume_claim` 和跨 M 的 transfer 更新仍由 Fiber 外层
负责。非 Linux x86_64 回退 Boost.Context。

coost 的 `tb_context_make/jump` 已作为布局参考，但没有直接复制其共享栈、8 槽
固定调度器或 `_exit` 终止路径。coost 的 Buffer 不是自动动态栈；本后端同样不
提供 Go 式 `morestack`，栈容量仍需通过配置显式设置。

native 后端高负载三次采样：调度 `113～133 ms`，计算 `1327～1526 ms`，混合
Mutex `6680～6895 ms`，IO `616～665 ms`；同一轮线程基准分别约 `40～46 ms`、
`1283～1439 ms`、`39～42 ms`、`1614～1704 ms`。上下文替换没有改变调度语义，
但显著降低了栈映射重复分配；调度仍慢于线程和 coost，混合锁仍是主要瓶颈。

IO WaitNode、ParkingCondition 和 sync WaitNode 的跨 M wake/disarm/arm 门使用
`HybridGate`：先对 atomic flag 做 64 次 `_mm_pause`/yield，持续竞争才进入
`std::mutex` 慢路径；门内仍保护 Scheduler 指针、Task shared ownership、
callback_active 和 operation 生命周期。它没有修改 Fiber 的 `fcontext` 并发规则：
同一 Fiber 仍必须由 Task execution claim 串行恢复，wake 只发布状态和队列节点。

与前一版同负载约 8.5～8.8 s 的混合锁相比，本轮 native Context+HybridGate 为
约 6.7～6.9 s（该基准宿主抖动较大），线程 `std::mutex` 仍约 40 ms。HybridGate
只能降低唤醒门开销，不能把 Fiber park/wake、FIFO handoff 和跨 M 调度变成一次
原子操作；高争用共享锁仍应按本文件的分片/原子规约使用。

## 2026-10-01 FiberBin 与 MLocalTaskQueue 复核

FiberBin 只复用已完成 Fiber 的对象和 `Impl`，不会复用挂起栈或跨 M 共享对象；
Fiber 栈映射仍由已有 TLS/全局受保护栈缓存处理。默认每个 M 保留 32 个对象，
上限 4096，可通过 `SchedulerConfig`、INI 和动态配置绑定调整。Release 高负载功能
测试和 CTest 通过；ASan/UBSan 单测可通过，但重复压力运行仍可能触发既有
`test_readiness_timeout_race` 的时序失败，不能据此宣称 sanitizer 全部稳定通过。
该失败在临时禁用 FiberBin 后仍可复现，未证明由本轮缓存引入。

本轮尝试的 M 私有任务队列没有保留：在 50,000 个任务、每个任务 4 次 yield 的
测试中出现任务完成超时。该方案需要重新设计 runnable 计数、局部配额、P/global
批量转移、sysmon 脱离和 shutdown 排空的统一状态机，当前不能把未验证实现当成
性能收益。现有高负载基线仍以 P 本地队列、incoming 分片和全局窃取为准。

## 2026-10-01：混合 Mutex 与同步等待快路径

本轮修复了混合 Mutex 的三个实际问题。`m_fast_locked`/`m_has_waiters` 拆分为
一个 `m_state` 原子字节；等待者在队列锁内发布 `kWaiter` 后再入队，避免
Unlock 覆盖尚未入队的等待者标志而丢唤醒。带 Context 的 WaitNode 增加 generation，
Context 旧回调不能污染 thread-local 缓存复用的下一代节点；取消与 `Arm()` 交错时
先发布终态，避免 Fiber 永久挂起。

性能路径增加了两项约束。已注册 G 被外部 M 唤醒时，优先投递到 G 最近运行的 P，
避免每次 handoff 经过 incoming stripe；P 本地回队只有在没有运行 M 或队列积压时
通知其他 worker，减少无效 futex 唤醒。同步等待新增 `Scheduler::park_wait()`，
沿用 IO 的原子 Waiting→Runnable 交接和 shutdown 二次检查，跳过普通 Mutex 等待
每次获取 scheduler admission 锁的开销。Fiber 在没有 native waiter 时最多协作重试
64 次；出现普通线程 waiter 后最多再试 4 次即进入 FIFO 队列，保证普通线程不会
被无限越过。

当前 Release 高负载（2,000 Fiber、32 个普通线程、每个 Fiber 100 次、每个普通线程
200 次）连续样本为 15、27、29、35 ms；完整高负载两次混合 Mutex 为 26/50 ms，
对应线程 `std::mutex` 为 45/40 ms。同期完整样本中计算为 1,506/1,522 ms（线程
1,443/1,411 ms），调度为 121/124 ms（线程 73/45 ms），Channel 为 24/26 ms
（线程 67/78 ms），IO 为 662/748 ms（线程 poll 1,895/1,911 ms）。这些数字是
本机重复运行结果，不是跨机器的性能承诺；调度器和 Fiber 栈切换仍可能受宿主调度
抖动影响。

验证命令：

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-native-context -j$(nproc)
ctest --test-dir build-native-context --output-on-failure
ctest --test-dir build-engineering-werror --output-on-failure
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./build-native-asan/go2cpp_tests
valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible \
  --error-exitcode=99 ./build-native-context/go2cpp_fiber_sync_demo
```

本轮 Debug/Release 14 项 CTest、Werror 3 项 CTest 和 ASan 单测均通过；Valgrind
ERROR SUMMARY 为 0，definite/indirect/possible leak 均为 0，仅有 288 bytes
still reachable 的进程级缓存。TSan 仍受 WSL `unexpected memory mapping` 环境错误
限制，不能宣称通过。
