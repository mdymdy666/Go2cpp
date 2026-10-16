# scenarios · C 可视化与输出（7 个场景详细设计）

> C 类回答"怎么把快照变成人能用、机器能读的形式"。共同前提：数据来源已在
> `design/01-capability-matrix.md` 与 `03-enumeration-and-registry.md` 中确认。

## C1 · 文本 dump（人类可读）

- **目的**：排障现场直接看。
- **触发**：`Go2Cpp dump scheduler --format=text`
- **期望形态（示例，字段名与 schema 一致）**：

```
scheduler: main   processors=4 workers=6  runnable=12 (global 3)  running=true
sysmon: running=true passes=4210
metrics: runs=100234 completions=100200 resume_ns=884213001 local=91234 steal=431

processor  state    queued
  0        Running     3
  1        Idle        0
  2        Running     0     (detached: machine 3 blocking task 42 for 0 long syscalls)
  3        Idle        1

machine  state    P  affinity(h/m)  detached  blocking  long_syscalls
  0      Running  0   812/44        false     0         0
  1      Idle     1   -             false     0         0
  3      Blocking 2   -             true      42        1

fiber    state     reason        depth  M  P  name            function                   line
  1      Running   None            1    0  0  s_main_task     task::worker_loop          128
  2      Waiting   Synchronization 2    0  0  (unregistered)  -(no metadata)             -
  3      Suspended Park            1    3  2  (unregistered)  -(no metadata)             -
```

- **现状**：字段全部可取（除 `name`/`function`/`line`，见 B1）；渲染层需新增。
- **注意**：`(unregistered)` 与 `-(no metadata)` 是**如实呈现缺信息**，
  不能用占位符伪装成已知值。

## C2 · ini 格式 dump

- **触发**：`--format=ini`
- **schema**：见 `design/02-dump-cli.md` §2（节结构 `[scheduler]`/`[processor.N]`/`[machine.N]`/`[fiber.N]`）
- **现状**：需新增序列化器；项目已有 `config` 模块（INI + 两阶段事务 + 热重载），
  但**写**方向需要确认是否有现成能力（**未确认**）。

## C3 · json 格式 dump

- **触发**：`--format=json`
- **schema**：见 `design/02-dump-cli.md` §3（含 `schema` 版本字段，便于后续兼容）
- **现状**：项目已有 `json.hpp`（nlohmann/json 薄封装），**可直接复用**，无需新依赖。

## C4 · 重定向到文件

- **触发**：`--output=<file>`（`-` 表示 stdout）
- **设计关注点**：
  - **原子性**：单次 dump 应尽量"要么完整要么不产生半截文件"（临时文件 + rename）；
  - **多进程**：建议默认文件名带 `<host>.<pid>.<timestamp>`，避免互相覆盖；
  - **追加 vs 截断**：`--append`/`--truncate` 显式选择；
  - **磁盘保护**：写入前检查可用空间（可选），失败不抛异常、只上报。

## C5 · 条件过滤

- **触发**：`--state=Waiting,Suspended`、`--min-depth=2`、`--processor=0`、`--include=...`
- **注意**：过滤必须**乘着取快照时做**（在 L2 层），而不是"取全部再筛" ——
  后者在大规模下会产生无谓的内存与序列化开销。
- **现状**：需新增查询层；数据字段齐全。

## C6 · 时间线可视化（事件流 → 甘特图 / SVG）

- **触发**：`--events=N --format=svg`（或其他渲染器）
- **输入**：`SchedulerEvent`（9 类，含 `task_id`/`machine_id`/`processor_id`/`task_state`/`timestamp`）
- **能画出什么**：
  - 每个 M 的忙碌/空闲时段（`kWorkerStarted`/`kWorkerStopped` + 任务区间）；
  - 每个 G 的生命周期条（`kTaskStarted` → `kTaskCompleted/kTaskFailed/kTaskCancelled/kTaskSuspended`）；
  - G 在 M 之间的迁移（同一 `task_id` 出现在不同 `machine_id`）；
- **必须标注的坑**：`kTaskSuspended` 是**兜底类别**（`scheduler.cpp:3843-3851`），
  它包含 `kWaiting`/`kRunnable` 等多种情形 → 图上**不能**把它简单画成"阻塞中"。
- **现状**：需新增渲染器；项目已有 SVG 生成经验（`ArchitectureDiagram/00-architecture.svg`）。

## C7 · 实时监视模式

- **触发**：`--watch=1s`（周期刷新）/ `--follow`（事件流跟踪）
- **设计关注点**：
  - 刷新必须**不阻塞** worker（消费在独立线程）；
  - 终端渲染考虑 ANSI 清屏与 `--color=auto|always|never`；
  - `--follow` 需要事件**环形缓冲**（现状缺口，见 A7）；
  - 退出条件：`Ctrl-C` 必须**干净退出**（不留下挂起的观测线程）。

## C 类共同缺口汇总

| 缺口 | 影响 | 备注 |
|---|---|---|
| 序列化层（text/ini/json） | C1/C2/C3 | json 可直接用项目 `json.hpp` |
| 文件输出（原子性/多进程/轮转） | C4 | 需新设计 |
| 查询与过滤层 | C5 | 应在取快照时生效 |
| 事件历史缓冲 | C6/C7 | 插件侧有界环形缓冲 |
| SVG/甘特渲染 | C6 | 需新增 |
| Fiber 元数据（名字/函数/行号） | C1 的完整性 | 见 B1，需定案 |
