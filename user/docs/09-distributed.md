# 分布式设施（交接文档）

> **定位**：三个基础原语——服务发现、分布式锁、选主——共享同一个**租约表**与同一个协调者。
> 可进程内嵌入，也可经 TCP 暴露给多进程。
> **源码**：`include/go2cpp/distributed/`、`src/distributed/`　**构建**：`cmake/Go2CppDistributed.cmake`（目标 `go2cpp_distributed`）
> **测试**：`tests/distributed/`（10 个可执行，ctest 标签 `distributed`，多进程用例另带 `long`）
> **设计全文**：[distributed.md](../../docs/modules/distributed.md)（语义、故障模型、API、性能基线）
> **汇编证据**：[asm-distributed.md](../../docs/reports/asm-distributed.md)（objdump、callgrind、vs etcd）

## 1. 这个模块是什么

一句话：**用一张租约表同时回答「谁在线」「谁持有这把锁」「谁是主」。**

| 原语 | 入口 | 语义 |
|---|---|---|
| 服务发现 | `ServiceRegistry` | 服务名 → 存活实例列表；心跳保活，租约过期自动剔除 |
| 分布式锁 | `DistributedLock` | key → 唯一持有者 + 租约；续约、过期释放、fencing token 单调 |
| 选主 | `LeaderElection` | 存活候选者中**租约编号最小（最早加入）**者为主；主失效自动重选，epoch 单调 |

三者不是三套状态机：服务实例、锁、选举候选者都是**同一条租约**（`LeaseId` + `deadline` + `owner` + `LeaseKind`），
协调者只维护这一张表，所以「谁在什么时候持有哪个资源」只有一个权威答案。

边界一句话：**这是「租约 + fencing + 过期回收」的嵌入式原语，不是 etcd**——单点、内存态、
无持久化、无 Raft、无 watch、无认证（见 §7）。

## 2. 架构 / 数据流

```
                 ┌──────────────────────────────┐
                 │        Coordinator           │  单进程权威状态机
                 │  m_leases / m_services       │  单 mutex + 惰性过期
                 │  m_locks  / m_elections      │  后台清扫线程（默认 10ms）
                 │  m_fencing（永不清零）        │
                 └───────────┬──────────────────┘
              ICoordinator   │  （本地直接调用 / 远程走 TCP）
        ┌────────────────────┼─────────────────────┐
        │                    │                     │
 ServiceRegistry      DistributedLock       LeaderElection
   Registration          LockHandle            Candidate
 （心跳 + 本地租约）    （心跳 + fencing）      （心跳 + leader 缓存）
        │                    │                     │
        └──────────── 同一套 detail::LeaseSession ─┘
                             │
                 ┌───────────┴───────────┐
                 │  CoordinatorServer    │  TCP：u32_be 长度 + 二进制帧
                 │  CoordinatorClient    │  实现 ICoordinator
                 └───────────────────────┘
```

### 2.1 两种部署形态

| 形态 | 组成 | 适用 |
|---|---|---|
| 本地 | 多线程共享一个 `Coordinator` 对象 | 单进程内模拟、嵌入使用、单元测试 |
| 远程 | `CoordinatorServer` 暴露 + `CoordinatorClient` 实现 `ICoordinator` | 多进程/多机；三个 facade 原样复用 |

切到远程只换构造参数（`ServiceRegistry remote_registry(client)`），**facade 代码不变**——
这是 `ICoordinator` 抽象的全部意义。

### 2.2 过期是惰性的（交接重点）

查询路径自己用 `Clock::now()` 过滤过期记录；后台清扫线程只负责**回收内存**与**推进选举任期**。
所以：

- 清扫线程被调度延迟时，外部**不可能**拿到过期资源；
- 测试可以关掉清扫线程，用同步 `SweepExpired()` 精确控制时间边界。

### 2.3 线程模型

| 组件 | 线程 | 说明 |
|---|---|---|
| `Coordinator` | 锁内线性化 + 1 条清扫线程 | 单 `m_mutex`；`Start()` 起清扫，`Stop()` join |
| `CoordinatorServer` | 1 accept + 每连接 1 线程 | 连接登记在 `relay::Hub`，`Stop()` 经 Hub 一次性关闭 |
| `CoordinatorClient` | 调用方线程 | 所有请求一把互斥锁串行化（一个连接不能并发写帧） |
| `Registration`/`LockHandle`/`Candidate` | 各自心跳线程（`relay::Heartbeat`） | `ttl/3` 续约；句柄析构 = 停止心跳 + 释放（锁/候选） |

要更高吞吐就按线程/句柄多建几个 `CoordinatorClient` 实例（客户端是串行的）。

### 2.4 时间口径

跨进程返回值全是**剩余毫秒**（`remaining`），不是绝对时间点——`steady_clock` 的 epoch
跨机器不保证一致。客户端用 `remaining` 算本地 deadline，只用于快速失败；**持有权的最终判定在协调者**。

## 3. 对外 API（关键接口）

### 3.1 协调者

```cpp
class ICoordinator {
    virtual Result<LeaseView> RegisterService(const ServiceRegistration& registration) = 0;
    virtual Result<LeaseView> RenewLease(LeaseId lease, std::string_view owner) = 0;
    virtual Outcome          ReleaseLease(LeaseId lease, std::string_view owner) = 0;
    virtual std::vector<ServiceInstance> LookupService(std::string_view service) const = 0;
    virtual std::vector<std::string>     ListServices() const = 0;
    virtual Result<LeaseView> AcquireLock(std::string_view key, std::string_view owner,
                                          Duration ttl) = 0;
    virtual std::optional<LockInfo> QueryLock(std::string_view key) const = 0;
    virtual Result<LeaseView> JoinElection(std::string_view election, std::string_view candidate,
                                           Duration ttl) = 0;
    virtual ElectionState QueryElection(std::string_view election) const = 0;
    virtual std::size_t SweepExpired() = 0;
    virtual CoordinatorStats Stats() const = 0;
};

class Coordinator final : public ICoordinator {
    explicit Coordinator(CoordinatorOptions options = {});
    void Start();    // 起后台清扫线程
    void Stop();
};
```

### 3.2 三个 facade

```cpp
explicit ServiceRegistry(ICoordinator& coordinator, ServiceRegistryOptions options = {});
Result<Registration> Register(std::string service, std::string instance, std::string endpoint,
                              Duration ttl = Duration::zero());   // 空 ttl = 默认值
std::vector<ServiceInstance> Lookup(std::string_view service) const;
std::vector<std::string> Services() const;
std::optional<ServiceInstance> Get(std::string_view service, std::string_view instance) const;

Result<LockHandle> Acquire(std::string key, std::string owner, Duration ttl,
                           LockOptions options = {});             // 非阻塞，占用返回 kConflict
std::optional<LockInfo> Query(std::string_view key) const;
std::vector<LockInfo> Locks() const;

Result<Candidate> Join(std::string election, std::string node, Duration ttl,
                       CandidateOptions options = {});            // 幂等：重复加入=原地续约
ElectionState State(std::string_view election) const;
std::optional<LeaderInfo> Leader(std::string_view election) const;
std::vector<std::string> Elections() const;
```

### 3.3 句柄

```cpp
class Registration {
    Outcome Renew();                 // 手动续约（默认心跳已开）
    void Stop();                     // 注销并停心跳
    void OnLost(std::function<void()> callback);   // 租约丢失回调
};
class LockHandle {
    std::uint64_t fencing_token() const;   // 单调、永不复用，必须传给下游
    Outcome Renew();
    void Release();                        // 幂等；析构也 Release
    void OnLost(std::function<void()> callback);
    bool valid() const;  bool lost() const;
};
class Candidate {
    bool is_leader() const;                // 心跳缓存（最终一致）
    std::string leader_id() const;
    std::uint64_t epoch() const;           // 选主场景的 fencing token
    Outcome Refresh();                     // 直查协调者，拿强一致判断
    Outcome Renew();
    void Leave();
    void OnLeaderChange(std::function<void(const std::optional<LeaderInfo>&)> callback);
};
```

### 3.4 结果类型

```cpp
enum class Status : std::uint8_t {
    kOk, kInvalidArgument, kNotFound, kConflict, kExpired,
    kNotOwner, kUnavailable, kProtocolError, kClosed,
};
template <typename T> struct Result { T value; Status status; std::string message; bool ok() const; };
struct Outcome { Status status; std::string message; bool ok() const; };
struct LeaseView {
    LeaseId id; LeaseKind kind; std::string name, instance, owner, endpoint;
    Duration ttl, remaining;
    std::uint64_t fencing;    // 锁：fencing token；选举：任期号
    bool leader;              // 选举：本租约是否当前 leader
    std::string leader_id; std::uint64_t epoch;
};
```

**不用异常**：锁被占用、租约过期都是正常业务分支，不是编程错误。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"
#include <chrono>
#include <iostream>

using namespace go2cpp::distributed;
using namespace std::chrono_literals;

int main() {
    Coordinator coordinator;          // 本地权威状态机
    coordinator.Start();
    ServiceRegistry  registry(coordinator);
    DistributedLock  locks(coordinator);
    LeaderElection   election(coordinator);

    auto registration = registry.Register("order-api", "pod-7", "10.0.0.7:9000", 3s);
    if (!registration.ok()) return 1;
    for (const auto& instance : registry.Lookup("order-api")) {   // 建连/更新 LB
        std::cout << instance.instance << " @" << instance.endpoint << "\n";
    }

    auto lock = locks.Acquire("job:42", "worker-3", 30s);
    if (!lock.ok()) { std::cout << "lock busy: " << lock.message << "\n"; return 1; }
    std::uint64_t token = lock.value().fencing_token();   // 单调，下游拒绝更小的 token
    std::cout << "fencing=" << token << "\n";
    // ... 临界区 ...
    lock.value().Release();

    auto candidate = election.Join("primary", "node-a", 5s);
    if (candidate.ok() && candidate.value().is_leader()) {
        std::cout << "I am leader, epoch=" << candidate.value().epoch() << "\n";
    }
    coordinator.Stop();
    return 0;
}
```

链接：`target_link_libraries(app PRIVATE go2cpp::distributed)`。要点：`Coordinator` 必须先 `Start()`
（否则没有清扫线程，过期记录不会被回收）；句柄析构会自动释放，但**不要依赖析构顺序**——
`Coordinator` 必须比所有句柄活得久。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/distributed/types.hpp`（全 inline） | 229 | `LeaseId`/`Status`/`Outcome`/`Result`/`LeaseView`/各类快照结构 |
| `include/go2cpp/distributed/coordinator.hpp` / `coordinator.cpp` | 269 / 629 | `ICoordinator`、`Coordinator`：租约表、加锁线性化、惰性过期、清扫、fencing |
| `include/go2cpp/distributed/lease_session.hpp` / `lease_session.cpp` | 138 / 229 | 句柄共用的续约会话（心跳 + 本地兜底租约 + `lost` 状态） |
| `include/go2cpp/distributed/registry.hpp` / `registry.cpp` | 159 / 207 | `ServiceRegistry` + `Registration` 心跳句柄 |
| `include/go2cpp/distributed/lock.hpp` / `lock.cpp` | 154 / 192 | `DistributedLock` + `LockHandle`（fencing、续约、释放） |
| `include/go2cpp/distributed/election.hpp` / `election.cpp` | 156 / 272 | `LeaderElection` + `Candidate`（leader 缓存、epoch、回调） |
| `include/go2cpp/distributed/transport.hpp` / `transport.cpp` | 158 / 1157 | `CoordinatorServer`/`CoordinatorClient`：14 个操作码、帧编解码、连接管理 |
| `cmake/Go2CppDistributed.cmake` | — | `go2cpp_distributed` + 9 个测试 + `go2cpp_distributed_bench` |

依赖复用：`relay::Heartbeat`（续约）、`relay::Lease`（本地兜底到期）、`relay::Hub`（服务端连接表）、
`network`（TCP socket）——没有另造定时器/连接表。

## 6. 性能数据

来自 [asm-distributed.md](../../docs/reports/asm-distributed.md)（WSL2 / Ryzen 9 8945HX / g++ 13.3 /
Release `-O3 -DNDEBUG` / `taskset -c 6,8`），数字原样引用：

| 指标 | 数值 |
|---|---|
| `Coordinator::RenewLease` 静态画像 | **291 指令 / 29 call / 296 B 栈帧 / 0 原子 / 2 个 mutex 调用点** |
| `Coordinator::AcquireLock` / `ReleaseLease` / `JoinElection` | 370 / 39 / 472 B；127 / 13 / 72 B；356 / 39 / 472 B（均为 0 原子） |
| 动态指令（callgrind Ir） | renew **837 条/次**、acquire+release **2490 条/次**、facade acquire+release **4380 条/次** |
| 动态指令构成 | 字符串赋值/拷贝占 renew **~44%**，mutex 仅 **~7%** |
| 本地续约（批量 / 单次含计时） | **65 ns / 79.6 ns** |
| 本地 acquire+release（直连 / facade） | **166 ns / 327 ns** |
| 同机 loopback TCP | renew **26.3 µs**、acquire+release **45.7 µs** |
| etcd 3.4（C++ keep-alive HTTP → gateway） | 租约读 **401 µs**、写（WAL fsync）**1104 µs** |
| 本地 vs etcd / TCP vs etcd | **5000–6200×** / **15–24×**（差距来自网络栈 + HTTP/JSON + Raft + fsync，不是同口径「实现更快」） |
| 模块级 mutex 调用点 | 5 个原语目标文件共 **139 个**（coordinator 49、lease_session 39、election 24、registry 15、lock 12） |
| `lock` 前缀原子指令 | 仅 `LeaseSession::Start` **11 条**（shared_ptr 引用计数，句柄创建时一次），热路径 **0 条** |

模块文档 §7 的 bench 基线（单轮采样，仅作量级参考）：注册+释放服务（本地）mean **248 ns** /
p50 225 ns；获取+释放锁（本地）mean **359 ns** / p50 290 ns；查询 1000 实例 p50 **155 µs**
（O(n) 全量拷贝，≈162 ns/条）；清扫 20000 条过期租约 p99 **6.74 ms**（337 ns/条）。

解读：**吞吐不是瓶颈，尾延迟才是**——锁竞争 p50 233 ns（4 线程）但 p99 38.7 µs；
单 mutex 是刻意的（正确性只靠一个线性化点），要降尾延迟就分片，而不是加 CAS。

## 7. 边界与已知限制

1. **单点**：协调者进程退出后所有租约丢失；客户端必须把「重新注册」写成幂等操作。无持久化/快照。
2. **无认证、无 TLS**：默认只监听 `127.0.0.1`；跨机部署必须套外层 TLS/VPN。能连上端口的人就能操作协调者。
3. **单锁**：协调者一把 mutex，16 线程锁竞争 p99 ≈ 70 µs；更高并发需要分片或读写锁（接口不变）。
4. **查询 O(n)**：`Lookup` 每次全量拷贝（1000 实例 162 µs）；大规模发现需要客户端缓存 + 增量 watch（**未实现**）。
5. **无 watch**：服务上下线靠轮询；leader 变化只有候选者本地 `OnLeaderChange` 回调。
6. **时钟**：判定基于协调者本进程 `steady_clock`；跨机时 `remaining` 只是提示，**安全边界是 fencing token**。
7. **不双持锁优先**：协调者判定过期就允许新持有者进入，旧持有者可能短暂仍认为有效——所以每次写操作都要
   带 `fencing_token()` 让下游校验单调，**不能只依赖 `valid()`**。
8. **选举规则按「最早租约」**而非字面「剩余时间最短」：按剩余时间排序会让 leader 随续约抖动漂移，
   甚至出现两节点交替自认主的窗口。
9. **无版本协商**：TCP 协议服务端与客户端必须同一构建，跨版本部署要重新编译。

## 8. 测试与验证方法

```bash
cmake -S . -B .build-dist -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_BENTO_STANDALONE=ON -DGO2CPP_DISTRIBUTED_TESTS=ON
ninja -C .build-dist go2cpp_distributed
ctest --test-dir .build-dist -L distributed --output-on-failure
./.build-dist/go2cpp_distributed_bench      # 只构建，不注册 ctest
```

| 测试 | 覆盖重点 |
|---|---|
| `go2cpp_distributed_registry_test` | 10 实例注册/发现、心跳保活、过期剔除、主动注销、幂等/抢占、lost 回调、move 语义 |
| `go2cpp_distributed_lock_test` | 互斥、自动/手动续约、超时释放、fencing 单调、错误 owner、幂等释放、强制回收后的 lost |
| `go2cpp_distributed_election_test` | 先到者为主、主动退出/过期重选、重新加入排尾、全员退出无主、epoch 单调、变化回调 |
| `go2cpp_distributed_random_test` | 1500 步随机操作 + 四条不变量（每 25 步校验，固定种子可复现） |
| `go2cpp_distributed_concurrent_test` | 64 线程 × 300 次锁竞争（临界区人数恒 1）、8 写/4 读服务搅拌、3 节点选主唯一性 |
| `go2cpp_distributed_transport_test` | server/client、断线重连、停机/重启、2001 实例大响应、4 客户端并发抢锁、未知操作码/超大帧 |
| `go2cpp_distributed_scenarios_test` | 7 个真实场景：注册中心崩溃剔除、任务锁超时接管、三节点主备、网络分区、滚动发布、协调者重启、+Hub |
| `go2cpp_distributed_multiprocess_test` | 3 进程选主 + SIGKILL 主进程后重选；4 进程 × 200 次抢锁（mmap 校验临界区人数） |

四条全局不变量（`test_random.cpp` 每 25 步检查）：① 发现不返回过期/重复实例；② 锁快照只有
`remaining > 0`，同 key token 严格单调；③ leader 必是候选者中租约编号最小者；④ 错误 owner 的
续约/释放被拒绝且不改变状态。

实测：`ctest -L distributed` **8/8 通过（9.29 s，含多进程 SIGKILL 重选；当时注册 8 个，现目录 10 个可执行，实际数量以 `ctest -N -L distributed` 为准）**；1e6 次压力 3 轮
`STRESS_OK`，fencing token 连续唯一（1..1000000），收尾 `locks=0`、`candidates=1`。

## 9. 交接注意

- **`ICoordinator` 是唯一的替换点**：要换 etcd，实现这个接口即可，三个 facade 不用动。
  新增原语先想「能不能用同一条租约表达」，否则会把一张表拆成三套状态机。
- **过期判定不要挪到清扫线程**：查询路径自己过滤是「清扫被延迟也不会发出过期资源」的前提；
  改成依赖清扫会引入一个隐蔽的一致性窗口。
- **fencing token 永不清零**：`m_fencing` 单调递增、释放/过期/重启都不回退。改任何回收逻辑都不能碰它。
- **`OnLost` 回调在心跳线程触发**：回调里不要做重活、不要析构句柄（会自 join）。回调必须快速返回。
- **`Stop()` 顺序**：先停句柄（`Registration::Stop`/`LockHandle::Release`/`Candidate::Leave`），
  再 `Coordinator::Stop()`，最后析构协调者。反过来会让心跳线程访问已销毁的协调者。
- **远程模式客户端串行**：`CoordinatorClient` 一把互斥锁，多线程共用会互相排队；按线程建实例。
- **协议无版本号**：改 `transport.cpp` 的 14 个操作码或字段顺序，必须同时升级服务端与客户端，
  否则 `kProtocolError`（不是静默错乱）。
- **未完成事项**：持久化/快照、Raft 多副本、watch、分页查询、mTLS、协调者分片。逐条见
  [distributed.md](../../docs/modules/distributed.md) §10。
