# relay 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/relay.hpp`、`src/bento/net/relay.cpp`、
  `tests/bento/test_relay.cpp`
- 压测工具：`tests/bento/audit_relay_stress.cpp`（独立程序，不注册 CTest）
- 构建：`.build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，`-O3 -DNDEBUG`，
  `-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU，20 GB 内存，GCC 13.3
- 结论：**无 P0**。修复 2 个 P1（Defrag 搬移放大、FIN 传播错误被吞）与
  1 组 P1 测试缺口/SKIP 逃逸；P2 记录 6 项。不 commit，未改动 relay/tests
  之外的源码。

## 1. 压测数据

### 1.1 真实 TCP 双向 128 MiB × 3 轮

| 轮次 | 聚合吞吐 | 单向 | 耗时 | peak_pending | RSS |
|---|---|---|---|---|---|
| 1 | 677.2 MiB/s | 338.6 MiB/s | 378 ms | 16 KiB / 16 KiB | 4.0 MB |
| 2 | 629.0 MiB/s | 314.5 MiB/s | 407 ms | 16 KiB / 16 KiB | 4.0 MB |
| 3 | 608.1 MiB/s | 304.0 MiB/s | 421 ms | 16 KiB / 16 KiB | 4.0 MB |

- 每轮双向各 128 MiB（合计 256 MiB），三轮累计搬运 768 MiB；逐块 `memcmp` 校验。
- fd 全程不变（前后均 5）；VmHWM 增量 512 KiB；无错误。
- 吞吐瓶颈是单线程切片轮转 + 每方向 16 KiB 读块下的 syscall/调度，不是内存
  （peak pending 仅 16 KiB）；read_chunk_bytes 仍取默认 16 KiB，调大后的收益
  不在本次结论内。

### 1.2 背压微基准：512 MiB、接收端每次只收 4 KiB（修复前后）

| 版本 | 7 轮范围 | 中位 | 耗时 | peak_pending |
|---|---|---|---|---|
| HEAD（旧 Defragment） | 6.65–6.92 GiB/s | ~6.83 GiB/s | 74–77 ms | 65536 |
| 修复后（半容量阈值） | 17.07–18.96 GiB/s | ~18.3 GiB/s | 27–30 ms | 61440 |
| 对照：不设限接收端 | 30–34 GiB/s | ~32 GiB/s | 15–17 ms | — |

- 同一二进制源、同一机器、同一 512 MiB 负载；仅替换 `relay.cpp` 的 Defragment 策略。
- 提升 **≈2.7 倍**，来自消除「写 4 KiB → memmove 60 KiB」的 15 倍内存带宽放大。
- 这是进程内微基准；真实 TCP 的接收缓冲会吸收部分写，收益依负载而定（1.1 节
  TCP 吞吐不受影响）。

### 1.3 1000 / 200 并发 relay

| 场景 | 结果 |
|---|---|
| 1000 条 MemoryTransport relay，双向各 8 KiB | 769–890 ms；RSS 143.6 MB（136.3–136.6 KB/relay）；fd 5→5；全部 Completed，peak_pending ≤64 KiB |
| 1000 条空闲 relay（1 s，poll=1 ms） | 进程 CPU 5.19–5.80 s（≈5.2–5.8 核） |
| 1000 条空闲 relay（1 s，poll=5 ms） | 进程 CPU 1.63–2.66 s（≈1.6–2.7 核） |
| 200 条真实 TCP relay，双向各 4 KiB | 158–170 ms；fd 5→807（4 fd/relay，含监听）→回 5；RSS +12–13 MB；全部 Completed |
| 200 次 TCP relay 建立/关闭 | fd 5→5，无泄漏 |

### 1.4 随机混沌（1200 组 × 5 个种子）

| 种子 | checks | 失败 |
|---|---|---|
| 1 | 19,560 | 0 |
| 7 | 19,504 | 0 |
| 42 | 19,520 | 0 |
| 999 | 19,484 | 0 |
| 20261029 | 19,504 | 0 |

- 组成：900 组正常完成（随机字节、随机读/写分片、随机 0–4 次写阻塞、5% FIN
  失败）、120 组 idle 超时、60 组永久写阻塞（kSlowConsumer）。
- 断言：不崩、不超时挂死（<5 s）、数据必须是输入的有序前缀/完整相等、双向
  pending ≤ buffer、统计数据 ≤ 输入量、FIN 只发生在 pending 排空后、无 OOM。
- 修复前同一组用例有 46 个 FIN 失败用例被误报为 `Completed`（见 P1-2）。

### 1.5 ≥5 个复杂场景

| 场景 | 结果 |
|---|---|
| 慢客户端（写永久阻塞） | 脚本传输：70 ms 判定 kSlowConsumer，读入 64 KiB；真实 TCP：358–579 ms 判定，读 4,321,920 B、写出 4,259,136 B，pending 62,784 B ≤64 KiB |
| 半关闭传播（FIN 后反向仍可写） | 请求/响应 32 KiB + 48 KiB 全部到达，`half_closes=2`；FIN 时刻 `bytes_at_shutdown=262144`（256 KiB 全部写完后才发 FIN） |
| idle 超时 | 无流量 99–102 ms 退出（配置 100 ms）；有流量（60 ms 间隔、idle=200 ms）620 ms 不误杀，`idle_timeouts=0` |
| 背压（512 KiB，上限 64 KiB） | 完整通过，peak_pending 53,248–61,440 B ≤65,536 |
| 长稳 60 s 双向 | 232.2/232.2 MiB 各方向，60.0 s；RSS 3.97→4.10 MB；fd 最大 9（基线 5+4 socket）；0 失败；进程 CPU 2.96 s |

### 1.6 内存与泄漏

- ASan/UBSan：`test_relay` 337 checks 全过；压测程序 scale=0.02、742 checks 全过，
  0 条 sanitizer 报告。
- Valgrind（scale=0.01，425 checks）：**0 errors、0 leaks**；1,331 allocs /
  1,331 frees，退出时 0 字节占用。
- TSan（`setarch -R` 关闭 ASLR 后）：337 checks 全过，0 条 ThreadSanitizer 警告。
- fd：所有场景结束后回到基线（TCP 并发 807→5；200 次循环 5→5）。

## 2. 问题清单

### P0

无。未发现崩溃、死锁、越界、数据损坏或资源泄漏。

### P1（全部已修复）

**P1-1 Defragment 在 4 KiB 部分写下产生 15 倍 memmove 放大 —— 已修复**

- 证据：缓冲 64 KiB、接收端每次只收 4 KiB 时，`begin` 每轮只推进 4 KiB，
  `end==capacity` 触发 `Defragment()`，每次搬移约 60 KiB；即每转发 4 KiB
  额外搬 60 KiB。512 MiB 负载下吞吐 6.65–6.92 GiB/s（不设限对照 30–34 GiB/s）。
- 修复：`Defragment()` 只在 `begin >= buffer.size()/2` 时搬移；未达阈值本轮
  不读（`contiguous_room()==0`），写侧继续排空，达到阈值再压缩，不会死锁。
- 结果：2.7 倍（17.07–18.96 GiB/s），relay.cpp:103-124。

**P1-2 `ShutdownWrite` 返回 kError 被忽略，半完成的关闭被报成 kCompleted —— 已修复**

- 证据：旧 `SendPendingFin` 执行 `(void)dst.ShutdownWrite()` 后无条件
  `fin_sent=true; ++half_closes`。混沌测试中 5% 注入 FIN 失败，46/1200 组仍返回
  `kCompleted`；专项用例中 `shutdown_error` 传输被报告为成功（`system_error=0`、
  `half_closes=1`）。
- 修复：`SendPendingFin` 返回 bool；`kError` → `m_system_error` + 调用方记
  `write_errors` 并以 `kWriteError` 结束；`kClosed` 只标记已关闭、不计
  `half_closes`。
- 结果：混沌与专项用例全部预期通过；relay.cpp:173-180, 302-323。

**P1-3 测试覆盖缺口与 SKIP 逃逸 —— 已修复**

- 证据：3 个真实 TCP 用例在 `MakeTcpPair()` 失败时直接 `Skip()` 返回；若
  listen/connect/accept 实现回归，测试会被标成 SKIP 而不是失败。缺少读错误、
  FIN 失败、FIN 顺序、慢而能动、配置归一化、跨运行统计、idle 复位、
  MemoryTransport、fd 循环等用例；原仅 80 checks。
- 修复：新增 `TcpLoopbackUsable()`（只有 socket/bind 本身失败才 SKIP，
  MakeTcpPair 失败改为 `BENTO_CHECK(false)`）；新增 10 个确定性用例。
- 结果：80 → **337 checks**，连续 30 次运行 0 失败。

### P2（记录，未改或已补文档）

| # | 问题 | 证据 | 处理 |
|---|---|---|---|
| P2-1 | 每个 Relay 在 Run 开始即分配 2×buffer_bytes（默认 128 KiB），空流/空闲也占 | 1000 并发 RSS 143.6 MB ≈136.3 KB/relay | 记录；懒分配会增加状态与复制复杂度，默认 64 KiB 下可接受 |
| P2-2 | 每条空闲连接每个 poll 周期唤醒一次；1000 条空闲连接 poll=1 ms 实测约 5–6 核 | 1.3 节 | 记录，并在 `relay.hpp` 说明：上千连接应调大 poll 或改用事件驱动；不默认调大以免增加转发延迟 |
| P2-3 | 传输实现抛异常或 buffer 分配失败时 `Run` 不会执行 `Close` | 代码路径：`Runner` 构造/`Execute` 内无捕获 | 头文件 `@note` 已明确调用方需自行释放；不引入 catch 改变错误模型 |
| P2-4 | `NowMs()` 截断到毫秒，idle 判定可能比配置早 0–1 ms | `TestIdleTimeout` 已留 5 ms 余量 | 记录；不影响正确性 |
| P2-5 | `test_util.hpp` 包含 `link.hpp`，relay 测试目标因此间接依赖 link 头（仅头文件，不链接） | `tests/bento/test_util.hpp:13` | 记录；`bento_test_relay` 仍只链接 `go2cpp_bento_transport` |
| P2-6 | 原 `TestTcpThroughput` 只断言 >1 MiB/s，是冒烟而非性能门禁 | `test_relay.cpp` 原 727 行 | 保留为冒烟；真实吞吐由独立压测程序给出并在本文记录 |

## 3. 模块解耦

- **头文件自包含**：只 `#include <chrono>/<cstddef>/<cstdint>` 与
  `transport.hpp`；单独 `#include` 后 `-fsyntax-only -Werror` 通过。
- **源码独立编译**：`relay.cpp` 单独 `-c` 通过；include 仅 relay.hpp + 标准库
  （`algorithm/chrono/cstring/thread/vector`）。
- **符号级**：`relay.o` 未定义符号只有 libc/libstdc++（`memmove`、`nanosleep`、
  `operator new/delete`、`steady_clock::now` 等），没有 `Link`/`Frame`/
  `Channel`/`TcpTransport`/`MemoryTransport`。
- **反向依赖**：`transport.hpp` 不引用 relay；CMake 中 relay 源只进
  `go2cpp_bento_transport`，测试目标只链接该库。
- 唯一耦合在测试层：`test_util.hpp` → `link.hpp`（P2-5）。

## 4. 测试复查（原 13 用例）

| 用例 | 结论 |
|---|---|
| tcp 100KB both directions | 断言有效；100 KiB 不构成压力，已由独立压测补 128 MiB |
| tcp request/response half close | 有效，验证 FIN 后响应完整、`half_closes=2` |
| slow consumer write stall | 有效；补充统计一致性断言（`stats.read == src.read_bytes`） |
| bounded backpressure | 有效，peak ≤64 KiB；修复后 peak 53–61 KiB |
| half close propagation | 有效 |
| empty streams | 有效 |
| idle timeout | 有效，下界 95 ms、上界 2 s |
| connect timeout / failure | 有效 |
| random chunk integrity | 有效（固定种子，可复现） |
| write error | 有效（EPIPE/kClosed 路径）；新增 kError 路径 |
| tiny buffer | 有效（64 B 缓冲、7 B 写） |
| tcp 4MB throughput | 断言 >1 MiB/s 偏弱，属冒烟；真实数据见 §1.1 |
| 3 个 TCP 用例的 SKIP | **逃逸，已修**：只有 loopback 不可用才 SKIP |

新增 10 用例：慢而能动不误杀、FIN 在 pending 排空后、读错误、FIN 错误、
写 kError、跨运行统计/峰值 max、配置归一化、idle 计时复位、
MemoryTransport 双向、50 次 TCP 循环 fd 不泄漏。

## 5. 修复与验证

改动文件（未 commit）：

| 文件 | 改动 |
|---|---|
| `src/bento/net/relay.cpp` | Defragment 半容量阈值；`SendPendingFin` 返回 bool 并处理 kError；注释 |
| `include/go2cpp/bento/net/relay.hpp` | `kWriteError`/`half_closes` 语义、缓冲与空闲 CPU 说明、异常路径 note |
| `tests/bento/test_relay.cpp` | SKIP 逃逸修复；10 个新用例；ScriptedTransport 增加 delay/error/FIN 观测 |
| `tests/bento/audit_relay_stress.cpp` | 新增独立压测程序（100 MiB、1000 并发、随机、场景、长稳） |

验证命令与结果：

```bash
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl
ninja bento_test_relay          # 0 警告（-Werror）
ctest -R bento --output-on-failure   # 48/48 通过（bento_netem 环境 Skip）
./bento_test_relay              # 337 checks，约 1.05 s
```

- 30 次连续 `bento_test_relay`：0 失败。
- ASan/UBSan、Valgrind、TSan：见 §1.6。
- 复现：`/tmp/audit_relay.md` §6（压测程序用法与原始日志路径）。

## 6. 未覆盖与限制

- 未在 relay 数据流中注入真实 RST/丢包（由 transport 模块审查覆盖）；
  慢客户端用「内核发送缓冲写满 + 对端停读」复现。
- 空闲 CPU 数字与本机 12 vCPU/WSL2 强相关，结论用于量级参考。
- TSan 仅覆盖 `test_relay`（337 checks），未覆盖 1000 线程并发压测。
- `/proc` 读取与 `setarch -R` 均为 Linux 专用；测试本身只在 Linux 运行。
- `bento_netem` 仍因环境跳过，与本次改动无关。
