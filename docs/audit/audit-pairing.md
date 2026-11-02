# pairing 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/pairing.hpp`、`src/bento/net/pairing.cpp`、
  `tests/bento/test_pairing.cpp`
- 压测工具：`tests/bento/audit_pairing_stress.cpp`（独立程序，不注册 CTest）
- 构建：`.build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，CMAKE_BUILD_TYPE=Release，
  `-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU，19 GB 内存，GCC 13.3
- 结论：**无 P0**。修复 1 个 P1（`Activate` 重复调用使 15s 超时/1s 重传永久失效）
  与 2 个 P2 加固（RESPONSE 的 key_hash 校验、未确认 ACK 拒绝）；2 项性能优化
  （完整配对 2.3 倍）；P2 记录 6 项。`ctest -R bento` 48/48 通过（1 项按设计 SKIP）。

## 1. 压测数据

### 1.1 32B 帧编解码（每项 2000 万次，3 次运行取中位）

| 项目 | ns/op | Mops/s | MB/s |
|---|---|---|---|
| encode | 6.08 | 164.4 | 5261 |
| decode | 6.22 | 160.6 | 5140 |
| encode+decode 往返 | 11.67 | 85.7 | 5429 |
| decode 拒绝（magic 坏，早退） | 1.20 | 830.8 | 26695 |

- 单帧 32B，编译器 GCC 13.3 `-O2`；循环内 8 次编码后读一次 buffer，避免被优化掉。
- 拒绝路径 1.2ns 说明校验是「读 4B magic 即返回」，合法路径成本主要在 6 个大端
  字段的读写；本次未改编码器（见 P2-7）。

### 1.2 完整配对与帧处理（Release，AMD/Intel 12 vCPU）

| 项目 | 次数 | 结果 |
|---|---|---|
| 完整配对（Register→Connect→Response→CONFIRM→ACK，含 2 次会话构造/析构） | 200 万 | **152.0 ns/pair，6.58 M pairs/s**；P50/P95/P99 = 0.12/0.12/0.13 µs |
| `HandleFrame`（服务端 Paired 重放 RESPONSE + 入队/出队） | 1000 万 | 11.36 ns/op，88.0 Mops/s |
| 会话构造+析构 | 500 万 | 21.65 ns/op |
| 密钥指纹（28B key，FNV-1a64） | 1000 万 | 12.10 ns/op |

**优化前后对比**（同一压测程序、同一机器）：

| 指标 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| 会话构造+析构 | 52.50 ns | 21.65 ns | **2.4×** |
| 完整配对 | 352.9 ns/pair（2.83 M pairs/s） | 152.0 ns/pair（6.58 M pairs/s） | **2.3×**；P99 0.34→0.13 µs |
| Pending 日志字段分配 | 每会话 1 次 `make_shared` + 每状态迁移 1 次 deque 分配 | 默认策略零分配 + 固定数组 | 每配对少 6 次 malloc/free |

### 1.3 1000 并发

| 场景 | 结果 |
|---|---|
| 单线程 1000 个会话同时 Register（不同 key） | 全部 Pending；RSS 增量 896 KB（≈0.9 KB/会话） |
| 1000 个会话逐一完成完整配对 | 1000/1000 Active；pair_id 两端一致 |
| 8 线程 × 125 = 1000 个并发完整配对 | 1000/1000 成功；1000 个自动 nonce 全部唯一（`std::unordered_set` 去重后仍为 1000） |

### 1.4 内存与泄漏

| 项目 | 结果 |
|---|---|
| 1000 个存活会话 | RSS +896 KB（Release）；ASan 下 +2.3 MB |
| 20 万轮「构造→完整配对→销毁」 | RSS 增量 **0 KB**（VmRSS 4736→4736） |
| valgrind（2.1 万轮，含 1000 会话存活） | 87,030 allocs / 87,030 frees，`in use at exit: 0 bytes`，0 错误 |
| ASan+UBSan（scale 0.02 全量） | 106,552 checks，0 失败、无 sanitizer 报告 |

### 1.5 随机与有损链路

| 场景 | 规模 | 结果 |
|---|---|---|
| 随机帧 fuzz（随机 type/version/status/key_hash/nonce/pair_id 喂给双会话） | 40,000 帧 | 不崩；状态合法、队列 ≤64；失败 0 |
| 随机操作状态机（Register/Connect/Response/Activate/投递/Tick/Close/Reset） | 200,000 步 | 31,609 次合法迁移；不变量（client Active ⇒ server Active、双端 Paired/Active 时 pair_id 相等）全成立 |
| 随机 32B 缓冲解码 | 1,000,000 次 | 125,205 次命中合法帧（12.5% 主动构造），其余拒绝；失败时不写 `out` 的契约成立 |
| 有损链路（30% 丢包 + 10% 重复 + 0..3 步乱序，15s 窗口） | 300 trials | **300/300 收敛**（修复前 220/300） |

## 2. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁或内存泄漏。

### P1-1（已修）`Activate` 重复调用使超时与重传永久失效

- 证据：有损链路压测中失败 trial 全部停在 `server=active, client=paired`，
  客户端 `deadline_ms` 被推到「当前时刻+15s」。`Activate` 每次调用都执行
  `m_deadline_ms = now + timeout; m_next_retransmit_ms = now + retransmit`。
  应用若在驱动循环里每帧调用一次（幂等 API 的常见写法），CONFIRM 丢失后
  `Tick` 永远不会重传，15s 超时也永不触发，客户端永久卡在 Paired。
  修复前 30% 丢包下 300 个 trial 只有 220 个完成。
- 修复：`Activate` 首次调用才设置计时器；`m_confirm_sent` 已为真时直接返回
  `kNone`，不重排队 CONFIRM、不改 deadline/next_retransmit。头文件注释同步。
- 验证：`TestActivateIdempotent`（连续 100 次重复 Activate 后 deadline 不变、
  500ms 处仍重传、4s 仍超时）；`TestLossyLinkConvergence` 200/200 收敛；
  压测 300/300。

### P2-1（已修，加固）成功 RESPONSE 未校验 key_hash

- 证据：`HandleResponse` 的 Pending 分支只校验 `nonce`，成功响应直接进入
  Paired；`Paired` 分支的重复响应重放也只比较 `pair_id`。串话或伪造响应
  （需猜中 64 位 nonce）可推进状态。
- 修复：成功响应必须回显 `m_key_hash`，否则返回 `kKeyMismatch` 且状态不变；
  Paired 分支重放 CONFIRM 同样校验 key_hash。失败响应不回显本端指纹，
  故不做该校验（保持原语义）。
- 验证：`TestResponseKeyHashGuard`。

### P2-2（已修，加固）未发 CONFIRM 时接受 ACK

- 证据：客户端 Paired 但尚未 `Activate`（`m_confirm_sent == false`）时，
  收到 key_hash/nonce/pair_id 全匹配的 CONFIRM（ACK）会直接迁移 Active，
  绕过「CONFIRM + ACK」双向确认。正常流程不可达（服务端收到 CONFIRM 才发
  ACK），但属于状态机防御缺口。
- 修复：`!m_confirm_sent` 时返回 `kInvalidState`，状态保持 Paired。
- 验证：`TestResponseKeyHashGuard`（先拒绝，Activate 后同一 ACK 才接受）。

### P2-3（记录，未修）不可达的防御分支

`HandleFrameImpl` 的 `switch` 中 `case kRegister` 与 `HandleConfirm` 开头的
`status != kNone` 检查在入口已提前拒绝，属于结构性死代码（覆盖率报告中标为
未执行）。保留作为防御性检查，删除会让 `-Wswitch` 在未来新增枚举值时失去保护；
记录而非修改。

### P2-4（记录，未修）时钟回绕

`deadline_ms = now_ms + timeout_ms` 使用无符号加法：`now_ms > UINT64_MAX-15000`
时回绕，deadline 变小导致立即超时。实测 `now=UINT64_MAX-100` → `deadline=14899`。
真实毫秒时钟（约 5.8 亿年）不会到达，仅作记录；`TestClockAnomaly` 固化该行为。

### P2-5（记录，未修）Closed 状态再次 Register/Connect 的错误码

`Register`/`Connect` 在任意非 Idle 状态都返回 `kAlreadyRegistered`（含 Closed），
而 `HandleFrame` 在 Closed 返回 `kClosed`。API 语义不够精确，现有测试已固化该
行为；改动需同步调用方，暂不调整。

### P2-6（记录，未修）`kAlreadyPaired` 从未返回

枚举值 `PairingError::kAlreadyPaired` 只被 `IsKnownPairingError`/名字表覆盖，
协议实现从不产生它（同 nonce 重复请求走幂等重放并返回 `kNone`）。属于协议
保留码，文档已注明「通常幂等处理，不外抛」。

### P2-7（记录，未修）剩余分配点与编解码

- `m_outbound` 仍是 `std::deque<PairingFrame>`，每会话 1–2 次堆分配，
  `handle_frame` 11.36ns 中约 3–4ns 来自 deque 的 push/pop。改固定环形缓冲
  需要 64×32B=2KB 的默认构造初始化（`PairingFrame` 非平凡），净收益不确定，
  未实施。
- `Encode/Decode` 的 64 位大端读写仍是逐字节循环；170 Mops/s 对握手流量
  （每会话约 5 帧）绰绰有余，未改。
- `Enqueue` 队列满时丢最旧帧：极端情况下可能丢掉尚未发送的 RESPONSE。
  上限 64 + 1s 重传可自愈，记录。

### P2-8（记录）测试 SKIP 情况

`ctest -R bento` 48 项中 `bento_netem` 因缺少 CAP_NET_ADMIN 按设计 SKIP
（`SKIP_RETURN_CODE 77`），与 pairing 无关。`test_pairing` 从不调用
`bento_test::Skip`，`Finish` 的 77 分支不可达，**无 SKIP 逃逸**。

## 3. 解耦检查

| 检查项 | 结果 |
|---|---|
| 头文件自包含 | `printf '#include "go2cpp/bento/net/pairing.hpp"' \| c++ -fsyntax-only -Wall -Wextra -Wpedantic -Werror` 通过 |
| `pairing.hpp` include | 仅 `<array> <cstddef> <cstdint> <deque> <functional> <memory> <string>` |
| `pairing.cpp` include | 自身头文件 + `<atomic> <cstddef> <utility>` |
| 链接期未定义符号 | `nm -C pairing.cpp.o` 只有 `operator new/delete`、`memmove`、异常/guard 运行时；**无 link/channel/session/transport/ITransport 的任何符号** |
| 依赖方向 | pairing 不 include `link.hpp`/`session*`/`transport.hpp`，只暴露纯值类型帧与状态机；`Link` 等上层可单向依赖 pairing |

结论：pairing 与传输层、链路层、会话层完全解耦，只依赖标准库与自身头文件。

## 4. 测试复查

### 4.1 规模与场景

| 项 | 审查前 | 审查后 |
|---|---|---|
| 测试函数（场景） | 13 | **20** |
| `BENTO_CHECK` 断言数 | 8,239 | **18,307** |
| 行覆盖率（pairing.cpp） | — | **98.56%**（485 行） |
| 分支执行率 | — | **100%**（293 分支） |
| 分支至少执行一次 | — | 85.32% |

新增场景（`tests/bento/test_pairing.cpp`）：

13. `TestActivateIdempotent`：重复 Activate 不重置计时器（P1 回归）。
14. `TestResponseKeyHashGuard`：RESPONSE key_hash 校验、未确认 ACK 拒绝（P2 回归）。
15. `TestLossyLinkConvergence`：30% 丢包 + 10% 重复 + 乱序，200/200 收敛。
16. `TestThousandPairings`：8 线程 ×125 = 1000 并发配对，nonce 唯一。
17. `TestRandomFrameFuzz`：2000 帧随机 fuzz，含非法 version/status/key/nonce。
18. `TestCodecThroughputSanity`：100 万次 32B 编解码 + 发送队列上限。
19. `TestNamesAndBoundaries`：全部枚举名字、越界枚举、空指针、尾部多余字节、Reset 保留统计。
20. `TestEdgeBranches`：timeout_ms=0 钳位、Pending 收 ACK/CONFIRM、Active 收错配帧。

### 4.2 断言有效性抽查

- 修复前 `TestRandomized` 只断言「状态枚举合法」，不检查跨会话不变量——本次在
  fuzz 中补了 `client Active ⇒ server Active` 与「双端 Paired/Active 时 pair_id 相等」。
- `TestLossyLinkConvergence` 的「≥98% 收敛」是聚合断言：P1 未修时实测 0%（旧版
  220/300 是修复中途数据），改动回退会立即失败，不是恒真断言。
- `TestThousandPairings` 断言 nonce 集合大小恰为 1000，可捕获 `NextNonce` 退化成常量。
- `TestCodecThroughputSanity` 逐帧比对 `pair_id/key_hash/nonce`，不是只统计次数。
- 未发现「硬编码返回值」「放宽断言掩盖问题」的用例；原有 13 个场景的断言逐条复核
  仍有效（错误码、状态、计数、帧字段均有对应断言）。

### 4.3 覆盖缺口（已尽量收敛）

剩余未覆盖行均为 P2-3 所述不可达防御分支（`HandleFrameImpl` 的 kRegister case、
`HandleConfirm` 的 status 检查）与 `DecodePairingFrame` 的二次 `IsKnownPairingError`
（前一行已按 u16 原值过滤，恒真）。不再为覆盖率添加无意义用例。

## 5. 修改清单

| 文件 | 修改 |
|---|---|
| `src/bento/net/pairing.cpp` | ① `Activate` 幂等（P1）；② 成功 RESPONSE 校验 key_hash（P2-1）；③ 未发 CONFIRM 拒绝 ACK（P2-2）；④ 默认策略改为进程级静态实例（构造 52.5→21.7ns）；⑤ `m_changes` 由 `std::deque` 改为 `std::array<StateChange,8>`（消除每状态迁移一次堆分配） |
| `include/go2cpp/bento/net/pairing.hpp` | 同步 `Activate` 幂等文档；`<array>`；`m_policy` 改为裸指针（所有权留在 `m_config.policy`）；`m_changes` 固定数组 + `kMaxChanges` |
| `tests/bento/test_pairing.cpp` | 新增场景 13–20（共 +10,068 条断言） |
| `tests/bento/audit_pairing_stress.cpp` | 新增独立压测/随机/场景程序（不注册 CTest） |

未改动 `bento` 其他模块、base、构建脚本；未 commit。

## 6. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 无警告构建 + 全量 bento 测试
cd .build-bento-wsl && ninja && ctest -R bento -j4 --output-on-failure

# 压测程序（独立编译，不依赖 CMake 目标）
cd /UserData/CodexWorkSpace/Go2cpp
c++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -I include -I tests/bento \
    tests/bento/audit_pairing_stress.cpp src/bento/net/pairing.cpp \
    -o .build-bento-wsl/audit/audit_pairing_stress -pthread

./.build-bento-wsl/audit/audit_pairing_stress 1.0 --mode=all
./.build-bento-wsl/audit/audit_pairing_stress 1.0 --mode=bench
./.build-bento-wsl/audit/audit_pairing_stress 1.0 --seed=12345 --mode=random

# ASan/UBSan（小规模）
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic -Werror -I include -I tests/bento \
    tests/bento/audit_pairing_stress.cpp src/bento/net/pairing.cpp \
    -o .build-bento-wsl/audit/audit_pairing_asan -pthread
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./.build-bento-wsl/audit/audit_pairing_asan 0.02 --mode=all

# valgrind
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
    ./.build-bento-wsl/audit/audit_pairing_stress 0.005 --mode=leaks
```

## 7. 未覆盖 / 后续

- 未做真实网络 UDP 往返（本模块不含 socket；`PairingSession` 是纯状态机，
  传输由调用方驱动）。有损链路用进程内「丢包/重复/乱序管道」模拟。
- `m_outbound` 环形缓冲（P2-7）未实施，收益/风险比不明确。
- P2-3 死代码与 P2-5 错误码语义未改，避免影响既有调用方。
- 本轮未跑 `-fsanitize=thread`：`PairingSession` 文档明确非线程安全（每线程
  独立实例），压测的 8 线程用例只共享 `NextNonce` 的原子计数器。
