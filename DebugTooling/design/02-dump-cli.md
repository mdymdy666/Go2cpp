# design · 02 dump CLI 与输出 schema（第 4 轮，骨架）

> 本轮确定：命令形态、输出 schema（ini / json）、**每个字段与现有接口的映射**、
> 20 个选项的行为定义，以及**哪些字段现在取不到**。

## 1. 命令形态

使用者给定：

```
Go2Cpp dump scheduler [--options] ... scheduler_name
```

补充约定（待确认）：

- `scheduler_name`：**项目当前没有"调度器名字"这个概念**（`SchedulerConfig` 无 name 字段，
  `scheduler.hpp:161-197`）。需要新增：要么命令行按**进程内注册名**查找，要么退化为
  「单进程只有一个调度器时省略」。这一点**必须在设计上先定，不能写死**。
- 输出目标：stdout（默认）/ 文件（`--output`）/ 观察者插件（远程）。

## 2. ini schema（节结构）

```ini
[scheduler]
name = main                     ; 需新增（见 §1）
processor_count = 4             ; Scheduler::processor_count()
worker_count = 6                ; Scheduler::worker_count()
runnable_total = 12             ; Scheduler::runnable_count()
runnable_global = 3             ; Scheduler::global_runnable_count()
is_running = true               ; Scheduler::is_running()
sysmon_running = true           ; Scheduler::sysmon_running()
sysmon_passes = 4210            ; Scheduler::sysmon_pass_count()

[scheduler.metrics]             ; Scheduler::metrics()
task_runs = 100234
task_completions = 100200
fiber_resume_ns = 884213001
local_queue_pops = 91234
steal_pops = 431

[processor.0]                   ; Scheduler::processors() -> ProcessorSnapshot
id = 0
state = Running                 ; PState
queued = 3

[processor.1]
id = 1
state = Idle
queued = 0

[machine.0]                     ; Scheduler::machines() -> MachineSnapshot
id = 0
state = Running                 ; MState
processor = 0
last_task_class = 1
affinity_hits = 812
affinity_misses = 44
processor_detached = false
blocking_task = 0
long_syscall_count = 0

[fiber.1]                       ; FiberContextFrame（由入口枚举得到）
id = 1
parent_id = 0                   ; 0 表示 main_fiber
active_parent_id = 1
depth = 1
main_fiber = false
alive = true
active = true
cancellation_requested = false
state = Running                 ; FiberState
suspend_reason = None           ; SuspendReason
last_thread = 140234...         ; std::thread::id（需转成可比较的整数）
execution.scheduler_token = ...
execution.task_id = 1
execution.machine_id = 0
execution.processor_id = 0
execution.managed = true
; 以下三项当前取不到，需要新增机制：
; name        = s_main_task
; function    = void worker_loop(int)
; line        = 128

[fiber.1.stack]                 ; Fiber::context_snapshot()（可选，--stacks）
0.id = 1
0.depth = 1
0.state = Running
0.alive = true
1.id = 0
1.depth = 0
1.main_fiber = true
```

## 3. json schema（字段树）

```json
{
  "schema": "go2cpp.dump.scheduler/1",
  "generated_at": {"steady_ns": 884213001, "wall_unix_ns": null},
  "scheduler": {
    "name": null,
    "processor_count": 4, "worker_count": 6,
    "runnable_total": 12, "runnable_global": 3,
    "is_running": true,
    "sysmon": {"running": true, "passes": 4210},
    "metrics": {"task_runs": 100234, "task_completions": 100200,
                "fiber_resume_ns": 884213001, "local_queue_pops": 91234,
                "steal_pops": 431}
  },
  "processors": [{"id": 0, "state": "Running", "queued": 3}],
  "machines": [{"id": 0, "state": "Running", "processor": 0,
                "last_task_class": 1, "affinity_hits": 812, "affinity_misses": 44,
                "processor_detached": false, "blocking_task": 0,
                "long_syscall_count": 0}],
  "fibers": [{
    "id": 1, "parent_id": 0, "active_parent_id": 1, "depth": 1,
    "main_fiber": false, "alive": true, "active": true,
    "cancellation_requested": false,
    "state": "Running", "suspend_reason": "None", "last_thread": 140234,
    "execution": {"scheduler_token": 0, "task_id": 1, "machine_id": 0,
                  "processor_id": 0, "managed": true},
    "debug": {"name": null, "function": null, "file": null, "line": null},
    "stack": [{"id": 1, "depth": 1, "state": "Running", "alive": true}]
  }],
  "locks": null
}
```

**注意 `generated_at`**：`SchedulerEvent::timestamp` 与 `steady_clock` 都不能直接跨机比较
（`scheduler.hpp:130-137` 注释明确）。json 里同时留 `steady_ns` 与 `wall_unix_ns` 两个位，
**是否填 wall 由实现阶段决定**（涉及多机时钟统一，见 E3 场景）。

## 4. 字段 → 现有接口 的映射（可核验）

| schema 字段 | 来源接口 | 证据 |
|---|---|---|
| `scheduler.processor_count` | `Scheduler::processor_count()` | `scheduler.hpp:582` |
| `scheduler.worker_count` | `Scheduler::worker_count()` | `scheduler.hpp:583` |
| `scheduler.runnable_total` | `Scheduler::runnable_count()` | `scheduler.hpp:584` |
| `scheduler.runnable_global` | `Scheduler::global_runnable_count()` | `scheduler.hpp:585` |
| `scheduler.is_running` | `Scheduler::is_running()` | `scheduler.hpp:448` |
| `scheduler.sysmon.*` | `sysmon_running()` / `sysmon_pass_count()` | `scheduler.hpp:595-598` |
| `scheduler.metrics.*` | `Scheduler::metrics()` | `scheduler.hpp:599`, `236-243` |
| `processors[]` | `Scheduler::processors()` | `scheduler.hpp:592`, `210-214` |
| `machines[]` | `Scheduler::machines()` | `scheduler.hpp:593`, `217-233` |
| `fibers[].id/state/suspend_reason/depth/...` | `FiberContextFrame` | `fiber.hpp:54-67` |
| `fibers[].stack[]` | `Fiber::context_snapshot()` | `fiber.hpp:206-211` |
| `fibers[].execution.*` | `FiberExecutionBinding` | `fiber.hpp:36-48` |
| `fibers[].debug.*` | **无来源** | ❌ 需新增（转译期注入或符号表） |
| `scheduler.name` | **无来源** | ❌ 需新增 |
| `locks` | **无来源** | ❌ 需新增（`Mutex::Impl` 全私有，`sync.cpp:665-686`） |

## 5. 二十个选项的行为定义

| 选项 | 行为 | 可实现性 |
|---|---|---|
| `--format=text\|ini\|json` | 选择渲染器 | 需新增序列化层 |
| `--output=<file>` | 写文件而非 stdout | 需新增 |
| `--append` / `--truncate` | 文件写入语义 | 需新增 |
| `--summary` | 只输出 `[scheduler]` 与 metrics | ✅ 数据已有 |
| `--fibers` | 枚举 Fiber 列表 | ⚠️ 需要「**如何枚举全部 Fiber**」的机制（见 §6） |
| `--fiber=<id>` | 过滤单个 | 同上 |
| `--stacks` | 附带调用栈 | ✅ `context_snapshot()` |
| `--stack-depth=N` | 截断深度 | ✅ 有 `depth` 字段 |
| `--state=<list>` | 按 G/Fiber 状态过滤 | ✅ 有枚举 |
| `--min-depth=N` | 深度阈值 | ✅ |
| `--processors` | 只输出 P | ✅ |
| `--machines` | 只输出 M | ✅ |
| `--metrics` | 只输出指标 | ✅ |
| `--events[=N]` | 最近 N 条事件 | ⚠️ 现有 Observer 是**推送**式的，**没有环形历史缓冲**，需新增 |
| `--locks` | 锁与等待者 | ❌ 需新增查询接口 |
| `--waiters` | 等待关系 | ❌ 同上 |
| `--watch[=interval]` | 周期刷新 | ⚠️ 需新增（可基于现有快照轮询） |
| `--follow` | 跟踪事件流 | ⚠️ 需新增（基于 Observer + 独立线程消费） |
| `--include=<fields>` | 字段选择 | 需新增 |
| `--color=auto\|always\|never` | 着色 | 需新增 |

## 6. 本轮暴露的两个**结构性问题**（必须先在设计中解决）

1. **「如何枚举全部 Fiber？」当前没有答案。**
   `Scheduler` 提供的是**计数**（`runnable_count()` 等）与 **P/M 快照**，
   没有"列出所有 G/Fiber"的接口；`Fiber` 侧的 `CurrentContextSnapshot()` 只能拿到
   **当前线程/当前 Fiber 的父链**，拿不到全局集合。
   → 要么新增注册表（Fiber 创建/销毁时登记），要么通过 P 队列/等待队列间接遍历。
   **这是 dump 功能的前置条件**，必须先定。

2. **事件历史没有缓冲。**
   `SchedulerObserver` 只有推送回调（`scheduler.hpp:173`），没有环形历史。
   `--events=N` 与 `--follow` 都需要一个**可选的、有界的**事件缓冲（由插件侧实现，
   不应侵入调度器热路径）。

## 7. 下一步

- 把 §6 的两个结构性问题写成独立的**前置设计任务**；
- 补 text 渲染的示例输出（人类可读形态）；
- 定 `--include` 的字段命名规范（与 json 路径一致）。


## 8. text 渲染示例（与 `scenarios/C-visualization.md` 的 C1 对齐）

```text
$ Go2Cpp dump scheduler --format=text --stacks --stack-depth=4
scheduler: main   processors=4 workers=6  runnable=12 (global 3)  running=true
sysmon: running=true passes=4210
metrics: runs=100234 completions=100200 resume_ns=884213001 local=91234 steal=431
snapshot: per-stripe consistent, NOT globally atomic (16 registry stripes)

processor  state    queued  active  running
  0        Running     3      1       1
  1        Idle        0      1       0
  2        Running     0      1       1
  3        Idle        1      0       0

machine  state     P  affinity(h/m)  detached  blocking  long_syscalls
  0      Running   0   812/44         false     0         0
  1      Idle      1   -              false     0         0
  2      Running   2   77/3           false     0         0
  3      Blocking  3   -              true      42        1

fiber  state      reason           depth  M  P  name            function                  line
  1    Running    None               1    0  0  s_main_task     task::worker_loop         128
  2    Waiting    Synchronization    2    0  0  (unregistered)  -(no metadata)             -
  3    Suspended  Park               1    3  3  (unregistered)  -(no metadata)             -
  4    Runnable   None               1    -  -  (unregistered)  -(no metadata)             -

stacks (fiber 2):
  [0] fiber=2  depth=2  state=Waiting    reason=Synchronization  alive=true  active=true
  [1] fiber=1  depth=1  state=Running    reason=None             alive=true  active=false
  [2] main_fiber depth=0  state=Running  reason=None             alive=true  active=false
```

**渲染约定（必须遵守）**：

1. **缺信息如实呈现**：`(unregistered)` 表示没有该 G 的元数据、`-(no metadata)` 表示该字段无来源；
   **不得**用 `unknown`/`N/A` 之类混同"已查询但为空"与"机制尚不存在"；
2. **快照一致性必须显式标注**：`snapshot: per-stripe consistent, NOT globally atomic`
   —— 让使用者知道这不是同一瞬间的全景（依据：`task_registries` 为 16 分片、逐把加锁，
   `scheduler.cpp:1694-1705`）；
3. **`kTaskSuspended` 的语义**在时间线类输出里必须标注为**兜底类别**
   （`scheduler.cpp:3839-3848`），不能在图上画成"阻塞中"；
4. **`collect_metrics=false` 时** metrics 全为 0，输出应显示
   `metrics: disabled (collect_metrics=false)` 而不是一串 0（依据：`3852-3862` 才有累计）；
5. 列宽按内容自适应；`--color=never` 时不输出任何 ANSI 序列。
