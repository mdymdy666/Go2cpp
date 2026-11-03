# 分布式设施（distributed）· 深入学习

> 面向「要接手改 `include/go2cpp/distributed/**` / `src/distributed/**` 的人」。
> 文中每个 `文件:行` 都在本树用 `grep -n` / `sed -n` 复核过（2026-11-03，分支 `feat/isolated` 工作区）；
> 性能只写台账出处，没有台账就写「无独立台账」，不给结论数字。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 定位：它解决什么问题

三个原语——**服务发现**、**分布式锁**、**选主**——共用一张**租约表**与一个协调者。
所有「存活」判定都不是布尔状态，而是「截止时间 + 惰性清扫」；所有「身份」判定都靠
`(LeaseId, owner)` 二元组，避免旧持有者误续/误放。

与浅层文档的分工：

| 文档 | 回答什么 | 不回答什么 |
|---|---|---|
| `user/docs/09-distributed.md` | 三个原语的语义、API 签名、两种部署形态、已知限制 | 锁保护哪段、过期怎么算、选主为什么按编号 |
| 本文 | 数据结构字段级、每步落在哪一行、失败路径、TCP 协议、测试怎么跑 | 不重复 API 手册，不抄浅层的性能表 |

**本树事实核对**（任务书里「若存在，先 find 确认」）：

- `include/go2cpp/distributed/` 7 个头，共 1263 行：`types.hpp` 229、`coordinator.hpp` 269、
  `election.hpp` 156、`lock.hpp` 154、`registry.hpp` 159、`transport.hpp` 158、`lease_session.hpp` 138。
- `src/distributed/` **存在** 6 个实现：`coordinator.cpp`、`transport.cpp`、`registry.cpp`、
  `lock.cpp`、`election.cpp`、`lease_session.cpp`（`ls -la src/distributed` 实测，非空）。
- 构建开关：`GO2CPP_WITH_DISTRIBUTED` 默认 ON（`cmake/Go2CppDistributed.cmake:25`）；
  测试开关 `GO2CPP_DISTRIBUTED_TESTS`（同文件 26）与多进程开关
  `GO2CPP_DISTRIBUTED_MULTIPROCESS_TESTS`（27-28）。

### 1.2 入口点清单

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `Coordinator::Coordinator(opts)` | `src/distributed/coordinator.cpp:51` | 应用/测试 | 建租约表；按 `start_sweeper` 起清扫线程 | 对象 |
| `Coordinator::Start/Stop` | `coordinator.cpp:59/69` | 应用 | 起/停清扫线程（幂等） | void |
| `Coordinator::RegisterService` | `coordinator.cpp:251` | `ServiceRegistry` | 校验 → 加锁 → 幂等续约或换租约 | `Result<LeaseView>` |
| `Coordinator::RenewLease` | `coordinator.cpp:311` | `LeaseSession` 心跳 | 校验 owner/截止时间后续期 | `Result<LeaseView>` |
| `Coordinator::ReleaseLease` | `coordinator.cpp:333` | 析构/注销 | 摘索引、删租约（幂等） | `Outcome` |
| `Coordinator::LookupService` | `coordinator.cpp:346` | 业务 | 按服务名列实例 | `vector<ServiceInstance>` |
| `Coordinator::AcquireLock` | `coordinator.cpp:398` | `DistributedLock` | 抢占或回收过期锁，分配 fencing | `Result<LeaseView>` |
| `Coordinator::JoinElection` | `coordinator.cpp:487` | `LeaderElection` | 加入候选表并重算 leader | `Result<LeaseView>` |
| `Coordinator::SweepExpired` | `coordinator.cpp:111` | 清扫线程/测试 | 全表扫描删过期租约 | 删除条数 |
| `Coordinator::Stats` | `coordinator.cpp:579` | 诊断 | 租约/服务/锁/选举计数 | `CoordinatorStats` |
| `ServiceRegistry::Register` | `src/distributed/registry.cpp:156` | 业务 | 组 `ServiceRegistration` → 调协调者 → 建 `LeaseSession` | `Result<Registration>` |
| `ServiceRegistry::Lookup/Get` | `registry.cpp:191/197` | 业务 | 查实例列表/取一个实例 | vector / optional |
| `DistributedLock::Acquire` | `src/distributed/lock.cpp:156` | 业务 | 调 `AcquireLock` → 包 `LockHandle` | `Result<LockHandle>` |
| `LockHandle::fencing_token` | `lock.cpp:80` | 业务 | 读授予时的 token | `uint64_t` |
| `LeaderElection::Join` | `src/distributed/election.cpp:212` | 业务 | 调 `JoinElection` → 包 `Candidate` | `Result<Candidate>` |
| `LeaderElection::Leader` | `election.cpp:253` | 业务 | 查当前 leader | `optional<LeaderInfo>` |
| `detail::LeaseSession::Start` | `src/distributed/lease_session.cpp:45` | facade 内部 | 建 `relay::Heartbeat`，起续约线程 | void |
| `CoordinatorServer::Start` | `src/distributed/transport.cpp:723` | 运维/测试 | 绑定端口、起 accept 线程 | `Outcome` |
| `CoordinatorServer::Stop` | `transport.cpp:750` | 运维 | 关监听、关连接、等 worker 归零 | void |
| `CoordinatorClient::Connect` | `transport.cpp:891` | 应用 | 建 TCP；失败可由下次操作重试 | `Outcome` |
| `CoordinatorClient::RegisterService` | `transport.cpp:901` | facade（远程） | 编码 `Op::kRegister` → 收响应 | `Result<LeaseView>` |
| `ICoordinator`（抽象） | `include/go2cpp/distributed/coordinator.hpp:92` | 所有 facade | 本地 `Coordinator` 与远程 `CoordinatorClient` 的共同接口 | 纯虚 |

关键配置默认值（`coordinator.hpp:74-79`，`Duration` 是毫秒，见 `types.hpp:45`）：
`sweep_interval{10}`、`min_ttl{10}`、`max_ttl{10*60*1000}`、`start_sweeper{true}`。

### 1.3 总架构图

```
             业务代码
   ┌─────────────┼──────────────────┐
   ▼             ▼                  ▼
ServiceRegistry DistributedLock   LeaderElection      ← facade（registry/lock/election.cpp）
   │             │                  │
   └──── 共用 detail::LeaseSession（心跳续约 + 到期回调）────┘
                 │
        ┌────────▼─────────┐
        │   ICoordinator   │  ← 抽象（coordinator.hpp:92）
        └───┬──────────┬───┘
            │          │
   ┌────────▼───┐  ┌───▼──────────────────┐
   │ Coordinator│  │ CoordinatorClient    │
   │ (进程内)   │  │ 单 socket + 一把互斥 │
   │ m_mutex    │  └───┬──────────────────┘
   │ m_leases   │      │ TCP：4B 长度 + op + 字段
   │ m_services │  ┌───▼──────────────────┐
   │ m_locks    │  │ CoordinatorServer    │
   │ m_elections│  │ accept 线程 + 每连接  │
   │ m_fencing  │  │ detach worker + Hub  │
   │ SweepLoop  │  └──────────────────────┘
   └────────────┘
```

线程与锁边界：

- `Coordinator` 内部**一把大锁** `m_mutex` 保护全部表与计数器；`EraseLeaseLocked` 的
  `Locked` 后缀就是「调用方必须已持锁」的约定（函数名 `EraseLeaseLocked` 的 `Locked` 后缀即为此约定，`coordinator.cpp:129`）。
- 清扫线程 `SweepLoop`（`coordinator.cpp:92`）周期调 `SweepExpired`，与业务线程争同一把锁。
- 每个 facade 句柄各有一条心跳线程（`relay::Heartbeat`，`include/go2cpp/relay/lease.hpp:163`），
  续约时也要抢同一把锁。
- 远程形态：`CoordinatorServer` 每条连接一个 detach 线程（`transport.cpp:454-476`），
  连接对象登记进 `relay::Hub`；客户端只有一条 socket，调用串行化（`transport.cpp:797` 起）。

### 1.4 复杂场景 A：注册服务 → 心跳续约 → 过期剔除（12 步）

1. 业务调 `ServiceRegistry::Register`（`registry.cpp:156`），组 `ServiceRegistration`；
   `owner` 直接取 `instance`，注释说明「同一实例可能由不同进程重新注册」（`registry.cpp:163`）。
2. TTL 为 0 时用 `m_options.default_ttl`（`registry.cpp:164`）。
3. 调 `m_coordinator.RegisterService`（`registry.cpp:168`）。
4. `Coordinator::RegisterService` 先校验三元组非空与 TTL 范围（`coordinator.cpp:252-263`），
   失败返回 `kInvalidArgument`。
5. 加 `m_mutex`（`coordinator.cpp:265`），取 `now`。
6. 查 `m_services[service][instance]` 得到旧 lease；**先复制 id 再替换**，因为回收旧租约可能把
   整个 service 键删掉、引用悬空（注释在 `coordinator.cpp:265-267`）。
7. 旧租约仍存活且 owner 相同 → 原地续约：更新 `endpoint/ttl/deadline`、`++revision`、
   **保留 `LeaseId`**（`coordinator.cpp:280-287`）。这是幂等注册的关键：实例「年龄」不被重置。
8. owner 不同或已过期 → `EraseLeaseLocked`（`coordinator.cpp:290`）回收旧租约。
9. 新建 `LeaseRecord`：`id = m_next_lease++`、`kind = kService`、`deadline = now + ttl`、
   `revision = 1`（`coordinator.cpp:293-307`）。
10. **先写索引 `m_services[...]` 再插 `m_leases`**，且全程持锁，不会出现「索引指向不存在租约」
    的窗口（注释在 `coordinator.cpp:304-305`）。
11. facade 拿到 `LeaseView` 后建 `LeaseSession`：续约间隔取 `options.renew_interval`，
    为零时回落 `ttl/3`（`lease_session.cpp:36-38`）；`Start` 里建 `relay::Heartbeat`
    （`lease_session.cpp:61`）并起线程。
12. 心跳每次调 `RenewLease`；失败时 `on_lost` 把 `Registration` 标为 lost。协调者侧若发现
    `deadline <= now`，`RenewRecordLocked` 返回 `kExpired`（`coordinator.cpp:244`），**不允许复活**。

失败/边界路径：清扫线程按 `sweep_interval` 调 `SweepExpired`（`coordinator.cpp:111-127`），
先全表收集过期 id，再逐个 `EraseLeaseLocked`；删除顺序按 kind 分别清 `m_services`/`m_locks`/
`m_elections` 索引，选举还会触发 `RecomputeLeaderLocked`。

### 1.5 复杂场景 B：抢锁 → fencing → 过期占用回收（12 步）

1. `DistributedLock::Acquire`（`lock.cpp:156`）校验 key/owner/ttl 后调协调者。
2. `Coordinator::AcquireLock` 校验空串与 TTL（`coordinator.cpp:400-409`）。
3. 加锁、取 `now`（`coordinator.cpp:411-412`）。
4. 查 `m_locks[key]`（`coordinator.cpp:414`）。
5. 存在且租约仍存活 → 返回 `kConflict`，message 带当前 owner（`coordinator.cpp:416-421`）。
6. 存在但已过期 → **先回收再分配**，注释明确「保证下面分配的新 token 一定大于前任」
   （注释在 `coordinator.cpp:419`）。
7. 新建记录：`kind = kLock`、`fencing = ++m_fencing[record.name]`（`coordinator.cpp:431-434`）。
   注意 `m_fencing` 是**按锁名**计数，不是全局计数。
8. `m_locks[name] = id`、`m_leases.emplace(id, ...)`，返回 `MakeViewLocked`（`coordinator.cpp:433-435`）。
9. facade 把 `LeaseId`、`fencing`、`owner` 存进 `LockHandle::Impl`（`lock.cpp:15-27`）。
10. 业务用 `fencing_token()`（`lock.cpp:80`）把 token 带进下游写请求；下游必须拒绝更小的 token。
11. 续约/释放统一走 `RenewLease`/`ReleaseLease`；release 幂等（`coordinator.cpp:333` 起，
    重复调用不产生副作用）。
12. 进程崩溃不释放：只能等 `deadline` 到期后被清扫或下一次 `AcquireLock` 顺手回收
    （注释在 `coordinator.cpp:419`），这中间的延迟最多一个 `sweep_interval` 加扫描时间。

### 1.6 复杂场景 C：远程协调者一次调用（12 步）

1. 应用 `CoordinatorClient(host, port)`（`transport.cpp:881`）；首次操作会自动建连
   （`transport.hpp:141-143` 的注释）。
2. `Connect()`（`transport.cpp:891`）→ `Impl::Connect`，失败保留现场，下次操作重试。
3. 调 `RegisterService`（`transport.cpp:901`）：序列化 `Op::kRegister` 与字段。
4. 帧格式：**4 字节长度前缀 + payload**（`kFrameHeaderBytes = 4`，`transport.cpp:174`）；
   写走 `WriteFrame`（`transport.cpp:261`），读走 `ReadFrame`（`transport.cpp:244`）。
5. 操作码枚举 14 个：`kPing=1` 到 `kSweep=14`（`transport.cpp:176-191`）；注释写明
   「服务端与客户端必须来自同一构建（不做事先版本协商）」。
6. 服务端 `AcceptLoop`（`transport.cpp:454`）accept 后设收发超时、登记进 `Hub`、
   `connections++`、起 detach 线程。
7. 每条连接 `Serve` 循环：`ReadFrame` → `requests++` → `Dispatch`（`transport.cpp:478-499`）。
8. `Dispatch`（`transport.cpp:502`）读 op，switch 到具体方法；空请求返回 `kProtocolError`。
9. 响应编码 `Status + message + body`；写失败即关连接。
10. 连接退出时 `hub.Unregister` → `Close` → 计数减一 → 唤醒 `Stop` 的等待（`transport.cpp:492-499`）。
11. `Stop`（`transport.cpp:750`）关监听、关全部连接、等 `workers` 归零；用计数而不是保存
    每个 `std::thread` 句柄，避免「线程自移除」竞态（`transport.cpp:454-476` 的 AcceptLoop）。
12. 客户端断线：下一次调用重新 `Connect`；`Status::kUnavailable` 表示协调者不可达
    （`types.hpp:52-64` 的枚举）。

### 1.7 关键不变量（破了就出 bug）

1. `m_leases` / `m_services` / `m_locks` / `m_elections` / `m_fencing` / `m_next_lease` 全部由
   `m_mutex` 保护；`EraseLeaseLocked` 必须在持锁下调用（`coordinator.cpp:129`）。
2. 删索引前必须核对「索引指向的 id 就是本租约」：同名实例可能已被新租约覆盖，盲删会误删
   新租约（`coordinator.cpp:142` 注释）。
3. 注册时先写索引、再插租约，中间不释放锁（`coordinator.cpp:304-305`）。
4. 选主按 **`LeaseId` 最小**，不是按剩余时间：剩余时间会因续约时序抖动导致 leader 漂移
   （`coordinator.cpp:183-186` 注释）。
5. leader 每次实际变化（含「有主 → 无主」）都 `++epoch`；epoch 不增表示同一任
   （`coordinator.cpp:195-197`）。
6. fencing token 必须严格大于前任；所以过期锁要先回收再分配（注释在 `coordinator.cpp:419`）。
7. 过期不可复活：`RenewRecordLocked` 发现 `deadline <= now` 返回 `kExpired`
   （`coordinator.cpp:238-247`）。
8. 同一 owner 重复注册保留 `LeaseId`，只 `++revision`（`coordinator.cpp:280-287`）。
9. 远程协议无版本协商，客户端/服务端必须同构建（`transport.cpp:174`）；混部会得到
   `kProtocolError` 而不是「尽量兼容」。

---

## 第二部分 · 分：逐块细讲

### 2.1 数据结构（字段级）

`LeaseRecord`（`coordinator.hpp:211-223`）：`id`、`kind`、`name`、`instance`、`owner`、
`endpoint`、`deadline`、`ttl`、`fencing`、`revision`。三种 kind 共用一条记录，靠 `name/instance`
语义区分：服务用 `name=service, instance=instance`，锁用 `name=key`，选举用 `name=election,
owner=candidate`。

`ElectionRecord`（`coordinator.hpp:225-231`）：`name`、`epoch`、`leader`、`candidates`
（node → LeaseId 的哈希表）。重算 leader 只遍历存活候选者，跳过「索引在但租约已过期」的项。

外部视图类型在 `types.hpp`：`LeaseView:147`、`LockInfo:167`、`CandidateInfo:180`、`LeaderInfo:188`、
`ElectionState:195`、`CoordinatorStats:203`。`RemainingUntil`（`types.hpp:216`）把 deadline 换算成
剩余时间，所有对外 `LeaseView.remaining` 都按协调者时钟算。

### 2.2 三索引与惰性清扫

索引结构：`m_services` 是 `service → (instance → lease)`，`m_locks` 是 `key → lease`，
`m_elections` 是 `election → ElectionRecord`。租约表 `m_leases` 是 `LeaseId → LeaseRecord`。

清扫两条路径，语义必须一致：

```cpp
// coordinator.cpp:111-127（节选）
std::size_t Coordinator::SweepExpired() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    for (const auto &[id, record] : m_leases) {
        if (record.deadline <= now) { expired.push_back(id); }
    }
    for (const LeaseId id : expired) { EraseLeaseLocked(id, now); }
```

注意它是「先收集、再删除」：不能在遍历 `m_leases` 时 erase，否则迭代器失效。

删除逻辑按 kind 分支（`coordinator.cpp:129-171`）。选举分支在删掉候选后立刻
`RecomputeLeaderLocked`，所以「leader 进程死亡」不需要额外事件，清扫就是唯一触发器。

### 2.3 选主状态机

| 状态 | 触发 | 结果 |
|---|---|---|
| 无候选 | 最后一个候选过期/退出 | `leader=""`，`epoch++`（`coordinator.cpp:195-197`） |
| 有候选无主 | 首个 `JoinElection` | 该候选成为 leader，`epoch=1`（首次变化） |
| 有主 | 新候选加入且编号更大 | leader 不变（编号最小者胜） |
| 换主 | 旧 leader 租约被删 | 剩余最小 `LeaseId` 者接任，`epoch++` |

`JoinElection` 的幂等分支（`coordinator.cpp:506-512`）同样保留 `LeaseId`，因此**重复加入不会
丢掉排队位置**；只有过期后重新加入才会拿到更大的编号、排到队尾。

### 2.4 锁与 fencing

`fencing` 的意义：锁过期后新持有者拿到更大的 token，下游用「拒绝更小 token」把旧持有者的
迟到写挡掉。实现上 token 来自 `m_fencing[lock_name]`（`coordinator.cpp:436`），**按锁名独立**，
跨锁不比较。这是设计选择而不是缺陷：不同 key 的写通常落在不同下游资源上。

`ReleaseLease` 不重置 fencing 计数；只有进程重启（内存表清空）才会回到 1。所以 fencing 只在
「协调者不重启」的前提下单调——这是使用方必须知道的边界。

### 2.5 facade 与 LeaseSession

三个 facade 都是薄封装：

- `ServiceRegistry`：`Register` 组请求、建会话（`registry.cpp:156-190`）；`Lookup/Get` 直接转发
  （`registry.cpp:191-205`）。
- `DistributedLock`：`Acquire` 包 `LockHandle`（`lock.cpp:156-185`）；`Query/Locks` 转发。
- `LeaderElection`：`Join` 包 `Candidate`（`election.cpp:212-248`）；`State/Leader/Elections` 转发。

`LeaseSession`（`src/distributed/lease_session.cpp`）是三者共用的「自动续约器」：构造时归一化
`ttl`（<=0 夹到 1ms）与 `interval`（<=0 时 `ttl/3`，`lease_session.cpp:33-38`），`Start` 时
建 `relay::Heartbeat`（61）与 `relay::Lease`（75），失败回调只触发一次。它不拥有协调者，
协调者必须先于会话析构。

### 2.6 TCP 传输与协议

服务端结构 `CoordinatorServer::Impl`（`transport.cpp:431-452`）：`listener`、`accept_thread`、
`running/stopping`、`bound_port`、`requests`、`connections`、`relay::Hub hub`、`workers` 计数。

- 监听端口 0 时由内核分配，`port()` 返回实际端口（`transport.cpp:783`），测试靠这个拿端口。
- accept 失败视为「超时/临时错误」继续循环，直到 `stopping`（`transport.cpp:454-466`）。
- 每个连接 detach，`Stop` 靠 `workers` 计数等待（`transport.cpp:750-780`）。
- 客户端 `Impl`（`transport.cpp:797`）持有 `net::Socket`、请求互斥；没有请求多路复用，
  一条连接上调用是串行的。

### 2.7 错误码语义（`types.hpp:52-64`）

| 码 | 何时出现 | 调用方该做什么 |
|---|---|---|
| `kInvalidArgument` | 空 service/instance/owner、TTL 越界 | 修正参数，别重试 |
| `kNotFound` | 续约/释放的 lease 不存在 | 当作已失去，重建会话 |
| `kConflict` | 锁被有效持有 | 退避后重试或放弃 |
| `kExpired` | 续约时已过期 | **不要重试续约**，重新注册/抢锁 |
| `kNotOwner` | owner 不匹配 | 说明身份错乱，检查是否复用 owner |
| `kUnavailable` | 远程断线/超时 | 可重试（幂等操作） |
| `kProtocolError` | 响应无法解析/版本不一致 | 检查两端构建 |
| `kClosed` | 协调者已停 | 停业务或重建 |

### 2.8 数据与性能

- **无独立台账**。`docs/metrics-ledger.md` 的 M01–M16 中没有 distributed 条目（本轮
  `grep -n 'distributed' docs/metrics-ledger.md` 无命中）。
- `docs/reports/asm-distributed.md` 是**报告**不是台账，本文不引用其中数字；`user/docs/09`
  §6 的性能段落也不在此复述（未按台账口径复核）。
- 可确定的结构性事实：所有协调者操作串行在一把 `m_mutex` 上；每次 `SweepExpired` 是
  O(租约数) 全表扫描；远程每次调用是一次同步 RTT，无批量接口。要压测请自己写 bench
  （`tests/distributed/bench_distributed.cpp` 已存在，但本文未跑）。

### 2.9 边界与陷阱

1. **过期是惰性的**：`deadline` 过了不会有人立刻收到通知；`LookupService` 是否过滤未清扫的
   过期项，本文未逐行核实（入口 `coordinator.cpp:346`），读代码时先确认这一点。
2. **清扫周期 = 回收延迟下限**：默认 10ms（`coordinator.hpp:76`），但 `SweepExpired` 持锁扫全表，
   租约多时业务调用会被清扫阻塞。
3. **单锁串行**：注册、续约、抢锁、查服务共用 `m_mutex`；高 QPS 下这是第一瓶颈，
   不要以为「锁是 per-key」。
4. **fencing 按锁名**：不能拿 A 锁的 token 去比较 B 锁的写；跨资源排序没有意义。
5. **协调者重启 = 全部状态丢失**：租约、epoch、fencing 都在内存里；重启后旧持有者可能
   与新持有者同时认为自己有效（需要下游自己做幂等/版本号）。
6. **协议无版本协商**：`transport.cpp:174` 明说两端必须同构建；滚动升级会失败。
7. **owner 复用**：`ServiceRegistry` 把 `owner` 设成 `instance`（`registry.cpp:163`），
   同一实例换进程重新注册时，协调者会认为是「同一 owner 续约」而不是抢占——这是有意的，
   但也意味着**不能靠 owner 区分进程实例**。
8. **`max_ttl` 是硬上限** 10 分钟（`coordinator.hpp:78`）；超过直接 `kInvalidArgument`。
9. **客户端自动重连**：`Close` 后下一次操作会重新建连（`transport.hpp:141-143`），但正在
   等待的调用不会自动重放；重试责任在调用方。

### 2.10 测试

| 测试 | 文件 | 覆盖 |
|---|---|---|
| 服务发现 | `tests/distributed/test_registry.cpp` | 注册/续约/过期/替换 |
| 锁 | `tests/distributed/test_lock.cpp` | 冲突/fencing/过期回收 |
| 选主 | `tests/distributed/test_election.cpp` | 最早加入者胜、epoch |
| 随机 | `tests/distributed/test_random.cpp` | 随机操作序列 |
| 并发 | `tests/distributed/test_concurrent.cpp` | 多线程抢同一 key |
| 传输 | `tests/distributed/test_transport.cpp` | TCP 编解码/断线 |
| 场景 | `tests/distributed/test_scenarios.cpp` | 组合流程 |
| 多进程 | `tests/distributed/test_multiprocess.cpp` | fork/exec，标签 `long` |
| 压测 | `tests/distributed/bench_distributed.cpp` | 自写 bench |

CMake 里测试目标名与标签：`distributed_registry`…`distributed_scenarios` 打
`LABELS distributed`（`cmake/Go2CppDistributed.cmake:133-144`）；多进程用例额外带 `long`
（154）。常用命令：

```bash
cmake -S . -B build -DGO2CPP_WITH_DISTRIBUTED=ON -DGO2CPP_DISTRIBUTED_TESTS=ON
cmake --build build -j
ctest --test-dir build -L distributed --output-on-failure
# 只跑多进程（较慢）
ctest --test-dir build -L long --output-on-failure
```

### 2.11 改这个模块前的检查清单

- [ ] 新操作是否也要走 `ICoordinator`？加上后 `Coordinator` 与 `CoordinatorClient` 都要实现，
      且 `Op` 枚举与服务端 `Dispatch` 要同步改（`transport.cpp:176-191`、`502`）。
- [ ] 新状态是否要进 `Stats`？`CoordinatorStats`（`types.hpp:203`）与 `Stats()`（`coordinator.cpp:579`）
      两处都要改。
- [ ] 删除路径是否核对了「索引指向本租约」？参考 `EraseLeaseLocked` 的写法。
- [ ] 是否会在遍历 `m_leases` 时 erase？必须收集后删。
- [ ] 新字段是否需要出现在 `LeaseView`/`LockInfo`？外部结构在 `types.hpp`，改动会破坏 ABI。
- [ ] 是否依赖「协调者不重启」？fencing/epoch 都只在内存里，明确写进使用方文档。
