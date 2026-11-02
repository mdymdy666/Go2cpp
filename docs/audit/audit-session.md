# session 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/app/session.hpp`、`src/bento/app/session.cpp`、
  `tests/bento/test_session.cpp`
- 压测工具：`tests/bento/audit_session_stress.cpp`（独立程序，不注册 CTest；
  刻意不 include `test_util.hpp`，以免被 `link.hpp` 污染依赖）
- 构建：`.build-bento-wsl`（Ninja，Release，`-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，GCC 13.3，12 vCPU
- 结论：**无 P0**。修复 2 个 P1（Closing 窗口把 in-flight 数据误判协议错误；
  部分写后配对重传插到半帧前面）+ 2 个 P2（关闭窗口内定时器覆盖正常关闭；
  写路径 O(n²) 前缀擦除），并做 1 项读路径吞吐增强；补 7 个测试场景
  （20/266 → **27/378 checks**）；P2 记录 9 项。`ctest -R bento` **48/48 通过**
  （串行 `-j1`；`bento_netem` 按设计 SKIP。`-j4` 并行时
  `bento_peer_hub` 偶发端口竞争，单独重跑通过，与本次 session 改动无关）。

## 1. 压测数据

### 1.1 1000 并发会话（MemoryTransport，单线程 pump，3 次取中位）

| 指标 | 数值 |
|---|---|
| 会话数 | 1000 对（client+server，共 2000 个 Session） |
| 建连（Start → 双方 Active） | **0.012 s**（3 轮 Poll；构造 0.004 s） |
| 数据（10 条 × 64 B × 1000 会话） | **0.003 s**，10,000 条，**3.10 M msg/s**、**189 MiB/s** |
| 关闭（Close → 双方 Closed） | **0.001 s**，1000/1000 |
| RSS | 3,712 → 18,048 KiB（活跃，≈14.0 MiB）；析构后 18,048 KiB（glibc arena 未归还，非泄漏） |
| fd | 7 → 7（MemoryTransport 不占 fd） |

### 1.2 10 万次状态转换

| 指标 | 数值 |
|---|---|
| 转换数 | 100,000（10,000 轮 建连→Active→数据→断开） |
| 耗时 | **0.038 s**（中位） |
| 速率 | **2.61 M transitions/s**，**383 ns/transition** |
| 非法迁移 / 错误 | 0 / 0 |

### 1.3 随机状态机（1000+ 操作）

- 3 个 seed × **20,000 操作** = 60,000 操作、**327,906 checks/seed**，**0 失败**。
- 操作分布（单 seed）：Poll 6,659 / Send 2,894 / RequestControl 1,649 / Close 1,561 /
  推进假时钟 / 注入畸形帧 1,198 / Kill 连接 770 / 重建 943。
- 每个 Session 的 `on_state` 都校验 `IsSessionTransitionAllowed`：非法迁移 **0**。
- 故障注入（写失败、底层 Close、坏 magic、陌生 peer Control）下 `on_error` 累计 5 次，
  均为预期的错误关闭；进程不崩、状态恒在枚举内。

### 1.4 五个真实场景

| # | 场景 | 结果 |
|---|---|---|
| 1 | 连接失败（真实 TCP `127.0.0.1:1`） | `kConnectFailed`（ECONNREFUSED，0 ms）；重复 200 次失败重试 fd **7 → 7** |
| 2 | 配对超时（假时钟 +1 ms 越过 deadline） | `kPairingTimeout`，状态序列 Connecting→Closing→Closed，3 transitions |
| 3 | Closing 状态 in-flight 数据 | 50 轮注入 Data/Control/ChannelOpen/坏帧：`last_error=none`、`protocol_errors=0`、无 on_error（修复前 `last_error=13 kProtocolError`） |
| 4 | 重连（断开 → 重建 → Active） | Memory 200 轮 0 失败；真实 TCP 50/50 轮 0 失败，fd **7 → 7** |
| 5 | 60 s 长稳（反复建连/断开） | **15,483,882 轮 / 154,838,820 次状态转换**，0 失败；RSS 17,772 → 17,772 KiB（**全程恒定**）；fd 7 → 7 |

### 1.5 资源与泄漏

| 项目 | 结果 |
|---|---|
| 全量压测（all，60.31 s） | **343,299 checks，0 失败** |
| ASan + UBSan（test_session + 压测 scale 0.02） | 378 + 17,956 checks，0 失败，**0 sanitizer 报告** |
| valgrind（leaks 模式 0.02） | **9,276 allocs / 9,276 frees**，`in use at exit: 0 bytes`，**0 errors** |
| 覆盖率（gcov，test_session ∪ audit） | 行 **84.28%**（686/814）；test 单跑 82.40%、audit 单跑 68.25% |

## 2. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁或内存泄漏（ASan/UBSan/valgrind/随机测试全绿）。

### P1-1（已修）Closing 窗口把 in-flight 数据误判为协议错误

- 证据：`HandleData`/`HandleControl`/`HandleChannelOpen`/`HandleChannelAck` 只接受
  `kActive`，其余状态一律 `ProtocolFail`。真实 TCP 半关闭语义下，本端 `Close()`
  发 FIN 后，对端在收到 FIN 之前发出的数据仍在途；本端读到它就是
  `kProtocolError`（`last_error=13`），正常关闭被日志/监控误报。压测场景 3 在
  修复前 50/50 轮复现（`last_error=13`、`protocol_errors=1`、`on_error` 触发），
  修复后 0 次。
- 修复：
  - `HandleFrame` 在 `kClosing` 只处理 `kClose`（直接 `FinishClose`），其余帧丢弃；
  - `ProcessFrames` 解码失败、`PumpRead` 读错误在 `kClosing` 直接 `FinishClose`，
    不覆盖 `last_error`；
  - 正常关闭仍是 `kNone`，`on_error` 不触发。
- 回归：`TestClosingIgnoresInflightFrames`（注入 Data/Control/ChannelOpen）+
  压测场景 3；旧实现跑该测试 21/377 checks 失败。

### P1-2（已修）部分写后配对重传插到半帧前面

- 证据：`Flush` 部分写后 outbox 队首是「剩余字节」，而 `DrainPairingOutbound`
  每次 Flush 都无条件 `push_front` 重传的配对帧（CONFIRM/RESPONSE）。两者叠加时
  对端收到「半帧 + 新帧 + 剩余」→ 帧边界错乱（协议错误/数据损坏）。触发条件：
  通道握手期写超时 + 配对重传窗口（默认 500 ms），真实弱网可复现。
- 修复：
  - outbox 增加 `outbox_offset`（队首帧已写前缀），部分写后从断点续写；
  - `DrainPairingOutbound` 在 `outbox_offset != 0` 时改为 `push_back`（等半帧
    发完），仅队首完整时才插队首；
  - 一并去掉 `bytes.erase(begin, begin+bytes)`：大帧 + 多次超时是 O(n²) 拷贝。
- 回归：`TestPartialWritePairingRetransmit`（RESPONSE 只写 7 B → 推进到重传 →
  恢复写 → 双方 Active + 数据正确）；旧实现该用例无法 Active 且协议错误。

### P1-3（已修）测试覆盖缺口

- 原测试实际 20 场景 266 checks（`docs/modules/bento-modules.md` 仍写 13/200，
  文档漂移，见 P2-9）。缺：Closing in-flight、部分写 + 配对重传、关闭窗口定时器、
  应用队列满（`kQueueFull`）、重连、真实 TCP 失败、状态/错误目录全矩阵。
- 已补 7 个场景（+112 checks，**378**）：上述 P1-1/P1-2/P2-1 的回归 + 覆盖缺口。
- 反向验证：把 `session.cpp` 临时恢复为 HEAD 版本跑新测试，**21/377 checks 失败**
  （集中在三个回归场景）；恢复修复版后 378/378 通过。

### P2（已修）

1. **关闭窗口内定时器覆盖正常关闭**：`Close()` 后 `TickTimers` 仍驱动 pairing Tick
   与心跳；当 `pairing_timeout_ms`/`heartbeat_timeout_ms` 小于 `close_timeout_ms`
   （如 100/200 vs 1000）时，正常关闭被改写成 `kPairingTimeout`/`kHeartbeatTimeout`
   并触发 `on_error`。修复：`TickTimers` 在 `kClosing` 只检查 `close_deadline_ms`。
   回归：`TestCloseWindowSkipsTimers`（旧实现失败）。
2. **写路径 O(n²) 前缀擦除**：见 P1-2 的 `outbox_offset` 改造（顺带消除）。
3. **单次 Poll 只读 4 KiB**：`PumpRead` 一次 `Read` 后即返回，高吞吐时读上限被
   4 KiB/Poll 锁死。修复：读满一屏后以 0 预算继续读，单次 Poll 最多 8 屏
   （32 KiB），避免饿死定时器。bench 小消息场景未单独量化（读路径非瓶颈），
   作为吞吐增强记录。

### P2（记录，本次不改）

1. `SendFrame` 编码失败与发送队列满都返回 `kQueueFull`，调用方无法区分。
2. `kClose` 帧载荷长度不做校验（1 B `SessionError` 仅文档约定），关闭路径选择
   宽容处理，避免把对端实现差异变成协议错误。
3. `DrainPairingOutbound` 绕过 `max_outbox_frames`：队列上限只约束应用帧，
   配对/确认帧优先（合理但未在头文件声明）。
4. `Session::WaitFor` 用真实时间 + `Poll(1)` 忙等，注入假时钟时不适用；建议
   调用方自行 pump。
5. 配置未校验 `heartbeat_interval_ms` 与 `heartbeat_timeout_ms` 的大小关系、
   `io_timeout_ms` 上界；非法组合只会表现为更早超时，不崩溃。
6. `control->Join()` 返回值未检查：`max_participants` 过小时通道仍会 Active，
   但控制权仲裁缺少参与者（配置边界）。
7. `Closed` 是终态，不支持原地重连；需重新构造 `Session`（头文件已说明）。
   测试与压测均按「新对象重连」验证。
8. `Closing` 窗口丢弃的接收帧不计入 `dropped_frames`（该字段语义是发送队列满），
   头文件已注明。
9. 文档数字漂移：`docs/modules/bento-modules.md` 记 13/200，实际改前 20/266、
   改后 27/378；本次边界不含 docs。

## 3. 解耦审查

| 检查项 | 方法 | 结果 |
|---|---|---|
| 头文件自包含 | 单独 `#include "go2cpp/bento/app/session.hpp"` 的 TU，`-Werror -fsyntax-only` | **通过** |
| include 树 | `g++ -H` | 只有 `session.hpp → session_control.hpp + transport.hpp`；**无** link/peer_hub/session_table |
| 目标文件符号 | `nm -C -u session.cpp.o` | 未定义符号仅 pairing/heartbeat/frame/transport/session_control/tcp_transport + libstdc++；**0 个 Link/PeerHub** |
| 最小链接 | 压测程序只链 `libgo2cpp_bento_session/transport/bento`，不 include `link.hpp` | 构建通过；二进制**无** `Link`/`PeerHub`/`SessionTable` 符号 |
| 依赖方向 | `session.hpp` 仅依赖 net 原语；`session.cpp` 额外用 frame（线格式）、heartbeat、pairing、`TcpTransport`（默认工厂） | 符合「只依赖 transport/pairing/channel/heartbeat/session_control」 |

## 4. 测试复查

| 项目 | 结论 |
|---|---|
| 断言有效性 | 新测试经反向验证（旧实现 21/377 失败）；确定性用例，无真实 sleep 依赖 |
| 覆盖缺口 | 合并行覆盖 84.28%；剩余 128 行集中在协议错误分支（畸形长度/peer/channel）、工厂抛异常、pairing start 失败、switch 兜底 return（不可达防御） |
| SKIP 逃逸 | `test_session` 无 `Skip()` 调用；`CMake` 的 `SKIP_RETURN_CODE 77` 不触发。`test_util.hpp::Finish` 只按 failures 判定、不校验 `g_checks > 0`（空测试会静默 PASS，属框架层，记录） |
| 脆弱断言 | `TestPairingRejected` 允许 client 两种错误（时序竞争，可接受）；`TestQueueFullDropsInternalFrames` 依赖激活期帧数（内部实现耦合，记录） |
| 文档一致性 | `bento-modules.md` 13/200 与现状 27/378 不一致（P2-9） |

## 5. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 构建 + 单元测试
ninja -C .build-bento-wsl
./.build-bento-wsl/bento_test_session                # 378 checks
(cd .build-bento-wsl && ctest -R bento -j1)          # 48/48（netem SKIP）

# 压测（独立编译，不注册 CTest）
g++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
    tests/bento/audit_session_stress.cpp src/hash/crc32.cpp \
    .build-bento-wsl/libgo2cpp_bento_session.a \
    .build-bento-wsl/libgo2cpp_bento_transport.a \
    .build-bento-wsl/libgo2cpp_bento.a \
    -o .build-bento-wsl/audit_session_stress
./.build-bento-wsl/audit_session_stress --mode all --scale 1.0     # ≈60 s
./.build-bento-wsl/audit_session_stress --mode bench               # 1000 并发 + 10 万转换
./.build-bento-wsl/audit_session_stress --mode random --seed 7
./.build-bento-wsl/audit_session_stress --mode scenarios --scale 1.0
./.build-bento-wsl/audit_session_stress --mode soak --seconds 60

# ASan/UBSan
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -Itests/bento \
    tests/bento/test_session.cpp src/bento/app/session.cpp src/bento/net/frame.cpp \
    src/bento/net/pairing.cpp src/bento/net/heartbeat.cpp \
    src/bento/net/session_control.cpp src/bento/net/protocol.cpp \
    src/bento/net/transport.cpp src/bento/net/memory_transport.cpp \
    src/bento/net/tcp_transport.cpp src/hash/crc32.cpp -o /tmp/bento_session_asan
ASAN_OPTIONS=detect_leaks=1 /tmp/bento_session_asan

# valgrind
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
    ./.build-bento-wsl/audit_session_stress --mode leaks --scale 0.02
```

## 6. 改动文件

| 文件 | 改动 |
|---|---|
| `include/go2cpp/bento/app/session.hpp` | Closing 语义/统计字段/Poll 读预算注释（+11/−4 行） |
| `src/bento/app/session.cpp` | Closing 帧过滤、解码/读错误收尾、`outbox_offset` 断点续写、配对帧不插队、关窗定时器、读循环（+97/−34 行） |
| `tests/bento/test_session.cpp` | +7 场景（Closing in-flight、部分写重传、关窗定时器、队列满、重连、TCP 失败、状态/错误目录），266 → 378 checks |
| `tests/bento/audit_session_stress.cpp` | 新增独立压测工具（1,225 行，不注册 CTest） |
