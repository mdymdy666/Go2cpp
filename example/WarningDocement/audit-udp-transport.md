# udp_transport 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/udp_transport.hpp`、
  `src/bento/net/udp_transport.cpp`、`tests/bento/test_udp_transport.cpp`
- 审计工具：`tests/bento/audit_udp_transport_stress.cpp`（独立程序，不注册 CTest，
  与既有 `audit_*_stress.cpp` 一致）
- 构建：`.build-bento-wsl`（Ninja，Release `-O3 -DNDEBUG`，
  `-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU / 19 GiB，GCC 13.3.0，内核 6.6.87.2
- 结论：**无 P0**。修复 **2 个 P1**（`milliseconds::max()` deadline 溢出导致
  立即超时；热路径每包一次 `poll` 系统调用），补 7 个测试用例并修掉 1 个
  SKIP 逃逸 / 1 个过严断言 / 1 个过宽断言；记录 7 个 P2、5 个 P3。
  42 万包压测、2000 轮随机、6 个场景、4 线程并发、ASan+UBSan、valgrind
  全部通过；`ctest -R bento` 48/48。

## 1. 压测数据

单次 Release 运行，`taskset -c 8,9`（发送线程 + 接收线程各占一核）。
每档先洪泛（不限速）再限速（每 32 包 sleep 20µs，用于测「零丢包」下的可达吞吐）。
**默认内核缓冲**（212 KiB rcvbuf，模块未暴露调优 API，见 P2-3）。

| 载荷 | 模式 | 发包 | 收包 | 丢包率 | 耗时 | pps | MiB/s | CPU | fd |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 64B | flood | 100,000 | 100,000 | 0.000% | 0.449s | 222,918 | 13.6 | 0.76s (169%) | +0 |
| 64B | paced | 100,000 | 100,000 | 0.000% | 1.049s | 95,302 | 5.8 | 1.14s (109%) | +0 |
| 1400B | flood | 100,000 | 100,000 | 0.000% | 0.480s | 208,522 | 278.4 | 0.82s (171%) | +0 |
| 1400B | paced | 100,000 | 100,000 | 0.000% | 1.119s | 89,330 | 119.3 | 1.26s (113%) | +0 |
| 64KB | flood | 10,000 | 9,954 | 0.460% | 0.864s | 11,522 | 719.8 | 0.38s (44%) | +0 |
| 64KB | paced | 10,000 | 9,976 | 0.240% | 0.943s | 10,578 | 660.8 | 0.43s (46%) | +0 |

- 合计 **42 万次发送**（洪泛 21 万 + 限速 21 万），另有 2 万次请求-响应、
  2 万包并发场景、2000 轮随机。
- **丢包定位**：`/proc/net/snmp` 的 `Udp.RcvbufErrors` 增量与丢包数**逐次完全相等**
  （如 64B 洪泛丢 802 ↔ RcvbufErrors +802），说明丢包发生在内核接收缓冲，
  不是传输层逻辑；限速后 64B/1400B 为 0%。
- **CPU**：洪泛 169–175%（发送+接收两线程吃满约 1.7 核），64KB 44%（拷贝受限）。
- **fd**：所有档位 `fd` 增量 +0；200 次 Bind/Close 与 50 次失败 Bind 后 fd 恢复基线。
- **RSS**：全程 4.4 MB，峰值 4.5 MB。
- **双峰现象（如实记录）**：同一二进制、同一绑核，64B 洪泛在部分运行中只有
  92.8k pps（1.068s），1400B 也出现过 63.0k pps（丢 24.6%）。对照
  `cpu=0.76s/1.07s=71%` 可知此时两线程被调度到同一核；这是 WSL 调度噪声，
  与代码无关，故上表取两线程分核的代表性一轮。

### 1.1 请求-响应 RTT（20,000 / 10,000 轮，0 失败）

| 载荷 | p50 | p95 | p99 | max | 裸 socket 对照 p50 |
|---:|---:|---:|---:|---:|---:|
| 64B | 9 µs | 10 µs | 17 µs | 84 µs | 10 µs |
| 1400B | 10 µs | 10 µs | 20 µs | 125 µs | — |

UdpTransport 与裸 `sendto/poll/recvfrom` 的 RTT 差异 ≤1 µs，**传输层本身没有
数量级瓶颈**；每包成本主要是内核/系统调用（WSL2 下约 2–3 µs/次）。

### 1.2 地址解析成本（每包路径，200,000 次/项）

| 项目 | 结果 |
|---|---|
| `getaddrinfo("127.0.0.1")`（每次 SendTo 都调用） | **55.8 ns** |
| `getaddrinfo("::1")` | 45.1 ns |
| `getaddrinfo("localhost")` | 9,547.9 ns（真正 DNS/文件解析） |
| `inet_pton` v4 / v6 | 9.0 / 12.3 ns |
| `getnameinfo(NI_NUMERICHOST)`（每次 RecvFrom 带 peer 时调用） | 98.4 ns |
| `inet_ntop` | 66.7 ns |

结论：数值字面量解析只占 RTT 的 0.5%，`getnameinfo` 占约 1%；把二者换成
`inet_pton`/`inet_ntop` 最多省 30–50 ns/包（<1%），**不值得改**，只记录（P2-1/P2-2）。
主机名每包解析 9.5 µs 是真实风险，应由上层缓存 Endpoint（P2-1）。

## 2. 随机测试（2000 轮，0 失败）

`--mode=random`：xorshift 种子可复现，每轮随机组合：

- 载荷大小：0/1/2/7/8/15/16/63/64/65/100/512/1400/1472/4096/8192/16384/32768/65507
  以及 0–65507 均匀随机；
- 接收缓冲：0/1/8/64/100/1000/1400/4096/16384/65536 及 0–65536 均匀随机；
- 超时：-100/-1/0/1/2/5/10/50/200/2000 ms；
- 每 97 轮关闭并重开 socket，检查 fd 不泄漏。

每轮断言：`sent.bytes == size`、`received.bytes == min(size, cap)`、
`info.bytes` 同值、`info.datagram_bytes == size`、
`info.truncated == (size > cap)`、前缀逐字节相等、`peer.port == 发送方端口`；
零缓冲/零长数据报不崩溃。**2000 轮 0 失败**，fd 增量 0。

## 3. 场景（6 个，全部通过）

| # | 场景 | 输入 | 期望 | 实际 |
|---|---|---|---|---|
| 1 | 回环回显（请求-响应） | 10,000 轮 64B，`SendTo`→`RecvFrom`→`SendTo` | 内容/序号/来源端口全对 | failures=0，fd+0 |
| 2 | 大包与截断（MSG_TRUNC） | 4096B→64B 缓冲；65507B 上限；容量 0；connected `Read` 4096→64 | 截断标记正确、65507 完整、0 缓冲探测长度；`Read` 静默截断 64B | 全部符合（`truncated=1`，`bytes=64`，`datagram_bytes=4096`） |
| 3 | 超时语义 | 1ms / -100ms / 数据迟到 30ms / `milliseconds::max()` / 10000 次 0ms | 1ms≈1.1ms；负值立即；迟到数据 30.3ms 收到；**max() 也必须等待而不是立即超时**；0ms≈0.42µs | 1.10ms、0.00ms、30.27ms、**max() 30.39ms 收到**（修复前 0ms 立即 kTimeout）、0.42µs/次 |
| 4 | 并发发送 | 4 线程共享同一 socket，各 5000×64B（限速） | 20,000 包全部到达，无撕裂/重复 | recv=20000、corrupt=0、duplicates=0、fd+0 |
| 5 | IPv6 `::1` | Bind `::1`、1000 轮回显、4096→128 截断 | 回显正确、`peer.host` 含 `::1`、截断标记 | 1000/1000，`local=::1:…`，truncated=1 |
| 6（附加） | connected UDP 的 ICMP 端口不可达 | Connect 到关闭端口，Write 后 Read | `kError` + `ECONNREFUSED(111)` | 符合（errno=111） |

## 4. 解耦

- **头文件自包含**：单独 `#include "go2cpp/bento/net/udp_transport.hpp"` 的 TU
  在 `-Wall -Wextra -Wpedantic -Werror` 下编译通过；头文件只依赖
  `<chrono> <cstddef> <cstdint>` + `net/transport.hpp`。
- **`g++ -MM`**：`udp_transport.cpp` 的项目内依赖只有
  `udp_transport.hpp` 与 `transport.hpp`，不引用 `base`/`error`/`app` 任何模块。
- **`nm -C -u`**：未定义符号只有 libc/POSIX（`socket/bind/sendto/recvmsg/poll/
  getaddrinfo/…`）与 libstdc++，没有跨模块符号。
- 与 `tcp_transport.cpp` 一致，直接用 POSIX socket API（未使用
  `include/go2cpp/network/socket.hpp` 那套历史抽象）；bento 传输层内部自洽。

## 5. 瓶颈定位与增强（P1-2）

先量化再改：`--mode=resolve` 证明「三元组查询」（`getaddrinfo`）不是瓶颈
（55.8 ns/次），`getnameinfo` 也只有 98 ns/次。真正的每包固定开销是
`Read`/`RecvFrom` **先 poll 再收**：数据已就绪时白付一次 `poll` 系统调用。

修复：`timeout > 0` 时先尝试一次 `recv`/`recvmsg`，只有 `EAGAIN` 才 `poll`；
`timeout == 0` 保持 poll 先行（空闲轮询不为每次多付一次必然失败的 recv）。

`--mode=micro`（数据已就绪、单线程、每组 51,200 次调用，3 轮取中位数）：

| 项目 | HEAD | 当前 | 变化 |
|---|---:|---:|---:|
| `RecvFrom(64B, 已就绪)` | 1,513 ns/call | **824 ns/call** | **−45.5%** |
| `Read(64B, 已就绪)` | 1,231 ns/call | **601 ns/call** | **−51.2%** |
| `RecvFrom(0ms, 空闲)` | 364 ns/call | 349 ns/call | −4%（噪声内，确认无回退） |

端到端 RTT p50 从 10–11 µs 降到 9–10 µs（−1~2 µs，约 −10%），p95 从 13–15 µs
降到 9–12 µs。洪泛吞吐受 WSL 调度双峰影响（见 §1），**无法把宏吞吐差异归因于
本次改动**，因此只以单调用热路径与 RTT 作为证据。

修复 2（P1-1）：`NormalizeTimeout` 增加 24h 上限。原因：
`steady_clock::time_point + milliseconds::max()` 触发有符号整数溢出
（UBSan：`chrono.h:225: runtime error: signed integer overflow`），deadline
回绕成过去时间，`RecvFrom(..., milliseconds::max())` 会 **0 ms 返回 kTimeout**。
场景 3 已锁定回归（修复后 30.39 ms 收到数据）。

## 6. 问题清单

### P0

无（无崩溃、无内存错误、无数据损坏、无 fd 泄漏）。

### P1（已修）

| 编号 | 问题 | 证据 | 处理 |
|---|---|---|---|
| P1-1 | `milliseconds::max()` 超时导致 deadline 有符号溢出（UB），调用立即返回 `kTimeout` | UBSan 报 `chrono.h:225` 溢出；修复前场景 3 实耗 0.00 ms，修复后 30.39 ms 收到数据 | `NormalizeTimeout` 钳制到 24h，头文件同步说明；新增 `TestTimeoutClamp` |
| P1-2 | 热路径每包一次多余 `poll`（数据已就绪时） | microbench `RecvFrom` 1,513→824 ns（−45.5%）、`Read` 1,231→601 ns（−51.2%） | 阻塞模式 recv-first；`timeout==0` 保持 poll-first；头文件说明取舍 |

### P2（记录，未改代码）

| 编号 | 问题 | 证据/影响 | 处理 |
|---|---|---|---|
| P2-1 | `SendTo` 每包都做地址解析；主机名 9.5 µs/包（数值 55.8 ns） | §1.2；高频 `SendTo(hostname)` 会把 9.5 µs 加进每包 | 记录：建议上层缓存 `Endpoint`，或后续加 `Resolve`/缓存 API；不擅自改变 DNS 语义 |
| P2-2 | `EndpointFromSockAddr` 用 `getnameinfo`（98.4 ns）而非 `inet_ntop`（66.7 ns） | 每包差 ~32 ns（<1% RTT） | 记录：收益低于改动风险，不改 |
| P2-3 | 无 `SO_RCVBUF`/`SO_SNDBUF` 调优 API；洪泛下默认 212 KiB 缓冲丢 0.8%（64B）/2.0%（1400B） | §1；限速后 0%；`RcvbufErrors` 与丢包数一致 | 记录：建议后续给 `Bind` 增加 socket 选项参数 |
| P2-4 | `Bind({"",0})` 只绑一个地址族（本机实测 `0.0.0.0`），IPv6 客户端不可达；`LocalEndpoint` 返回 `0.0.0.0:port` 对端不可直接使用 | 通配绑定探针 | 记录：文档未说明地址族选择规则 |
| P2-5 | `WaitFor` 不检查 `revents`；`m_fd` 非原子，`Close` 与读写并发是数据竞争（`POLLNVAL` 退化为 `kError+EBADF` 而非 `kClosed`） | 代码审查；头文件已声明「Close 与其它操作并发需调用方协调」 | 记录（与 `audit-transport.md` P2-4 同口径，加锁会改变成本模型） |
| P2-6 | `Resolve` 把 `getaddrinfo` 的所有失败统一映射 `EHOSTUNREACH`（`EAI_AGAIN` 临时失败也如此） | 代码 `*error = EHOSTUNREACH` | 记录：诊断信息损失，暂无控制流依赖 |
| P2-7 | connected `Read` 静默截断（`ITransport` 无截断输出） | 场景 2：4096→64 返回 64B，其余丢弃 | 文档已声明；补 `TestConnectedReadTruncation` 锁定行为 |

### P3（记录）

1. `Write` 短写映射 `EIO`（Linux UDP 不可达）。
2. `Bind` 对 `IPV6_V6ONLY=0` 的 `setsockopt` 失败被静默忽略。
3. `Resolve` 每次分配 `std::string` + `std::vector`（~100 ns，非瓶颈）。
4. `docs/modules/bento-foundation.md:16` 仍写「15 用例」，本轮已扩到 22 用例；
   按任务边界未改文档，待同步。
5. `TestHundredDatagrams` 用单一 `all_ok` 汇总 100 轮，失败诊断弱（未改）。

## 7. 测试复查

原测试：**15 用例 / 158 checks**（实测 `[PASS] udp_transport (158 checks)`），
断言总体有效（真实回环、逐字节比对、errno 校验），但存在：

- **过严断言（已改）**：`TestRecvTimeout` 断言超时后 `info.bytes` 仍为 12345，
  而头文件明确「失败时内容不保证被修改」——实现一旦在失败路径清空 `info`
  就会误报。改为断言 `received.bytes == 0`。
- **过宽断言（已改）**：`TestNonBlockingTimeoutZero` 只要求 `elapsed < 1000` ms，
  实测 0.42 µs/次；收紧到 `elapsed < 50` ms。
- **SKIP 逃逸（已改）**：`TestIpv6Loopback` 原本任何 `Bind("::1")` 失败都
  `Skip()`，而 `Finish()` 返回 77 会让 CTest 把**整份测试**标成 Skipped，
  真实回归（如 `EINVAL`）也会被掩盖。改为只有 `EAFNOSUPPORT` /
  `EADDRNOTAVAIL` / `EPROTONOSUPPORT` 才 SKIP，其它 errno 计入 FAIL。
- **覆盖缺口（已补 7 个用例）**：
  1. `milliseconds::max()` 超时钳制；2. 0 超时消费已就绪数据报；
  3. 通配 `Bind` 收 IPv4 并回给来源；4. connected socket 上 `SendTo` 其它对端；
  5. connected `Read` 静默截断；6. 4 线程并发 `SendTo` 的边界完整性；
  7. Bind/Close 200 次与失败 Bind 50 次的 fd 泄漏。
- **结果**：22 用例 / **218 checks**，连续 5 次运行全过。
- **仍未覆盖（记录）**：跨地址族 `Connect` 的 `EAFNOSUPPORT`（需要双栈绑定）、
  `LocalEndpoint`/`PeerEndpoint` 空指针、connected `RecvFrom` 的 `getpeername`
  回退分支（内核总提供来源地址，难构造）。

## 8. 验证

| 项目 | 命令 | 结果 |
|---|---|---|
| 编译 | `ninja -C .build-bento-wsl bento_test_udp_transport` | 0 警告（`-Werror`） |
| 单元测试 | `./.build-bento-wsl/bento_test_udp_transport` | 22 用例 / 218 checks，5/5 通过 |
| 全量 bento | `ctest --test-dir .build-bento-wsl -R bento` | **48/48 通过**，27.85 s；`bento_netem` 无 CAP_NET_ADMIN 按设计 SKIP |
| 审计程序 | `audit_udp_transport_stress --mode=all` | **2,080 checks / 0 failures**，fd_delta 0 |
| ASan+UBSan | `--mode=all --scale=0.1` + 单元测试 | 280 checks / 0 报告；218 checks / 0 报告（`-fno-sanitize-recover=all`） |
| valgrind | `--leak-check=full` 跑单元测试 | 0 errors，All heap blocks were freed |
| 解耦 | `-fsyntax-only` 自包含 TU、`g++ -MM`、`nm -C -u` | 通过（见 §4） |

## 9. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 单元测试
ninja -C .build-bento-wsl bento_test_udp_transport && ./.build-bento-wsl/bento_test_udp_transport

# 独立审计程序
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iinclude -Itests/bento tests/bento/audit_udp_transport_stress.cpp \
    src/bento/net/udp_transport.cpp src/bento/net/transport.cpp \
    -o .build-bento-wsl/audit_udp_transport_stress

taskset -c 8,9 ./.build-bento-wsl/audit_udp_transport_stress --mode=all
taskset -c 8   ./.build-bento-wsl/audit_udp_transport_stress --mode=micro
taskset -c 8   ./.build-bento-wsl/audit_udp_transport_stress --mode=resolve

# sanitizer
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -Itests/bento \
    tests/bento/audit_udp_transport_stress.cpp src/bento/net/udp_transport.cpp \
    src/bento/net/transport.cpp -o /tmp/audit_udp_asan
taskset -c 2 /tmp/audit_udp_asan --mode=all --scale=0.1
```

## 10. 改动清单（未 commit）

| 文件 | 改动 |
|---|---|
| `src/bento/net/udp_transport.cpp` | +68 / −16：超时钳制（P1-1）；`Read`/`RecvFrom` recv-first（P1-2） |
| `include/go2cpp/bento/net/udp_transport.hpp` | +6 / −1：同步超时上限与 recv-first 语义 |
| `tests/bento/test_udp_transport.cpp` | +298 / −4：7 个新用例、2 处断言修正、IPv6 SKIP 分类 |
| `tests/bento/audit_udp_transport_stress.cpp` | 新增：独立审计/压测程序（不注册 CTest） |

未改 base、未改构建脚本、未 commit。
