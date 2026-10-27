# 05 默认配置漂移（文档一致性）问题记录

> 基线：`Go2Cpp-work` @ `fc287c6`（2026-10-23），ctest 135/135。
> 类型：**治理问题**，不是运行期缺陷——「同一语义的默认值存在多个事实源」+「代码改默认值时文档未同步」。
> 触发：用户在讨论 scheduler→fiber 栈大小传递链时指出「**多个默认配置一直改，可能存在多次文档不一致**」。
> 状态：已记录（待核对清单见 §4；本次未改任何代码/文档正文，只在 `docs/review/` 内新增本文件）。

## 1. 问题陈述

默认值至少有 **4 个独立事实源**，改一处不会自动带动其他三处：

| # | 事实源 | 位置 | 说明 |
|---|---|---|---|
| S1 | `SchedulerConfig` 的 C++ 默认成员初始化 | `include/go2cpp/concurrency/scheduler/scheduler.hpp:205-275` | 程序化创建 Scheduler 时生效 |
| S2 | `go2cpp.ini` | `go2cpp.ini:15-27` | 有 ini 时覆盖 S1，多数部署的实际值 |
| S3 | `Fiber` 类的静态默认值 / 默认参数 | `include/go2cpp/concurrency/fiber.hpp:281-290`、`:314` | `Task::run()` 的回落值与 `AcquireForScheduler` 的默认参数 |
| S4 | 文档表格 | `docs/configuration.md:19-29`、`docs/review/02-config.md` | 人工维护，无 CI 校验 |

任何一次调参（如 2026-10 的 2KB 初始栈改造）都必须同步 S1~S4，漏一格就产生一条「文档说 A、运行是 B」。

## 2. 已确认的不一致（本轮证据）

### D-01 `fiber_stack_size`：默认值与允许范围**双重过时**

- 代码：`SchedulerConfig::fiber_stack_size = 2048`（`scheduler.hpp:227`），注释明确「默认 2048（Go 的 2KB 初始栈）」；`Task::run()` 里 `0` 回落 `Fiber::DefaultStackSize()` = 128KB（`scheduler.cpp:839-841`）；实际归一化 `max(page_align_up(2048), 4096) = 4096`（`fiber.cpp:441`、`scheduler.hpp:229-230`）。
- 校验：`kMinFiberStack = 2048`，报错文案 `必须为 0 或处于 2048..67108864 字节`（`src/config/config.cpp:161`、`:540-542`）。
- 文档：`docs/configuration.md:23` 仍写「默认值 `0`，允许 `0` 或 **`16384..67108864`** 字节」——**默认值错、下限错**，且会让人把合法值 2048..16383 误判为非法。
- 次级文档：`docs/review/02-config.md:189` 仍写「需求：`fiber_stack_size < 16KiB` 时告警」，阈值已失效。
- 变更来源：`codex.md:1219-1220` 记录「`kMinFiberStack` 16384 → 2048，`go2cpp.ini` 的 `fiber_stack_size` 0 → 2048」——**代码与 ini 改了，文档漏改**。

### D-02 `fiber_bin_capacity`：代码默认 128，文档与 ini 都是 32

- 代码：`SchedulerConfig::fiber_bin_capacity = 128`（`scheduler.hpp:258`）；`TaskOptions::fiber_bin_capacity = 128`（`scheduler.hpp:280`）。
- 文档：`docs/configuration.md:25` 写默认 `32`；`go2cpp.ini:27` 也是 `32`。
- 语义补充：`go2cpp.ini` 存在时 32 会覆盖代码默认 128，所以**运行时确实是 32**；但「默认值 32」这句话对不读 ini 的程序化嵌入方是错的。同类事实源分裂，性质不同于 D-01（这是 S1 vs S2/S4，不是单纯漏改）。
- 旁证：`Fiber::AcquireForScheduler` 的 `bin_capacity` 默认参数是 `32`（`fiber.hpp:314`），即 S3 又给了第三个数。

### D-03 `stack_size = 0` 的双回落（语义分叉，非文档问题但同源）

- 经 `Scheduler::spawn()`：`0` → `config.fiber_stack_size` = **2048**（`scheduler.cpp:2422-2424`）。
- 绕过 `spawn` 直接构造 `Task` 并入队：`Task::run()` 里 `0` → `Fiber::DefaultStackSize()` = **128KB**（`scheduler.cpp:839-841`）。
- 相差 **64 倍**；`scheduler.hpp:278-280` 的注释承认了这条旁路，但没有约束它。

## 3. 结构性成因

1. **0 作为哨兵值贯穿三层**：`TaskOptions.stack_size=0` → `SchedulerConfig.fiber_stack_size` → `Fiber::DefaultStackSize()`，每一层都能「兜底」，每层各自有一个数。
2. **参数有 C++ 默认、INI 默认、文档默认三个面**，而校验范围（`config.cpp:161/540`）只覆盖其中一个面。
3. **文档表格是纯手写**，与常量没有任何自动关联；`docs/configuration.md` 的表格行数（11 行）与 `SchedulerConfig` 字段数（>15）本身就不匹配。
4. 变更记录（`codex.md`）写得很细，但**只描述「改了什么」，没有「文档同步清单」**，所以每次调参后都要靠人回忆。

## 4. 待核对清单（建议按此顺序排查）

| 优先级 | 项 | 核对方式 |
|---|---|---|
| P0 | `SchedulerConfig` 全部数值字段 vs `docs/configuration.md:19-29` 表格 | 逐字段比对（本轮只做了 `fiber_stack_size` / `fiber_bin_capacity` / `local_queue_limit`） |
| P0 | `go2cpp.ini` 每一项 vs `config.cpp` 的 `kMin*/kMax*` 校验区间 | ini 里写 32、代码默认 128 这类分裂会继续出现 |
| P1 | `local_queue_limit`（代码 256 / ini 256 / 文档 256，**已核对一致**） | 已核对，留作对照样本 |
| P1 | `fiber_stack_reserve_size`（代码 8MiB `scheduler.hpp:231` / `Fiber::DefaultStackReserveSize()` 8MiB `fiber.hpp:288-290`） | 数值一致，但两处独立定义，需标注「必须同步」 |
| P1 | `fiber_stack_growable` 三处开关：`TaskOptions`(`scheduler.hpp:287`)、`SchedulerConfig`(`scheduler.hpp:235`)、环境变量 `GO2CPP_GROWABLE_STACK`(`fiber.cpp` 宏判断) | 确认环境变量优先级与文档是否写明 |
| P2 | `docs/performance-baseline-2026-10-19.md:452/516` 的「16KB 栈」是压测参数 | 补一句「非默认值」，避免与 D-01 混淆 |

## 5. 建议修复（不改行为，先建立约束）

1. **单一事实源**：`Task::run()` 的回落从 `Fiber::DefaultStackSize()` 改为 `SchedulerConfig{}.fiber_stack_size`（或直接 `config.fiber_stack_size`），消掉 D-03 的 64 倍分叉。
2. **文档自动校验**：加一个测试（`tests/config/`）解析 `docs/configuration.md` 的表格，断言「默认值」列与 `SchedulerConfig{}.字段`、`kMin*/kMax*` 区间一致；不一致就 ctest 失败。这是唯一能防止 D-01 复发的机制。
3. **变更记录模板**：`codex.md` 的「配置兼容」条目追加固定字段「文档同步：docs/configuration.md:NN 已改 / 无需改」，把同步动作显式化。
4. **收尾 D-01**：`docs/configuration.md:23` → `默认 2048`，范围 `2048..67108864`；`docs/review/02-config.md:189` 阈值改 `2KiB` 或删除。

## 6. 本轮改动

- 新增本文件；未改动任何源码、配置与既有文档正文。

## 7. 完整比对（2026-10-23 执行 §4 的 P0 清单）

### 7.1 `docs/configuration.md:19-29` 表格逐项核对

| 参数 | 文档默认 | 文档范围 | 代码默认 | 代码校验（`src/config/config.cpp`） | 判定 |
|---|---|---|---|---|---|
| `processor_count` | 0 | 0 或 1..32 | 0（`scheduler.hpp:206`） | `0 或 ≤32`（:179-181） | 一致 |
| `min_workers` | 0 | 0 或 1..32 | 0（:212） | `0 或 ≤32` | 一致 |
| `max_workers` | 0 | 0 或 1..32 | 0（:207） | `0 或 ≤32` | 一致 |
| `local_queue_limit` | 256 | 1..1048576 | 256（:209） | `>0 且 ≤1048576`（:183-185） | 一致 |
| `fiber_stack_size` | **0** | **16384..67108864** | **2048**（:227） | `0 或 2048..67108864`（:187-190） | **不一致（默认+范围）** |
| `task_affinity_budget` | 4 | 0..1048576 | 4（:237） | `≤1048576`（:191-193） | 一致 |
| `fiber_bin_capacity` | **32** | 0..4096 | **128**（:258） | `≤4096`（:194-196） | **不一致（默认）** |
| `idle_wait_ms` | 10 | 1..86400000 | 10ms（:208） | `>0 且 ≤24h`（:199-207） | 一致 |
| `idle_worker_timeout_ms` | 250 | 1..86400000 | 250ms（:220） | 同上 | 一致 |
| `sysmon_interval_ms` | 10 | 1..86400000 | 10ms（:247） | 同上 | 一致 |
| `long_syscall_threshold_ms` | 50 | 1..86400000 | 50ms（:248） | 同上 | 一致 |

结论：11 项里 **9 项一致，2 项不一致**（D-01、D-02 得到确认，其余暂无需改）。

### 7.2 文档表中完全缺失的字段（`SchedulerConfig` 有、表里没有）

| 字段 | 默认值 | 位置 | 影响 |
|---|---|---|---|
| `fiber_stack_reserve_size` | **8 MiB** | `scheduler.hpp:231` | 每 G 的虚拟地址预留，直接决定 VmSize；文档只字未提 |
| `fiber_stack_growable` | `true` | `scheduler.hpp:235` | 关闭即退化为固定栈、不装 SIGSEGV handler |
| `allow_idle_shrink` | `false` | `scheduler.hpp:244` | 基础 M 是否空闲退出；下文布尔清单也没列它 |
| `terminal_collect_interval` | `256` | `scheduler.hpp:270` | 终态 G 回收节拍 |

另：`collect_metrics`、`allow_worker_oversubscription`、`enable_sysmon`、`pin_workers_to_cpu` 只出现在表格下方的一句话里，没有进表，读表的人会以为它们不存在。

### 7.3 关键语义纠正：`fiber_stack_size = 0` **不等于** 2048

- `config.fiber_stack_size = 0` → `Scheduler::spawn` 注入 `0`（`scheduler.cpp:2422-2424`）→ `Task::run` 再回落 `Fiber::DefaultStackSize()` = **128 KiB**（`scheduler.cpp:839-841`）。
- 即 0 的真实语义是 **128KiB**，不是 2KiB。文档 :23「0 使用后端默认值」表述含糊，`go2cpp.ini:23-24` 反而写对了（明说 128KiB）。

### 7.4 16KB 的误读路径（回应「我记得是 16KB」）

1. 旧代码：`constexpr std::size_t kMinFiberStack = 16U * 1024U;`，报错文案「小于 16384 时可能无法建立安全栈」——**16KB 是下限，不是默认值**；
2. 当前 `docs/configuration.md:23` 的**范围列仍写 16384**，读者很容易把它当默认值；
3. `SchedulerConfig::fiber_stack_size` 的 git 历史只有 `= 0` → `= 2048`，从未等于 16KB；`go2cpp.ini` 亦然（`0` → `2048`）。

## 8. 文档同步清单（待办，本文件不改正文）

> 用户要求「这部分要完全同步」。以下为精确改动项，按序执行即可完全一致；本次只登记，不执行。

| # | 位置 | 现状 | 应改为 | 优先级 |
|---|---|---|---|---|
| T1 | `docs/configuration.md:23` | 默认 `0`；范围 `0 或 16384..67108864` | 默认 `2048`；范围 `0 或 2048..67108864`；备注「0 = 回落 128KiB」 | P0 |
| T2 | `docs/configuration.md:25` | `fiber_bin_capacity` 默认 `32` | 默认 `128`，并注明 `go2cpp.ini` 覆盖为 32 | P0 |
| T3 | `docs/configuration.md` 表格 | 缺 4 个字段 | 增 `fiber_stack_reserve_size`(8MiB)、`fiber_stack_growable`(true)、`allow_idle_shrink`(false)、`terminal_collect_interval`(256) | P1 |
| T4 | `docs/review/02-config.md:189` | 「`< 16KiB` 时告警」 | 改 `2KiB` 或删除 | P1 |
| T5 | `go2cpp.ini:21-24` | 已正确（2048 / 4096 / 128KiB） | 不改，作为「写对了」的基准样本 | — |
| T6 | `codex.md` 变更条目 | 无「文档同步」字段 | 加固定字段「文档同步：`docs/configuration.md:NN` 已改 / 无需改」 | P2 |

### 防复发机制（建议，非本次执行）

新增 `tests/config/test_config_doc_sync.cpp`：解析 `docs/configuration.md` 的表格 → 断言「默认值」列等于 `SchedulerConfig{}` 的对应字段、「范围」列等于 `config.cpp` 的 `kMin*/kMax*`。不一致直接让 ctest 失败。这是唯一能阻止 D-01 再次发生的机制。

## 9. 8KB / 16KB 记忆溯源（2026-10-23 排查完成）

> 用户反馈：「感觉很不对，我记得之前 8KB 或者 16KB 是默认的」并要求排查、写进文档。
> 排查方法：`git log -p/-S` 全历史追踪三个默认值来源 + 全仓搜索 `8192/16384/8 * 1024/16 * 1024` 的栈上下文。

### 9.1 结论：8KB 与 16KB **都不是**启动栈的默认值

| 记忆中的值 | 真实身份 | 位置 | 是默认值吗 |
|---|---|---|---|
| **8KB** | **栈上读缓冲**（原 64KB，调优后降到 8KB） | `benchmarks/gmp_vs_epoll_fair_benchmark.cpp:154` `char buffer[8 * 1024]` | 否，是 benchmark 的连接读缓冲 |
| **16KB** ① | 旧的**最小允许值**（校验下限） | 旧 `kMinFiberStack = 16U * 1024U`，报错「小于 16384 时可能无法建立安全栈」 | 否，是下限 |
| **16KB** ② | **性能实验的栈参数**（`GO2CPP_BENCH_STACK_KB=16`） | `docs/performance-baseline-2026-10-19.md:452`、`:461` | 否，是实验旋钮 |
| **128 KiB** | `Fiber::DefaultStackSize()` | `fiber.hpp:281` | 是，但只对**直接构造 Fiber** 的用户生效 |
| **2048** | `SchedulerConfig::fiber_stack_size` | `scheduler.hpp:227` | 是，调度器 G 的默认请求（实际提交 4096） |

### 9.2 最可能的记忆来源（按可信度排序）

1. **性能基线 §9.1 的表格行**：「`8KB 读缓冲 + 16KB 栈` → 虚拟内存 482MB」以及结论 #2「16KB 栈进一步降到 482MB」（`docs/performance-baseline-2026-10-19.md:448-461`）。**8KB 是读缓冲、16KB 是实验栈参数**，两者并列书写，极易被合并记成「默认 8KB/16KB 栈」。
2. **旧的下限文案**：`大于等于 16384` 的校验 + 报错「小于 16384 时可能无法建立安全栈」，读起来很像「16KB 是推荐/默认值」。
3. **当前文档残留**：`docs/configuration.md:23` 的范围列仍写 `16384..67108864`（见 D-01）。

### 9.3 代码历史事实（可用 `git log` 复现）

- `SchedulerConfig::fiber_stack_size`：全历史只有 **`= 0` → `= 2048`** 两态，**从未出现 8KB/16KB**。
- `Fiber::DefaultStackSize()`：自首次引入就是 `128U * 1024U`，**从未变过**。
- `bench_stack_kb()`：默认返回 **128**（`benchmarks/gmp_vs_epoll_fair_benchmark.cpp:67-73`），只有设了 `GO2CPP_BENCH_STACK_KB` 才变成 8/16/32。
- 8KB 读缓冲**已落地**（同名文件 `:154`），但 `:716` 仍有一处 `char buffer[64 * 1024]` 未优化（见 R2）。

### 9.4 用户决策（需落地的需求）

> 「现在无论哪个情况，都以调优后的参数为默认（不管 Scheduler 层还是其他层），我记得是 8KB 或者 16KB。」

即：把默认初始栈从 **2048** 提升到 **8192 或 16384**，并要求 Scheduler 层与 Fiber 层口径一致。

**落地前必须定夺的两件事**：

| 待决 | 说明 |
|---|---|
| 取 **8KB** 还是 **16KB** | 性能基线只测到 16KB（虚拟内存 482MB vs 32KB 的 607MB），**8KB 栈未测**；两者都没有 RSS 差异（未触碰页不计入 RSS） |
| 下限 `kMinFiberStack` 是否同步 | 若取 2048→8192，下限 2048 仍合法；若默认改 16384，建议下限抬到 4096 或保持不动 |

**影响面（改动前评估）**：
- 初始提交：2048 → 1 页(4096)；8192 → 2 页；16384 → 4 页。`fiber_stack_reserve_size` 仍 8MiB，**虚拟内存不变**；RSS 只在实际触碰时增长，故 8000 连接级别的 RSS 预期变化很小（与基线文档结论一致）。
- 改动点共 **4 处**：`scheduler.hpp:227`、`go2cpp.ini:25`、`scheduler.cpp:839`（0 的回落，是否改为 config 值）、`docs/configuration.md:23`。若同时统一「0 的语义」，还要动 `fiber.cpp:446` 的 `normalize_stack_size`。

### 9.5 新增待办

| # | 事项 | 优先级 |
|---|---|---|
| R1 | **定 8KB 还是 16KB**（建议 16KB：有实测虚拟内存数据；若选 8KB 需补测） | P0（阻塞后续） |
| R2 | 复核 `benchmarks/gmp_vs_epoll_fair_benchmark.cpp:716` 的 `char buffer[64*1024]` 是否为遗漏路径 | P1 |
| R3 | 改默认后跑 `ctest`（135 项）+ 8000 连接 RSS/虚拟内存对比，确认与基线结论不冲突 | P1 |
| R4 | 若统一「0 = 128KiB」这一隐藏语义，同步改 `docs/configuration.md:23` 的备注与 `go2cpp.ini:23-24` 注释 | P2 |
