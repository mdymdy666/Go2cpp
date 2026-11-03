# 可观测（observability）· 深入学习

> 面向「要接手改这块代码的人」。本文只讲 `include/go2cpp/os/monitor/` + `src/os/monitor/`、
> `bpf/`、`include/go2cpp/debug/` + `src/debug/`、`include/go2cpp/metrics/` + `src/metrics/`。
> 所有 `文件:行` 都用 `grep -n` / `sed -n` 在 `/UserData/CodexWorkSpace/Go2cpp` 下逐条核对过；
> 没核到的写「未核实」。不写「达标」结论——本模块在 `docs/metrics-ledger.md` 里没有条目。

---

## 第一部分 · 定位

### 1.1 解决什么问题

这套代码解决的是「Go 风格的运行时/服务，怎么在没有外部 agent 的前提下看清自己」：

- **资源面**：CPU / 内存 / 磁盘 / 网络四个 `/proc` 采集器（`CpuMonitor`、`MemMonitor`、
  `DiskMonitor`、`NetMonitor`），给出利用率与速率。
- **内核面**：eBPF 采集 syscall 延迟、调度延迟、futex 等待、TCP 重传/RTT、文件 IO 延迟，
  以及 TC hook 上的每网卡收发字节/包数（`NetEbpfMonitor`）。
- **聚合与转运**：`Aggregator` 把 N 个采集器的结果合并成一个 `Snapshot`；`Pusher` 周期
  HTTP POST 给 `Manager`；`Manager` 是接收端，提供 `/api/push`、`/api/query/*`、`/api/health`。
- **应用面指标**：`metrics::Counter/Gauge/Histogram` + `prometheus::Exporter`，无锁原子实现。
- **诊断面**：`debug::EventRing` 每线程环形事件 + `debug_dump` / `diagnostic_agent` 的离线 dump。

一句话：**os/monitor 是「机器视角」，metrics 是「业务视角」，debug 是「运行时内部视角」**，
三者互不依赖，只有 `Manager`/`Pusher` 这一对走 HTTP 解耦。

### 1.2 与浅层文档 `user/docs/10-observability.md` 的关系

实测结论（可复核）：

```bash
grep -rn 'os/monitor\|kernel_ebpf\|ringbuf\|bpf' user/docs/10-observability.md   # 无输出
```

即浅层文档只覆盖 **metrics / logging / config / debug** 四条线（其章节目录为
「指标」「Prometheus 导出与端点」「日志与脱敏」「轮转与配置热更新」「事件环与在线诊断」），
**完全不含 `os/monitor` 与 `bpf/`**。因此本文的增量是：

| 主题 | 浅层 `user/docs/10-observability.md` | 本文（deep） |
|---|---|---|
| metrics / prometheus | 有（API + 导出格式） | 只补原子直方图实现与基数陷阱 |
| debug 事件环 | 有（用法与出口） | 补写入协议、分区、降采样、丢弃路径 |
| os/monitor 十采集器 | **无** | 本文主体（§3.1–§3.3） |
| bpf/ 七个程序 | **无** | §3.3 + §5 |
| Manager / Pusher | **无** | §3.2 |

### 1.3 先纠正三个容易传错的口径

1. **「15 个采集器」不成立。** `include/go2cpp/os/monitor/` 下确实是 15 个头文件，但其中
   `aggregator.hpp`、`manager.hpp`、`pusher.hpp`、`interfaces.hpp`、`monitor.hpp`（数据结构与
   JSON/健康分）不是采集器。真正的采集器是 **10 个**，由 `CollectorKind` 的 10 个枚举值定义
   （`include/go2cpp/os/monitor/aggregator.hpp:47-58`）：cpu / mem / disk / net / ebpf /
   syscall / sched / futex / tcp / fileio。
2. **没有 ringbuf。** 全树 `grep -rn 'ringbuf\|BPF_MAP_TYPE_RINGBUF' bpf/ src/os/monitor/`
   无输出。内核侧用的是 `BPF_MAP_TYPE_ARRAY`（全局统计/直方图）与 `BPF_MAP_TYPE_LRU_HASH`
   （起始时间戳），用户态**轮询 map**，不是事件流。`bpf/net_stats.bpf.c:37` 的
   `net_stats_map`、`bpf/futex_latency.bpf.c:70-89` 的 `futex_start_map`/`futex_global_map`
   都是这个模式。
3. **Manager 不是采集调度者。** 采集调度在 `Aggregator`（每采集器一个定时器 + 一条聚合
   Fiber）；`Manager` 只是 HTTP 接收端（`include/go2cpp/os/monitor/manager.hpp:67` 注释：
   「Start() 后由后台 Fiber 接受连接并处理请求」）。

---

## 第二部分 · 架构

### 2.1 文件清单

`os/monitor`（15 头 + 18 源）：

| 层 | 文件 | 职责 |
|---|---|---|
| 数据结构 | `include/go2cpp/os/monitor/monitor.hpp`（501 行） | `Snapshot`（:396）、`SyscallStats`（:182）、`SchedStats`（:202）、`FutexStats`（:235）、`TcpStats`（:278）、`FileIoStats`（:336）、`KernelHistUpperUs`（:156）、`SnapshotToJson`（:487）、`HealthScore`（:499） |
| 接口 | `interfaces.hpp` / `interfaces.cpp` | `SnapshotSink`(:28)、`SnapshotSource`(:46)、`SnapshotStore`(:61)、Memory/File/Http 实现 |
| 采集器 | `cpu/mem/disk/net_monitor.*` | `/proc` 族，差分基线在对象内 |
| 采集器 | `net_ebpf_monitor.*` | TC clsact ingress/egress 统计 |
| 采集器 | `syscall/sched/futex_monitor.*` | tracepoint + 直方图 |
| 采集器 | `tcp_monitor.*` / `fileio_monitor.*` | kprobe/kretprobe 延迟 |
| eBPF 底座 | `src/os/monitor/kernel_ebpf.hpp/.cpp` | `KernelBpfObject`：加载/attach/`ProgramCost`/`ReadHashMap`/`ReadArrayMap` |
| 聚合 | `aggregator.hpp/.cpp`（559 行） | 定时采集 → Channel → 合并 → latest_ → sink |
| 转运 | `pusher.hpp/.cpp` | 周期 `PushOnce` → `HttpSnapshotSink` |
| 接收 | `manager.hpp/.cpp`（513 行） | HTTP 路由 + `MemorySnapshotStore` + 摘要/评分 |

`bpf/`：`net_stats.bpf.c`、`syscall_latency.bpf.c`、`sched_latency.bpf.c`、`futex_latency.bpf.c`、
`tcp_monitor.bpf.c`、`fileio_monitor.bpf.c`、`monitor_hist.h`（共用对数桶）。

`debug/`：14 个 UTF-8 头 + 4 个 cpp；核心是 `debug_event_ring.hpp`（739 行）与
`debug_event_ring.cpp`（2248 行），其余是 dump/breakpoints/scheduler/signal/diagnostic 的封装。

`metrics/`：`metrics.hpp`（526 行）/`metrics.cpp`（1201 行）、`prometheus.hpp/.cpp`、
`endpoint.hpp/.cpp`。

### 2.2 入口点清单

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Aggregator::Start()` | `src/os/monitor/aggregator.cpp:409` | 应用 | 建 `Scheduler`、`MakeChannel(16)`、spawn 聚合 Fiber、每采集器 `schedule_every` | bool；重复 Start false |
| `Aggregator::Stop()` | `src/os/monitor/aggregator.cpp:455` | 应用 | 取消定时器 → 关 Channel → 等 Fiber → shutdown → 停采集器 | void，幂等 |
| `Aggregator::CollectNow()` | `src/os/monitor/aggregator.cpp:506` | 测试/确定性调用 | 持 `lifecycle_mutex` 同步跑 10 个 `UpdateOnce` | `Snapshot` |
| `Aggregator::PublishLatest()` | `src/os/monitor/aggregator.cpp:526` | 发布定时器 | 去重后 `sink->Push` | bool |
| `Aggregator::PublishNow()` | `src/os/monitor/aggregator.cpp:530` | 测试 | 同步采集后直接 Push（不更新 latest） | bool |
| `Pusher::Start()` | `src/os/monitor/pusher.cpp:56` | 应用 | 2 线程 Scheduler + `schedule_every(interval)` | bool |
| `Pusher::PushOnce()` | `src/os/monitor/pusher.cpp:78` | 定时器/测试 | `source.Latest()` → HTTP POST | bool |
| `Manager::Start()` | `src/os/monitor/manager.cpp:215` | 应用 | 建 TCP、bind、listen(128)、`getsockname` 回填端口、注册 5 条路由 | bool；端口占用 false |
| `Manager::Ingest()` | `src/os/monitor/manager.cpp:483` | HTTP handler / 测试 | `store->Append` + 计数 | bool |
| `SnapshotToJson` | `src/os/monitor/monitor.cpp:693` | Pusher | 序列化快照 | `std::string` |
| `HealthScore` | `src/os/monitor/monitor.cpp:712` | Manager `/api/query/stats` | 加权算 0–100 | double |
| `EventRing::Record` | `src/debug/debug_event_ring.cpp:853` | 调度器/原语热路径 | 写本线程环（先清 seq） | void，noexcept |
| `EventRing::Snapshot` | `src/debug/debug_event_ring.cpp:1114` | dump/diagnostic | 汇总所有线程环并排序 | `vector<EventRecord>` |
| `EventRing::DumpText` | `src/debug/debug_event_ring.cpp:1196` | CLI/信号 dump | 文本导出（含 `dropped_threads`） | `std::string` |
| `Histogram::Observe` | `src/metrics/metrics.cpp:818` | 业务 | 二分找桶 + 3 次原子加 | void；NaN 直接 return |
| `Exporter::Render` | `src/metrics/prometheus.cpp:831` | HTTP `/metrics` | `Registry::Render()` + 分位数追加 | `std::string` |

### 2.3 总架构图

```
采集（用户态）                                    转运                     接收
┌──────────────────────────────┐
│ Aggregator                    │
│  Scheduler(workers)           │
│   ├ timer(cpu)   ─┐           │      Channel<CollectorResult>(16)
│   ├ timer(mem)    │  UpdateOnce│      ┌──────────────┐
│   ├ timer(disk)   ├──────────► TrySend ─► 聚合 Fiber │
│   ├ timer(net)    │           │      └──────┬───────┘
│   ├ timer(ebpf)   │           │             │ MergeLocked
│   ├ timer(syscall)│           │             ▼
│   ├ timer(sched)  │           │        latest_（mutex 保护）
│   ├ timer(futex)  │           │             │
│   ├ timer(tcp)    │           │      PublishLatest（按 timestamp 去重）
│   └ timer(fileio) ┘           │             ▼
└──────────────────────────────┘        SnapshotSink
                                            ├ HttpSnapshotSink ──HTTP POST──► Manager /api/push
                                            ├ MemorySnapshotSink（测试）        │
                                            └ FileSnapshotSink（JSONL）         ▼
                                                                        MemorySnapshotStore(256)
                                                                          ├ /api/query/latest
                                                                          ├ /api/query/stats
                                                                          ├ /api/query/kernel
                                                                          └ /api/health

采集（内核态，无 ringbuf）
  tracepoint/kprobe/TC 程序
    ├ ARRAY map：全局聚合 + 32 桶直方图
    └ LRU_HASH ：entry 时间戳（key=pid/tid）
         ▲
         │ 用户态每周期 bpf_map_get_next_key + bpf_map_lookup_elem 轮询
         └── ReadHashMap / ReadArrayMap（kernel_ebpf.hpp:91/119）
```

### 2.4 线程 / 锁边界

| 对象 | 线程 | 锁 |
|---|---|---|
| `Aggregator` | 自己的 `Scheduler`（默认 `max(2, hardware_concurrency)`，`aggregator.cpp:423-425`）；1 条聚合 Fiber | `lifecycle_mutex`（Start/Stop/CollectNow）、`mutex`（latest_/stats）、`collect_mutexes[10]`（同种采集器串行）、`sink_mutex` |
| 采集器内部 | 定时器线程 | 多数采集器**自己不加锁**，靠 `collect_mutexes` 保护（`aggregator.cpp:117-120` 注释明确「Cpu/Disk/Net 内部没有锁」） |
| `NetEbpfMonitor` | 同上 | 自己的 `mutex` 串行化 UpdateOnce / 热插拔扫描 / Stop（`net_ebpf_monitor.cpp:72`） |
| `Pusher` | 2 线程 Scheduler | `lifecycle_mutex`；HTTP sink 内部串行 |
| `Manager` | HTTP Server 的 Fiber + `getsockname` 在主线程 | `lifecycle_mutex`、`mutex`（store/计数） |
| `EventRing` | **每线程无锁写自己分片** | 仅配置用 `g_debug_config_mutex`（`debug_event_ring.cpp:451`）；环本身靠 seq 二次校验 |
| `metrics` | 调用方线程直接写 | 全 `std::atomic`（relaxed） |

---

## 第三部分 · 关键实现

### 3.1 采集调度：10 个独立定时器 + 1 条聚合 Fiber

`Aggregator::Start`（`src/os/monitor/aggregator.cpp:409`）步骤：

1. 取 `lifecycle_mutex`；`running` 已 true 直接返回 false（:411-413）。
2. 若 `collectors_stopped` 为真，先 `CreateCollectors()` 重建采集器（:416-419）。原因是
   `Stop` 会 `Close()` 掉 eBPF 对象（detach + 关 map fd），这些对象不会自愈；不重建则第二个
   运行周期里五个内核采集器永久停在 `reason="stopped"`。
3. **先置 `running = true` 再 spawn**（:422）——聚合 Fiber 的循环条件就是 `running`，
   顺序反过来会让新 Fiber 首次调度即退出。
4. 建 `Scheduler(workers)`，`MakeChannel<CollectorResult>(16)`（:429）。
5. spawn 聚合 Fiber（:432-441）：`while (running) { Recv(); AcceptResult(); }`，`Recv` 返回空
   即 Channel 关闭 → break。
6. `StartTimers()`（:209-238）：对每个启用的采集器 `schedule_every(period, ...)`，回调里
   `Collect(kind, partial)` 然后 **`channel->TrySend`**（:219-221）。
7. `StartPublisher()`（:297-301）：再挂一个同周期定时器，只做 `PublishLatest()`。

**失败路径（这里是最容易改坏的地方）**：

- `TrySend` 失败（Channel 满 / 已关）时**直接丢样本**，注释写明「不能阻塞在 Channel 上」
  （:219-221）。这保证采集线程不会被聚合线程拖死，代价是 §5 的「窗口丢失」。
- `AcceptResult`（:193-203）：`kOk` → `MergeLocked` + 刷新 `timestamp_ms` + `++stats.snapshots`；
  `kError` → `++stats.errors`；`kUnsupported`（非 root 降级）**两者都不计**，所以
  「`snapshots` 增长慢」不等于故障。
- spawn 失败（:442-448）：回滚 `running=false`、释放 channel、shutdown scheduler，返回 false。

### 3.2 发布去重与 sink 语义

`PublishLatest`（`aggregator.cpp:267-294`）是本模块唯一有「正确性故事」的小函数：

1. 先 `Sink()` 取当前 sink（持 `sink_mutex`），空则 false。
2. 持 `mutex` 读 `latest.timestamp_ms`；为 0 或等于 `last_published_ms` 直接 false（去重）。
3. 拷贝快照后**释放锁**再 `Push`（HTTP 可能阻塞数秒，不能持 `mutex`）。
4. Push 失败**不推进** `last_published_ms`（:281-285 注释）。原实现先记账再 Push，sink 瞬断会
   让该帧永久丢失；现在下个周期会重试同一帧。
5. Push 成功后再持锁把 `last_published_ms` 推进到快照时间戳（:288-293）。

`Pusher` 是另一条路：它不持有 Aggregator 类型，只依赖 `SnapshotSource`
（`pusher.hpp:46`），构造时把 HTTP 细节交给 `HttpSnapshotSink`（`pusher.cpp:38-39`）。
`Pusher::Push` 直接转发 `PushSnapshot`（`pusher.cpp:86-88`），所以 Pusher 也能当 sink
塞回 `Aggregator::SetSink`——这是测试里做「采集→推送」全链路的方式。

### 3.3 eBPF：加载、降级、map 轮询

**可用性判定**只有两条：构建期 `GO2CPP_HAVE_EBPF` + 运行期 `geteuid() == 0`
（`src/os/monitor/kernel_ebpf.cpp:16-30`）。没有 cap_bpf / 容器特权的分支，因此非 root 一律降级。

单个采集器的初始化（以 syscall 为例，`src/os/monitor/syscall_monitor.cpp:55-88`）：

1. `KernelBpfUsable()` 假 → `reason = KernelBpfUnusableReason()`，`available=false`，return。
2. `object.Load(go2cpp_syscall_latency_elf, ...)`：从**内嵌 ELF 字节**加载，不读磁盘文件。
3. `MapFd("syscall_stat_map")`；`< 0` → reason 写「lacks …」并 `Close()`。
4. `AttachTracepoint("syscall_enter", "raw_syscalls", "sys_enter")` 与 exit 侧；任一失败 →
   reason + `Close()` + `stat_fd=-1`。
5. 全成功才 `available=true; reason.clear()`。
6. `StopInternal()` 把 `reason` 置为 `"stopped"`（:89-96），所以「停过又没重建」和「从来没成功」
   在外部看起来都是不可用，但 reason 不同——排障时要看这个字符串。

**可选探针回滚**：`KernelBpfObject::DetachLast(count)`（`kernel_ebpf.hpp:57-64`）用于
「entry 成功、return 失败」时回滚 entry，避免留下永不结算的起始时间记录。

**数据读取**（不是 ringbuf）：

- 直方图/全局计数器在 `ARRAY` map 里，用户态 `ReadArrayMap(fd, index, &out)`
  （`kernel_ebpf.hpp:119-132`），syscall 侧按 syscall id 逐个读（`syscall_monitor.cpp:130`）。
- entry 时间戳在 `LRU_HASH` 里，用户态 `bpf_map_get_next_key` + `bpf_map_lookup_elem` 遍历
  （`kernel_ebpf.hpp:91-116`）。中途失败返回已读部分——「宁可少一条样本，不要整次失败」。
- 桶边界在 `bpf/monitor_hist.h:22-64` 用 5 次比较+移位手算 `floor(log2)`，因为 BPF 后端不支持
  `__builtin_clz` 降级出的 libcall；溢出走 bucket 31（:36-37）。

**内核自身开销**：`ProgramCost` 读 `bpf_prog_info.run_time_ns/run_cnt`（`kernel_ebpf.hpp:71-77`），
内核未开统计时两者为 0。

### 3.4 metrics：原子直方图（一次 Observe 三次原子操作）

`Histogram::Observe`（`src/metrics/metrics.cpp:818-830`）：

1. `EnsureSeries("Observe")`：带标签的根对象直接 `Observe` 会抛 `std::logic_error`
   （`metrics.hpp:59`）。
2. NaN 直接 return（:820-823）——NaN 会污染 Sum 与分位数。
3. `BucketIndex(boundaries, value)` 二分，`le` 语义（第一个 `>= value` 的边界）。
4. `m_bucket_counts[index].fetch_add(1, relaxed)`（:827）。
5. `AddDouble(m_sum_bits, value)`：float 以 IEEE-754 位模式存 `atomic<uint64_t>`，CAS 循环
   （:350 附近，`metrics.cpp:314` 注释说明为什么不用 `atomic<double>`）。
6. `m_count.fetch_add(1, relaxed)`（:829）。

成本恒定、内存 O(buckets) 与请求量无关（`metrics.hpp:341-348`）；代价是 `relaxed` 允许
Count/Sum/桶三者被读到时略有偏差——监控允许近似，但不能丢更新（:826 注释）。

导出侧：`Exporter::Render`（`src/metrics/prometheus.cpp:831`）先 `Registry::Render()`
（内部持锁拿一致快照，`prometheus.hpp:287`），再对声明了分位数的直方图追加 `quantile` 序列。

### 3.5 debug：每线程环形事件（先清 seq 的发布协议）

`EventRing` 的分区（`include/go2cpp/debug/debug_event_ring.hpp`）：

- 普通区 `kCapacity`（默认 256，:538）、关键区 `kCriticalCapacity`（默认 64，:540）、
  线程上限 `kMaxThreads = 128`（:545）。
- 关键区给 wake 系列 / enqueue 拒绝等「丢了就断链」的事件（:43-51）；但它**仍是环形缓冲**，
  超过 64 条照样覆盖。

写入协议 `WriteSlot`（`src/debug/debug_event_ring.cpp:207-222`）：

1. 读 `segment.head`（relaxed）取写位置。
2. **先写 `slot.seq = 0`**（:215）——读者此刻复制到的是残缺记录。
3. `memcpy` 其余字段（:216-218；POD 整块写，注释说每条省约 10 ns）。
4. **最后写 `slot.seq = record.seq`**（:220）作为「已完整发布」的判据。
5. `head.store(write_index + 1, release)`（:221）。

读取侧靠 `seq` 不匹配丢撕裂记录；`Snapshot`（:1114）汇总所有线程环后按
`(ts_ns, thread_id, seq)` 全序排序（`EventOrderLess`，:225-232）。`ReadSince` 的游标也从
全局 seq 改成了三元组（`debug_event_ring.hpp:90-92`），因为 v6 起 seq 是**每线程单调**。

降采样：`Record` 在累计计数**之前**对高频调度事件按 `SchedEventSampleEvery()` 抽样
（`debug_event_ring.cpp:864-874`），注释给出的原因是 pc/select 场景下 enqueue/push/pop/claim/resume
合计 4~8 条/op，环形窗口被覆盖 490~1354 倍。

丢弃路径：线程数超过 `kMaxThreads` 时 `dropped_threads.fetch_add(1)`（:168），
`DroppedThreads()`（:1099）在 `DumpText` 里输出为 `dropped_threads=`（:1224）。

---

## 第四部分 · 数据 / 性能

**本模块在 `docs/metrics-ledger.md` 中无独立台账。** 该文件现有条目只有
M01/M01b/M02/M03/M04/M05/M05b/M06/M07/M08/M09/M10/M11/M12/M13/M14/M15/M16，
逐条核对后没有覆盖 `os/monitor`、`bpf/`、`debug/`、`metrics/` 的任何一条。唯一沾边的是
**M10「os 批量 writev/readv 加速比」**，但它测的是 `os` 模块的 IO 批量接口，不是监控采集，
不能拿来当本模块的数字。

`DebugTooling/perf-observer-overhead.md` **只是报告，不是台账**：它没有 Mxx 编号，
也没有 `tools/verify_metrics.sh` 的复现口径。引用它可以，但不要说「台账里有」。

因此本文对性能只写「结构性事实」，不写结论：

- 采集频率由 `AggregatorConfig::interval` 决定（默认 1000 ms，`aggregator.hpp:28`），
  10 个采集器各一个定时器，即每周期最多 10 次 `UpdateOnce`。
- `Channel` 容量 16（`aggregator.cpp:429`）：一轮 10 条结果，正常不会满；但 `CollectNow`
  与定时器并发、或聚合 Fiber 被 `MergeLocked` 卡住时，`TrySend` 会丢（:219-221）。
- `Histogram::Observe` 是 3 次 relaxed 原子（1 次桶 + CAS Sum + 1 次 count），没有锁、
  没有分配（§3.4）。
- `EventRing::Record` 是纯线程本地写 + 1 次 `head.store(release)`，热路径无原子竞争
  （seq 普通自增，`debug_event_ring.cpp:877`）。
- eBPF 程序自身的 CPU 时间可用 `ProgramCost` 读 `run_time_ns/run_cnt`（`kernel_ebpf.hpp:71-77`），
  但仓库里没有把它接进 Snapshot/Manager 的路径（未核实有无上层消费者）。

---

## 第五部分 · 边界与陷阱

1. **eBPF 需要 root，且没有 capability 分支。** `KernelBpfUsable()` 只看
   `geteuid() == 0`（`kernel_ebpf.cpp:16-30`）。用 `setcap cap_bpf+ep` 的非 root 进程会被判为
   不可用；容器里即使有 `CAP_BPF` 但 uid 非 0 也一样。改这里要同时改 `KernelBpfUnusableReason`
   的文案，否则排障信息会骗人。
2. **非 root 降级不是一种状态，而是三种 reason。** ①从未成功：`KernelBpfUnusableReason()`；
   ②加载/attach 失败：`"eBPF load failed: …"` / `"tracepoint attach failed: …"`；③停过：
   `"stopped"`（`syscall_monitor.cpp:89-96`）。`Aggregator::EbpfReason()` 只透传，调用方不能
   用「reason 非空」判断是不是权限问题。
3. **`Stop()` 之后必须重建采集器。** `Aggregator::Start` 靠 `collectors_stopped` 标志重建
   （`aggregator.cpp:416-419`）；若有人改成「复用对象」，五个内核采集器会永久 `stopped`。
   四个 `/proc` 采集器的差分基线也会从零开始，重启后第一帧速率为 0（`aggregator.hpp:85-88`）。
4. **聚合窗口会丢帧，而且不是「完整轮次」。** `stats.snapshots` 是「每个采集器每次成功 +1」
   （`aggregator.hpp:62-64`），十项全成功一轮 +10；`TrySend` 满/关时直接丢（`aggregator.cpp:220`）。
   所以 `snapshots` 的增速不能当采集轮数用。
5. **指标基数爆炸。** `With()` 建出的子序列**只增不删**（`metrics.cpp:570` 注释
   「children 只增不删、引用稳定」）。用请求路径、用户 ID 当标签，序列数会随流量无界增长；
   `Registry::Render()` 每次要把全部序列拼成文本，延迟和内存一起涨。
6. **debug dump 的锁风险（部分未核实）。** 配置读写确实持全局 `g_debug_config_mutex`
   （`debug_event_ring.cpp:451/628/676`），而 `EventRing::Record` 在热路径上会读采样配置；
   一旦有人在持锁期间做重活（文件 IO、格式化），调度器热路径会被拖住。`DumpText` 本身是否
   持该锁**未核实**——改 dump 前先看 `debug_event_ring.cpp:1196` 附近的实际加锁范围。
7. **eBPF LRU_HASH 会静默丢起始时间戳。** entry 记录进 `LRU_HASH`，map 满时内核淘汰最旧项，
   return 侧查不到就跳过该样本（`kernel_ebpf.hpp:84-90` 的「宁可少一条」注释是同一取舍）。
   表现为延迟分布偏向短样本，而不是报错。
8. **桶边界有两份定义，必须同步。** `bpf/monitor_hist.h:19-20` 的 `GO2CPP_HIST_BUCKETS 32`
   与用户态 `kKernelHistBuckets`（`monitor.hpp:142`）/`KernelHistUpperUs`（`monitor.cpp:61`）
   必须一致；注释已写「改动时必须同步两边」，但没有编译期断言把两边钉死（未核实是否有
   static_assert 覆盖桶边界数值）。
9. **`Manager` 的存储是内存环形，进程退出即丢。** 默认 `MemorySnapshotStore(256)`
   （`manager.hpp:31`），容量满淘汰最旧；要持久化必须注入自定义 `SnapshotStore`
   （`manager.hpp:68-69`）。

---

## 第六部分 · 测试

`tests/os/monitor/` 共 9 个源文件，全部通过 `cmake/Go2CppMonitor.cmake:153-230` 的
`go2cpp_os_monitor_test()` 注册为 ctest，名字即目标名：

| 测试文件 | ctest 名 | 覆盖点 |
|---|---|---|
| `test_monitor_collectors.cpp` | `go2cpp_os_monitor_collectors` | 四个 `/proc` 采集器 + 十项 `UpdateOnce` |
| `test_monitor_interfaces.cpp` | `go2cpp_os_monitor_interfaces` | MockSink / 注入 Store / File+Http sink / Pusher-as-Sink |
| `test_monitor_ebpf.cpp` | `go2cpp_os_monitor_ebpf` | TC 采集；非 root 走降级断言 |
| `test_monitor_e2e.cpp` | `go2cpp_os_monitor_e2e` | Aggregator→Pusher→Manager 全链路 |
| `test_monitor_stress.cpp` | `go2cpp_os_monitor_stress` | 并发启停/采集压力 |
| `test_kernel_monitor.cpp` | `go2cpp_os_kernel_monitor` | syscall/sched/futex 三监控器的降级与真实数据 |
| `test_tcp_io_monitor.cpp` | `go2cpp_os_tcp_io_monitor` | TCP 重传/RTT 与文件 IO 延迟 |
| `test_monitor_ebpf_nonroot.cpp` | `go2cpp_os_monitor_ebpf_nonroot` | 非 root 契约；root 下返回 77 → SKIP |
| `test_monitor_overhead.cpp` | `go2cpp_os_monitor_overhead` | 1/5/20 Hz 单次成本 + 3 秒进程 CPU 占比 |

root 专用三个（`sudo -S` 重跑同一可执行文件的 `--require-ebpf`，无法提权返回 77 → SKIP）：
`go2cpp_os_monitor_ebpf_root`、`go2cpp_os_kernel_monitor_root`、
`go2cpp_os_tcp_io_monitor_root`（`cmake/Go2CppMonitor.cmake:207-230`）。

`tests/metrics/`：`test_metrics.cpp`、`test_prometheus.cpp`、`test_prometheus_endpoint.cpp`、
`bench_prometheus.cpp`。ctest 名为 `go2cpp_metrics_test`、`go2cpp_metrics_prometheus_test`、
`go2cpp_metrics_endpoint_test`（注册在 `cmake/Go2CppMetricsPrometheus.cmake:19/39` 与
`cmake/Go2CppTests.cmake:866`；`bench_prometheus` 是基准，不是 ctest）。

`tests/debug/`：17 个源文件（`test_debug_dump.cpp`、`test_debug_events.cpp`、
`test_debug_scheduler.cpp`、`test_debug_breakpoints.cpp`、`test_debug_signal_dump.cpp`、
`test_diagnostic.cpp`、`test_resource_history*.cpp` 等）。`test_debug_dump.cpp` 编进
`go2cpp_tests`（`cmake/Go2CppTests.cmake:67`），其余有独立目标（如
`go2cpp_debug_record_correctness`、`go2cpp_debug_module_demo`）。完整 ctest 名以
`ctest -N` 为准——上表之外的名字本文未逐个核实。

运行命令（在已有构建目录里）：

```bash
# 只跑监控
ctest --test-dir .build-p0-asan -R 'go2cpp_os_(monitor|kernel|tcp_io)' --output-on-failure
# 指标
ctest --test-dir .build-p0-asan -R 'go2cpp_metrics' --output-on-failure
# debug
ctest --test-dir .build-p0-asan -R 'go2cpp_debug' --output-on-failure
# 单独看某个用例
ctest --test-dir .build-p0-asan -R 'go2cpp_os_monitor_ebpf_nonroot' -V
```

注意：`*_root` 用例会执行 `echo root | sudo -S`，CI 里没有免密 sudo 时会记 SKIP
（`SKIP_RETURN_CODE 77`），**SKIP 不等于通过**。
