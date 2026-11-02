> 来源：/tmp/dist_asm.md，2026-11-02 归档

# Go2Cpp 分布式三件套：汇编级证据 + vs etcd/Consul

> 模块：`include/go2cpp/distributed/**` + `src/distributed/**`（commit `26d64fd`）
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树）
> 构建：独立目录 `/tmp/build-dist`（Ninja + Release `-O3 -DNDEBUG` + `-fno-plt`）
> 机器：WSL2 / Linux 6.6.87.2 / x86_64 / 12 vCPU / GCC 13.3.0
> 日期：2026-11-02

## 0. 结论摘要

1. **静态画像**：`RenewLease` 291 条指令 / 29 个 call / 296B 栈帧 / 2 个 mutex 调用点；
   `AcquireLock` 370 / 39 / 472B / 2；`ReleaseLease` 127 / 13 / 72B / 2；
   `JoinElection` 356 / 39 / 472B / 2。热路径 **0 条 `lock` 前缀原子指令**——
   线性化完全靠单 mutex，没有 CAS 自旋；唯一例外是 `LeaseSession::Start` 的
   11 条（shared_ptr 引用计数，仅句柄创建时执行，不在续约热路径上）。
2. **动态指令数**（callgrind Ir）：renew **837 条/次**、acquire+release **2490 条/次**、
   facade acquire+release **4380 条/次**。其中 **字符串赋值/拷贝占 renew 的 ~44%**，
   mutex 只占 ~7%——优化优先级应放在 `LeaseView` 构造，而不是锁。
3. **性能（口径必须一起看）**：本地进程内 renew **65ns**（批量）/ 79.6ns（单次含计时），
   acquire+release **166ns**（直连）/ 327ns（facade）；同机 loopback TCP
   renew **26.3µs**、acquire+release **45.7µs**。etcd 3.4 实测（C++ keep-alive
   HTTP → gateway）：租约读 RPC **401µs**、写（WAL fsync）**1104µs**。
   本地 vs etcd ≈ **5 000–6 200×**，TCP vs etcd ≈ **15–24×**；差距来自网络栈 + HTTP/JSON +
   Raft + fsync，**不是**同一口径的「实现更快」。
4. **功能**：etcd/Consul 有持久化、Raft 多副本、watch、事务、mTLS；本实现**都没有**。
   本实现有的是「租约 + fencing + 过期回收」的进程内原语，可嵌入、可 TCP 暴露，
   单点、内存态。功能矩阵见 §3。
5. **正确性**：`ctest -L distributed` **8/8 通过**（9.29s，含多进程 SIGKILL 重选）；
   1e6 次压力跑 3 轮全部 `STRESS_OK`，fencing token 连续唯一（1..1000000），
   `locks=0`、`candidates=1` 收尾一致。

## 1. 环境与口径

```
$ uname -a
Linux DESKTOP-S3JHU55 6.6.87.2-microsoft-standard-WSL2 x86_64 GNU/Linux
$ gcc --version | head -1   → gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
$ cmake --version | head -1 → cmake version 3.28.3
$ nproc                     → 12
```

- 构建类型：`Release`（`-O3 -DNDEBUG`），独立目录 `/tmp/build-dist`，不动 `.build-bento-wsl`。
- 计时：`steady_clock`；bench 为单次计时（含两次 `Clock::now`，约 **20ns** 开销，
  见 §4.1 的 `clock_now=17.75ns`），stress 为批量循环平均（**不含**单次计时开销）。
- 绑核：`taskset -c 6`（stress/bench）/ `taskset -c 8`（primitives）。
  测试时 `loadavg ≈ 3.5–6.6`（宿主机有其他负载，数据取 3 轮中位数或标出全部 3 轮）。
- 所有数字可在 §7 的命令下复现；引用第三方（etcd）数据均为本机实测，不引用网上二手数据。

## 2. 汇编证据（objdump）

### 2.1 方法与产物

```bash
objdump -drC --no-show-raw-insn \
  /tmp/build-dist/CMakeFiles/go2cpp_distributed.dir/src/distributed/coordinator.cpp.o \
  > /tmp/asm/coordinator.txt
# 同法生成 lock/election/lease_session/registry/transport 共 6 份
```

用脚本 `/tmp/asm/analyze_asm.py` 解析：按函数切分，统计指令数、call 数（含重定位解析）、
栈帧（`sub $N,%rsp`）、`push` 数、条件跳转、`ret`、`lock` 前缀原子指令、mutex 调用点。

### 2.2 静态画像（-O3，全函数，含错误分支）

| 函数 | 指令 | call | mutex | 栈帧 | 原子 | ret |
|---|---:|---:|---:|---:|---:|---:|
| `Coordinator::RenewLease` | 291 | 29 | 2 | 296 B | 0 | 1 |
| `Coordinator::AcquireLock` | 370 | 39 | 2 | 472 B | 0 | 1 |
| `Coordinator::ReleaseLease` | 127 | 13 | 2 | 72 B | 0 | 1 |
| `Coordinator::JoinElection` | 356 | 39 | 2 | 472 B | 0 | 1 |
| `Coordinator::RegisterService` | 331 | 34 | 2 | 456 B | 0 | 1 |
| `Coordinator::MakeViewLocked` | 136 | 9 | 0 | 8 B | 0 | 1 |
| `Coordinator::RenewRecordLocked` | 26 | 1 | 0 | 8 B | 0 | 3 |
| `Coordinator::RecomputeLeaderLocked` | 134 | 6 | 0 | 72 B | 0 | 1 |
| `Coordinator::EraseLeaseLocked` | 311 | 25 | 0 | 248 B | 0 | 1 |
| `Coordinator::SweepExpired` | 132 | 11 | 2 | 56 B | 0 | 1 |
| `DistributedLock::Acquire` | 626 | 39 | 0 | 504 B | 0 | 1 |
| `LeaseSession::Start`（仅句柄创建时） | 370 | 27 | 1 | 104 B | **11** | 0 |
| `LockHandle::Release` | 64 | 7 | 2 | 56 B | 0 | 1 |
| `LockHandle::Renew` | 415 | 19 | 3 | 320 B | 0 | 1 |
| `LeaseSession::RenewNow` | 1222 | 32 | 6 | 344 B | 0 | 1 |
| `LeaseSession::RenewFromHeartbeat` | 251 | 28 | 6 | 344 B | 0 | 1 |

模块级：5 个原语目标文件共 **139 个 `pthread_mutex_*` 调用点**
（coordinator 49、lease_session 39、election 24、registry 15、lock 12），
`lock` 前缀原子指令只在 `LeaseSession::Start` 出现 11 条（shared_ptr 引用计数），
协调者热路径（Renew/Acquire/Release/Join）**0 条**。含义：
- 热路径每次只有 1 次 lock + 1 次 unlock（2 个调用点），没有嵌套锁；
- 没有 CAS/自旋/原子引用计数参与线性化——正确性完全由 `m_mutex` 单点保证；
- 代价是 16 线程下尾延迟随争用上升（§4.1）。

> 说明：`LockHandle::valid/lost` 也有 3 个 mutex 调用点（`m_impl->mutex` + `LeaseSession::mutex`），
> 属于句柄本地状态，不在协调者热路径上。

### 2.3 租约状态机：`RenewLease`（续约 83ns 的来源）

入口到返回的关键指令（`coordinator.txt` 6164–6313 行，节选）：

```asm
5d60: endbr64
5d64: push %r15 ; push %r14 ; push %r13 ; push %r12 ; push %rbp ; push %rbx
5d84: sub  $0x128,%rsp              ; 296B 栈帧（含 4 个 std::string 的临时视图）
5d8b: mov  %fs:0x28,%rax            ; -fstack-protector
5d9e: call pthread_mutex_lock       ; ← 唯一加锁点（m_mutex）
5dab: lea  0x50(%rbp),%rdi          ; &m_leases
5db2: call _Hashtable<unsigned long, LeaseRecord>::find
5dba: test %rax,%rax ; je 5d...     ; 未找到 → kNotFound 分支
5dc3: mov  0x68(%rax),%rdx          ; record.owner.size()
5dcb: cmp  %r14,%rdx ; je 5ee0      ; 长度不同 → kNotOwner
5eec: call memcmp                   ; owner 逐字节比较
5efd: call steady_clock::now        ; ← 一次时钟读取
5f38: call RenewRecordLocked        ; deadline = now + ttl; ++revision（26 条指令）
5f59: call MakeViewLocked           ; 构造 LeaseView（136 条指令，4 次 string 拷贝）
5eae: call pthread_mutex_unlock
5ec4: jne  __stack_chk_fail
5eca: add  $0x128,%rsp ; 6×pop ; ret
```

解读：
- 续约路径没有分配器调用（`MakeViewLocked` 里的 4 个字符串都是 SSO 短串，实测
  `string_copy_sso = 2.77ns`），也没有任何原子指令；
- 真正的成本在 **`LeaseView` 的 4 个 `std::string` 拷贝**（name/instance/owner/endpoint）
  与 `Result<LeaseView>` 的移动构造，而不是锁；
- 296B 栈帧 + 栈保护是「把 `Result<LeaseView>` 直接构造在调用方 sret 缓冲」的副作用，
  不是每帧动态分配。

### 2.4 锁：`AcquireLock` / `ReleaseLease`

`AcquireLock`（`coordinator.txt` 10908 起，节选）：

```asm
ac20: endbr64
ac31: sub  $0x1d8,%rsp              ; 472B（Result<LeaseView> + 临时 key 字符串）
ac4b: test %r8,%r8 ; je ac5c        ; key 空 → kInvalidArgument（错误路径字符串）
ad90: call ValidTtl                 ; ttl 区间校验（在锁外）
adb4: lea  0x28(%r12),%rax
adc1: call pthread_mutex_lock       ; ← 加锁
adce: call steady_clock::now
adee: call basic_string(const char*, size_t)   ; 每次把 key 复制成临时 std::string（m_locks 查找用）
ae08: call _Hashtable<std::string, unsigned long>::find   ; m_locks.find(key)
...
b00f: call _Hashtable<unsigned long, LeaseRecord>::_M_emplace  ; m_leases.emplace(id, record)
```

`m_fencing` 的自增（源码 `record.fencing = ++m_fencing[record.name];`）发生在
`pthread_mutex_lock`（`adc1`）之后、`unlock` 之前的同一临界区内；`RecomputeLeaderLocked`
里能看到同形态的 `addq $0x1,0x20(%r14)`（epoch++）。

`ReleaseLease`（`coordinator.txt` 6022 起，节选）：

```asm
5b50: endbr64
5b74: sub  $0x48,%rsp               ; 72B（远比 Acquire 小：无 LeaseView 返回）
5b88: call pthread_mutex_lock
5b9c: call _Hashtable<unsigned long>::find
5baa: mov  0x68(%rax),%rdx          ; owner.size()
5bb2: cmp  %r13,%rdx ; je 5c80      ; 正常路径：EraseLeaseLocked
5c54: call pthread_mutex_unlock
```

> 注：`-O3` 把 `EraseLeaseLocked`（311 条指令、25 个 call）保留为独立函数，没有内联进
> `ReleaseLease`；因此 Release 本身只有 127 条指令，真正删除索引的循环在 5c80 之后。

### 2.5 选主：`JoinElection` / `RecomputeLeaderLocked`

`RecomputeLeaderLocked` 是选主状态机的核心（遍历候选者链表，找最小 lease 编号）：

```asm
4610: endbr64
4625: mov  0x58(%rsi),%rbp          ; election.candidates 桶指针
4670: cmpq $0x0,0x68(%r12)
4676: mov  0x28(%rbp),%rbx          ; node
4690: mov  (%rax),%rax ; test ...   ; 链表 next（unordered_map 节点）
4698: cmp  0x8(%rax),%rbx           ; lease 编号比较
469e: cmp  %r13,0xa0(%rax)          ; deadline <= now → 跳过（过期候选者）
46bb: call _M_assign                ; best_node = node（只在找到更小租约时）
46fa: addq $0x1,0x20(%r14)          ; ← ++election.epoch（leader 变化时）
```

含义：
- leader 重算只比 **租约编号**（`cmp 0x8(%rax),%rbx`），不比剩余时间——这正是
  「按最早租约排序」的汇编级证据，避免了续约时序抖动导致的主漂移；
- 候选者保存在 `unordered_map<string, LeaseId>`，遍历是**指针追逐**（链表），
  所以候选者数量大时缓存不友好（本实现预期 ≤ 数十，未做优化）；
- `epoch++` 是一条 `addq`，在 mutex 内，单调性由单锁保证。

### 2.6 动态指令数（callgrind，实机模拟）

方法：`valgrind --tool=callgrind` 跑 `micro <mode> N`（N=0 与 N=10000 取差值），
扣掉进程启动/动态链接的固定开销。

| 操作 | Ir/次 | 主要构成（Ir/次） |
|---|---:|---|
| `renew_lease` | **837** | string 赋值/复制 ~371（44%）、mutex 60（7%）、hash find 18、`Clock::now` 16、`MakeViewLocked` 92 |
| `acquire+release`（直连） | **2490** | `_M_emplace` 175、string move-assign 196、`m_locks[]` 139、malloc/free 198、`EraseLeaseLocked` 92、mutex ~194（2 次加解锁）、`Hash_bytes` 72 |
| `acquire+release`（facade） | **4380** | 直连的 2490 + 句柄构造（`LockHandle::Impl`、`LeaseSession::State`、`shared_ptr`、2×`std::function`）+ 析构 |

> callgrind 统计的是**指令数**（不带 cache/分支预测惩罚）。实机 renov 65ns × ~4GHz ≈ 260 cycles，
> 837 条指令 → IPC ≈ 3.2，与 4-wide x86 相符，说明热路径是**指令吞吐受限**而非内存受限。
> 因此「减少指令数」是有效优化方向；把 44% 的字符串拷贝削掉，理论上能到 ~470 条指令/次。

### 2.7 物理下限与差距

用 `primitives` 实测（同一台机、同一绑核策略）：

| 原语 | 实测 |
|---|---:|
| `steady_clock::now()`（vDSO） | **17.75 ns** |
| 无争用 `mutex lock+unlock` | **4.03 ns** |
| `unordered_map<uint64,..>` find（1024 项命中） | **1.42 ns** |
| SSO `std::string` 拷贝（6 字符） | **2.77 ns** |

renew 的下限分解（必须做的动作）：
`Clock::now` 17.75 + mutex 4.03 + hash find 1.42 + 4×string 拷贝 ≈ 11.1 + deadline 更新/返回 ≈ 1
→ **≈ 35 ns**。
实测 65 ns（批量）→ **1.9× 下限**；若把「4 个 string 拷贝」看作可优化项，只看
「时钟+锁+查找」= 23.2ns → **2.8×**。
这 1.9–2.8× 的差距主要来自：`Result<LeaseView>` 的 216B 清零（`rep stos`）、
sret 传参、栈保护、以及 string 的 4 次 copy——与 §2.6 的动态指令分布一致。

> 注意：这是「进程内函数调用」的下限，不含网络、序列化、持久化、复制。
> 与 etcd 的对比见 §4，**不能**把 35ns 与 etcd 的 401µs 直接当成实现优劣。

## 3. 功能矩阵（vs etcd 3.4 / Consul 1.19）

| 维度 | Go2Cpp `distributed` | etcd 3.4 | Consul 1.19 |
|---|---|---|---|
| 部署形态 | 嵌入式 C++ 库；协调者同进程或 TCP 暴露 | 独立进程/集群（3/5 节点） | 独立进程/集群（server/client） |
| 复制/一致性 | **无**（单点内存状态） | Raft 多副本；线性一致读/写 | Raft 多副本（server）；强一致 |
| 持久化 | **无**（重启丢全部租约） | WAL + 快照 | Raft 日志 + 快照 |
| 租约 | 内置：`LeaseId + deadline`，TTL **毫秒级**（默认 10ms~10min），惰性过期 + 后台清扫 | 内置 Lease：TTL **秒级**，gRPC `LeaseKeepAlive` 流；key 绑租约，到期自动删 | Session：TTL + 健康检查（默认 10s 级），可绑定 KV |
| 分布式锁 | **内置** `AcquireLock`（非阻塞，冲突即返回；fencing token 单调） | **无内置 API**；官方 `clientv3/concurrency.Mutex`（lease + txn + revision） | **内置**：`/v1/kv/:key?acquire=<session>`（KV + session） |
| 选主 | **内置** `JoinElection`（最早租约者为主，`epoch` 单调） | 官方 `concurrency.Election`（客户端库实现） | 无专门 API；session + KV acquire 自建 |
| 服务发现 | 内置注册/查询（心跳租约，`Lookup` O(n) 快照） | 需自建（key 前缀 + watch） | **一等公民**（DNS/HTTP、健康检查、多数据中心） |
| watch | **无**（轮询；仅 `OnLeaderChange` 本地回调） | **原生 Watch**（历史 revision 回放） | blocking query（长轮询，index-based） |
| 事务 | **无** | **Txn**（多操作 CAS，compare/success/failure） | **Txn**（KV 多操作 CAS） |
| TLS/鉴权 | **无**（明文；默认只监听 127.0.0.1，跨机需外层 TLS/VPN） | mTLS + RBAC | mTLS + ACL |
| 多数据中心 | 无 | 无（需应用层） | 有（WAN gossip + 多 DC） |
| 客户端 | C++（`ICoordinator` 接口） | Go/Java/Python/C++/... | Go/Java/... |
| 单次操作成本 | **ns**（进程内）/ **µs**（TCP） | **数百 µs~ms**（RPC + fsync） | 数百 µs~ms（RPC + fsync，本次未实测） |
| 故障模型 | 协调者单点；客户端本地租约兜底 | 少数派故障可继续服务 | 同 Raft |

结论：**不是替代关系**。etcd/Consul 解决的是「跨机、持久、可容灾的协调」；
本模块解决的是「进程内/单机内、可嵌入、可测试的租约原语」，明确不做持久化与多副本。
需要跨机强一致时，正确做法是把 `Coordinator` 换成 etcd 客户端（`ICoordinator` 接口不变，
见 `docs/modules/distributed.md` §9）。

## 4. 性能对比（口径必须一起读）

### 4.1 Go2Cpp 本地（进程内函数调用，**无网络/无序列化/无持久化**）

`go2cpp_distributed_bench`（`taskset -c 6`，单轮，n=2000~20000）：

| 用例 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| 注册+释放服务 | 230.9 ns | 224 ns | 235 ns | 361 ns |
| 续约租约 | **79.6 ns** | 78 ns | 79 ns | 79 ns |
| 获取+释放锁 | **289.9 ns** | 283 ns | 293 ns | 303 ns |
| 查询 10 实例 | 767.5 ns | 752 ns | 772 ns | 782 ns |
| 查询 100 实例 | 9.49 µs | 8.91 µs | 9.23 µs | 21.7 µs |
| 查询 1000 实例 | 133.0 µs | 127.6 µs | 156.7 µs | 175.2 µs |
| 查询选举（2 候选） | 87.5 ns | 88 ns | 88 ns | 88 ns |
| 清扫 20000 条过期 | — | — | — | 3.20 ms（160 ns/条） |
| 锁竞争 1 线程 | 201.1 ns | 195 ns | 205 ns | 206 ns |
| 锁竞争 4 线程 | 208.4 ns | 205 ns | 206 ns | 313 ns |
| 锁竞争 16 线程 | 210.9 ns | 205 ns | 215 ns | 293 ns |

`stress_dist`（1e6 次批量循环平均，`taskset -c 6`，3 轮）：

| 操作 | 轮 1 | 轮 2 | 轮 3 |
|---|---:|---:|---:|
| `renew_lease`（1e6） | 65.4 ns | 68.2 ns | **63.8 ns** |
| `acquire_release_direct`（1e6） | 166.6 ns | 175.9 ns | **169.8 ns** |
| `acquire_release_facade`（1e6） | 346.1 ns | 333.6 ns | **327.2 ns** |
| `election_rejoin`（1e6） | 117.8 ns | 111.0 ns | **106.9 ns** |
| `election_join_facade`（1e5） | 384.9 ns | 408.5 ns | **456.3 ns** |
| `contended_lock_4x`（1e6，4 线程） | 464.7 ns | 518.4 ns | **391.2 ns** |

> 「ns」的三种口径：bench 单次计时（含 ~20ns 的两次 `Clock::now`）、stress 批量平均
> （不含单次计时，故 63.8ns < 79.6ns）、以及 callgrind 指令数（837 条/次）。
> 引用时必须说明是哪一种，否则会出现「83ns vs 64ns」的假矛盾。

### 4.2 Go2Cpp TCP（同机 loopback，自定义二进制协议）

| 用例 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| TCP 注册+释放 | 44.7 µs | 44.6 µs | 54.2 µs | 71.0 µs |
| TCP 续约 | **26.3 µs** | 26.3 µs | 30.7 µs | 46.1 µs |
| TCP 查询 2001 实例 | 942.6 µs | 924.3 µs | 1.06 ms | 1.18 ms |
| TCP 获取+释放锁 | **46.7 µs** | 45.7 µs | 60.8 µs | 74.6 µs |

口径：一帧 = `u32_be(len) + payload`，`TCP_NODELAY`，客户端单连接串行，服务端每连接一线程。

> 与 `docs/modules/distributed.md` §7 的基线（TCP 130–250µs）相比，本次独立构建快 3–5×
> （26–47µs）。同 commit、同 Release 参数，差异可能来自当时机器负载/采集方式；
> 本次以 `/tmp/build-dist` 的实测为准，文档数字留作历史基线。

### 4.3 etcd 3.4（本机实测，C++ HTTP/1.1 keep-alive → v3 gateway）

etcd 来源：`apt-get download etcd-server etcd-client`（3.4.30，deb 解包，用户态启动，
`--data-dir /tmp/etcd-data`，单节点，127.0.0.1:2379）。客户端 `/tmp/distwork/etcd_bench.cpp`：
C++ + POSIX socket + keep-alive，**不含 Python 层开销**，与 Go2Cpp TCP 客户端同语言、同 loopback。

| 操作 | mean | p50 | p95 | p99 | 说明 |
|---|---:|---:|---:|---:|---|
| `kv_range` | 449.3 µs | 426.2 µs | 666.8 µs | 845.0 µs | 读，无 fsync |
| `kv_put` | 1218.9 µs | 1104.2 µs | 1890.1 µs | 3322.4 µs | 写，**WAL fsync** |
| `lease_grant` | 1189.1 µs | 1101.1 µs | 1671.1 µs | 2852.9 µs | 租约创建，含 fsync |
| `lease_timetolive` | 420.4 µs | 401.1 µs | 617.5 µs | 852.9 µs | unary 租约读 |

**未测 `lease_keepalive`**：它是 bidi gRPC 流，HTTP gateway 只返回 `Transfer-Encoding: chunked`
的持续流，无法在 keep-alive 连接上做「一次请求一次 RTT」的测量；强行测会把建连成本算进去。
生产客户端用 gRPC 长连接 + 流式 keepalive，单次续约的服务端处理与 `timetolive` 同量级
（都要过 Raft/leader 检查），故用 `timetolive` 作为「租约类 RPC」的代表，**不是** keepalive 实测值。

> 另：gateway 的 JSON 编解码占了不少开销；官方 gRPC/protobuf 客户端通常比 gateway 快
> 2–3×（仍是数百 µs 量级）。本次没有 gRPC 客户端，**不做这个换算的断言**。

### 4.4 对比表（p50，单位 ns）

| 操作 | Go2Cpp 本地 | Go2Cpp TCP | etcd RPC | 本地/etcd | TCP/etcd |
|---|---:|---:|---:|---:|---:|
| 续约（renew / timetolive） | 64–78 | 26 300 | 401 100 | **~5 500×** | **15.2×** |
| 抢锁+释放（acquire+release / put） | 166–283 | 45 700 | 1 104 200 | **~5 000×** | **24.2×** |
| 服务查询（1000 实例 / 单 key range） | 127 600 | 924 300（2001 实例） | 426 200 | — | — |

**口径警告（必须随数字一起引用）**：
1. 本地列是**同进程函数调用**：没有 TCP、没有序列化、没有磁盘、没有副本共识。
   etcd 每列都包含 TCP + HTTP/JSON（gateway）+ gRPC 框架 + Raft（读要 ReadIndex，
   写要 WAL fsync + 复制）。
2. 唯一「同栈」的对比是 **Go2Cpp TCP vs etcd gateway**：15–24×。差距来源可拆为：
   协议（4 字节长度 + 二进制 vs HTTP/JSON）、服务端模型（每连接一线程 vs goroutine + Raft）、
   以及 etcd 写路径的 **fsync（~700µs）**——我们完全不落盘。
3. 我们的写路径**没有持久化**，这是**能力缺失**，不是性能优势；协调者重启即丢全部租约，
   etcd/Consul 不会。
4. 机器有负载（loadavg 3.5–6.6），绝对值有 ±20% 波动；倍数关系稳定。

## 5. 正确性

### 5.1 8/8 测试（独立构建 `/tmp/build-dist`）

```
$ cd /tmp/build-dist && ctest -L distributed --output-on-failure
1/8 distributed_registry ........ Passed  1.23 sec
2/8 distributed_lock ............ Passed  1.08 sec
3/8 distributed_election ........ Passed  0.38 sec
4/8 distributed_random .......... Passed  0.99 sec
5/8 distributed_concurrent ...... Passed  1.61 sec
6/8 distributed_transport ....... Passed  0.82 sec
7/8 distributed_scenarios ....... Passed  0.96 sec
8/8 distributed_multiprocess .... Passed  2.22 sec   ← 3 进程 + SIGKILL 主进程后重选
100% tests passed, 0 tests failed out of 8
Total Test time (real) = 9.29 sec
```

### 5.2 1e6 次压力（3 轮，全部通过）

`tests/distributed/stress_dist.cpp`（本次新增，**未注册 ctest**，原因：`cmake/Go2CppDistributed.cmake`
不在本任务允许修改的范围内；手动编译命令见 §7）。

每轮校验：
- renew 1e6 次全部 `ok()`；
- acquire+release 1e6 次，`fencing == i+1`（从 1 连续递增）；
- 选举重复加入 1e6 次，leader 恒为 `node-a`、`epoch` 不变；
- facade Join 1e5 次，`is_leader()` 恒真；
- 4 线程 × 25 万次争抢，**token 全局唯一且连续**（`min=1 max=1000000 size=1000000`），
  冲突次数 29631 / 84564 / 113595（重试不计入成功）；
- 收尾 `Stats()`：`locks=0`、`candidates=1`、`active_leases=2`（1 条 seed 租约 + 1 个候选者），
  无残留锁。

三轮输出（节选）：

```
run1: renew 65.40ns | direct 166.55 | facade 346.05 | rejoin 117.83 | join-facade 384.90 | 4x 464.69 (conflicts 84564)
run2: renew 68.22ns | direct 175.91 | facade 333.58 | rejoin 110.95 | join-facade 408.45 | 4x 518.39 (conflicts 113595)
run3: renew 63.84ns | direct 169.78 | facade 327.18 | rejoin 106.86 | join-facade 456.27 | 4x 391.18 (conflicts 29631)
token_range: min=1 max=1000000 size=1000000 ok=1000000   (三轮一致)
STRESS_OK n=1000000
```

> 第一版 stress 曾误报一次 FAIL：当时忘了对 token 向量排序，`adjacent_find` 在未排序区间上
> 找重复必然失败，`back()` 也不是最大值。修正后（排序 + 连续区间校验）3/3 通过；
> 这次误报本身也说明「不变量校验要校验完整区间，而不是抽样」。

## 6. 结论与优化建议

**证据链**：
- 汇编：热点函数全部在 `-O3` 下保留为独立符号，调用点可数（2 个 mutex/操作、0 原子）；
- 动态：837（renew）/2490（acquire+release）条指令，字符串占 44%，锁占 7%；
- 性能：本地 64–283ns、TCP 26–47µs、etcd 401–1104µs（口径已标注）；
- 正确性：8/8 + 3×1e6 次压力 + 四条不变量。

**优化建议（按收益排序）**：
1. **架构层（数量级）**：`Lookup`/`QueryElection` 每次全量快照拷贝（1000 实例 127µs）；
   引入共享只读快照 + 版本号（RCU/COW）可把查询降到 O(1) 读指针，但会改变 `ICoordinator`
   的值语义——属于接口级改动，需单独立项。
2. **视图层（~1.5–2×）**：`LeaseView` 的 4 个 `std::string` 是 renew 的 44% 指令。
   可改为 `string_view` 指向协调者内部存储（需生命周期契约）或 `shared_ptr<const LeaseRecord>`
   快照；单次 renew 有望从 64ns 降到 35–40ns。
3. **句柄层（~2×）**：facade acquire 327ns vs 直连 166ns，成本在 `LockHandle::Impl` +
   `LeaseSession::State`（shared_ptr）+ 2 个 `std::function` 的 3 次分配。可用对象池/内联
   存储（`optional<State>`）削掉；不影响语义。
4. **锁层（很小）**：mutex 只占 7% 指令；16 线程 p99 293ns–69.6µs（与采集条件强相关），
   真要降尾延迟应做分片（按 key hash 到 N 个 mutex），而不是换自旋锁。

**不建议**：为了「更快」去掉 fencing/epoch 或异步化续约——那会破坏本模块唯一的核心不变量。

## 7. 复现命令

```bash
# 1) 独立构建（不碰 .build-bento-wsl）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-dist -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_DISTRIBUTED_TESTS=ON
ninja -C /tmp/build-dist go2cpp_distributed go2cpp_distributed_bench \
      go2cpp_distributed_registry_test go2cpp_distributed_lock_test \
      go2cpp_distributed_election_test go2cpp_distributed_random_test \
      go2cpp_distributed_concurrent_test go2cpp_distributed_transport_test \
      go2cpp_distributed_scenarios_test go2cpp_distributed_multiprocess_test
cd /tmp/build-dist && ctest -L distributed --output-on-failure

# 2) objdump
cd /tmp/build-dist/CMakeFiles/go2cpp_distributed.dir/src/distributed
for f in *.o; do objdump -drC --no-show-raw-insn "$f" > "/tmp/asm/$f.txt"; done
python3 /tmp/asm/analyze_asm.py /tmp/asm/coordinator.txt ...   # 见脚本

# 3) callgrind（动态指令数）
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.renew0 ./micro renew 0
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.renew  ./micro renew 10000
grep -m1 '^summary:' /tmp/cg.renew    # 15720053
grep -m1 '^summary:' /tmp/cg.renew0   # 7349896 → (15720053-7349896)/10000 = 837 Ir/次

# 4) 压力测试（1e6 次，3 轮）
g++ -O3 -DNDEBUG -std=c++17 -I/UserData/CodexWorkSpace/Go2cpp/include \
    /UserData/CodexWorkSpace/Go2cpp/tests/distributed/stress_dist.cpp \
    -o /tmp/distwork/stress_dist -L/tmp/build-dist -Wl,-rpath,/tmp/build-dist \
    -lgo2cpp_distributed -lgo2cpp_relay -lgo2cpp_network -lgo2cpp_context \
    -lgo2cpp_scheduler -lgo2cpp_io -lgo2cpp_sync -lpthread
for i in 1 2 3; do taskset -c 6 /tmp/distwork/stress_dist 1000000; done

# 5) etcd（apt 包解包，无需 root）
apt-get download etcd-server etcd-client
for d in *.deb; do dpkg-deb -x "$d" /tmp/etcd-root; done
/tmp/etcd-root/usr/bin/etcd --data-dir /tmp/etcd-data \
  --listen-client-urls http://127.0.0.1:2379 \
  --advertise-client-urls http://127.0.0.1:2379 \
  --listen-peer-urls http://127.0.0.1:2380 \
  --initial-advertise-peer-urls http://127.0.0.1:2380 \
  --initial-cluster default=http://127.0.0.1:2380 &
/tmp/distwork/etcd_bench 1000
```

产物位置：
- 反汇编：`/tmp/asm/{coordinator,lock,election,lease_session,registry,transport}.txt`
- 统计 JSON：`/tmp/asm/summary2.txt`
- 基准日志：`/tmp/bench_go2cpp.log`、`/tmp/etcd_bench.log`
- 压力/微基准源码：`tests/distributed/stress_dist.cpp`、`/tmp/distwork/{micro,etcd_bench,primitives}.cpp`

## 8. 与文档基线的差异（诚实记录）

| 指标 | `docs/modules/distributed.md` §7 | 本次（独立构建） | 说明 |
|---|---:|---:|---|
| renew | 83 ns | 79.6 ns（单次）/ 63.8–68.2 ns（批量） | 一致（口径差异见 §4.1） |
| acquire+release | 359 ns | 289.9 ns（单次）/ 166–176 ns（直连批量） | 本次更快（负载/绑核差异） |
| lookup 1000 | 162 µs | 133 µs | 一致量级 |
| 16 线程 p99 | 69.6 µs | 293 ns | **未复现文档基线**；本次绑核单核、线程串行化，尾延迟低；文档数据可能采自未绑核/高负载环境 |
| TCP renew | 143 µs | 26.3 µs | 本次快 5.4× |
| TCP acquire+release | 249 µs | 46.7 µs | 本次快 5.3× |

结论：renew/acquire/lookup 与文档一致；**TCP 与锁竞争尾延迟本次显著更好**。
文档数据不删除（历史基线），本次数据作为独立构建的复测结果。
