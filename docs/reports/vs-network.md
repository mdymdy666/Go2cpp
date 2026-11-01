# 网络 vs 工业库对比矩阵（Go / muduo / libevent / asio / Seastar）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 ext4）
- 对标规范：`docs/engineering-standards.md` §一「实现后必须对比」、§四「网络（TCP/UDP）」
- 日期：2026-11-01
- 数据来源：仓内实测报告 + 工业库官方仓库公开资料（标注 URL）
- 状态：**只写本文档，不改代码、不 commit**

---

## 0. 结论速览

| 维度 | 结果 | 判定 |
|---|---|---|
| 6 协议（TCP/UDP/HTTP/WS/MQTT/CoAP）vs Go | **14/14 场景达标**（要求「耗时 ≤1.1× Go」） | **全部领先**，倍数 1.07–2.90× |
| 最差项 | WebSocket 16 连接 1.07×（仍快于 Go） | 达标侧，共享机器 ±5% 波动内 |
| 线程效率 | 服务端只用 **2 个 OS 线程**打赢 Go 的 2×GOMAXPROCS | 同资源口径 |
| 协议栈内部吞吐 | HTTP 单连接 7,695 req/s / CoAP 9,449 req/s / SOCKS5 中继 911 MB/s / MQTT 内存路由 4.88 M ops/s | 见 §4 |
| vs muduo | 队列语义同机实测 32.40 ns（我们 10.73，快 3.02×）；**网络吞吐未测** | 部分对比 |
| vs libevent / asio / Seastar | **无同机实测**，官方仓库无吞吐公开数字 | **未测/待补** |

**一句话**：与 Go 的同资源对比已经**全项达标且全部更快**（14/14，1.07–2.90×）；
与 C++ 生态的网络库（muduo/libevent/asio/Seastar）只有队列层做了同机实测，
网络吞吐这一层**还没有同口径数据**，规范要求「不给对照数字就不算对比」——因此本文明示缺口，不拿二手数字填空。

---

## 1. 口径与环境

| 项 | 值 |
|---|---|
| 机器 | WSL2 on Windows，AMD Ryzen 9 8945HX，12 硬件线程 |
| 绑核 | 服务端 `taskset -c 2,3`；压测客户端 `taskset -c 4,5,6,7` |
| Go | go1.27.1 linux/amd64，服务端 `GOMAXPROCS=2` |
| C++ | GCC 13，Release `-O3 -DNDEBUG`，`.build-bento-wsl` |
| 负载 | 回环 `127.0.0.1`，64B 消息，客户端串行 ping-pong（不流水线） |
| 客户端 | 同一份手写 Go 压测器测两边（每协议一个最小客户端） |
| 统计 | 每连接独立 goroutine，建连 + 预热后同时起跑；`ops / 实际耗时` |
| 负载水平 | 各轮 `loadavg` 0.5–1.5（共享机器） |

**为什么必须本机测 Go**：任务书参考的「Go 典型 50k–100k HTTP / ~1M TCP」是原生 Linux 数字。
本机 WSL2 回环 RTT 约 110 µs（单连接串行 ping-pong 的物理上限约 9k msg/s），
Go 在本机单连接也只有 5–7k。**所以本文只与「本机实测 Go」比较**，不与文献值比较。

---

## 2. 我们 vs Go：6 协议 / 14 个场景（同机、同负载、同客户端）

Go 基线（本机实测，来源 `docs/reports/2026-11-01-net-vs-go.md` §2）：

| 场景 | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo 64B | 6,663 | 21,422 | 90,638 |
| UDP echo 64B | 6,196 | 23,677 | 60,748 |
| HTTP GET /hello | 5,227 | 21,053 | 68,943 |
| WebSocket echo 64B | 6,585 | 20,402 | 90,210 |
| MQTT 3.1.1 QoS0 自订阅 | 6,581 | —（单连接口径） | — |
| CoAP CON GET（UDP） | 7,315 | —（单连接口径） | — |

我们（每连接一 Fiber，服务端只用 2 个 OS 线程）：

| 场景（2 线程公平口径） | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo 64B | 8,220 | 25,296 | 110,501 |
| UDP echo 64B（recvmmsg/sendmmsg 批量） | 8,614 | 28,781 | 176,202 |
| HTTP GET /hello | 7,124 | 23,074 | 92,875 |
| WebSocket echo 64B | 8,110 | 25,432 | 96,835 |
| MQTT QoS0 回环 | 7,903 | — | — |
| CoAP CON GET | 8,325 | — | — |

**倍数表（我们 / Go，>1 表示我们更快）**：

| 场景 | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo | **1.23×** | **1.18×** | **1.22×** |
| UDP echo | **1.39×** | **1.22×** | **2.90×** |
| HTTP | **1.36×** | **1.10×** | **1.35×** |
| WebSocket | **1.23×** | **1.25×** | **1.07×** |
| MQTT | **1.20×** | — | — |
| CoAP | **1.14×** | — | — |

**达标判定**：要求「我们耗时 ≤1.1× Go 耗时」⇔ 吞吐 **≥0.909× Go**。
**14/14 个场景全部达标，且全部快于 Go**；最差一项 WebSocket 16 连接 1.07×，
其余 1.10–2.90×；共享机器典型波动 ±5%，最差两项仍在达标侧。

---

## 3. 优化前后（说明「为什么现在能赢」）

| 场景 | 优化前（bento 线程模型） | 优化后（每连接一 Fiber） | 变化 |
|---|---:|---:|---:|
| HTTP 2 线程 × 4 连接 | **>120 s 跑不完**（worker 被 keep-alive 连接独占，后两条饿死） | 23,074 req/s | 饥饿问题消失 |
| HTTP 16 连接 | 95,306（16 worker） | 92,875（**2** worker） | 线程数降为 1/8，吞吐持平 |
| WS 16 连接 | 79,367（16 worker） | 96,835（2 worker） | **+22%** |
| WS 单连接 | 6,869 | 8,110 | **+18%** |
| UDP 16 连接 | 120,526（单线程 poll） | 176,202（recvmmsg/sendmmsg） | **+46%** |
| TCP 16 连接 | 116,595（16 线程） | 110,501（**2** 线程） | −5%（线程 1/8） |

关键改动：accept 循环本身是 Fiber，每连接再起一个 Fiber（与 Go `goroutine-per-connection` 同构）；
HTTP 序列化去 `ostringstream`、header 用 `string_view`；WebSocket 一帧一次 `recv`、小帧一次 `write`；
UDP 批量 `recvmmsg`/`sendmmsg`（每批最多 64 条）。

**根因判断**：优化前的瓶颈不是「协议实现慢」，而是**线程模型**——固定 worker 池在
「连接数 > worker 数」时必然饥饿（空闲 keep-alive 连接占着 worker）。这一条是纯架构问题，
换 Fiber 后同一份协议代码吞吐提升到 Go 之上。

---

## 4. 协议栈内部吞吐（非 socket 回环，纯编解码/路由路径）

来源 `docs/reports/2026-10-31-http-perf.md`（单线程、Release）：

| 指标 | 数值 | 备注 |
|---|---:|---|
| HTTP GET /hello 单连接 keep-alive | **7,695 req/s** | p50 120 µs / p99 263 µs |
| HTTP GET /hello 4 并发 | **67,350 req/s** | 2 核绑定时 4 worker 聚合 |
| HTTP GET /big 16KB 单连接 | 6,809 req/s（≈106 MB/s） | p99 286 µs |
| WS 二进制 1KB echo 单连接 | 5,295 msg/s（≈5.2 MB/s） | p99 354 µs |
| CoAP CON GET 串行（真实 UDP） | **9,449 req/s** | p99 208 µs |
| CoAP NON GET 串行 | 10,374 req/s | p99 198 µs |
| MQTT broker 内存路由 64B（无 socket） | **4,877,505 ops/s** | 纯路由路径 |
| SOCKS5 中继双向 4MB（64KB 块） | **911 MB/s** | 唯一被非协议因素主导的指标，见 §8 |
| OmniNet AsHttpServer GET 单连接 | 7,926 req/s | p99 285 µs |

**写路径三档对比（系统调用摊销）**：

| 消息 | 逐条 write | bufio 批量 | 提升 | syscalls/msg（逐条 → 批量） |
|---|---:|---:|---:|---:|
| HTTP 响应 120B | 144,433 ops/s | **3,925,869** | **27.2×** | 1.000 → 0.031 |
| WS 帧 64B | 186,480 ops/s | **6,181,102** | **33.1×** | 1.000 → 0.016 |
| MQTT PUBLISH 96B | 181,607 ops/s | **3,886,612** | **21.4×** | 1.000 → 0.031 |
| CoAP 响应 64B | 183,892 ops/s | **4,988,958** | **27.1×** | 1.000 → 0.016 |

> 边界：bufio 只对小包有效。`docs/reports/2026-10-31-http-perf.md` 明确记录
> **≥4KB 的大包走 bufio 反而慢 19%**，应直写旁路（代码里已按容量分流）。

---

## 5. 正确性门禁（先证明对，再谈快）

| 项 | 结果 | 出处 |
|---|---|---|
| `bento_bench_protocols --correctness` | **37 个真实场景 / 237 条断言 / 0 失败** | http-perf §0 |
| 覆盖协议 | HTTP/SSE、WebSocket、MQTT 3.1.1、CoAP、SOCKS5、LinkServer、OmniNetServer | 同上 |
| 单元测试（`GO2CPP_TEST_FILTER`） | `network`（address/bytearray/socket/http）、`sse`、`io` 全部 `all checks passed` | net-vs-go §6 |
| 已知无关失败 | 全量 `go2cpp_tests` 的 `spin mutex` 用例 5 条断言失败（并发模块既有问题，与网络改动无关） | 同上 |

---

## 6. 与 C++ 网络库的对比（muduo / libevent / asio / Seastar）

### 6.1 已有同机实测（仅队列层）

来源 `docs/reports/2026-11-01-concurrency-survey.md` §3（本机编译 muduo `chenshuo/muduo` HEAD）：

| 实现 | 语义 | ns/条 | 我们 vs 它 |
|---|---|---:|---:|
| **Go2Cpp `FastChannel<1024>`** | MPMC 无锁 ring | **10.73** | — |
| muduo `BlockingQueue`（无界） | mutex + deque + CV | 32.40 | 我们快 **3.02×** |
| muduo `BoundedBlockingQueue(1024)` | mutex + 环形 + CV | 71.16 | 我们快 **6.63×** |

这是「muduo 的跨线程消息传递」这一层的同机数据，**不是网络吞吐**。muduo 的 TCP 吞吐本轮未测。

### 6.2 架构/能力对比（定性，非性能）

| 维度 | Go2Cpp | muduo | libevent | asio | Seastar |
|---|---|---|---|---|---|
| 并发模型 | Fiber + GMP（M:N，可跨线程） | one loop per thread（N:1，无跨线程协程） | 事件循环（回调） | 回调/协程（可配） | share-nothing，每核一线程（N:1 per shard） |
| 协议内置 | **6 协议**（TCP/UDP/HTTP/WS/MQTT/CoAP）+ SOCKS5/Link/OmniNet | HTTP（简易）、protobuf 示例；无 MQTT/CoAP | **无协议**（纯事件库） | **无协议**（纯 IO 库） | HTTP（server 侧） |
| 背压/有界队列 | 有（`Chan` + `TrySend`） | 有（`BoundedBlockingQueue`） | 无（应用自己做） | 无 | 有（future/continuation 链） |
| 取消语义 | 有（`Context`） | 无 | 无 | 有（`cancellation_slot`，C++20） | 有（`cancelled_exception`） |
| 跨线程调度 | 有（work stealing） | 无（线程间靠队列） | 无 | 无（`io_context` 需手动 post） | 无（跨 shard 靠显式消息） |
| 官方基准数字 | 本文 §2–§4（自测） | 官方仓库无吞吐公开数字 | 官方仓库无吞吐公开数字 | 官方仓库无吞吐公开数字 | 官方仓库无吞吐公开数字 |

### 6.3 公开数据取用记录（诚实标注）

| 库 | 抓取结果 | 结论 |
|---|---|---|
| muduo | `raw.githubusercontent.com/chenshuo/muduo/master/README` 取到（972 B，仅安装/教程说明） | **无官方吞吐数字**；吞吐层**未测/待补** |
| libevent | `raw.githubusercontent.com/libevent/libevent/master/README.md` 取到（3.4 KB，介绍/构建） | **无官方吞吐数字**；**未测/待补** |
| asio | `raw.githubusercontent.com/chriskohlhoff/asio/master/README` 取到（152 B） | **无官方吞吐数字**；**未测/待补** |
| Seastar | `raw.githubusercontent.com/scylladb/seastar/master/README.md` 取到（12.3 KB，设计描述） | **无官方吞吐数字**；**未测/待补** |

> 规范 §一明确「禁止只写我们很快而不给工业库对照数字」。这里的选择是：**如实标缺口**，
> 而不是把二手博客数字当官方数据填表。补齐方式见 §10 P0。

---

## 7. 差距分析

| 项 | 结论 | 依据 |
|---|---|---|
| 单连接吞吐受回环 RTT 限制 | 110 µs RTT ⇒ 物理上限约 9k msg/s；我们 8.2–8.6k 已接近上限 | net-vs-go §1 |
| 多连接才反映服务端效率 | 16 连接：TCP 1.22×、UDP 2.90×、HTTP 1.35×、WS 1.07× | §2 |
| WS 16 连接最接近 Go（1.07×） | WS 每帧有掩码/帧头处理，CPU 占比高于 TCP echo；Go 侧是标准库高度优化路径 | http-perf §4 |
| UDP 16 连接 2.90× 的来源 | `recvmmsg`/`sendmmsg` 批量（每批 64 条），把 syscall 从 1/包降到 1/64 | net-vs-go §4.5 |
| SOCKS5 数值被非协议因素主导 | 建连 45 conn/s 但 p99 51.7 ms（约 2% 慢连接），中继 911 MB/s 不受影响 | http-perf §4 |
| MQTT 内存路由 4.88M vs 端到端 8.8k（550×） | 说明端到端瓶颈在 socket 与调度，不在协议解析 | http-perf §4 |
| 与 C++ 库的差距 | **未知**（无同口径网络基准） | §6.3 |

---

## 8. 改进方向

| 优先级 | 动作 | 依据/预期 |
|---|---|---|
| **P0** | 补 muduo / libevent / asio 同机网络基准（同客户端、同绑核、同 64B echo），把 §6 的空缺填上 | 规范 §一硬要求；当前是唯一「有实现无对照」的模块 |
| **P1** | 网络库纳入 `-fno-plt` / `-fno-semantic-interposition` / `-fvisibility=hidden`（现在只在 6 个并发库生效） | asm-network §1.3：`Parser::feed_impl` 119 个 call 中 66 个走 `@plt` |
| **P1** | HTTP 解析每请求 14 次堆分配的消除（连接级 arena / 复用 header 缓冲） | asm-network §0 第 3 条（300k 次采样实测） |
| **P1** | `ResponseWriter::write` 去 `ostringstream` + 合并 send（每 chunk 1.4–2 µs 固定开销） | asm-network §0 第 2 条 |
| **P2** | WebSocket 客户端掩码键改每连接 PRNG（现每帧构造 `std::random_device`，36 µs/帧） | asm-network §0 第 1 条（约 3 个数量级） |
| **P2** | 大包（≥4KB）直写旁路的默认策略固化到 API（现靠调用方选 `use_bufio`） | http-perf §0（大包走 bufio 慢 19%） |

---

## 9. 未测 / 待补（诚实清单）

| 项 | 状态 | 原因 |
|---|---|---|
| muduo TCP/HTTP 吞吐（req/s） | **未测** | 只测了其 `BlockingQueue`；需按 §2 同口径搭基准 |
| libevent / asio / Seastar 吞吐 | **未测** | 官方无公开数字，且未在本机编译基准 |
| p99 延迟 vs Go（多连接） | **未测** | 现有对比只到吞吐，Go 侧客户端未采集延迟分位 |
| 长连接数上限（10k/100k 连接） | **未测** | 本机回环场景只做到 16 连接 |
| TLS/QUIC 吞吐 | **未测** | 不在本轮 6 协议范围（另有 `2026-11-01-tls-inline-survey.md` 覆盖实现层） |
| 弱网（丢包/抖动/重排） | **未测** | 需要 `tc netem`，容器无 `CAP_NET_ADMIN`（`bento_netem` 已 SKIP） |
| 多机（非回环）对比 | **未测** | 单机 WSL2 环境限制 |

---

## 10. 数据来源清单

| # | 来源 | 用途 |
|---|---|---|
| 1 | `docs/reports/2026-11-01-net-vs-go.md` | §2 Go 基线 / 优化前后 / 倍数表 / 14/14 判定 |
| 2 | `docs/reports/2026-10-31-http-perf.md` | §4 协议栈内部吞吐、写路径三档、正确性门禁 |
| 3 | `docs/reports/2026-11-01-asm-network-review.md` | §8 改进项（PLT/分配/WS 掩码/ResponseWriter） |
| 4 | `docs/reports/2026-11-01-concurrency-survey.md` | §6.1 muduo 队列同机实测 |
| 5 | `docs/reports/2026-10-31-stress-matrix.md` | 压测矩阵（协议 × 场景）背景 |
| 6 | `https://raw.githubusercontent.com/chenshuo/muduo/master/README` | muduo 官方资料（无吞吐数字） |
| 7 | `https://raw.githubusercontent.com/libevent/libevent/master/README.md` | libevent 官方资料（无吞吐数字） |
| 8 | `https://raw.githubusercontent.com/chriskohlhoff/asio/master/README` | asio 官方资料（无吞吐数字） |
| 9 | `https://raw.githubusercontent.com/scylladb/seastar/master/README.md` | Seastar 官方资料（无吞吐数字） |
