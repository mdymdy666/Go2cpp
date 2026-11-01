# 分布式设施（服务发现 / 分布式锁 / 选主）

> 模块路径：`include/go2cpp/distributed/`、`src/distributed/`
> 构建目标：`go2cpp_distributed`（`cmake/Go2CppDistributed.cmake`）
> 依赖：`relay`（`Lease` / `Heartbeat` / `Hub`，commit 225af5b 系列）+
> `network`（TCP 协调者）+ `Threads`
> 测试：`tests/distributed/`，ctest 标签 `distributed`（多进程用例另带 `long`）

## 1. 定位

三个基础原语，共享同一个**协调者**（`Coordinator`）：

| 原语 | 入口 | 一句话语义 |
|---|---|---|
| 服务发现 | `ServiceRegistry` | 服务名 → 存活实例列表；心跳保活，租约过期自动剔除 |
| 分布式锁 | `DistributedLock` | key → 唯一持有者 + 租约；续约，过期释放，fencing token 单调 |
| 选主 | `LeaderElection` | 候选者租约最早者为主；主失效自动重选，任期 epoch 单调 |

三者不是三套独立状态机：服务实例、锁、选举候选者都表示为**同一条租约**
（`LeaseId` + `deadline` + `owner` + 类别）。协调者只维护这一张租约表，
因此「谁在什么时候持有哪个资源」只有一个权威答案。

## 2. 架构

```
                 ┌──────────────────────────────┐
                 │        Coordinator           │  单进程权威状态机
                 │  m_leases / m_services       │  单 mutex + 惰性过期
                 │  m_locks  / m_elections      │  后台清扫线程（默认 10ms）
                 │  m_fencing（永不清零）        │
                 └───────────┬──────────────────┘
              ICoordinator   │  （本地直接调用）
        ┌────────────────────┼─────────────────────┐
        │                    │                     │
 ServiceRegistry      DistributedLock       LeaderElection
   Registration          LockHandle            Candidate
   （心跳 + 本地租约）    （心跳 + fencing）      （心跳 + leader 缓存）
        │                    │                     │
        └──────────── 同一套 detail::LeaseSession ─┘
                             │
                 ┌───────────┴───────────┐
                 │  CoordinatorServer    │  TCP（4 字节长度 + 二进制帧）
                 │  CoordinatorClient    │  实现 ICoordinator
                 └───────────────────────┘
```

- **本地模式**：多线程共享同一个 `Coordinator`（单进程内模拟/嵌入使用）。
- **远程模式**：`CoordinatorServer` 暴露协调者，`CoordinatorClient` 实现同一个
  `ICoordinator` 接口，三个 facade 原样复用（见 `test_transport.cpp`、`test_multiprocess.cpp`）。
- **依赖复用**：句柄的后台续约用 `relay::Heartbeat`（单线程 + 条件变量 + 取消），
  本地兜底用 `relay::Lease`（单调时钟 + 到期回调一次），服务端连接生命周期用
  `relay::Hub`（`CloseKey` 在锁外关闭全部连接）。没有另造定时器/连接表。

### 2.1 为什么「过期」是惰性的

查询路径自己按 `Clock::now()` 过滤过期记录；后台清扫只负责回收内存和推进
选举任期。因此清扫线程被调度延迟时，外部**不可能**拿到过期资源；反过来，
测试可以关掉清扫线程，用同步 `SweepExpired()` 精确控制时间边界。

### 2.2 时间口径

所有跨进程返回值都是**剩余毫秒**（`remaining`），不是绝对时间点：
`steady_clock` 的 epoch 跨机器不保证一致。客户端据 `remaining` 计算本地
deadline，只用于快速失败；持有权的最终判定在协调者。

## 3. 三个原语的语义

### 3.1 服务发现

- `Register(service, instance, endpoint, ttl)`：同 `(service, instance)` 且同
  owner 的重复注册是**幂等续约**（租约编号不变，endpoint 更新）；不同 owner
  视为旧实例被替换（新编号更大）。
- 句柄按 `ttl/3` 心跳；本地 `Lease` 兜底：一个完整 TTL 没成功续约就标记
  `lost()`，`valid()` 立即变 false。
- `Lookup` 按实例 id 字典序返回，过期项被过滤；`Services()` 只返回至少有一个
  存活实例的服务。
- 与 `relay::Hub` 的组合：Hub 管连接，注册中心管元数据，两者刻意不合并——
  一次租约抖动不应该连带关闭正在传输的流（场景见 `test_scenarios.cpp`）。

### 3.2 分布式锁

- `Acquire` 是原子的「检查 + 授予」，**非阻塞**：被占用返回 `kConflict`
  （`message` 带当前 owner），不排队、不等待。
- 每次授予分配 **fencing token**：同一 key 严格单调、永不复用。锁释放、
  过期、进程崩溃都不会让 token 回退。
- 续约窗口 `ttl/3`，短暂 GC/调度抖动不丢锁；连续失败或超过本地 deadline
  则 `lost()`。
- `Release` 幂等；错误 owner 的续约/释放返回 `kNotOwner`，不能把别人的锁顶掉。

**不丢锁 vs 不双持锁的取舍**：异步网络下两者不可能同时绝对成立。本实现
优先**不双持锁**：协调者一旦判定过期就允许新持有者进入；旧持有者的本地
`valid()` 立即（或最迟一个 TTL 内）变 false，且 token 更小。因此正确用法是
**每次写操作都带 `fencing_token()` 让下游校验单调**，而不是只依赖 `valid()`。

### 3.3 选主

- 规则：**存活候选者中租约编号最小（最早加入）者为主**。
- 关于「租约最短者为主」：本实现按**最早取得的租约**落地。若按剩余时间排序，
  续约时序抖动会让 leader 在两台机器之间漂移（谁先续约谁剩余更长），甚至出现
  两个节点交替自认为主的窗口；按编号排序是唯一稳定、可验证的解释。
- 主失效（进程崩溃/停止续约超过 TTL）→ 清扫时从候选集合移除并立即重算；
  每次 leader 实际变化（含「有主 → 无主」）`epoch` 加一。epoch 是选主场景的
  fencing token：旧主恢复后不会「记起」自己还是主。
- 重新加入的节点拿到更大的租约编号，排在队尾（先到先得的公平性）。
- `Candidate::is_leader()` 是心跳缓存（最终一致）；需要强一致判断用
  `LeaderElection::Leader()`（直查协调者）或 `Candidate::Refresh()`。
- `OnLeaderChange` 回调在心跳线程触发，参数为空表示无主。

## 4. 并发与故障模型

| 场景 | 行为 |
|---|---|
| 客户端线程卡死/GC | 本地租约到期 → `lost()`；协调者 TTL 后回收资源 |
| 进程崩溃（多进程） | TCP 连接断开；租约按 TTL 过期；锁/主自动释放/重选 |
| 协调者重启 | 内存状态丢失；客户端续约失败 → `lost()`；重新注册得到新编号 |
| 网络分区（客户端与协调者） | 心跳失败 → `lost()`；旧 token 被下游拒绝 |
| 短时丢包/调度抖动 | `ttl/3` 续约窗口吸收；不丢锁 |
| 并发抢锁/选主 | 协调者单 mutex 线性化；同一 key 最多一个有效授予 |

**保证的全局不变量**（`test_random.cpp` 每 25 步检查一次）：

1. 服务发现不返回过期实例、不返回重复实例；
2. 锁快照只会出现 `remaining > 0` 的记录，同一 key 的 token 严格单调；
3. 选举 leader 若存在，必是候选者中租约编号最小者，且候选者按编号升序；
4. 错误 owner 的续约/释放被拒绝且不改变状态。

## 5. 主要 API

```cpp
#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/registry.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/election.hpp"

using namespace go2cpp::distributed;

Coordinator coordinator;                    // 本进程权威状态机
ServiceRegistry registry(coordinator);
DistributedLock locks(coordinator);
LeaderElection election(coordinator);

// 服务发现
auto registration = registry.Register("order-api", "pod-7", "10.0.0.7:9000", 3s);
for (const auto& instance : registry.Lookup("order-api")) { /* 建连/更新 LB */ }

// 分布式锁（token 必须传给下游）
auto lock = locks.Acquire("job:42", "worker-3", 30s);
if (lock.ok()) {
    lock.value().fencing_token();   // 单调，下游拒绝更小的 token
    // ... 临界区 ...
}                                   // 析构 = Release

// 选主
auto candidate = election.Join("primary", "node-a", 5s);
if (candidate.ok() && candidate.value().is_leader()) { /* 主逻辑 */ }
```

远程模式（同一套 facade）：

```cpp
CoordinatorServer server(coordinator);      // 默认监听 127.0.0.1:0
server.Start();
CoordinatorClient client("127.0.0.1", server.port());
ServiceRegistry remote_registry(client);
```

## 6. 传输协议

- 帧：`u32_be(payload_len)` + payload；请求 `u8(op) + fields`，响应
  `u8(status) + str(message) + fields`；字符串 `u32_be(len) + bytes`。
- 操作码 14 个（register/renew/release/lookup/list/acquire/query/list/join/
  query/list/sweep/stats/ping）。**无版本协商**：服务端与客户端必须来自同一
  构建，跨版本部署需要重新编译。
- 安全边界：默认只监听 `127.0.0.1`；跨机部署需要外层 TLS/VPN。协议不做
  认证/授权，任何能连上端口的人都能操作协调者——这是**明确的边界**，不是遗漏。
- 客户端不自动重试「已发出但没收到响应」的请求：重试一次 `AcquireLock`
  可能让调用方误判自己是新持有者。正确做法是失败后 `QueryLock` 对账。
- 断线不立即撤销租约：一次 TCP 重传抖动不应让正在执行任务的进程瞬间丢锁；
  租约按 TTL 自然过期。
- 服务端：1 个 accept 线程 + 每连接 1 个处理线程；连接登记在 `relay::Hub`，
  `Stop()` 通过 Hub 一次性关闭。
- 客户端：所有请求一把互斥锁串行化（一个连接上不能并发写帧）；需要更高
  吞吐时按线程/句柄创建多个客户端实例。

## 7. 性能基线

环境：WSL2（Linux 6.x，x86_64），Release（`-O3 -DNDEBUG`），单机。
命令：`.build-bento-wsl/go2cpp_distributed_bench`（只构建、不注册 ctest）。
数据为单轮采样（bench 内每项 2000~20000 次），仅作量级参考。

| 用例 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| 注册+释放服务（本地） | 248 ns | 225 ns | 234 ns | 234 ns |
| 续约租约（本地） | 83 ns | 84 ns | 85 ns | 85 ns |
| 获取+释放锁（本地） | 359 ns | 290 ns | 300 ns | 486 ns |
| 查询 10 实例 | 1.08 µs | 814 ns | 871 ns | 1.54 µs |
| 查询 100 实例 | 10.5 µs | 9.48 µs | 20.6 µs | 25.6 µs |
| 查询 1000 实例 | 162 µs | 155 µs | 207 µs | 283 µs |
| 查询选举（2 候选） | 92 ns | 85 ns | 94 ns | 131 ns |
| 清扫 20000 条过期租约 | — | — | — | 6.74 ms（337 ns/条） |
| 锁竞争 1 线程 | 252 ns | 215 ns | 318 ns | 355 ns |
| 锁竞争 4 线程 | 1.93 µs | 233 ns | 5.92 µs | 38.7 µs |
| 锁竞争 16 线程 | 8.25 µs | 4.04 µs | 28.9 µs | 69.6 µs |
| TCP 注册+释放 | 238 µs | 222 µs | 384 µs | 447 µs |
| TCP 续约 | 143 µs | 131 µs | 237 µs | 283 µs |
| TCP 查询 2001 实例 | 1.35 ms | 1.30 ms | 1.88 ms | 2.30 ms |
| TCP 获取+释放锁 | 249 µs | 225 µs | 408 µs | 490 µs |

- 查询是 O(n) 全量快照拷贝（1000 实例 162 µs ≈ 162 ns/条），符合预期；
  大规模发现应配合客户端缓存/分页（未实现）。
- 锁竞争在 p50 上很低（233 ns @4 线程），但 p99 随线程数上升（38.7 µs @4、
  69.6 µs @16）：单 mutex 的尾延迟，不是丢锁/双持。
- TCP 往返 130~250 µs 主要是每连接一线程 + 客户端串行 + loopback 系统调用；
  不是协议本身的开销。**注意**：首个版本把帧头和负载分两次 `send`，在
  loopback 上触发 Nagle + 延迟 ACK，单次注册 87 ms；合并为一次发送并设置
  `TCP_NODELAY` 后降到 130 µs 量级（约 500×，见报告 §3）。

## 8. 测试

| 目标 | 覆盖 |
|---|---|
| `go2cpp_distributed_registry_test` | 10 实例注册/发现、心跳保活、过期剔除、主动注销、幂等/抢占、参数校验、手动续约、lost 回调、move 语义 |
| `go2cpp_distributed_lock_test` | 互斥、自动/手动续约、超时释放、fencing 单调、错误 owner、幂等释放、强制回收后的 lost 回调、Query |
| `go2cpp_distributed_election_test` | 先到者为主、主动退出重选、过期重选、重新加入排尾、全员退出无主、leader 变化回调、epoch 单调、参数校验 |
| `go2cpp_distributed_random_test` | 1500 步随机操作 + 四条不变量（固定种子可复现） |
| `go2cpp_distributed_concurrent_test` | 64 线程 × 300 次锁竞争（临界区人数恒 1）、8 写/4 读服务搅拌、3 节点选主唯一性、混合负载 |
| `go2cpp_distributed_transport_test` | server/client、断线重连、服务器停机/重启、2001 实例大响应、4 客户端并发抢锁、未知操作码/超大帧防御 |
| `go2cpp_distributed_scenarios_test` | 7 个真实场景：注册中心崩溃剔除、任务锁超时接管、三节点主备、网络分区、滚动发布、协调者重启、注册中心 + Hub 连接池 |
| `go2cpp_distributed_multiprocess_test` | 3 进程选主 + SIGKILL 主进程后重选；4 进程 × 200 次抢锁（mmap 共享内存校验临界区人数、fencing、错误数） |
| `go2cpp_distributed_bench` | 上表数据（只构建、不注册） |

运行：

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_BENTO_STANDALONE=ON -DGO2CPP_DISTRIBUTED_TESTS=ON
ninja -C .build-bento-wsl go2cpp_distributed
cd .build-bento-wsl && ctest -L distributed --output-on-failure
./go2cpp_distributed_bench
```

## 9. 复用与取舍（开源检索）

动手前检索过同类方案，结论如下（详见任务报告）：

| 方案 | 取舍 |
|---|---|
| etcd / Consul | 生产级，但需要独立部署、运维一套 Raft 集群；本项目是嵌入式 C++ 库，引入外部进程不符合交付形态 |
| Redis Redlock | 依赖 Redis 的租约；Martin Kleppmann 对 Redlock 的时钟/GC 假设有公开批评，且本任务不能引入 Redis |
| ZooKeeper | 更重（JVM + ZAB），同样需要独立运维 |
| 自研（本模块） | 复用项目已有 `relay::Lease`/`Heartbeat`/`Hub` 与 `network::Socket`，单进程协调者可嵌入、可 TCP 暴露；明确不做持久化与多副本共识 |

本实现**不声称**达到 etcd 的容错级别：协调者是单点（重启丢状态），没有
Raft/Paxos 复制。它的价值是把「租约 + fencing + 过期回收」这套不变量以
可测试、可嵌入的方式落地；需要跨机强一致时，应把 `Coordinator` 换成 etcd
客户端（`ICoordinator` 接口不变）。

## 10. 已知边界与后续

- **单点**：协调者进程退出后所有租约丢失；客户端必须把「重新注册」写成
  幂等操作。未实现持久化/快照。
- **无认证**：TCP 协议没有 TLS/鉴权，默认只监听回环。
- **单锁**：协调者用一把 mutex；锁竞争 p99 在 16 线程时 ~70 µs。更高并发
  需要分片或读写锁（接口不变）。
- **查询 O(n)**：`Lookup` 每次全量拷贝；1000 实例 162 µs。大规模场景需要
  客户端缓存 + 增量 watch（未实现）。
- **无 watch**：服务上下线/leader 变化靠客户端轮询或 `OnLeaderChange`
  （仅候选者本地回调）。
- **时钟**：协调者判定基于本进程 `steady_clock`；跨机部署时 `remaining`
  只是提示，安全边界是 fencing token。
- 选举规则按「最早租约」而非字面「剩余时间最短」解释（理由见 §3.3）。
