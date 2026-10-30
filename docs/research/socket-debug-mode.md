# Socket Debug 模式调研（任务 1.2）

> 状态：**调研 + 设计**，不含实现、不改动任何源码、不 commit。
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`，`HEAD e7f3ec4`（`git status` 干净）。
> 任务来源：`docs/modules/bento-roadmap.md:17-22`（接口 `SocketDebug::Enable/Snapshot/Events/DumpTo(debug::View&)`，
> 关闭近零、采样可配、接入现有 debug 10 视图 / 事件环、验证项为开销对比与事件完整性）。
> 阅读约定：本文所有结论都给出文件与行号；凡属推测的结论都显式标注「推断」。

---

## 0. 结论速览

1. **推荐方案 D（A+B 混合）**：热路径做**每 socket 计数**（`SocketDebug` 自持注册表），**事件走既有事件环的资源事件通道**（不新建缓冲、不新建服务），**视图走既有 `debug::View` 管线**（新增 `socket` 组件与第 11 个视图名 `sockets`）。
2. **接口**：`SocketDebug::Enable / Disable / Enabled / Snapshot / Events / DumpTo(debug::View&)`，外加 4 个仅供 `socket.cpp` 调用的内部记账函数（`Register / Unregister / RecordSend / RecordRecv`）。
3. **关闭近零**：编译期 `GO2CPP_DEBUG_SOCKET=OFF`（默认）时全部宏空展开、零代码；编译开但运行时 `Disable()` 时，热路径只多 **1 次 relaxed 原子读 + 1 个分支**（与现有 `DebugConfig::Enabled` 同量级）。
4. **开销目标**（待 §4 验证）：计数档 <3%（TCP 回环）/ <5%（UDP）；事件档（1/64 采样）<8%；追踪档 <30% 且仅用于排障。**历史教训**：单事件 19.65–22.47 ns（`docs/debug-reliability.md:157`）不能外推成端到端开销，必须做整机压测（`docs/debug-overhead.md:60-61`）。
5. **三条必须先解决的工程约束**（否则实现一定返工）：
   - 依赖方向：`network` **不能**直接包含 `debug_dump.hpp`（它 include 了 channel/fiber/io/scheduler/sync）。需要照 `go2cpp_debug_event` 的先例（`cmake/Go2CppModules.cmake:213-221`）拆一个零依赖目标。
   - **不要把每个 socket 注册进资源注册表**：`kMaxResourceEntries = 4096` 且**含墓碑**，满了会批量淘汰最旧的死条目、全活时拒绝注册（`src/debug/debug_event_ring.cpp:1405-1407`）。长跑 10k 连接必然撞墙。
   - **socket 事件必须跳过栈/链捕获**：`RecordResourceEvent` 无条件尝试 `CaptureStackSampleForResource`（`src/debug/debug_event_ring.cpp:2211-2217`），而 socket 收发点不是 park 点，栈样本既贵又无用。

---

## 1. 现状

### 1.1 base debug 模块：10 视图 / 事件环 / trace / replay

| 能力 | 实现位置 | 现状要点（已核对源码） |
|---|---|---|
| 事件环 | `include/go2cpp/debug/debug_event_ring.hpp` | 每线程两段环形缓冲：普通区 256 + 关键区 64（`:108-116`、`:535-549`）；`EventRecord` 固定 40 字段（`:438-460`）；`seq` 自 v6 起为**每线程自增**，全序由 `(ts_ns, thread_id, seq)` 定义（`:18-26`）；读写是 best-effort，撕裂记录靠二次校验丢弃（`:77-86`）。 |
| 关闭零开销 | `:656-737` | `GO2CPP_DEBUG_DUMP=OFF` 时 `GO2CPP_DEBUG_EVENT/_SAMPLED/_TIMER_EVENT/_PARK_HINT` 全部展开为 `((void)0)`，参数**不参与求值**。 |
| 粒度开关 | `include/go2cpp/debug/debug_config.hpp:67-141` | 6 个事件类别（lifecycle/sched/park/primitives/timer/io）+ stacks/chains + `sample_shift`；`GO2CPP_DEBUG_CATEGORIES` 白名单语义；`Enabled(kNone)` 恒 true（Doctor 事件不受控）。 |
| 资源历史 | `include/go2cpp/debug/debug_resources.hpp` | `ResourceId` 进程内单调不复用、析构后可查（墓碑，`:45-47`）；资源表上限 4096 含墓碑；10 类并发结构里 **8 类已插桩**（`debug_dump.cpp:2677-2701`）。 |
| 资源事件 | `debug_resources.hpp:340-374`、`:460-604` | `ResourceEventRecord` 除 code/result/waiters 外已有 v5 扩展字段 `fd / bytes / aux`；宏 `GO2CPP_DEBUG_RESOURCE_EVENT_V5(...)` 在**构造 112 B 结构体之前**做类别与采样判定。 |
| 10 视图 | `src/debug/debug_dump.cpp:2707-2712` | `gmp / timeline / sched / park / timer / primitives / history / locks / channels / resources`；`VIEWS` 帮助只列 6 个（`debug_server.cpp:1028-1034`），是「帮助比实现保守」。 |
| DebugServer | `src/debug/debug_server.cpp` | `/tmp/go2cpp-<pid>.sock`、0600、一连接一命令；`PING/LIST/STATUS/DUMP/TEXT/JSON/EVENTS/VIEW/VIEWS/SUBSCRIBE/HELP`；`SUBSCRIBE events` 用 `ReadSince(EventCursor)` 增量推送、慢客户端丢批并计数（`debug_server.hpp:29-61`）。 |
| dump 契约 | `include/go2cpp/debug/debug_dump.hpp:60-84`、`:147-176` | 组件名是稳定常量（scheduler/fiber/channel/parking_condition/mutex/io/events/runtime/timer）；`DumpPayload` 带 `available/reason`；一致性探针是**提示**不是事务。 |
| Doctor | `src/debug/diagnostic_agent.cpp`（3510 行） | 单执行线程命令平面，`status/dump/deadlock-report/force-wake/mutex-unlock/ctx.cancel/inject-fiber/stats`；白名单只碰注册过的对象（`docs/modules/debug.md:244-289`）。 |
| trace | `docs/trace_format.md:1-80` | 文本行格式 `#N INPUT|OUTPUT EVENT k=v` 的**设计稿**（C-L1 阶段）。按 grep 结果仓库内只有**读取端**（`tools/cli/graph.cpp --replay` 支持 `# go2cpp-trace`），**未发现写入端实现**（`grep TraceRecord/TraceWriter` 无命中）。 |
| replay | `tools/replay/**`、`docs/replay-service.md:1-90` | 语义级 record/replay，覆盖 channel/select/timer/ctx；`GO2CPP_REPLAY=ON` 时资源事件在写环前先转发给 `SetResourceReplaySink` 安装的回调（`debug_resources.hpp:63-80`）。**没有 socket 事件模型**。 |

### 1.2 tcp_socket / udp_socket 的现有可观测点

base 层并没有 `TcpSocket`/`UdpSocket` 两个类：TCP/UDP/Unix 共用 `net::Socket`（`include/go2cpp/network/socket.hpp:48-174`，实现 `src/network/socket.cpp`，684 行），UDP 走 `CreateUDP + send_to/recv_from`。bento 层另有一个独立的 `bento::UdpSocket`（`include/go2cpp/bento/net/link.hpp:139`），不属于 base。

| 观测点 | 位置 | 现在能看到什么 | 缺什么 |
|---|---|---|---|
| `Socket` 对象 | `socket.hpp:48-174` | `fd()/family()/type()/protocol()/connected()/last_error()`（`getsockopt(SO_ERROR)`）、收/发超时字段、`cancel_read/cancel_write` 代际 | **没有任何计数、耗时、事件**；`m_inflight` 只在关闭流程内部使用（`socket.cpp:157-180`） |
| 等待入口 `wait_ready` | `socket.cpp:417-478` | 三种策略分派：自定义 waiter / IOManager / 原生 poll；受管路径按 **100 ms 分片**循环 `wait_for`；普通线程同样 100 ms 分片 `poll` | 分片次数、等待耗时、超时/取消/关闭各多少、一次请求要几片——全不可见 |
| 收发循环 | `socket.cpp:488-516` | `sendmsg/recvmsg` 返回 `EAGAIN` 后进入等待重试；`EINTR` 继续；其余 errno 直接返回 | 每次调用的 bytes/errno/重试次数/是否 partial 均不记录 |
| accept / connect | `socket.cpp:269-329` | `accept4(SOCK_CLOEXEC|SOCK_NONBLOCK)`，对新 fd 调 `hook::mark_system_nonblocking`；connect 走 `EINPROGRESS` + 可写等待 + `SO_ERROR` | backlog 空转次数、握手耗时、失败 errno 分布不可见 |
| `FdManager` | `fd_manager.hpp:26-68` | fd → `FdContext`（initialized/closed/user_nonblock/system_nonblock/recv_timeout/send_timeout），带互斥；getter 无同步保护（头文件已警告） | 无 IO 计数、无归属、无 socket 语义 |
| `IOManager::wait` | `src/network/io.cpp:2243-2389` | 入口记 `kIoWait`（gid=0，arg2=fd，`io.cpp:2247`）；`GO2CPP_DEBUG_RESOURCES` 下再记 `kIoWait(blocked, waiters=depth)` 与 `kIoRead/kIoWrite(result)` 资源事件 | **bytes 恒为 0**（注释明说真实字节数在 socket 层，`io.cpp:2373-2376`）；等待没有归属到逻辑 socket |
| `IOManager::DebugSnapshot` | `include/go2cpp/network/io.hpp:194-208`、`io.cpp:2902` | 13 个字段：fd_slots / slots_with_waiters / epoll_registered / reader_waiters / writer_waiters / pending_read / pending_write / timers / scheduler_runnable / scheduler_workers 等 | 只有聚合量，没有 per-socket；`epoll_registered` 不等于连接数 |
| `io` 组件 | `debug_dump.cpp:548-597` | 把上表 13 字段以 `schema=go2cpp.dump.io/1` 输出，`consistent=per_call` | 同样无 socket 粒度 |
| Hook 错误计数 | `include/go2cpp/network/hook.hpp:125-136`、`hook.cpp:964-980/1644-1658` | 5 个进程级计数：`io_timeouts / connect_timeouts / eintr_retries / eintr_visible / ioctl_passthroughs`；`relaxed` 读 | **没有接入任何视图/CLI/MCP**（只在 `tests/network/test_hook_p0.cpp` 里断言）；也不区分具体 socket |
| bento `LinkStats` | `bento/net/link.hpp`（`LinkStats` 结构） | 链路层 frames/bytes/errors/handshake/retransmit 等 | 是上层协议栈的统计，**不能**当 base socket 的观测点 |

### 1.3 已有调试手段（能力 / 成本 / 局限）

| 手段 | 能看到 | 成本与前提 | 局限（与 socket 相关） |
|---|---|---|---|
| **strace** | syscall 次数与时序。项目里已有大量实证：`epoll_ctl 10.3/请求`、`futex 9.3`、`epoll_wait 2.1`（`docs/AGENT-HANDOFF.md:215-224`）；`ADD/DEL/MOD` 分类（`docs/gmp-optimization-log.md:1157-1197`） | 需 ptrace 权限、**要重启进程或 attach**；每次 syscall 陷入，绝对性能不可比；WSL2 可用但容器里常被 seccomp 限制 | 只有 syscall 视角：知道「epoll_ctl 10.3 次」但**不知道哪 10 次是 100 ms 分片造成的**；fd 复用后无法归属到逻辑连接；看不到 `EAGAIN → park` 的应用语义 |
| **eBPF monitor（已有）** | `bpf/` 下 6 个程序：`net_stats / syscall_latency / sched_latency / futex_latency / tcp_monitor / fileio_monitor`；`TcpMonitor` 给重传数与 srtt 的 p50/p99（`tcp_monitor.hpp:1-32`）、`NetEbpfMonitor` 给每网卡字节/包（`net_ebpf_monitor.hpp:1-15`） | **需要 root 与内核 BTF**（`docs/modules/os.md:330-340`）；用户态 1 Hz 读 map，TCP 每次 `UpdateOnce` 读 1 条全局 + 至多 4096 条连接（每条 288 B），耗时 0.2–0.5 ms（`tcp_monitor.hpp:30-32`） | 零侵入但只有**内核视角**：没有 go2cpp 的 park/取消/100 ms 分片语义；fd 复用导致归属歧义；短连接可能来不及进 map；非 root 环境不可用；`tcp_probe` 在部分裁剪内核缺失（头文件已写降级路径） |
| **DebugServer / Doctor** | 10 视图、事件流订阅、死锁判定、受控唤醒 | 只在 `GO2CPP_DEBUG_DUMP=ON` 构建；Doctor 另需 `GO2CPP_DIAGNOSTIC=ON` | **没有 socket 组件、没有 socket 视图**；`kIoWait` 只有 fd，`kIoRead/kIoWrite` 的 bytes 恒 0 |
| 日志 / gdb | 断点、单步、变量 | gdb 会打断时序；日志需要预先埋点 | 没有 socket 级埋点，只能看调用栈 |

### 1.4 盲区（每条都能被现有事实回答「为什么现在做不到」）

- **G1 连接账本缺失**：`Socket` 无计数，进程内无法回答「当前多少 socket、按 type/family 各多少、峰值多少、close 时还有几个在途操作」。
- **G2 字节与调用账本缺失**：`kIoRead/kIoWrite` 的 bytes 恒 0（`io.cpp:2373-2376`），所以「端到端收发字节/调用数/partial 写次数」在调试面完全空白。
- **G3 EAGAIN 与超时不可区分**：只有 hook 的 `io_timeouts` 能区分「内核缓冲空」和 `SO_RCVTIMEO` 到期（`hook.hpp:115-118`），但它没接视图，也只到进程级。
- **G4 100 ms 分片不可数**：`wait_ready` 的分片循环（`socket.cpp:444-459`）正是 `epoll_ctl 10.3/请求` 的头号嫌疑（`AGENT-HANDOFF.md:228-234`），但分片次数**没有任何计数器**，每次只能重跑 strace 反推。
- **G5 归属丢失**：`kIoWait` 的 arg2 是数字 fd，fd 关闭即复用；没有稳定 id，事件流无法把「等待 → 唤醒 → 超时」串回同一个逻辑连接。
- **G6 关闭期不可观测**：`release/close` 有 `m_closing / m_close_pending / m_inflight / m_generation` 一套状态机（`socket.cpp:129-180`、`:225-256`），但对外只有 `bool` 返回值，「延迟关闭是否发生、在途操作几个」不可见。
- **G7 第三方 fd 无覆盖**：hook 接管的、`socketpair` 的、用户自建 fd 不走 `Socket`，任何「只改 Socket」的方案都覆盖不到——这是方案 C 存在的理由，也是方案 D 必须显式声明 coverage 缺口的原因（与 `resources` 视图的 `coverage` 段同一风格）。

---

## 2. 方案对比

### A. 自研 SocketDebug（计数 / 事件 / 快照）

- **形态**：`SocketDebug` 单例 + 每 socket 一个 `SocketStats`（1–2 条 cacheline 的原子计数）+ 全局注册表（`shared_mutex` + `unordered_map<id, entry>`）+ 自建事件环形缓冲；`Enable/Snapshot/Events/DumpTo` 全自持。
- **优点**：语义最准（应用层字节/成败/超时/取消/分片），能归属稳定 id，close 时能输出终态快照（排障最值钱），不依赖 root/内核，与 `DebugConfig` 风格一致。
- **缺点**：**新造一套事件缓冲**，与既有事件环/订阅/replay/视图管线并行，重复实现订阅游标、丢弃计数、文本/JSON 契约；热路径侵入最多；自建 ring 的生命周期与 `Socket` 的 `m_inflight_keepalive` 交叉，容易踩悬垂。
- **代价**：`SocketDebug` 约 600–900 行 + 事件环约 300 行 + 测试/基准约 500 行。

### B. 复用 debug 事件环（socket 事件注入）

- **形态**：在 `accept/connect/send/recv/wait/timeout/close` 处发资源事件（复用 `GO2CPP_DEBUG_RESOURCE_EVENT_V5` 的 `fd/bytes/aux` 字段）；视图侧新增 `socket` 组件 + `sockets` 视图；`GO2CPP_DEBUG_CATEGORIES` 增加 `socket` 类别。
- **优点**：零新基础设施；自动获得 per-thread ring、`(ts,thread,seq)` 全序、`TotalCount`、`ReadSince` 游标、`SUBSCRIBE`、DebugServer/Doctor/MCP 通路、replay sink 转发；开销模型已有实测（单事件 ~20 ns；全开 4–40×，按类别白名单可关）。
- **缺点**：事件环只保留**最近窗口**（每线程 320 条），长跑只能看累计计数（每码一个），做不了 per-socket 累计与分位数；IO 事件高频，`kIoWait` 未采样就能把窗口刷满；`EventRecord` 是固定字段，socket 语义要靠「字段复用约定」（易错、难 review）；**没有注册表**，`socket_list/socket_close`（任务 1.3）无处落地。
- **代价**：约 200 行插桩 + 视图 ~250 行；但「每 socket 账本」这个需求它根本满足不了。

### C. eBPF 无侵入（monitor 已有）

- **形态**：新增 `socket_monitor.bpf.c`（`sys_enter/exit` 的 sendto/recvfrom/sendmsg/recvmsg、`tcp_connect/tcp_close` tracepoint 或 `tcp_sendmsg` kprobe），用户态 `SocketMonitor : Monitor` 挂到 `os::monitor::Manager`，输出 per-fd/per-四元组计数与延迟。
- **优点**：**零代码侵入、零重编译**，覆盖所有进程（含非 go2cpp）；内核真值（重传/RTT/队列/丢包）。
- **缺点**：需 root + BTF（`docs/modules/os.md:330-340`）；只有 fd/四元组视角，**无法关联 `Socket` 对象、park 原因、取消语义、100 ms 分片**；fd 复用导致归属歧义；短连接可能在 map 更新前就消失；`tcp_probe` 有内核裁剪风险；与 `TcpMonitor` 的 4096 连接上限/直方图口径要重新设计，容易做出第二套「RTT 口径争议」。
- **代价**：BPF 程序 ~250 行 + 用户态 ~500 行 + root 环境的 CI 跳过逻辑；**不满足**「socket 级语义 + 1.3 MCP」的目标。

### D. 混合（A+B）——把「账本」和「时间线」分开

- **账本（A）**：`SocketDebug` 自持**有界注册表**（默认 8192，可配），每 socket 一条 `SocketStats`（原子计数）；`Snapshot` 返回终态；关闭后归档最近 N 条（默认 1024），供「事后看这个连接发生了什么」。**不碰资源注册表**（4096 含墓碑的坑）。
- **时间线（B）**：事件不改自建缓冲，而是写**既有事件环的资源事件通道**；`socket_id` 用高位命名空间编码进 `resource_id`（`kSocketIdTag = 1ull<<63`），避免与 `ResourceId` 冲突、避免 `history <id>` 串味；类别用新 `kSocket` 位，默认 1/64 采样，`timeout/cancel/close/error` **全量**。
- **视图（B）**：新增 `kComponentSocket = "socket"` 组件 + 第 11 个视图名 `sockets`；`DumpTo(debug::View&)` 同时被 `DUMP socket` 与 `VIEW sockets` 复用。
- **为什么不选纯 A**：重造 ring 会得到第二套订阅/丢弃/契约实现，与 `SUBSCRIBE`、replay、MCP 全部要再对接一遍。
- **为什么不选纯 B**：事件环没有「每 socket 累计」，长跑 5 分钟后所有细节被覆盖；也没有注册表去支撑 `socket_list/close`。
- **为什么不选纯 C**：只有 root 能用，且答不出「是不是 100 ms 分片造成的」这类项目当前最痛的问题（`AGENT-HANDOFF.md:228-234`）。

### 2.5 横向对比

| 维度 | A 自研 | B 复用事件环 | C eBPF | **D 混合（推荐）** |
|---|---|---|---|---|
| 语义准确度（应用层 bytes/超时/取消/分片） | 高 | 中（受固定字段限制） | 低（只有内核视角） | **高** |
| 每 socket 账本 / 生命周期 | 有 | **无** | 部分（fd/四元组） | **有** |
| 时间线 / 因果（park→wake→超时） | 自建 | 复用 | 无 | **复用** |
| 关闭态开销 | 0（编译期删） | 0（宏） | 0 | **0（宏）** |
| 开启态开销 | 最高（自建 ring） | 中（20–60 ns/事件） | 低（内核内） | **中（计数 ~10–25 ns）** |
| 侵入性 | 高 | 中（8–10 个插桩点） | 零 | **中（同 B + 计数）** |
| 依赖 | 无 | debug_event（零依赖） | root + BTF + libbpf | **debug_event（零依赖）** |
| 覆盖第三方 fd | 否 | 否 | **是** | 否（需 coverage 声明） |
| 能否支撑任务 1.3（`socket_list/stats/events/close`） | **能** | 否（无注册表） | 部分（需 root） | **能** |
| 工程成本 | 高（~1500 行） | 中（~450 行） | 高（~750 行 + root CI） | 中（~900 行） |

### 2.6 复用评估（避免重复造轮子）

| 现成能力 | 能否直接复用 | 结论 |
|---|---|---|
| `go2cpp::metrics`（Counter/Gauge/Histogram + Prometheus 文本，`include/go2cpp/metrics/metrics.hpp`） | 可复用**聚合层**：把 `SocketDebug` 的全局计数导出为 `go2cpp_socket_*_total`；**不要**按 socket_id 做标签（基数爆炸） | 采纳为可选导出，不作为主接口 |
| `go2cpp_debug_event` 零依赖目标 | 直接参照其拆分方式（`cmake/Go2CppModules.cmake:213-221`） | 采纳，新增 `go2cpp_debug_socket` |
| `ResourceEventRecord` 的 `fd/bytes/aux` 字段 | 直接复用（`debug_resources.hpp:340-374`） | 采纳 |
| `DebugServer` 组件注册 + `VIEW` 管线 | 直接复用（`Register()` / `BuildViewJson`） | 采纳，需扩签名 |
| Linux `inet_diag` / `ss -tin` 的 `bytes_sent/bytes_received/retrans/rtt` | 可作为**可选增强**：snapshot 里 join 一次 netlink 查询即可拿到内核侧账本，零 eBPF | 建议 phase 2（能补上「内核视角」但不需要 root 的缺口） |
| BCC 的 `tcplife/tcpconnect/tcpaccept/tcpretrans` 事件语义 | 可对齐**命名与字段**（accept/connect/close/bytes），降低使用者学习成本；不引入 bcc 依赖 | 采纳为命名参考 |
| `tools/replay` 的语义重放 | 事件模型无 socket 类型（`docs/replay-service.md:38-58`） | **不纳入** phase 1；sink 侧按 code 过滤，避免污染 replay 文件格式 |

---

## 3. 推荐方案与接口草案

### 3.1 分层结构

```text
socket.cpp / socket.hpp (热路径)
   │  GO2CPP_SOCKET_* 宏（OFF 时零求值）
   ▼
SocketDebug（go2cpp_debug_socket，零依赖：只 <atomic>/<cstdint>/<mutex>）
   ├── 有界注册表：socket_id → SocketStats（1–2 cacheline/条）
   ├── 计数：accept/connect/send/recv/wait/超时/取消/错误/分片
   ├── 终态归档：最近 N 条 Snapshot（含 local/remote/时长/错误）
   └── 事件：GO2CPP_DEBUG_RESOURCE_EVENT_V5 → go2cpp_debug_event（既有事件环）
                                   │
                                   ▼
debug（go2cpp_debug）：DumpSocket(SocketDebug) → DumpPayload
   ├── DUMP socket / JSON socket          （组件）
   ├── VIEW sockets [n]                   （第 11 个视图）
   └── socket_events（过滤 resource_id 高位置位的事件）
                                   │
                                   ▼
DebugServer socket / go2cpp-cli / Doctor / MCP（1.3）
```

### 3.2 接口草案（C++17，命名与现有风格一致）

```cpp
// include/go2cpp/network/socket_debug.hpp —— 热路径头，零 debug 依赖
namespace go2cpp::net {

enum class SocketDebugMode : std::uint8_t {
    kCounters = 0,  // 只记计数（默认）
    kEvents   = 1,  // 计数 + 资源事件（采样）
    kTrace    = 2,  // 计数 + 事件 + 每次操作耗时直方图
};

struct SocketDebugConfig {
    SocketDebugMode mode{SocketDebugMode::kCounters};
    std::uint32_t event_sample{64};   // 1=全量；只稀释 ok/would_block
    std::size_t   max_sockets{8192};  // 注册表上限；超出计入 dropped
    std::size_t   max_finished{1024}; // 已关闭 socket 的归档条数
    bool          capture_latency{false}; // kTrace 才为 true
};

struct SocketCounters {              // 计数集中在一组原子量里
    std::uint64_t accept_ok{0}, accept_eagain{0};
    std::uint64_t connect_ok{0}, connect_fail{0}, connect_timeout{0};
    std::uint64_t send_calls{0},  send_bytes{0},  send_eagain{0};
    std::uint64_t send_partial{0}, send_errors{0};
    std::uint64_t recv_calls{0},  recv_bytes{0},  recv_eagain{0};
    std::uint64_t recv_eof{0},    recv_errors{0};
    std::uint64_t wait_calls{0},  wait_ready{0},  wait_timeout{0};
    std::uint64_t wait_cancel{0}, wait_closed{0}, wait_slices{0}; // 100ms 片数
    std::uint64_t shutdown_calls{0}, close_calls{0};
    std::uint64_t inflight_peak{0};
    std::int32_t  last_errno{0};
};

struct SocketSnapshot {              // 一条连接的全量账本
    std::uint64_t socket_id{0};      // 稳定 id，单调不复用；0=无效
    std::int32_t  fd{-1};
    std::int32_t  family{0}, type{0}, protocol{0};
    std::uint32_t state{0};          // created/listening/connected/closing/closed
    char          local[64]{}, remote[64]{};  // 文本端点（与 ss 输出对齐）
    std::uint64_t created_ns{0}, closed_ns{0}, last_activity_ns{0};
    SocketCounters counters{};
    std::uint32_t flags{0};          // graceful/aborted/released/deferred-close
};

struct SocketEvent {                 // 由事件环记录反序列化而来
    std::uint64_t seq{0}, ts_ns{0}, socket_id{0};
    std::uint32_t kind{0};           // accept/connect/send/recv/wait/timeout/close
    std::uint32_t result{0};         // 复用 ResourceResult 数值
    std::int32_t  fd{-1};
    std::uint32_t bytes{0};
    std::int32_t  sys_errno{0};
    std::uint32_t flags{0};          // 方向/partial/eof/sampled
};

class SocketDebug final {
public:
    // ---- 开关（编译期 OFF 时 Enable 返回 false，reason=socket-debug-disabled）
    static bool Enable(const SocketDebugConfig& config = {});
    static void Disable() noexcept;
    static bool Enabled() noexcept;              // 热路径：1 次 relaxed load
    static SocketDebugConfig Config() noexcept;
    static const char* LastError() noexcept;

    // ---- 读取（dump 路径，允许分配）
    static std::size_t Snapshot(std::vector<SocketSnapshot>* out,
                                bool include_closed = false);
    static std::size_t Events(std::uint64_t after_seq,
                              std::vector<SocketEvent>* out,
                              std::size_t max = 0);
    static bool DumpTo(debug::View& view);       // false=不可用，已写 Unavailable

    // ---- 内部 API：只允许 socket.cpp 调用（宏包装，OFF 时为空）
    static std::uint64_t Register(void* owner, int fd, int family, int type,
                                  int protocol) noexcept;
    static void Unregister(std::uint64_t id, std::uint32_t close_flags) noexcept;
    static void RecordSend(std::uint64_t id, int fd, std::uint32_t bytes,
                           int sys_errno, bool partial) noexcept;
    static void RecordRecv(std::uint64_t id, int fd, std::uint32_t bytes,
                           int sys_errno, bool eof) noexcept;
    static void RecordWait(std::uint64_t id, int fd, std::int32_t result,
                           std::uint32_t slices) noexcept;
};

}  // namespace go2cpp::net
```

`debug::View` 的最小契约（放在零依赖头 `include/go2cpp/debug/debug_view.hpp`，只 `<string_view>`/`<cstdint>`）：

```cpp
namespace go2cpp::debug {
class View {                       // text 与 JSON 两种适配器都实现它
public:
    virtual ~View() = default;
    virtual void Key(std::string_view key, std::uint64_t value) = 0;
    virtual void Key(std::string_view key, std::string_view value) = 0;
    virtual void Array(std::string_view name) = 0;   // 进入数组
    virtual void Object() = 0;                       // 数组内一个元素
    virtual void End() = 0;                          // 关闭 Object/Array
    virtual void Unavailable(std::string_view reason) = 0;
};
}  // namespace go2cpp::debug
```

> 依赖说明：这个头**只有纯虚接口、零符号依赖**，`go2cpp_network` 包含它不会建立对 `go2cpp_debug` 的链接依赖；`debug_dump.cpp` 里实现 `TextDumpView` 与 `JsonView` 两个适配器。若 review 不接受「框架包含 `go2cpp/debug/*`」，退化为 `debug::DumpSocket(SocketDebug&, View&)`（放在 debug 侧），`SocketDebug` 只保留 `Enable/Snapshot/Events`。

### 3.3 字段字典（口径必须写死在文档里，否则实现会各写一套）

| 字段 | 口径 | 为什么容易错 |
|---|---|---|
| `send_calls` / `recv_calls` | `Socket::send/sendv/send_to` 的**每次调用**计 1，与 syscall 次数解耦（partial 写会在同一调用里多次 `sendmsg`） | 直接按 syscall 计数会与「用户看到的调用」不一致 |
| `send_bytes` | 成功返回的字节数累加（partial 只累加实际写入） | 与 `kIoRead/kIoWrite.bytes` 的 0 口径不同，不能混用 |
| `send_eagain` | 每次 `errno==EAGAIN/EWOULDBLOCK` 计 1（**含**随后成功的那次） | 与 `wait_calls` 不是一一对应（分片循环里一次 EAGAIN 可能对应多片 wait） |
| `wait_slices` | `wait_ready` 里每进入一次 100 ms 分片计 1（`socket.cpp:444-459` 的循环体） | 这正是 `epoll_ctl 10.3/请求` 的解释变量，必须单列 |
| `wait_timeout` | `wait_ready` 因总超时返回 `ETIMEDOUT` 的次数 | 与 hook 的 `io_timeouts`（`SO_RCVTIMEO` 触发）**口径不同**，两者可交叉验证但不可相加 |
| `recv_eof` | `recvmsg` 返回 0（对端关闭）的次数 | 与 `recv_calls` 互斥，回答「连接是正常关闭还是异常」 |
| `inflight_peak` | `m_inflight` 的观测峰值（`socket.cpp:165`） | 只在调试档维护，避免热路径 CAS 循环 |
| `socket_id` | 进程内单调、**不复用**；关闭后仍可在归档窗口查到 | 复用 fd 不能当 id |
| `flags` | `graceful / aborted / released / deferred-close`（对应 `release`/`close_pending` 两条路径） | G6 的可观测化落点 |

### 3.4 插桩点（8 处，全部在 `src/network/socket.cpp`）

| 函数 | 行号（当前 HEAD） | 记什么 | 档位 |
|---|---|---|---|
| `Socket::accept` | `:269-302` | `accept_ok/accept_eagain`、新 socket 注册（`Register`） | 计数 |
| `Socket::connect` | `:303-329` | `connect_ok/connect_fail/connect_timeout`、`SO_ERROR` 的 errno | 计数 |
| `Socket::wait_ready` | `:417-478` | `wait_calls/wait_ready/wait_timeout/wait_cancel/wait_closed/wait_slices` | 计数 |
| `Socket::send_impl` | `:488-501` | `send_calls/send_bytes/send_eagain/send_partial/send_errors` | 计数 |
| `Socket::recv_impl` | `:503-516` | `recv_calls/recv_bytes/recv_eagain/recv_eof/recv_errors` | 计数 |
| `Socket::release` | `:225-256` | `flags=released`、`inflight` 快照、归档 | 计数 |
| `Socket::close` / `finalize_deferred_close` | `:330-370` 区间 | `close_calls`、`flags=deferred-close`、归档 | 计数 |
| `Socket::shutdown` | `socket.cpp` 内 `shutdown` 实现 | `shutdown_calls` | 计数 |

事件（`kEvents`/`kTrace` 档）只在这 8 处的**终态**发一条：`accept / connect / send / recv / wait / timeout / cancel / close`，共 8 个事件码。

### 3.5 关闭近零的实现

```cpp
// 编译期：GO2CPP_DEBUG_SOCKET=OFF（默认）→ 宏空展开、Socket 不增加字段
#if defined(GO2CPP_DEBUG_SOCKET) && GO2CPP_DEBUG_SOCKET
#define GO2CPP_SOCKET_STAT(op)  op
#else
#define GO2CPP_SOCKET_STAT(op)  ((void)0)
#endif

// 运行期：编译开但 Disable() 时，只多一次 relaxed load + 分支
inline bool SocketDebug::Enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}
```

- `Socket` 里只加一个 `#if GO2CPP_DEBUG_SOCKET` 保护的 `SocketStats* m_stats{nullptr}`，所以 **OFF 构建 `sizeof(Socket)` 与 HEAD 完全相同**（可作为回归断言）。
- 计数用 `fetch_add(1, relaxed)` / `fetch_add(bytes, relaxed)`；每 socket 的计数集中在 1 条 cacheline，跨线程写同一 socket 才会 false sharing（同 socket 多线程收发本来就少）。
- `now()` 只在 `kTrace` 档取；`kCounters` 档不取时钟（这是与事件环最大的成本差异）。
- 事件走 `GO2CPP_DEBUG_RESOURCE_EVENT_V5`，宏在**构造 112 B 结构体之前**做类别+采样判定（`debug_resources.hpp:376-393`），所以 63/64 的调用只花一次线程局部分支。
- **跳过栈/链捕获**：`RecordResourceEvent` 默认会 `CaptureStackSampleForResource`（`debug_event_ring.cpp:2211-2217`）；socket 事件必须走新增的 `RecordSocketEvent`（复用写环代码但不采栈），否则 `kTrace` 档会变成 backtrace 档。

### 3.6 依赖方向与 CMake

```cmake
# 新增零依赖目标（照 go2cpp_debug_event 的先例，Go2CppModules.cmake:213-221）
add_library(go2cpp_debug_socket src/network/socket_debug.cpp)
target_link_libraries(go2cpp_debug_socket PUBLIC go2cpp_debug_event)  # 只为 EventRing
# network 仅在开关打开时链接它，避免默认构建多一个依赖
if (GO2CPP_DEBUG_SOCKET)
    target_link_libraries(go2cpp_network PRIVATE go2cpp_debug_socket)
endif()
```

- `debug -> {network, debug_event}` 方向不变；`network -> debug_socket -> debug_event` 不构成环（与 `scheduler/io -> debug_event` 同构）。
- `debug_dump.cpp` 负责把 `SocketDebug` 的 snapshot 转成 `DumpPayload`；`SocketDebug` 本身不 include 任何 `debug_dump.hpp`。
- 新 CMake 选项：`GO2CPP_DEBUG_SOCKET`（默认 OFF），并在 `run_debug_overhead.sh` 的 `defs_for` 里同步读取（脚本已从 `CMakeCache.txt` 提取宏，`tests/bench/run_debug_overhead.sh:100+`）。

### 3.7 接入现有 10 视图 / 事件环 / MCP

1. **事件码**：`SchedEventCode` 追加 `140..147`（当前最大 139，`debug_event_ring.hpp:277-280`），8 个 socket 事件；`DebugCategoryOf` 映射到新类别 `kSocket`。
2. **类别**：`DebugCategory` 新增 `kSocket = 1U << 8`，`kAll` 从 `(1<<8)-1` 改为 `(1<<9)-1`（`debug_config.hpp:67-79`），`DebugConfig` 增加 `bool socket`，`ParseCategories` 接受 `socket/sock` 别名。**这是 ABI 变更**（`DebugConfig` 结构体大小变化），需在变更记录里写明；只影响 debug 构建。
3. **组件**：`debug_dump.hpp` 增加 `kComponentSocket = "socket"`（`:60-68` 一组稳定常量），`DumpSocket(SocketDebug&)` 产出 `schema=go2cpp.dump.socket/1`，字段：`tracked/alive/finished/dropped/peak/...` + `socket.<i>.*` 数组（沿用 text 的 `<name>.<index>.<field>` 约定）。
4. **视图**：`IsKnownViewName` 加 `sockets`（`debug_dump.cpp:2707-2712`）；`BuildViewJson` 当前签名固定 3 个 JSON 输入（`debug_dump.hpp:248`、`debug_server.cpp:1057-1064`），需加第 4 个 `socket_json`；`VIEW sockets [n]` 输出 top-N（按 bytes/errors）+ 聚合分布 + `coverage`（明确列出「hook 直调 fd 与 bento UdpSocket 不在覆盖内」，风格对齐 `resources` 视图的 coverage 段）。
5. **事件订阅**：`socket` 事件自动进 `SUBSCRIBE events`（同一个 `ReadSince` 游标），客户端按 `code` 过滤即可，无需新协议。
6. **Prometheus**：可选 `SocketDebug::RenderMetrics()` 注册到 `metrics::Registry`（聚合值，不加 socket_id 标签），由 `go2cpp-cli`/MCP 暴露。
7. **MCP（任务 1.3）**：`socket_list` → `Snapshot()`；`socket_stats` → 聚合；`socket_events` → `Events(after_seq)`；`socket_close` → 白名单 + 审计（复用 Doctor 的「只碰注册过的对象」原则，`diagnostic_agent.hpp:54-71`），**不接受裸 fd**。

### 3.8 开销预算（目标值，§4 验证后才能写进结论）

| 档位 | 每次 send/recv 额外动作 | 目标端到端开销 | 说明 |
|---|---|---|---|
| 编译期 OFF（默认） | 0（宏空展开） | **0%** | `sizeof(Socket)` 不变 |
| 编译 ON + `Disable()` | 1 relaxed load + 1 分支 | **<0.5%** | 与 `DebugConfig::Enabled` 同量级 |
| `kCounters` | 2–4 次 relaxed `fetch_add` | **<3%**（TCP 回环）/ **<5%**（UDP 小包） | 不取时钟 |
| `kEvents`（1/64） | 上述 + 每 64 次一次资源事件（约 20–25 ns） | **<8%** | timeout/close/error 全量 |
| `kTrace` | 上述 + 2 次 `now()` + 直方图 | **<30%** | 只用于排障，不作生产档 |

内存预算：每 socket 2 条 cacheline（128 B）+ 注册表节点约 64 B；8192 个 socket ≈ 1.5 MB；归档 1024 条 × 约 120 B ≈ 120 KB。事件侧**不新增缓冲**（复用事件环）。

### 3.9 风险与开放问题

| # | 风险 | 处置 |
|---:|---|---|
| R1 | `network` 包含 `debug/debug_view.hpp` 被质疑破坏「框架不依赖 debug」 | 该头零符号依赖；退化为 debug 侧 `DumpSocket(SocketDebug&, View&)`；在 `zero-overhead-verification` 的 grep 检查里加白名单并说明 |
| R2 | 资源表 4096 含墓碑，长跑被淘汰 | **不注册** socket 到资源表；socket_id 用高位命名空间，`history` 不解析 |
| R3 | 事件环窗口被 IO 事件刷满 | socket 事件默认 1/64；`kIoWait` 仍是未采样（现状），文档建议把 io 也纳入采样（另行评估，不在本任务范围） |
| R4 | `replay` sink 收到未知 code | recorder 侧按 code 过滤（检查 `tools/replay` 的 switch 是否有 default）；phase 1 不录制 socket 事件 |
| R5 | 同 socket 多线程收发导致计数竞争 | `relaxed` 原子，弱一致可接受（与事件环 best-effort 一致）；文档写明不是事务 |
| R6 | `Socket` 可能因 `m_inflight_keepalive` 比外部 shared_ptr 活得更久 | 在 `close/release` 归档（不是析构），析构只释放槽位；归档窗口兜底 |
| R7 | `kAll`/`DebugConfig` 变更破坏 ABI | 仅 debug 构建；在 `AI_COLLAB`/模块文档记录；必要时保留旧 `kAll` 别名 |
| R8 | 采样导致事件不完整 | 所有**不变量断言只允许用 counters**；事件只用于时间线与归因（与 v7「采样相位第 1 次必记」一致，`debug_event_ring.hpp:414-416`） |

---

## 4. 验证计划（若采纳）

### 4.1 开销压测（端到端，不用单事件成本外推）

- **基准扩展**：在 `tests/bench/debug_overhead.cpp` 增加 3 个场景（现有 6 个不动）：
  1. `socket_tcp_rr`：回环 TCP，两个 Fiber 通过 `Socket::send/recv` ping-pong，N=200k；
  2. `socket_udp_rr`：回环 UDP `send_to/recv_from`，N=200k，报文 64 B（最坏小包）；
  3. `socket_accept_close`：短连接 churn，50k 次 accept→close（覆盖 G6 与 `accept4` 路径）。
- **配置**：`OFF`（关闭构建）；`ON+Disable`；`ON+Counters`；`ON+Events(1/64)`；`ON+Trace`。同一二进制用环境变量/`Enable()` 切换（参照现有 `ON_FULL`/`ON_SAMPLED` 做法）。
- **门槛（硬性）**：`OFF` 与基线差 <1%；`ON+Counters` <3%（TCP）/ <5%（UDP）；`ON+Events` <8%；`ON+Trace` <30%。测 3 轮取中位数，`loadavg > 2.0` 的轮次标 `valid=0`（脚本已实现，`run_debug_overhead.sh` 头部注释）。
- **必须同时记录**：RSS 增量、`sizeof(Socket)`、每 socket 堆内存（用 `mallinfo2` 或自建计数器）、事件量/op。
- **复用现有驱动**：`tests/bench/run_debug_overhead.sh`（已做 flock 串行化、CMakeCache 宏注入、TSV/Markdown 汇总），只加场景与配置列。

### 4.2 事件完整性与不变量（用 counters 断言，事件只做校验）

1. `send_calls == send_ok + send_eagain终态 + send_errors`（每个调用恰好一个终态；`send_eagain` 是中间态，需再单列 `send_retries`）；
2. `recv_calls == recv_ok + recv_eof + recv_errors`；
3. `wait_calls == wait_ready + wait_timeout + wait_cancel + wait_closed`；
4. 回环单连接：`A.send_bytes == B.recv_bytes + kernel_buffer_remainder`（关闭后 remainder 应可解释）；
5. `accept_ok == 已完成 accept 的 socket 数`，且每个 accepted socket 恰好一条 `close` 归档；
6. 采样开启时：`events(send) × 64 ≈ counters.send_calls`（允许第 1 次必记的相位偏差 ±1）；`timeout/close/error` 事件条数**等于** counters 对应值（关键事件不稀释）；
7. 所有事件的 `socket_id` 必须能在 registry 活条目或归档窗口里解析（否则算 bug）。

### 4.3 长稳

- **2 h 加速**：10k 长连接 + 每分钟 10% 重连 + 随机 RST/超时注入；每 10 s 采一次 `Snapshot()`。
- 断言：`/proc/<pid>/fd` 计数 == registry 活条目数（±监听 fd）；RSS 斜率 < 1 MB/h；registry 不持续增长（`dropped` 有上限且可观测）；`Snapshot()` p99 < 5 ms（8192 条）；事件环 `gaps=0`。
- **24 h 可选**：若 CI 允许，跑一次 `SOAK=24h` 变体，重点看 fd 泄漏与归档淘汰逻辑。

### 4.4 与既有手段交叉验证（证明新观测是真值，不是自证）

- **strace 对照**：5 s 压测同时跑 `strace -f -c -e trace=sendmsg,recvmsg,sendto,recvfrom,accept4,connect,epoll_ctl`，比较 syscall 次数与 counters 的比值（允许 partial 写造成的 sendmsg > send_calls）。
- **解释历史数字**：用 `wait_slices/请求` 去解释 `AGENT-HANDOFF.md:215-224` 的 `epoll_ctl 10.3/请求`。若 `wait_slices ≈ 10`，则该表得到机制级解释（100 ms 分片），并可用于验证 A1 修复（`mark_system_nonblocking`）是否把 `wait_slices` 降到 ~1。
- **eBPF 对照**：root 环境下用现有 `TcpMonitor` 的 `retransmits/rtt` 与 socket 侧 `send/recv` 计数对齐（字节级允许内核缓冲差）。

### 4.5 回归与验收清单

- [ ] `ctest` 全绿（默认构建 + `GO2CPP_DEBUG_SOCKET=ON` 各一遍），0 编译警告；
- [ ] OFF 构建 `nm -C libgo2cpp_network.a | grep -c SocketDebug` == 0；`sizeof(Socket)` 与 HEAD 相同；
- [ ] 现有 6 个 debug 场景（`channel/mutex/spawn/timer/resources/select`）ON/OFF 比值无回归（±5%）；
- [ ] `DUMP socket` / `VIEW sockets` / `SUBSCRIBE events` 三条通路各有 e2e 脚本（参照 `tests/debug/run_diagnostic_e2e.sh` 的断言风格）；
- [ ] `docs/modules/debug.md` §4.3（视图表）、§8（开销）、§10（API 表）同步更新；
- [ ] 变更记录写入 `AI_COLLAB.md`（仅在实际实现时）。

---

## 5. 附录

### 5.1 关键代码位置索引（本次已核对）

| 主题 | 位置 |
|---|---|
| Socket 类定义 / 生命周期状态机 | `include/go2cpp/network/socket.hpp:48-174`、`src/network/socket.cpp:129-180`、`:225-256` |
| 唯一等待入口（100 ms 分片） | `src/network/socket.cpp:417-478` |
| 收发循环（EAGAIN/EINTR） | `src/network/socket.cpp:488-516` |
| IO 等待事件与资源事件 | `src/network/io.cpp:2243-2389` |
| IOManager 调试快照 | `include/go2cpp/network/io.hpp:194-208`、`io.cpp:2902` |
| Hook 错误计数 | `include/go2cpp/network/hook.hpp:125-136`、`src/network/hook.cpp:964-980`、`:1644-1658` |
| 事件环容量/编码/宏 | `include/go2cpp/debug/debug_event_ring.hpp:104-116`、`:163-280`、`:656-737` |
| 资源事件字段与宏 | `include/go2cpp/debug/debug_resources.hpp:340-374`、`:460-604` |
| 资源表上限（4096 含墓碑） | `src/debug/debug_event_ring.cpp:1405-1407` |
| 资源事件写环与采栈 | `src/debug/debug_event_ring.cpp:2173-2218` |
| 10 视图与 `BuildViewJson` | `src/debug/debug_dump.cpp:2707-2795` |
| DebugServer 命令与 VIEW | `src/debug/debug_server.cpp:1028-1064`、`:382-392` |
| 组件名稳定常量 | `include/go2cpp/debug/debug_dump.hpp:60-68` |
| debug_event 零依赖拆分 | `cmake/Go2CppModules.cmake:213-241` |
| 开销基准与驱动 | `tests/bench/debug_overhead.cpp`、`tests/bench/run_debug_overhead.sh` |
| eBPF 采集器 | `bpf/*.bpf.c`、`include/go2cpp/os/monitor/*.hpp` |

### 5.2 与任务 1.3（MCP）的接口映射

| MCP 工具 | 数据来源 | 备注 |
|---|---|---|
| `socket_list` | `SocketDebug::Snapshot(&v, false)` | 输出 id/fd/端点/state/关键计数 |
| `socket_stats` | `Snapshot()` 的聚合 + `SocketDebug::Config()` | 进程级聚合，不做 per-socket 标签 |
| `socket_events` | `SocketDebug::Events(after_seq, &v, n)` | 复用 `SUBSCRIBE` 语义与游标 |
| `socket_close` | 白名单 + `SocketDebug` 记录审计事件 | 只接受注册过的 socket_id，不接受裸 fd |

### 5.3 口径备忘（避免下一轮再吵）

- **`kIoRead/kIoWrite.bytes` 恒 0**，不是 bug 而是分层决定（`io.cpp:2373-2376`）；socket 侧字节数不要试图从事件环拿。
- **`io_timeouts`（hook）≠ `wait_timeout`（socket）**：前者是 `SO_RCVTIMEO/SO_SNDTIMEO` 到期，后者是 `Socket::wait_ready` 的总超时判定；两者可交叉验证，不可相加。
- **`epoll_ctl 10.3/请求`** 的历史数字来自 strace（`AGENT-HANDOFF.md:224`），本文不重新测量；SocketDebug 的价值是给出**可在进程内复现的解释变量**（`wait_slices`），而不是替代 strace。
- **单事件 19.65–22.47 ns** 是批量 200k 的实测（`docs/debug-reliability.md:157`），只能用于「量级判断」，不能外推端到端（`docs/debug-overhead.md:60-61`）。

### 5.4 与并行任务 1.3 的协调（重要，2026-10-30 15:24 发现）

- 工作树里已有**未提交**的 1.3 在制品：`mcp/include/mcp/tools/socket_registry.hpp`（338 行）+ `mcp/src/tools/socket_registry.cpp`（1069 行）。它自建 fd→统计注册表、每 fd 一个 256 条事件环、用 `/proc/self/fd/<fd>` 的 `socket:[inode]` 做身份校验（防 ABA）、读 `TCP_INFO` 拿内核字节/重传/RTT、并写 JSONL 审计。
- 该头文件明确写着「**不修改 `src/network`**，业务方需在创建 socket 后调用 `Register()`」——后果是**未登记的 socket 不可关闭、只能只读观察**（`registered=false`/`closable=false`），覆盖不完整。
- 因此 1.2 与 1.3 必须统一账本，否则会出现两套环形缓冲、两套 id、两套口径：
  - 建议：`SocketDebug`（在 `src/network`，`Socket` 创建即自动注册）作为**唯一账本**；1.3 的 `SocketRegistry` 退化为适配层（`socket_id ↔ fd` 映射、白名单、审计），计数与事件全部读 `SocketDebug::Snapshot/Events`。
  - 若 1.3 已先行落地：1.2 实现时**复用**其 inode 身份校验与 `TCP_INFO` 采集，不要重写；`SocketDebug` 只补它没有的两块能力——「Socket 对象自动注册」与「应用层 bytes / 超时 / 100 ms 分片」。
- 对本文的影响：§3.6 的 `go2cpp_debug_socket` 建议**同时被 `mcp` 目标链接**（而不是让 MCP 另建注册表）；§2.6 里的「`inet_diag`/`TCP_INFO` 作为 phase 2」可提前——1.3 的在制品已经把它实现了。
