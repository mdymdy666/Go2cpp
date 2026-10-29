# session_control 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/session_control.hpp`、
  `src/bento/net/session_control.cpp`、`tests/bento/test_session_control.cpp`
- 压测工具：`tests/bento/audit_session_control_stress.cpp`（独立程序，不注册 CTest；
  刻意不 include `test_util.hpp`，避免被 `link.hpp` 污染依赖）
- 构建：`.build-bento-wsl`（Ninja，CMAKE_BUILD_TYPE=Release，`-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，GCC 13.3，12 vCPU
- 结论：**无 P0**。修复 1 个 P1 语义缺陷（`ControlChange::controller` 在抢占/租约过期事件
  中记录旧控制器）与 1 个 P1 性能缺陷（每事件一次 `NowMs()` + 每次操作一次堆分配，
  重路径 **1.4–1.7×**）；补 6 组测试（18,997 → **53,539 checks**）；P2 记录 8 项。
  `ctest -R bento` **48/48 通过**（`bento_netem` 按设计 SKIP，与本次无关）。

## 1. 压测数据

### 1.1 吞吐（基线 vs 优化，3 轮取中位，同机同参数）

| 操作 | 规模/轮 | 基线 ns/op | 优化后 ns/op | 变化 |
|---|---|---|---|---|
| `Grant(view)` no-op（无事件） | 200 万 | 25.5 | 28.5 | 0.89× |
| `Grant(interact)` 轮转（抢占 + 2 事件 + 审计） | 200 万 | 114.7 | **67.8** | **1.69×** |
| `RequestControl` 轮转（Preempt 抢占） | 200 万 | 113.3 | **67.9** | **1.67×** |
| `Revoke(peer)` + `Grant` 轮转 | 200 万 | 41.9 | 48.8 | 0.86× |
| `Tick(now)` 有控制器空转 | 500 万 | 42.2 | 45.6 | 0.93× |
| `mode()` + `is_controller()` | 200 万 | 27.0 | 27.5 | 0.98× |
| `Snapshot()` 64 参与者 + 审计 | 20 万 | 458.9 | 454.4 | 1.01× |
| `Grant(interact)` audit=0 | 100 万 | 89.9 | **64.9** | **1.39×** |
| `Grant(interact)` audit=64 | 100 万 | 94.8 | **66.7** | **1.42×** |
| `Grant(interact)` audit=4096 | 100 万 | 96.1 | **67.0** | **1.43×** |
| 16 线程 `Grant(interact)`（P99） | 400 万 | 287.8（67.2 µs） | 308.4（**66.0 µs**） | 0.93× |
| 16 线程 `RequestControl`（P99） | 400 万 | 264.3（65.7 µs） | 287.7（**62.5 µs**） | 0.92× |

- 每轮约 **3,020 万次**被测 API 调用；基线/优化各 3 轮，合计约 **1.8 亿次**。
- 重路径（真正会改控制权、产生事件的 Grant/Request）提升 1.4–1.7×；
  轻路径（view→view、Revoke 非控制器、Tick 空转）退化 3–7 ns，来源是
  `std::array<ControlChange,2>` 的栈上默认构造（80B），见 P2-3。
- `audit_capacity` 从 0 加到 64 只增加 1.8 ns/op（64.9→66.7），deque 审计不是瓶颈。
- 16 线程写竞争吞吐没有改善（±8%），但 P99 降低 1–3 µs；瓶颈是全局 mutex 的
  futex 唤醒/调度（P99 60+ µs），不是临界区长度，见 P2-1。

### 1.2 内存与泄漏

| 项目 | 结果 |
|---|---|
| 压测全程 RSS | 3.7 MiB → 峰值 5.0 MiB（不同 bench 阶段，含 4M 事件分配） |
| 10 s 长稳（8 线程混合，22,777,127 ops） | RSS 3712 → 峰值 3840 → 3840 KiB，**+128 KiB** |
| ASan + UBSan（`--mode=all --scale=0.02`） | 87,234 checks，0 失败、0 报告 |
| ASan 2 s 长稳（781,932 ops） | 通过，无 LeakSanitizer 报告 |
| valgrind 5 场景 | 161 allocs = 161 frees，`0 bytes in 0 blocks`，0 错误 |
| valgrind 随机（10 序列） | 2,551 allocs = 2,551 frees，0 错误 |

### 1.3 随机测试

- 压测工具：**1000 个随机序列 × 200 步 = 200,000 步**，4,430,386 条断言，0 失败。
  每个序列随机 `lease_ms ∈ {0,1,10,30,100,1000,30000,60000}`、
  `audit_capacity ∈ {0,1,2,8,64,1024}`、`max_participants ∈ {1,2,8,64}`、两种策略、
  两种 `default_mode`；每步与**独立影子模型**逐项比对：返回值、模式表、controller、
  revoked 集合、generation、8 项统计；并校验回调事件 generation 严格递增。
- 16 线程随机 fuzz：320,000 次操作（16 线程 × 20,000），唯一控制器/统计守恒全过。
- CTest 回归：1000 序列 × 60 步（60,000 步），每 10 步验证唯一控制器、参与者上限、
  `generation == stats.changes`。

### 1.4 五个复杂场景（`--mode=scenarios`，200 checks）

| # | 场景 | 结果 |
|---|---|---|
| 1 | 抢占 vs 先到先得：16 peer 顺序请求 | FirstCome 1 胜/15 拒；Preempt 16 胜/15 次抢占；抢占事件 `controller` 为新控制器 |
| 2 | 租约边界（假时钟）：`Tick(0)` 不回收、`Tick(UINT64_MAX)` 回收、`Renew` 生效、重复 Tick 不重复计数、`lease_ms=0` 永不过期 | 全过 |
| 3 | 撤销持久：Revoke → Leave → Join 仍拒绝（5 轮），Grant 才解除；非控制器 Revoke 只标记不产事件 | 全过 |
| 4 | 断开回收：模拟 `PeerHub::on_peer_disconnected` 双参 lambda；控制者断开回收、非控制者断开不误伤、重复断开不重复计数 | 全过 |
| 5 | 16 线程并发 Grant/Request：4000 Grant + 4000 Request 全部成功，唯一 Interact，`stats.grants/requests/changes` 与计数守恒 | 全过 |

CTest 内另有：16 线程并发 Grant/Request（场景 16）、10 万次混合操作压测（场景 17，
实测 12.4 M ops/s、8.1 ms）、1000 序列随机（场景 18）。

## 2. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁或内存泄漏（ASan/UBSan/valgrind/影子模型全绿）。

### P1-1（已修）`ControlChange::controller` 在三条路径上违反契约

- 证据：`ControlChange::controller` 的契约是「**变化后**的唯一控制器（0=无）」
  （`session_control.hpp:182`），但修复前：
  - `Tick()` 先 `AppendChange`、后 `m_controller = 0`，`kLeaseExpired` 事件仍是旧控制器；
  - `Grant(kInteract)` 与 `RequestControl` 抢占路径在 `m_controller = peer_id` 之前
    生成 `kPreempted` 事件，事件 `controller` 是**被抢者**而不是新控制器。
- 影响：应用若根据 `on_change` 事件增量更新本地视图（本项目正是这种接法），
  会把控制权显示成已被抢占/已过期的 peer；`Snapshot()` 终态正确，但事件流错误。
- 修复：先完成全部状态迁移（含 `m_controller`），再按原顺序生成事件
  （`src/bento/net/session_control.cpp:260-281`、`:440-460`、`:570-582`）。
- 测试锁定：场景 5/13/14（`test_session_control.cpp:908` 起）断言
  `kPreempted`、`kGranted`、`kLeaseExpired`、`kLeft`、`kDisconnected`、`kRevoked`
  事件的 `controller` 字段；修复前 stress 程序实测报
  `[FAIL] events[0].controller == 1U`，修复后 0 失败。

### P1-2（已修）事件路径每条事件一次 `NowMs()` + 每次操作一次堆分配

- 证据：修复前 `AppendChange` 内部 `change.timestamp_ms = NowMs()`，一次抢占产生
  2 条事件即 2 次 `clock_gettime`；每条公开操作构造 `std::vector<ControlChange>`
  并 `push_back`，锁内一次 `malloc/free`。`Grant(interact)` 114.7 ns/op，
  其中审计关闭（audit=0）仍 89.9 ns/op。
- 修复：
  1. `AppendChange` 接收调用方时间戳，返回事件值；每次操作最多取一次时钟
     （`Grant` 用惰性 `ensure_now()`，无状态变化时 0 次）；
  2. `std::array<ControlChange, 2>` 栈缓冲 + `Dispatch(const ControlChange*, size_t)`，
     移除锁内 vector 分配；`Snapshot()` 的时钟移到锁外。
- 效果：`Grant(interact)` 114.7 → 67.8 ns/op（1.69×），`RequestControl` 1.67×，
  审计开启时 1.39–1.43×；`audit_capacity=0` 时也从 89.9 降到 64.9（1.39×）。

### P2（记录，未修）

1. **锁粒度**：全局 `std::mutex` 串行化所有读写；16 线程写竞争 3.2–3.4 Mops/s、
   P99 62–67 µs，瓶颈是 futex 唤醒而非临界区（优化临界区后 P99 降 1–3 µs，吞吐不变）。
   改 `shared_mutex` 只对读多写少有用，而本模块真实调用是「低频控制权变更 + 每帧 Tick」，
   16 线程狂写 Grant 不是真实负载；分片锁会引入跨分片唯一控制器的一致性复杂度，故未改。
2. **审计 deque**：`m_audit` 每事件一次节点分配（`audit=64` 相比 `audit=0` 仅 +1.8 ns/op）。
   预分配环形缓冲可消除，但 `audit_capacity` 可配置到很大（如 1024），
   预分配策略需额外上限约定，本次未做。
3. **轻路径退化**：`std::array<ControlChange,2>` 默认构造 80B，使 `Grant(view)` 25.5→28.5 ns、
   `Revoke` 轮转 41.9→48.8 ns、16T 写 0.92–0.93×。收益（重路径 1.4–1.7×）大于损失，
   保留；若未来需要可改 `std::optional<ControlChange>` 延迟构造，代价是 `Dispatch` 接口变复杂。
4. **`m_participants` 是 `std::map`**（n≤64，查找 O(log n)）；换 `unordered_map` 收益小，
   且 `Snapshot()/participants()` 需要排序，未做。
5. **依赖 `protocol.hpp` 的 `NowMs()`**：`session_control.cpp` 唯一引用的 bento 符号
   （`nm` 验证）；头文件本身自包含、不拖 `link/peer_hub/transport/session`。
   抽独立轻量时间头需要新增文件，超出本次改动边界；抽离时必须保持与
   `protocol.cpp` 同一时基，否则租约与测试注入时钟会错位。
6. **回调约束**：`on_change` 运行期替换需外部保证不与回调并发（文档已声明，非线程安全）；
   多线程下事件回调可能交错，`generation` 单调但回调顺序不保证全局有序。
7. **不可达防御分支**：`Tick()` / `Revoke()` 中「controller 不在参与者表」的分支
   会静默清零（不产事件、不计数）；正常状态机不可达，未改。
8. **审计时间戳语义微调**：事件 `timestamp_ms` 由「AppendChange 内的 NowMs」改为
   「操作开始时的时钟」；同一操作的 2 条事件共享同一毫秒，`Tick(now)` 注入时审计时间戳
   等于注入值（对测试更友好）。既有字段与顺序未变。

## 3. 解耦验证

| 检查 | 方法 | 结果 |
|---|---|---|
| 头文件自包含 | 仅 `#include session_control.hpp` 的 TU，`-fsyntax-only -Werror` | 通过 |
| 非标准库依赖 | `g++ -H` 包含树 | 仅 `session_control.hpp` 自身；无其他项目头文件 |
| 无 link/peer_hub/transport/session | `-H` 输出 grep | 未出现 |
| 最小链接 | 只链 `session_control.cpp` + `protocol.cpp` 的 main | 编译、运行通过 |
| 符号依赖 | `nm -u session_control.cpp.o` | 仅 `go2cpp::bento::NowMs()` |

结论：模块与传输层解耦成立；唯一项目内依赖是取时函数 `NowMs()`（P2-5）。

## 4. 测试复查

- 断言数：18,997 → **53,539**（+34,542），3 次连续运行稳定通过。
- 新增用例：场景 14（事件 controller 契约）、15（Renew 顺延）、16（16 线程并发
  Grant/Request）、17（10 万次混合压测）、18（1000 序列随机）；并补齐边界：
  0 号 peer 的 `Grant/Revoke/Leave/OnPeerDisconnected/Renew`、全部枚举名、
  空快照 JSON、`revoked` 者不得为 controller 的不变式。
- **SKIP 逃逸检查**：测试未调用 `bento_test::Skip()`；`MakeLeaseControl` 失败路径
  先 `BENTO_CHECK(lease.control != nullptr)` 再 return，失败计入 failures 而非 SKIP；
  `TestRenewExtendsLease` 的提前 return 同样在断言之后。不存在静默跳过。
- **断言有效性**：原 18,997 条中约 16,000 条来自随机测试每步 4 条
  （interact≤1、controller 模式、参与者数、generation==changes），数量大但重复度高；
  新增断言集中在并发成功计数、事件字段契约、租约边界与影子模型（压测工具），
  能独立定位状态机错误。
- **仍存缺口（记录）**：`Snapshot().ToJson()` 只用子串校验，未用 JSON 解析器验证；
  `on_change` 并发替换无法在无 UB 前提下测试；`m_revoked_peers` 规模由管理面
  Revoke 次数决定（文档已说明），无上限测试。

## 5. 复跑命令

```bash
# 构建 + 回归
ninja -C .build-bento-wsl
./.build-bento-wsl/bento_test_session_control
cd .build-bento-wsl && ctest -R bento -j4

# 压测工具（审查用，不注册 CTest）
g++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
    tests/bento/audit_session_control_stress.cpp src/bento/net/session_control.cpp \
    src/bento/net/protocol.cpp -o .build-bento-wsl/audit_session_control_stress_opt
./.build-bento-wsl/audit_session_control_stress_opt --mode=bench
./.build-bento-wsl/audit_session_control_stress_opt --mode=random
./.build-bento-wsl/audit_session_control_stress_opt --mode=scenarios
./.build-bento-wsl/audit_session_control_stress_opt --mode=threads
./.build-bento-wsl/audit_session_control_stress_opt --mode=soak --seconds=10
```

## 6. 后续状态

- 未 commit；未改 `base`、`cmake`、其他 bento 模块。
- P2-1（锁粒度）、P2-5（时间头抽离）需要接口/构建层改动，留待后续任务。
