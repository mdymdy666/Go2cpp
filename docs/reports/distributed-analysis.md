# 分布式设施现状分析与目标设计

> 范围：`include/go2cpp/distributed/`（7 头，1263 行）、`src/distributed/`（6 实现，2686 行），
> 以及相邻的 `rpc/`、`storage/`、`config/`、`metrics/`、`bento/net/mqtt.hpp`。
> 性质：**只读分析**，不改代码、不 commit；文中"建议/待确认"均未落地。
> 行号复核方式：`grep -n` + `sed -n`，复核时间 2026-11-04，分支 `feat/isolated`（HEAD `39804de`）。
> 真树：WSL `/UserData/CodexWorkSpace/Go2cpp`（小写 `pp`），**不是** `E:\CodexWorkspace\Go2Cpp`。

---

## 1. 现状清单

### 1.1 分布式模块本体（9 个条目，8 个组件）

| # | 文件:行 | 类 / 接口 | 能力（一句话） | 依赖 |
|---|---|---|---|---|
| 1 | `types.hpp:41` | `namespace distributed` / `types.hpp:52` `Status` | 值对象层：状态码、`Outcome`(:72)、`Result<T>`(:93)、`LeaseKind`(:116)、`ServiceRegistration`(:123)、`LeaseView`(:147)、`LockInfo`(:167)、`ElectionState`(:195)、`CoordinatorStats`(:203) | 仅 STL |
| 2 | `coordinator.hpp:174` / `coordinator.cpp:59` | `Coordinator`（实现 `ICoordinator`，`coordinator.hpp:92`） | 单进程权威租约表：服务/锁/选举三种 kind 共用 `m_leases`；单 mutex 线性化；10ms 清扫线程；fencing 按锁名单调 | 仅 STL + 自身 types |
| 3 | `coordinator.cpp:251/311/333/398/487` | `RegisterService` / `RenewLease` / `ReleaseLease` / `AcquireLock` / `JoinElection` | 注册（幂等续约或换租约）、续约、释放、抢锁（冲突即返回）、加入选举 | 同上 |
| 4 | `coordinator.cpp:111` | `SweepExpired` / `SweepLoop:92` | 全表 O(n) 扫描删过期租约；选举 leader 重算在 `EraseLeaseLocked:129` 内触发 | 同上 |
| 5 | `lease_session.hpp:65` / `lease_session.cpp:45` | `detail::LeaseSession` | 句柄共用的续约会话：`relay::Heartbeat` 心跳 + `relay::Lease` 本地兜底 + 一次性 `lost` 回调 | `relay::Heartbeat`(`relay/lease.hpp:163`)、`relay::Lease`(:94) |
| 6 | `registry.hpp:123` / `registry.cpp:156` | `ServiceRegistry` + `Registration`(:65) | 服务注册（`owner := instance`，`registry.cpp:163`）、`Lookup`(:191)、`Get`(:197)；句柄自动续约 | `ICoordinator` + `LeaseSession` |
| 7 | `lock.hpp:131` / `lock.cpp:156` | `DistributedLock` + `LockHandle`(:69) | 非阻塞抢锁、fencing token(:80)、续约/释放、`OnLost` | `ICoordinator` + `LeaseSession` |
| 8 | `election.hpp:130` / `election.cpp:212` | `LeaderElection` + `Candidate`(:65) | 最早租约者为主、`epoch` 单调、leader 缓存 + 变更回调(:109) | `ICoordinator` + `LeaseSession` |
| 9 | `transport.hpp:63/116` / `transport.cpp:431/797` | `CoordinatorServer` / `CoordinatorClient` | 手写二进制 TCP：4 字节长度帧(:174)、14 个操作码(:177)、1 accept + 每连接 1 线程(:454)、客户端单 socket + 一把互斥(:797) | `network`（`net::Socket`）、`relay::Hub` |

> 组件计数口径：类型层 1 + 协调者 1 + 会话工具 1 + 三个 facade 3 + 传输两端 2 = **8**。
> `cmake/Go2CppDistributed.cmake:85` 产出 `go2cpp_distributed`；`:105` 链接 relay + network + Threads；
> `tests/distributed/` 11 个文件（9 个 ctest 用例 + bench + stress + support 头）。

### 1.2 相邻设施（可复用清单）

| 设施 | 文件:行 | 现成能力 | 当前是否被 distributed 使用 |
|---|---|---|---|
| RPC 服务端 | `rpc/server.hpp:144`，`Register:160`，`Listen:182`，`Stop:195` | 方法表 + 流式 `ServerStream`(:60，`Send:79`)，1 行注册宏 | **否** |
| RPC 客户端 | `rpc/client.hpp:351`，`Call:394`，`TryCall:419`，`AsyncCall:475` | 同步/异步/流式调用 | **否** |
| RPC 注册表 | `rpc/registry.hpp:494`，`AddService:510`，`Freeze:544` | ADL 宏注册、无锁查找 | **否** |
| LSM KV | `storage/kv.hpp:126`，`Put:136`，`Write:140`，`NewIterator:145` | 持久化 KV + 原子批 + 有序迭代 | **否** |
| 前缀 Watch | `storage/watch.hpp:77`，`Watch:93`，`Replay:107`，`Offset:110` | 变更日志 + 游标重放 + 同步/异步派发 | **否** |
| 持久化队列 | `storage/queue.hpp:81`，`Append:93`，`Consume:97`，`Ack:101`，`Head:112` | topic / consumer / 游标 / 至少一次 | **否** |
| 配置热更新 | `config/watch.hpp:215` `FileWatcher`、`:353` `ConfigStore`、`:982` `ConfigWatcher` | inotify + 轮询、无锁读快照、原子发布 | **否** |
| 指标 | `metrics/metrics.hpp:471` `Registry`，`Default:480`，`GetCounter:483`，`Render:509` | Prometheus 文本、标签子指标、直方图 | **否**（`Stats()` 只回结构体） |
| MQTT | `bento/net/mqtt.hpp:500` `MqttBroker` | 完整 pub/sub 协议（CONNECT/PUBLISH/SUBSCRIBE/遗嘱） | **否** |
| HTTP 限流 | `app/gateway.hpp:455` `RateLimitMiddleware` | 进程内回调式 429 | **否**（且非分布式） |

**实测证据**：`grep -rln 'go2cpp/distributed' --include=*.hpp --include=*.cpp .` 除
`include/go2cpp/distributed/`、`src/distributed/`、`tests/distributed/` 外**零命中**——
该模块目前是一座孤岛。

---

## 2. 能力矩阵

| 能力 | 现状 | 判定 | 依据 / 缺口 |
|---|---|---|---|
| 服务发现 | 注册 + 心跳 + 过期剔除 + 全量 `Lookup` | **弱** | 无 watch（`user/docs/09-distributed.md:274` 自认）、无本地缓存、无分页、无健康检查/权重；每次查询 O(n) 拷贝（1000 实例 162µs，`docs/modules/distributed.md:197`） |
| 选主 | 最早租约者胜、`epoch` 单调、自动重选 | **弱** | 规则可验证，但协调者单点：协调者重启后 `epoch` 从 0 重来，旧主与新主可能同时自认（无 quorum、无持久化） |
| 分布式锁 | 非阻塞抢锁、fencing、续约、过期回收 | **有** | 语义完整；但 fencing 计数只在内存（`coordinator.hpp:256-258`），协调者重启即回退，破坏"永不复用" |
| 租约 | 统一租约表、惰性过期、`ttl/3` 续约、本地兜底 | **有** | 三原语共用一条不变量，是本模块最扎实的部分 |
| 配置同步 | 无 | **缺** | 无跨节点配置载体；`config::ConfigStore` 只解决单机热更新 |
| 消息（pub/sub） | 无 | **缺** | `storage::Queue` 与 `bento::net::MqttBroker` 都在，但未接入 |
| 限流 | 无 | **缺** | 仅 `app/gateway.hpp:455` 的进程内 HTTP 限流，不跨节点 |
| 追踪 | 无 | **缺** | 无 `trace_id`/`span_id`；`metrics` 有指标但无请求级关联 |
| 观测（指标） | `CoordinatorStats` 结构体 + `Stats()` | **弱** | 未注册到 `metrics::Registry`，无 Prometheus 端点；锁竞争尾延迟无直方图 |
| 认证 / 传输安全 | 无 | **缺** | 协议无 TLS/鉴权（`docs/modules/distributed.md:174-175` 明确为边界） |
| 持久化 / 多副本 | 无 | **缺** | 内存态，重启丢全部租约与 fencing |

---

## 3. 问题诊断

### 3.1 过度设计：1157 行手写协议，旁边就是 rpc 框架

`transport.cpp` 共 1157 行，自建帧格式（`kFrameHeaderBytes:174`）、14 个操作码（`Op:177`）、
accept 线程 + 每连接一线程（`AcceptLoop:454`）、客户端单 socket 串行（`Client::Impl:797`）。
而项目已有 `rpc::Server`（`server.hpp:144`，支持 `Listen:182` 与流式 `ServerStream`）与
`rpc::Client`（`client.hpp:351`）。两套东西解决同一类问题，维护成本翻倍，且新操作码要同时改
`ICoordinator`、`Coordinator`、`CoordinatorClient`、`Op`、`Dispatch` 五处
（`user/deep/09-distributed.md:344-345` 已列为检查项）。

**建议**：短期保留（已有 8 个测试覆盖），中期把 14 个 op 收敛为 `CoordinatorApi` 结构体，
用 `GO2CPP_RPC_SERVICE` 注册到 `rpc::Server`；协议层只保留一个"远程适配器"。
**待确认**：`reflect` 序列化是否覆盖 `LeaseView`/`ElectionState` 这类嵌套结构（需先验证）。

### 3.2 缺少最简方案：没有 watch、没有 KV、没有消息，全靠轮询与自造

- **无 watch**：服务上下线只能轮询；`storage/watch.hpp:77` 已有"前缀订阅 + 游标重放"（`Watch:93`/`Replay:107`）。
- **无通用 KV**：配置、开关、集群元数据没有载体，只能塞进 `LeaseRecord`；`storage/kv.hpp:126` 现成。
- **无消息**：`storage/queue.hpp:81`（topic/consumer/Ack）与 `bento/net/mqtt.hpp:500`（完整 broker）都未接入。
- **无持久化**：`Coordinator` 重启丢 `m_fencing`（`coordinator.hpp:256-258`），而 fencing 的意义是"跨故障仍单调"；
  重启回退是**语义缺陷**，不只是"单点"。

### 3.3 组件复用不足：模块孤岛 + 三套错误模型

- 模块外零引用（§1.2 实测）。`app`/`rpc`/`bento` 都不知道分布式原语存在，业务接入要自己写胶水。
- 错误模型三套：`distributed::Status`（`types.hpp:52`）、`rpc::Status`、`storage` 的
  `bool + LastError()`、`config` 的 `error` 字符串。调用方无法写统一重试策略。
- 观测未接入：`CoordinatorStats`（`types.hpp:203`）未注册到 `metrics::Registry`（`metrics.hpp:471`）。
- 线程不共享：每个句柄一条 `Heartbeat` 线程（`lease_session.hpp:27-31`），1000 实例即 1000+ 线程。

### 3.4 接口不一致：运维接口混入业务接口，身份概念缺失

- `ICoordinator::SweepExpired`（`coordinator.hpp:162`）是测试/运维入口，却出现在业务接口上，
  远程客户端也实现了它（`transport.cpp:1117`）——任何能连上协调者的人都能触发全表扫描。
- `ServiceRegistry::Register` 把 `owner` 设成 `instance`（`registry.cpp:163-166`）。`owner` 本应是
  节点身份，现在无法区分"同一实例被新进程接管"与"原进程续约"。
- 全项目 `grep -rn 'NodeId\|node_id' include/go2cpp --include=*.hpp` **零命中**：没有节点身份类型，
  全是裸 `std::string`。
- `Status` 是 `uint8_t` 枚举（`types.hpp:52`），没有可扩展的错误详情（`message` 是自由文本），
  不利于上层做结构化告警。

---

## 4. 目标设计：一套完整分布式设施

分三层，原则：**能复用就不新造**；每层只解决一类问题；所有跨节点调用最终落到一个可替换的
`ICoordinator`（或 `IMetadataStore`）上。

### 4.1 基础层（Layer 0）：节点身份、时钟、传输

#### 4.1.1 `NodeId`（新增）

```cpp
// include/go2cpp/distributed/node.hpp（建议新增）
namespace go2cpp::distributed {

struct NodeId {
    std::string value;              // "host-pid-bootid"，进程内稳定
    static NodeId Generate();       // hostname + getpid + 启动随机数
    static NodeId Parse(std::string_view text);
};

struct NodeInfo {
    NodeId id;
    std::string host;               // 可路由地址
    std::uint16_t port{0};
    std::string zone;               // 机架/可用区
    std::uint64_t boot_id{0};       // 识别"同 id 的新进程"
};

} // namespace go2cpp::distributed
```

**关键语义**：`NodeId` 只做标识，不做认证；`boot_id` 让协调者能识别"同一 NodeId 的新进程"，
避免旧租约被误续（替代现在 `owner == instance` 的混淆）。
**与现有代码关系**：新增；`ServiceRegistration.owner`、`LeaseView.owner`、`Candidate::node()`
逐步改为 `NodeId`（保留 `std::string` 兼容层）。
**失败/超时**：无网络语义；`Generate()` 不失败（退化时用 pid+时间）。

#### 4.1.2 时钟（复用 + 收口）

保留 `Clock`（`types.hpp:43`）与 `RemainingUntil`（`types.hpp:216`），新增唯一入口，禁止业务自己减时间：

```cpp
struct Deadline { TimePoint at{}; static Deadline After(Duration); Duration Remaining() const; };
```

**语义**：跨机只传 `remaining`（`types.hpp:22-26`），不传绝对时间。
**待确认**：选主 TTL 过短会放大 NTP 偏差，是否需要偏差检测。

#### 4.1.3 传输（改造：复用 rpc）

```cpp
// 建议：把 14 个 op 收敛为方法表，而不是继续扩 Op 枚举
struct CoordinatorApi {
    Result<LeaseView> RegisterService(const ServiceRegistration& r);
    Result<LeaseView> RenewLease(std::uint64_t lease, const std::string& owner);
    Outcome ReleaseLease(std::uint64_t lease, const std::string& owner);
    std::vector<ServiceInstance> LookupService(const std::string& service);   // 其余 10 个略
};
GO2CPP_RPC_SERVICE(CoordinatorApi, RegisterService, RenewLease, /*...*/);

// 远程适配器：实现 ICoordinator，内部持 rpc::Client，每个方法 = 一次 Call
class CoordinatorRpcClient final : public ICoordinator {
public:
    explicit CoordinatorRpcClient(std::string_view address, rpc::ClientOptions options = {});
};
```

**关键语义**：
- 失败：`rpc::Status` 映射到 `distributed::Status`（`kUnavailable`/`kProtocolError`），映射表集中在一处；
- 超时：沿用 `ClientOptions.timeout`（`rpc/client.hpp:122`），不新增重试——"已发出未收到响应"的重试
  仍由调用方 `QueryLock` 对账（现有语义，`transport.hpp:30-31`）；
- 脑裂：协调者仍是单点，不做多副本；`epoch`/fencing 靠持久化（见 4.2.3）而非 quorum。

**与现有代码关系**：改造 `transport.cpp`（1157 行 → 预计 300 行适配器）；保留 `CoordinatorServer`
的 `Stop()` 连接管理思路，换成 `rpc::Server` 的生命周期。
**风险**：`rpc` 协议无 metadata/版本协商（实测 `rpc/protocol.hpp` 无 `metadata` 命中），
`LeaseView` 需要 `reflect` 支持——**先做 1 个方法的原型再全量迁移**。

### 4.2 协调层（Layer 1）：注册发现、选主、租约、锁、配置

#### 4.2.1 元数据存储 `IMetadataStore`（新增，协调层的底座）

```cpp
struct ValueView { std::string key, value; std::uint64_t revision{0}; };  // revision 全局单调

struct WatchEvent {
    enum class Kind : std::uint8_t { kPut, kDelete };
    Kind kind{Kind::kPut};
    std::string key, value;          // Delete 时 value 为空
    std::uint64_t revision{0};
};

class IMetadataStore {
public:
    virtual ~IMetadataStore() = default;
    virtual Result<ValueView> Get(std::string_view key) const = 0;
    // expected_revision == 0 无条件写；否则 CAS，冲突返回 kConflict。
    virtual Result<std::uint64_t> Put(std::string_view key, std::string_view value,
                                      std::uint64_t expected_revision) = 0;
    // 长轮询：返回 revision > since 的事件；空 vector + kOk 表示超时（非错误）。
    virtual Result<std::vector<WatchEvent>> Watch(std::string_view prefix,
                                                  std::uint64_t since_revision,
                                                  Duration timeout) = 0;
    virtual std::uint64_t Revision() const = 0;
};
```

**实现落点**：`Coordinator` 内新增 `m_kv`（`key → (value, revision)`）与有界 `m_changes`
（环形缓冲，默认保留 10k 条/60s）；`Watch` 用 `condition_variable` 等待，锁外拷贝。
持久化可选：用 `storage::KV`（`kv.hpp:126`）做快照 + `storage::Watcher`（`watch.hpp:77`）
做变更日志，重启后 `Replay`（`:107`）到 `since_revision`。
**关键语义**：
- 线性化点仍是 `Coordinator::m_mutex`；`revision` 与 `LeaseId` 同源递增，保证跨对象单调；
- Watch 是**至少一次**投递，客户端按 `revision` 去重（与 `storage::Watcher` 的游标语义一致）；
- 脑裂：单协调者不存在脑裂；协调者重启后 `revision` 从持久化快照恢复，若快照丢失则拒绝启动
  （**fail-stop 优于静默回退**，这是对 fencing 回退问题的修复）。
**与现有代码关系**：新增接口；`Coordinator` 实现之；`CoordinatorRpcClient` 增加 `Get/Put/Watch`
三个方法（对应 3 个新 op，若走 rpc 则零新增 op）。

#### 4.2.2 注册与发现（改造 `ServiceRegistry`）

```cpp
class ServiceRegistry {
public:
    Result<Registration> Register(std::string service, NodeInfo node, Duration ttl);
    std::vector<ServiceInstance> Lookup(std::string_view service) const;   // 保留
    Result<std::vector<ServiceInstance>> LookupPage(std::string_view service,
                                                    std::size_t offset, std::size_t limit) const;

    // 新增：服务变更订阅（内部 Watch 前缀 "svc/<service>/"）
    using ChangeFn = std::function<void(const std::vector<ServiceInstance>&)>;
    std::uint64_t Subscribe(std::string_view service, ChangeFn cb);
    void Unsubscribe(std::uint64_t id);
};
```

**关键语义**：`Subscribe` 首次回调立即给全量快照，之后增量；断线重连后从本地 `revision` 续订，
事件重复由 `revision` 去重；回调在后台线程执行，必须短小（沿用现有 `OnLost` 约束）。
**与现有代码关系**：改造 `registry.cpp:156-205` + 内部加一个 `Watcher` 线程（或复用 `LeaseSession`
的调度器）；`Lookup` 保留全量语义，`LookupPage` 新增。
**待确认**：`Watch` 长轮询的 timeout 与 `CoordinatorClient` 的 `io_timeout`（默认 2000ms，
`transport.hpp:99`）需要区分——长轮询要单独设 30s 级超时，不能复用请求超时。

#### 4.2.3 选主（保留 + 增加 Observer）

```cpp
class LeaderElection {
public:
    Result<Candidate> Join(std::string election, NodeInfo node, Duration ttl,
                           CandidateOptions options = {});   // 保留
    // 新增：只观察不参选，避免每个进程都建候选租约
    Result<LeaderObserver> Observe(std::string election, LeaderChangeFn cb);
};
```

**关键语义**：leader 判定仍由协调者单点做（最早 `LeaseId` 胜，`coordinator.cpp:175` 重算）；
`epoch` 必须持久化（随 `MetadataStore` 快照），否则重启后旧主恢复会与新主同时自认。
**与现有代码关系**：`election.cpp:212` 基本不动；`Observe` 复用 `Candidate::Impl::UpdateCache`
（`election.cpp:37`）的缓存逻辑，去掉续约线程。
**待确认**：是否引入"租约剩余时间下限"（如 leader 必须在剩余 < ttl/3 时主动降级），
现有实现没有，建议加为可选项。

#### 4.2.4 分布式锁（保留 + 两个易用性补丁）

```cpp
class DistributedLock {
public:
    Result<LockHandle> Acquire(std::string key, NodeId owner, Duration ttl,
                               LockOptions options = {});           // 保留
    // 新增：带 deadline 的阻塞获取（Watch 唤醒，不轮询）；批量续约降 RTT
    Result<LockHandle> AcquireFor(std::string key, NodeId owner, Duration ttl,
                                  Duration wait, LockOptions options = {});
    Outcome RenewAll(std::span<LockHandle* const> handles);
};
```

**关键语义**：`AcquireFor` 超时返回 `kConflict`（不是 `kUnavailable`）；等待期间持有者释放/过期
由 Watch 唤醒，唤醒丢失时退化为 50ms 轮询兜底；`RenewAll` 部分失败返回首个错误，成功项已续约。
**与现有代码关系**：`lock.cpp:156` 保留；`AcquireFor` 在 facade 层组合 `Acquire` + `Wait`；
`RenewAll` 需要 `ICoordinator` 批量接口（或客户端侧并行调用）。
**fencing 修复**：`m_fencing` 必须随快照持久化，并在恢复时校验"不得小于快照值"。

### 4.3 应用层（Layer 2）：消息、限流、追踪

#### 4.3.1 消息 `MessageBus`（复用 `storage::Queue` + `rpc` 流）

```cpp
struct Message {
    std::string topic;
    std::uint64_t offset{0};       // topic 内单调，用于去重/断点续传
    std::string payload;
    std::uint64_t timestamp_ms{0};
    std::string producer;          // NodeId
};

class MessageBus {
public:
    static Result<std::unique_ptr<MessageBus>> Open(const MessageBusOptions& options);
    Result<std::uint64_t> Publish(std::string_view topic, std::string_view payload);
    Result<Message> Consume(std::string_view topic, std::string_view consumer,
                            Duration timeout);                  // 长轮询
    Outcome Ack(std::string_view topic, std::string_view consumer, std::uint64_t offset);
    Result<std::uint64_t> Head(std::string_view topic);         // 当前末尾 offset
};
```

**实现落点**：本地持久化直接用 `storage::Queue`（`queue.hpp:81`，`Append:93`/`Consume:97`/`Ack:101`）；
跨节点投递用协调者的 `MetadataStore` 发现订阅者，再走 `rpc::ServerStream` 推送（或先做"拉模式"，
只依赖 `Queue` + 协调者，零新增传输）。
**关键语义**：至少一次（Ack 前重投），分区内有序（`offset`），跨分区无序；重复消费由业务按
`(producer, offset)` 幂等；消费端崩溃 → 游标不推进 → 重投；队列满 → 阻塞（`storage::Queue`
既有背压语义，`watch.hpp:46` 同款）。
**与现有代码关系**：新增 facade，复用 `storage::Queue`；不新造 broker。对外 IoT 场景可直接用
`bento::net::MqttBroker`（`mqtt.hpp:500`），两者不必合并。
**待确认**：`storage::Queue` 是单机文件队列，跨机复制需要额外设计（建议一期只做单机 + 协调者路由）。

#### 4.3.2 限流 `RateLimiter`（本地桶 + 协调者配额租约）

```cpp
class RateLimiter {
public:
    struct Options {
        std::uint32_t local_burst{1000};       // 本地令牌桶容量
        double local_rate_per_sec{100.0};      // 本地补充速率
        Duration quota_ttl{1000};              // 从协调者领取的配额有效期
        bool fail_open{true};                  // 协调者不可达时是否放行
    };

    // permits 为本次请求消耗的令牌数；返回 kOk 表示放行，kConflict 表示限流。
    Outcome Acquire(std::string_view key, std::uint32_t permits = 1);
};
```

**两层设计**：本地令牌桶（`std::atomic` + CAS，无 RTT）先放行；桶空时向协调者申请一批配额
（`Put("quota/<key>/<node>", "remaining", expected_revision)` + TTL），配额租约到期未用完即作废
（宁可少放行，不可超发）。协调者不可达时按 `fail_open` 降级。
**与现有代码关系**：新增；令牌桶可复用 `concurrency` 的原子设施，不需要新线程；协调者侧复用
`MetadataStore` 的 CAS，不新增原语。HTTP 网关的 `RateLimitMiddleware`（`app/gateway.hpp:455`）
可以接这个 facade，替换现有的进程内回调。

#### 4.3.3 追踪 `TraceContext`（轻量，不引 OTel SDK）

```cpp
struct TraceContext {
    std::uint64_t trace_id_hi{0}, trace_id_lo{0}, span_id{0};
    std::uint8_t flags{0};          // bit0: sampled
    bool valid() const noexcept;
};

class Tracer {
public:
    // 无父上下文时新建 trace；有父时续 span。
    TraceContext StartSpan(std::string_view name, const TraceContext* parent = nullptr);
    void EndSpan(const TraceContext& span, Duration elapsed, bool error);
};
```

**实现落点**：`trace_id` 用 `os` 的随机源；采样率默认 1%；`EndSpan` 写
`metrics::Registry`（`metrics.hpp:471`）的直方图（`Histogram:358`），标签为 `method`/`status`；
日志侧通过线程局部 `TraceContext` 注入 `trace_id`（现有 log 模块加一个字段即可）。
**关键语义**：跨进程传播需要 rpc 帧头扩展——**实测 `rpc/protocol.hpp` 无 metadata**，
一期只做进程内 + 日志关联；二期在 rpc 帧头加 24 字节 TLV（需同步改两端）。
**与现有代码关系**：新增；导出复用 metrics，不引入 OpenTelemetry（外部依赖与嵌入式交付形态冲突）。

---

## 5. 落地顺序

| 阶段 | 内容 | 依赖 | 预计人天 | 风险 |
|---|---|---|---|---|
| **P0** | `NodeId` + `TraceContext` + `CoordinatorStats` 接入 `metrics::Registry` | 无 | 1.5 | 低 |
| **P1** | `IMetadataStore`（KV + revision + Watch 长轮询）+ `Coordinator` 快照持久化（`storage::KV`），修复 fencing/epoch 重启回退 | P0 | 4 | 中（快照一致性） |
| **P2** | `ServiceRegistry` Watch + 本地缓存 + 分页；`AcquireFor`/`RenewAll` | P1 | 3 | 中（长轮询超时） |
| **P3** | `ConfigCenter`（`MetadataStore` + `config::ConfigStore` 无锁读） | P1 | 3 | 低 |
| **P4** | `MessageBus`（`storage::Queue` + 协调者路由） | P1/P2 | 3 | 中（跨机复制） |
| **P5** | `RateLimiter`（本地桶 + 配额租约） | P1 | 2 | 低 |
| **P6** | rpc 传输替换手写 TCP（保留兼容层） | P1 | 3 | 高（reflect 覆盖） |
| **P7**（可选） | etcd 适配器（`ICoordinator` 第二实现） | P1 | 5 | 中（外部依赖） |

合计：**P0–P5 约 16.5 人天**；含 P6/P7 约 24.5 人天。建议先做 P0+P1（一周内可验证），
因为 fencing 重启回退是唯一会**静默破坏正确性**的问题，优先级高于任何新功能。

---

## 6. 与服务网格 / K8s 的边界

| 能力 | 本项目可替代 | 必须依赖外部 | 建议 |
|---|---|---|---|
| 服务发现 | 无 K8s 的单机/小集群、嵌入式场景 | K8s 生产环境用 Endpoints/DNS 更成熟 | 保留 `ServiceRegistry`，K8s 下用 Downward API 注入实例 id，不重复造轮子 |
| 选主 | 单集群、可接受秒级切换 | 跨可用区强一致选主需 etcd/consul | 本项目只做"单协调者 + 租约"，明确不承诺分区容错 |
| 分布式锁 | 任务去重、滚动发布互斥 | 跨机房关键路径建议 etcd + fencing | 保留 `DistributedLock`，把 `ICoordinator` 作为 etcd 适配点 |
| 配置同步 | 进程内热更新 + 小集群广播 | 大规模配置中心（Apollo/Nacos） | `ConfigCenter` 只做轻量版，不替代专业配置中心 |
| 消息 | 单机/小集群任务队列 | Kafka/Pulsar 级吞吐与持久化 | `MessageBus` 定位为"够用"，不对标 Kafka |
| 限流 | 单服务/单集群配额 | 全局限流需 Redis/网关 | 本地桶 + 配额租约已能覆盖大部分场景 |
| 追踪 | 进程内 span + Prometheus 指标 | 全链路追踪需 OTel collector/Jaeger | 先做日志关联，二期再考虑 OTel 导出 |
| mTLS / 身份 | 无 | SPIFFE/Istio/Linkerd | 明确边界：本项目只做 `NodeId` 标识，不做认证；跨机必须套 TLS/VPN |
| 流量治理 | 无（熔断/重试/金丝雀） | 服务网格 | 不在本项目范围，避免重复实现 |

**一句话边界**：这是"嵌入式协调原语 + 够用的应用层设施"，**不是**服务网格的替代品；价值在无外部依赖、
可单进程嵌入、语义可测试。需要 Raft 级容错时，把 `ICoordinator`/`IMetadataStore` 换成 etcd 客户端，
facade 不动（`user/docs/09-distributed.md:312` 也这样定位）。

---

## 附：本次分析的验证方式与未决项

- 所有行号来自 `grep -n`/`sed -n` 实测；文档中的性能数字只引用台账/模块文档，未复跑。
- 「模块外零引用」是 `grep -rln 'go2cpp/distributed'` 的实测结果，不含构建产物。
- 未决项：① `reflect` 能否序列化 `LeaseView`/`ElectionState`（P6）；② `storage::Queue` 跨机复制（P4）；
  ③ 长轮询与 `io_timeout` 冲突（P1）；④ 快照落盘频率与 fsync（P1）。
- 本轮只新增本文件，未改代码、未 commit；工作区已有他人未提交改动
  （`collab/acceptance.md`、`src/bento/net/crypto.cpp` 等），与本报告无关。
