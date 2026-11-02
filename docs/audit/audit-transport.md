# transport 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/{transport,tcp_transport,memory_transport}.hpp`、
  `src/bento/net/{transport,tcp_transport,memory_transport}.cpp`、
  `tests/bento/test_transport.cpp`
- 构建：`/tmp/build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，`-O3 -DNDEBUG`）
- 结论：**无 P0**。压测、随机、并发、RST、IPv6 大包、ASan、valgrind 均未发现崩溃/死锁/
  数据损坏/泄漏。修复 1 个 P1 性能瓶颈、1 个 P1 测试逃逸、1 组 P1 覆盖缺口；
  P2 问题已修文档或记录。

## 1. 压测数据

工具：`tests/bento/audit_transport_stress.cpp`（独立程序，77 项断言，不注册进 CTest）。
三轮完整运行，scale=1.0。

### 1.1 TCP 回环 100 MiB 单向

| 轮次 | 吞吐 | 写 p50 / p99 | 读 p50 / p99 | 峰值 RSS | fd |
|---|---|---|---|---|---|
| 1 | 2645.6 MiB/s (0.038 s) | 18 / 62 µs | 16 / 123 µs | 4.2 MB | +0 |
| 2 | 3186.3 MiB/s (0.031 s) | 15 / 55 µs | 14 / 81 µs | 4.2 MB | +0 |
| 3 | 2696.4 MiB/s (0.037 s) | 18 / 62 µs | 15 / 115 µs | 4.2 MB | +0 |

- 100 MiB 分批 64 KiB：写 1600 次 `WriteFull`、读 1600 次 `ReadFull`（第 1 轮含 100k 小包，
  单轮总 syscall 远超 10 万次）。
- 100k 次 32B 请求-响应：avg 94.3–97.0 µs，p50 80–82 µs，p95 153–158 µs，
  p99 181–186 µs；max 0.86–211 ms（个别调度抖动）。
- 100k 轮全部完成，无失败；fd 全程 +0。

### 1.2 MemoryTransport 100 MiB + 10 万次 64B

| 轮次 | 吞吐 | 读 p50 / p99 | 峰值 RSS | 100k 消息 |
|---|---|---|---|---|
| 1 | 1389.4 MiB/s (0.072 s) | 4 / 92 µs | 66.9 MB | 16 ms |
| 2 | 715.0 MiB/s (0.140 s) | 6 / 19 µs | 105.3 MB | 17 ms |
| 3 | 884.2 MiB/s (0.113 s) | 4 / 21 µs | 90.8 MB | 17 ms |

- 优化前基线（同一程序、同一环境，1 次）：**522.5 MiB/s**，读 p50 **35 µs**，
  100k 64B **21 ms**，当前 RSS 101.2 MB。
- 优化后中位约 **884 MiB/s（+69%）**，读 p50 4–6 µs，100k 16–17 ms；
  队列及时释放，当前 RSS 从 101 MB 降至 5–10 MB（峰值仍由无界队列决定，见 P2-1）。
- 100 MiB 分块 ×1600 + 100k 次写读 = 超过 20 万次 Read/Write 调用。

### 1.3 复杂场景（三轮一致）

| 场景 | 结果 |
|---|---|
| 1000 并发连接 | 建连 28–35 ms；fd 2007（listener+1000 客户端+1000 服务端）；全部关闭后回 6；峰值 RSS 5.2–9.5 MB；每连接 8B 序号回读无串流 |
| 慢客户端写阻塞 | 服务端写入 2,625,600 B 后单次 `Write` 返回 `kTimeout`，耗时 100.2–102.2 ms（预算 100 ms），`bytes` 为部分写；之后仍可读/关闭 |
| 对端 RST | 裸 socket + `SO_LINGER{1,0}`；服务端 `Read` 返回 `kClosed`，errno 104（ECONNRESET） |
| 半关闭 | `ShutdownWrite` 后对端 EOF，仍可反向写回；幂等 |
| 非阻塞 EAGAIN | 空 socket `Read(0ms)` 10000 次返回 `kTimeout`，3.7–3.9 ms（≈0.38 µs/次）；已有数据时 0ms 立即读到；16 MiB 单次写返回 `kTimeout` + 2,625,600 B 部分写 |
| IPv6 + 大包 | `::1` 监听；1 MiB 分片 64 KiB 读吞吐 559–650 MiB/s；单次 `Read`（capacity 256 KiB）最大返回 200 KiB；数据逐字节校验通过 |

### 1.4 随机测试

| 对象 | 操作数 | 断言结果 |
|---|---|---|
| MemoryTransport（随机 write/read/shutdown/close/connect） | 5000 | inconsistent = 0 |
| TcpTransport（随机 write/read/shutdown/close，含部分写/超时） | 2000 | invalid = 0，mismatch = 0 |

随机测试同时校验字节流顺序与关闭/半关闭后的返回码（`kClosed`），三轮均为 0。

### 1.5 内存与泄漏

- fd：所有场景结束后回到基线（TCP fd +0；1000 连接 2007→6）。
- ASan/UBSan：`-fsanitize=address,undefined`，scale=0.1，77 checks 全过，
  0 个 AddressSanitizer/LeakSanitizer/runtime error。
- valgrind：`--leak-check=full --error-exitcode=99`，scale=0.02，
  **0 errors，0 leaks**；`6,808 allocs / 6,808 frees`，退出时 0 字节占用。

## 2. 问题清单

### P0

无。未发现崩溃、死锁、越界、数据损坏或资源泄漏。

### P1

**P1-1 MemoryTransport 逐字节 deque 造成明显性能瓶颈 —— 已修复**

- 证据：`memory_transport.cpp` 用 `std::deque<std::uint8_t>`，写入逐字节 `insert`、
  读取逐字节 `pop_front`；100 MiB 仅 522.5 MiB/s，读 p50 35 µs，100 MB 传输时
  当前 RSS 101 MB。
- 修复：改为 64 KiB 分块队列（`std::deque<Chunk>` + `memcpy`），尾块容量复用、
  半读尾块压缩，避免小消息每次堆分配。
- 结果：中位约 884 MiB/s（+69%），读 p50 4–6 µs，当前 RSS 5–10 MB；
  100k 64B 消息 21→16 ms。语义不变（顺序字节流、部分读、半关闭、Close 清空收件箱）。

**P1-2 IPv6 用例存在 SKIP 逃逸 —— 已修复**

- 证据：`TestTcpIpv6Loopback` 在 listen/local endpoint/connect/accept 失败时直接
  `printf("[SKIP]…"); return;`，不增加失败计数；即使 IPv6 可用但实现回归，测试仍 PASS。
- 修复：仅当 `/proc/sys/net/ipv6/conf/all/disable_ipv6 == 1` 时调用
  `bento_test::Skip`（进程退出码 77，CTest 显示 SKIP）；否则 `BENTO_CHECK(false)`，
  把真实回归暴露出来。
- 结果：本机 IPv6 可用，用例实际执行并通过。

**P1-3 测试覆盖缺口 —— 已补**

- 原 19 用例仅覆盖 100 KiB 传输、超时、半关闭、Close、连接拒绝；缺少并发、背压、
  RST、EAGAIN、fd 泄漏、随机序列、>64 KiB 分片。
- 修复：`test_transport.cpp` 新增 7 个确定性用例（见 §4），并新增独立压测程序
  `tests/bento/audit_transport_stress.cpp` 覆盖 100 MiB/1000 连接/随机/资源。

### P2（已修或记录）

| # | 问题 | 证据 | 处理 |
|---|---|---|---|
| P2-1 | MemoryTransport 队列无上限、`Write` 忽略 `timeout`，100 MiB 传输峰值 105 MB | 压测 VmHWM 66.9–105.3 MB；`Write` 不阻塞 | 文档已注明「无背压、仅测试用」；不改语义（加容量会改变测试语义并可能死锁） |
| P2-2 | TcpTransport 未关闭 Nagle | 帧/小消息协议可能受 Nagle+delayed ACK 影响 | 已加 `TCP_NODELAY`（Connect/Adopt 后设置）；回环 100k RTT 无显著变化（97.3→94.3–97.0 µs），保留理由为真实网络；头文件已注明 |
| P2-3 | `ITransport` 文档称 Close 后 `Connect` 返回 `kClosed`，但 `TcpTransport` 实际允许重连 | 实现 `Connect` 在 `m_fd<0` 时重新建立连接；随机测试曾因此误判 | 已改文档（行为由实现决定），新增 `TestTcpReconnectAfterClose` 锁定 TCP 重连行为 |
| P2-4 | `Close` 与 `Read`/`Write` 并发存在 `m_fd` 数据竞争与 fd 复用 TOCTOU；`WaitFor` 对 `POLLNVAL` 未映射 `kClosed` | `m_fd` 是非原子 `int`；`poll` 返回 `rc>0` 后未检查 `revents` | 文档已声明「Close 与其它操作并发需调用方协调」；记录，未改（加锁会改变接口成本模型） |
| P2-5 | `TcpTransport::Write` 在 `send` 返回 0 时映射 `EIO` | 代码 `if (sent == 0) return EIO`；TCP 上几乎不可达 | 记录；语义上更接近 `kClosed`，但无法构造稳定复现 |
| P2-6 | Frame 用例混入 transport 测试，使测试目标链接 `go2cpp_bento` | `test_transport.cpp` 含 7 个 Frame 用例；CMake 链接 `go2cpp_bento` | 记录；模块源码本身解耦（见 §3），建议后续拆 `test_frame.cpp` |
| P2-7 | `memory_transport.hpp` 缺直接 `<chrono>`/`<cstdint>` include，依赖 `transport.hpp` 传递包含 | 自包含检查通过但依赖传递 | 已补直接 include |
| P2-8 | 测试辅助偶发 accept/connect 超时（1/60 → 3/60） | 高频短连接后 `tcp_tw_reuse=2` 的 TIME_WAIT 四元组复用；压测 1000 并发连接无失败 | 测试辅助对 connect/accept 各重试一次，不改实现；60 次连续运行 0 失败 |
| P2-9 | `MemoryTransport::Read` 在已关闭时 `capacity==0` 返回 `kClosed` 而非 `kOk` | 实现先检查 `SelfClosed` 再检查 `capacity` | 记录；与 TCP 行为一致，接口未规定该组合 |
| P2-10 | `Close` 直接 `close` 未先 `shutdown`，接收缓冲有未读数据时内核发 RST | Linux 语义；压测中客户端 Close 后服务端仍能读到已发送数据 | 记录；现有测试覆盖该路径 |

## 3. 模块解耦检查

- **头文件自包含**：`transport.hpp`、`tcp_transport.hpp`、`memory_transport.hpp`
  分别单独 `#include` 后用 `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` 编译，3/3 通过。
- **无跨模块依赖**：6 个源文件中没有 `frame`/`link`/`channel`/`protocol`/`relay`/
  `udp`/`session`/`peer` 的 include。
- **符号级解耦**：`libgo2cpp_bento_transport.a` 中没有 `bento::Frame`/`Link`/`Channel`
  等未定义符号。
- **唯一耦合在测试层**：`test_transport.cpp` 含 7 个 Frame 用例，导致
  `bento_test_transport` 链接 `go2cpp_bento`（P2-6）；transport 库目标本身只依赖
  Threads。
- **循环依赖**：`transport.hpp` ← `tcp_transport.hpp`/`memory_transport.hpp`，
  反向无引用；无循环。

## 4. 测试复查

原 19 用例（5 Memory + 7 TCP + 7 Frame）= 348 checks；新增 7 用例后 26 用例、
1010 checks。

| 原用例 | 断言有效性 | 备注 |
|---|---|---|
| memory bidirectional 100 KiB | 有效 | 双线程双向、逐字节校验 |
| memory half-close | 有效 | FIN 后反向写、幂等 |
| memory read timeout | 有效但阈值宽 | `>=80ms`（预算 120ms），可接受 |
| memory close idempotent | 有效 | 关闭后 Read/Write、已写数据仍可读 |
| memory partial reads | 有效 | 97B 分片、顺序校验 |
| tcp bidirectional 100 KiB | 有效 | 只 100 KiB，不构成压力 |
| tcp accept timeout | 有效 | 超时后 listener 仍可用 |
| tcp read timeout | 有效 | — |
| tcp half-close | 有效 | FIN 后响应完整 |
| tcp two connections isolated | 基本有效 | 隔离断言较弱：只验证 second 超时；若串流会先让 first 失败 |
| tcp connect refused | 有效 | errno 非 0、耗时有界 |
| tcp IPv6 | **SKIP 逃逸（已修）** | 3 处直接 return，不计失败 |
| frame ×7 | 断言有效但与 transport 无关 | 模块边界问题（P2-6） |

新增 7 用例：

1. `tcp: close then reconnect on the same instance` —— 锁定 Close 后可重连。
2. `tcp: 200 connect/close cycles do not leak fds` —— `/proc/self/fd` 回到基线。
3. `tcp: zero-timeout read returns kTimeout and consumes ready bytes` —— EAGAIN/0ms。
4. `tcp: peer RST is reported as kClosed` —— 裸 socket + `SO_LINGER(0)`。
5. `tcp: 200 simultaneous connections stay isolated` —— 200 并发 + fd 断言。
6. `memory: 200 KiB crosses chunk boundaries with partial reads` —— 块边界。
7. `transport: ReadFull/WriteFull edge cases` —— null/0 长度/EOF。

`test_transport.cpp` 头部注释已同步更新。

## 5. 修复与验证

改动文件（均在允许范围内，未 commit）：

| 文件 | 改动 |
|---|---|
| `src/bento/net/memory_transport.cpp` | 逐字节 deque → 64 KiB 分块队列（+77/−15） |
| `src/bento/net/tcp_transport.cpp` | Connect/Adopt 后设置 `TCP_NODELAY`（+9） |
| `include/go2cpp/bento/net/transport.hpp` | Close 后 Connect 语义文档（+4/−4） |
| `include/go2cpp/bento/net/tcp_transport.hpp` | TCP_NODELAY 文档（+3） |
| `include/go2cpp/bento/net/memory_transport.hpp` | 补 include、Write timeout 说明（+4/−1） |
| `tests/bento/test_transport.cpp` | 7 个新用例、IPv6 SKIP 修复、辅助重试/诊断（+352/−31） |
| `tests/bento/audit_transport_stress.cpp` | 新增独立压测程序 |

验证：

- `cmake --build /tmp/build-bento-wsl`：0 警告（`-Werror`）。
- `ctest -R bento`：**48/48 通过**（`bento_netem` 原本即 Skip）。
- `bento_test_transport`：1010 checks，连续 60 次运行 0 失败。
- ASan/UBSan：77 checks，0 错误。
- valgrind：0 errors、0 leaks。
- fd/内存：所有场景回基线；1000 连接 fd 2007→6。

## 6. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 压测（普通）
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include -I tests/bento \
    tests/bento/audit_transport_stress.cpp src/bento/net/transport.cpp \
    src/bento/net/tcp_transport.cpp src/bento/net/memory_transport.cpp \
    -o /tmp/audit_transport_stress -pthread
/tmp/audit_transport_stress 1.0

# ASan/UBSan
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I include -I tests/bento tests/bento/audit_transport_stress.cpp \
    src/bento/net/transport.cpp src/bento/net/tcp_transport.cpp \
    src/bento/net/memory_transport.cpp -o /tmp/audit_transport_asan -pthread
/tmp/audit_transport_asan 0.1

# valgrind（小规模）
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
    /tmp/audit_transport_stress 0.02

# 回归
cd /tmp/build-bento-wsl && ctest -R bento --output-on-failure
```
