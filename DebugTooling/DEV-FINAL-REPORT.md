# Go2Cpp 调试模块 · 最终交付报告

> 分支 `feature/gmp-debug-module`；窗口 2026-10-16 13:00 – 18:00；
> 设计依据 `DebugTooling/design/`；项目规范 `PROJECT_RULES.md`。

## 0. 一句话结论

在**不改动调度器/协程/任务核心行为**的前提下，交付了一套可用的 GMP 调试模块：
统一 `dump` 接口（scheduler / events，text/ini/json）、锁与等待组观测、**锁争用选择钩子（D3）**、
非阻塞事件采集、**协作式断点**；全部模块带压测 + 随机 + ≥5 真实场景测试，
**全量 ctest 79/79 通过**。

## 1. 交付概览

| 项 | 值 |
|---|---|
| 分支 | `feature/gmp-debug-module` |
| 提交数 | **12**（`b7f172a` → `1d44e39`） |
| 变更文件 | **26** 个 |
| 代码变更量 | **+4832 / −1** 行 |
| 新增源码头文件 | 7（`debug.hpp` + 6 个子模块） |
| 新增测试文件 | 7（全部登记 CMake 与 `test_main`） |
| 新增基准 | 2（Release，登记 `GO2CPP_BUILD_BENCHMARKS`） |
| 新增文档 | 4（DEV-PROGRESS、design/09、design/10、perf-observer-overhead） |
| 全量测试 | **ctest 79/79 通过** |

## 2. 各阶段成果与证据

| 阶段 | 交付内容 | 关键文件 | 提交 |
|---|---|---|---|
| **P0** | `DebugFiber : public Fiber`（元数据）、`DebugRegistry`、`DebugScheduler` 代理、三格式序列化 | `debug_fiber.hpp`、`debug_scheduler.hpp` | `b7f172a`、`a7d3d48` |
| **P1** | 进程内 dump CLI、原子文件 sink（临时文件 + rename） | `debug_cli.hpp` | `a94c041` |
| **P2** | `EventBuffer`、`DebugEventCollector : SchedulerObserver` | `debug_events.hpp` | `ff55624` |
| **P3** | `Mutex` / `ConditionVariable` / `WaitGroup` 观测快照 + `Debug*` 代理 | `debug_sync.hpp` + `sync.hpp`/`sync.cpp` | `d3c1d25`、`5dfa897` |
| **D3** | `WaitSelector` 锁争用选择钩子 + 开销基准 | `sync.hpp`/`sync.cpp`、基准 | `b037417`、`4ce957f` |
| **P5** | 协作式断点（`DebugBreakpoints`、`Checkpoint`、`ScopedCheckpoint`） | `debug_breakpoints.hpp` | `1d44e39` |
| **P4** | **不做** —— 按使用者定案（结构类明确拒绝） | — | — |

**统一入口**（P0~P3 全部可从同一套接口使用）：

```
dump scheduler [--format=text|ini|json] [--output=<file>|-] [--append|--truncate]
               [--summary|--processors|--machines|--metrics] [--no-*] [name]
dump events    [--format=text|ini|json] [--output=<file>|-] [--append|--truncate]
```

## 3. 测试覆盖明细

每个模块均为「≥5 真实场景 + 压测 + 随机」：

| 测试文件 | 场景数 | 压测内容 |
|---|---:|---|
| `test_debug_fiber.cpp` | 5 | 1000 次创建+注册；20000 次注册+查询 |
| `test_debug_scheduler.cpp` | 5 | Snapshot ×2000（≈1.26 µs/次）；三格式 Dump ×200 |
| `test_debug_cli.cpp` | 6 | 10000 次参数解析 |
| `test_debug_sync.cpp` | 6 | DebugMutex::Snapshot ×20000（≈90 ns/次） |
| `test_debug_lock_selector.cpp` | 5 | 无竞争 10 万次（≈29 ns/次）；4 线程竞争 2 万次 |
| `test_debug_events.cpp` | 6 | EventBuffer Push/Pop ×100 万（43 ms） |
| `test_debug_breakpoints.cpp` | 8 | Checkpoint ×100 万（无断点 53 ns/次；不匹配断点 72 ns/次） |

> 真实场景包含：**受管 G 内 40 个任务并发经过同一协作检查点全部命中**、
> **接真实调度器采到任务级与 worker 级事件**、**3 线程排队后由选择器指定最后一个先获锁**、
> **容量 8 的采集器在 300 任务下必然丢弃**（证明有界性而非假装无损）等。

## 4. 性能实测汇总（全部来自 `build-bench` / Release）

| 项 | 结果 | 条件 |
|---|---|---|
| 观察者（单 worker） | **+3.4% / +6.1%**（≈27 ns/事件） | 20000 任务，1 worker，热身 1 轮 + 3 轮 |
| 观察者（4 worker） | +16~23%，**但基线本身异常**（见未决项 1） | 同上，4 worker |
| D3 选择器（无竞争） | **3.71 / 3.78 / 3.76 ns/op（三档一致）** | 20 万次/轮 × 3 轮，单线程 |
| D3 选择器（有竞争） | 档 0（无选择器）**15,785 / 18,416 / 19,842 ns/op** | 4 线程 × 2 万次/轮 |
| EventBuffer | Push/Pop **43 ns/次** | 100 万次往返 |
| Checkpoint | **53 ns/次**（无断点） | 100 万次 |

**两条重要结论**：
1. 选择器在 `Mutex::Unlock` 的 CAS 快路径上**零开销**（无竞争三档一致）；
2. 有竞争的 13~24 µs/op **是 `sync::Mutex` 自身的竞争成本**，
   因为**完全没有注册选择器的档 0 同样慢** —— 与选择器无关。

## 5. 对原框架的改动（精确清单）

**除下面两处外，`Scheduler`/`Task`/`Fiber`/`Channel`/其余 `sync` 内容一行未改。**

| 文件 | 改动 | 原因 |
|---|---|---|
| `include/go2cpp/sync.hpp` | 新增 3 个快照结构 + 3 个 `DebugSnapshotInternal()` + `WaitSelector` 接口 + 工厂声明 | P3 锁观测与 D3 钩子需要读内部状态 |
| `src/concurrency/sync.cpp` | 新增 3 个快照实现 + 选择器全局状态与 `ReorderWaitersForSelector` + `Unlock` 内一处调用 | 同上 |
| 实际改动量 | 见本节末尾由 `git diff --numstat` 生成 | |

> **一处已知偏离**：P3 原计划用「原类私有 `Debug*` 方法 + 代理 `friend` 访问」，
> 但实测 `friend` 对该内联代理**未生效**（前向声明齐备、插入位置正确仍报 private 访问）。
> 为避免影响面更大的做法（去掉原类 `final`、调整包含顺序），
> 改为**把方法放在 `public` 段并保留 `Debug` 前缀**，在头文件注释里写明原因与
> 「仅供 debug 模块调用」的约定。这一点已在 `design/09` §2.4 与本报告中如实记录。

## 6. 能力边界总表（明确声明，避免误用）

| # | 限制 | 原因 / 说明 |
|---|---|---|
| 1 | **没有运行时行号** | 需要编译期插桩，不在本模块范围 |
| 2 | **没有锁持有者身份** | `Mutex::Impl::m_state` 只有 locked/waiter 两个位；Go 风格所有权允许 A 加锁 B 解锁 |
| 3 | **没有完整死锁环检测** | 只能看到已声明阻塞，无法判定环 |
| 4 | **不支持任意行断点** | 项目永不强制 detach 任意 C++ 调用栈；只能在调用过 `Checkpoint` 的位置停 |
| 5 | **无跨机绝对时间对齐** | 双时间戳（steady + wall），三种格式中均显式标注 `cross_machine_aligned: false` |
| 6 | **不能重放/重建运行时** | 事件流是观测，不是可重放的状态记录 |
| 7 | **观测覆盖有边界** | 只看到**已声明**的事件与阻塞 —— **没显示阻塞 ≠ 没有阻塞** |
| 8 | **`dump` 是进程内形态** | 跨进程 dump 需要信号/注入机制，属后续阶段，本模块**不声称具备** |
| 9 | **CLI 的 `scheduler_name` 仅作标识** | 项目没有"调度器名字"概念，`SchedulerConfig` 无 name 字段 |
| 10 | **`long_syscall_count` 是次数不是时长** | 三种序列化中均有标注 |
| 11 | **`metrics` 全 0 可能是 `collect_metrics=false`** | 输出中标注，避免误读为"没有任务" |
| 12 | **事件缓冲有界、会丢弃** | 满则丢弃并计数；`captured + dropped` 守恒（测试断言） |
| 13 | **D3 不做饥饿保护** | 策略长期跳过某等待者需实现方自行加上限强制 FIFO —— 这是策略的责任 |

## 7. 未决项（如实保留，不视为已解决）

1. **4 worker 基线异常 —— 已诊断（第 16 轮），机制仍未确认**：
   - **事实**：1~2 worker 时约 1.2~1.7 µs/任务（正常并行扩展，2 worker 反而快 31%）；
     **3 worker 起跳变**（约 6.75x），4 worker 约 13~16x，6/8 worker 趋于平稳（约 23~26 µs）；
   - **事实**：该跳变两轮独立测量均出现，**可复现**；4 worker 下测量**不稳定**（离差可达 3.7 倍）；
   - **事实**：固定 4 worker 把任务数从 2,000 增到 80,000（40 倍），每任务耗时 19.2k → 23.1k ns
     （**仅 1.20x，没有下降**）→ **排除**「固定启动开销被摊薄」，支持「每任务都要付的、
     随并发度增长的持续开销」；
   - **仍未确认**：具体机制。**未做 profiling**，因此不指定是 work stealing、全局队列锁、
     `sysmon` 唤醒还是栈池竞争；
   - **与调试模块无关**：全部测量都在 `observer = nullptr`（未安装任何插件）下进行；
   - 详见 `DebugTooling/diag-worker-scaling.md`。
2. **`Mutex` 高竞争成本**：4 线程竞争下约 **18 µs/op**，而同宿主单线程无竞争仅 3.7 ns/op，
   相差约 5000 倍。属 `sync::Mutex` 自身行为，**本模块不声称原因**。
3. **一次基准超时根因未定位**：早前一次默认参数运行（5 轮 × 200 万无竞争迭代）
   **超过 15 分钟未结束**且输出未 flush 而丢失。改用分档单跑 + 逐轮 `fflush` 后每次数秒完成，
   故本报告结论**不依赖该次运行**，但该异常**不视为已解决**。

## 8. 与设计文档的对应关系

| 设计文档预期 | 实际 |
|---|---|
| P0 只读快照 + 元数据 | ✅ 完成（`DebugFiber` 继承方案按使用者定案） |
| P1 序列化 + sink，让 dump 可用 | ✅ 完成（进程内 CLI + 原子写；跨进程机制明确标注为未做） |
| P2 事件采集 + `--follow` | 采集 ✅；`--follow`（持续跟随）**未做**，属后续 |
| P3 锁观测 + D3 争用决策 | ✅ 完成 |
| P4 控制面 | ❌ **按使用者定案不做**（结构类明确拒绝） |
| P5 断点 | ✅ 完成，但**采用协作式**（非原设想的调度器安全点钩子），原因见 §5 |
| "枚举全部 G" 接口 | **未做**（`dump scheduler` 的 fibers 字段输出为 `null`，如实标注） |

## 9. 后续建议（按价值排序）

1. **排查未决项 1（4 worker 基线 12 倍劣化）** —— 这是调度器自身的扩展性问题，价值最高；
2. 实现"枚举全部 G"接口（需在 `Scheduler` 内加只读遍历），补齐 `dump scheduler` 的 fibers 段；
3. P2 的 `--follow`（持续跟随输出）；
4. 跨进程 dump（信号处理函数写文件），让 CLI 从"进程内"变成真正可对外使用；
5. 事件采集的"每 worker 线程局部暂存 + 批量发布"（本报告未实现，已在头文件标注）。

## 10. 复现方式

```sh
cmake -S . -B build-quality && cmake --build build-quality --parallel
cd build-quality && ctest                       # 全量 79/79
./go2cpp_tests                                  # 全部单元测试
GO2CPP_TEST_FILTER=debug_breakpoints ./go2cpp_tests   # 单模块

cmake -S . -B build-bench -DGO2CPP_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --parallel
./build-bench/go2cpp_debug_observer_benchmark 3 20000 1      # 观察者开销
./build-bench/go2cpp_debug_lock_selector_benchmark 3 200000 20000 all   # D3 开销
```

## 附：核心文件实际改动量

```
include/go2cpp/sync.hpp  | 104 +++++++++++++++++++++++++++++++++++++++++++++++
 src/concurrency/sync.cpp | 104 +++++++++++++++++++++++++++++++++++++++++++++++
 2 files changed, 208 insertions(+)
104	0	include/go2cpp/sync.hpp
104	0	src/concurrency/sync.cpp
```


---

# 附录 A · 第 2 天续做（2026-10-17）

> 上文主体写于 2026-10-16，当时有 **4 项未做项**。本附录记录第 2 天的补齐情况，
> 以及新增的架构图与「零影响」验证。**主体章节保留原样，作为当时的真实状态记录**，
> 本附录才是当前状态。

## A.1 四项未做项已全部完成

| # | 未做项（原 §8） | 完成情况 | 提交 |
|---|---|---|---|
| 1 | 「枚举全部 G」接口 | `TaskSnapshot` + `Scheduler::DebugSnapshotTasks()`（按 16 分片逐个持锁遍历）；`dump scheduler` 的 `fibers` 段真正有数据，text/ini/json 三格式，默认仍为 `null` 保持兼容 | `77f566d` |
| 2 | P2 的 `--follow` | `FollowEvents(collector, sink, interval, max_batches, overall_timeout)` + `--follow[=N]` / `--follow-interval` / `--follow-timeout`；逐事件输出 JSON 且每批 `fflush` | `84a3757` |
| 3 | 事件采集线程局部批量发布 | `EventBuffer::PushBatch`（原子操作从 count 次降到 1 次）+ `OnEvent` 线程局部暂存满 32 条提交；`SetFlushBatch` 可对比 | `56e92e8`、`b0eb49b` |
| 4 | 跨进程 dump | `DebugSignalDumper`（`SIGUSR1`）；**信号处理器只置原子标志**（async-signal-safe），真正 dump 在 `PollOnce`/watcher 线程 | `c2ed422` |

`example/debug/debug_module_demo.cpp` 同步从 7 节扩到 **10 节**，覆盖上述全部新能力（`505ede5`）。

## A.2 新增：每模块一张图（任务 2）

原任务链交付的是 `modules/G1..G10-*.md`（约 178KB **文字**讲解）。使用者的实际要求是
**每个模块一张图**，因此重建了 [`../ArchitectureDiagram/diagrams/`](../ArchitectureDiagram/diagrams/)：

| 组 | 文件 | 行数 | Mermaid |
|---|---|---:|---:|
| **G0** | 总览：10 组如何拼成一次 `go()` | 163 | 4 |
| G1 | 地基层（Fiber / Timer / ParkingCondition） | 308 | 5 |
| G2 | **GMP 调度**（Task/Scheduler/P/M/worker_loop/sysmon） | 325 | 5 |
| G3 | 取消与错误 | 218 | 5 |
| G4 | 并发原语（Channel / Future / sync / WaitSelector） | 247 | 5 |
| G5 | IO 后端 | 195 | 5 |
| G6 | 网络基础 | 239 | 5 |
| G7 | 应用协议（HTTP / WebSocket / SSE） | 269 | 6 |
| G8 | 横切设施（log / config） | 246 | 5 |
| G9 | 工具与新手层 | 249 | 5 |
| G10 | **调试模块**（本次新增的 go2cpp::debug） | 315 | 6 |
| | **合计 11 个文件** | **2774** | **56** |

每张图固定四段，对应使用者的四点要求：① 类架构（每个类用了底层哪些能力 + 职责）
② 重要函数的功能与实现（含真实现分支）③ 向上层提供了什么 ④ 状态流转/流程图。
G2 里含使用者点名的 **G 完整状态机**（`kNew→kRunnable→kRunning→kWaiting/kRequeued→kDead/kFailed/kCancelled`，
含「只有 5 个安全点可挂起」说明），G1 里含 **Fiber 状态机与栈变化图**。

**核对**：文件 11/11 齐全、Mermaid 块 **56 个**经静态检查（图型合法、括号配对、stateDiagram 有转移、无全角引号）、
三章节齐全、索引 11/11。

## A.3 新增：「不影响原框架性能」的验证

使用者明确要求「调试部分不怎么需要性能，但**必须保证不影响原来的模块性能**」。
为此做了三件事，详见 [`zero-overhead-verification.md`](zero-overhead-verification.md)：

1. **修掉一处真实残留开销**：`Mutex::Unlock` 慢路径在等待者 ≥2 时仍会加锁并拷贝 `shared_ptr`，
   改为 `std::atomic<bool>` 二级快判，**未注册选择器时只读一次原子量**（`b8ff33d`）；
2. **结构性保证 + 实证**：`grep` 确认框架**零处** `include` debug 头；并把源码树复制到 `/tmp`、
   **剔除全部 debug 文件与 CMake 条目后，configure 与构建均成功** —— 即删掉整个 debug 模块框架照常编译；
3. **对照组实测**（`main` 无 debug vs HEAD 含 debug 但不启用）：被改动的框架路径全部
   **0.95~1.03x**（调度器扩展 w=1/2/4/8/12、spawn 开销、Fiber 计算并行、TimerService、Scheduler+Fiber）；
   超过 10% 的只有**不含本次改动**的对照组（原生 `std::thread` 1.41x、基准自带 `BoundedQueue` 1.36x /
   `NaiveQueue` 0.53x），且**方向不一致** → 判定为噪声，不能归因于 debug 模块。

## A.4 当前状态

| 项 | 值 |
|---|---|
| 分支 | `feature/gmp-debug-module` |
| 提交数 | **33** |
| 全量测试 | **ctest 80/80** |
| 调试模块文件 | 7 个头文件、8 个测试、3 个基准、1 个示例（10 节） |
| 架构图 | **11 个文件 / 2774 行 / 56 个 Mermaid** |
| 对原框架改动 | `sync.hpp` +104、`sync.cpp` +104（纯新增）+ `scheduler` 侧只读接口 |

## A.5 仍未解决的（不由调试模块引入）

1. **调度器 3 worker 起数量级劣化**：已诊断并排除启动开销（任务数 ×40 而每任务耗时未下降），
   但**未做 profiling**，具体机制未确认（见 `diag-worker-scaling.md`）；
2. `sync::Mutex` 4 线程竞争约 18 µs/op（单线程无竞争 3.7 ns/op）；
3. 一次默认参数基准运行超 15 分钟未结束，根因未定位。

这三项在昨天就存在，第 2 天的改动**没有触及**它们，也没有变得更差或更好。
