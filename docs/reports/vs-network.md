# 网络 vs 工业库对比矩阵（Go / muduo / libevent / asio / Seastar）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 ext4）
- 对标规范：`docs/engineering-standards.md` §一「实现后必须对比」、§四「网络（TCP/UDP）：muduo、libevent、asio、Seastar」
- 日期：2026-11-01
- 数据来源：仓内实测报告 + 工业库公开资料（公开数字缺口已逐项标注）
- 状态：**只写本文档，不改代码、不 commit**

---

## 0. 结论速览

| 维度 | 我们 vs Go（同机、同负载、同客户端） | 判定 |
|---|---|---|
| 6 协议 × 14 场景 | **14/14 达标（要求 ≤1.1× Go 耗时），且 14/14 全部快于 Go** | **领先 1.07–2.90×** |
| 服务端线程数 | 2 个 OS 线程（Go 侧 2×GOMAXPROCS） | 同资源口径 |
| 最差项 | WebSocket 16 连接 1.07×（仍在达标侧） | 领先幅度最小 |
| 领先最大项 | UDP 16 连接 2.90×（recvmmsg/sendmmsg 批量） | 批量 syscall |

| 维度 | 结论 |
|---|---|
| vs muduo | **只有队列层同机实测**（muduo `BlockingQueue` 32.40 ns vs 我们 `FastChannel` 10.73 ns，快 3.02×）；网络吞吐**未测** |
| vs libevent / asio / Seastar | **网络吞吐未测**；公开资料无同口径 req/s 数字（已核实官方仓库 README，见 §4.3） |
| 弱网可靠性 | TCP/UDP/Bento 三方 netem 实测已有（§5）；**Bento 在手写可靠层上弱于内核 TCP**（p50 0.02 ms vs 1.00 ms baseline），这是已知差距，不掩饰 |

**一句话**：对 Go 的同资源对比已全部达标且全部更快（6 协议 14 场景）；
对 C++ 生态的 muduo/libevent/asio/Seastar，本轮只能给出**架构级对照 + 队列层实测**，
网络吞吐的对照数据**尚未测量**，按规范标注「未测/待补」，不使用二手数字填充。

---

## 1. 口径与环境

| 项 | 值 |
|---|---|
| 机器 | WSL2 on Windows，AMD Ryzen 9 8945HX，12 硬件线程 |
| 绑核 | 服务端 `taskset -c 2,3`；压测客户端 `taskset -c 4,5,6,7` |
| Go | go1.27.1 linux/amd64，服务端 `GOMAXPROCS=2` |
| C++ | GCC 13.3，Release `-O3 -DNDEBUG`，`.build-bento-wsl` |
| 负载 | 回环 `127.0.0.1`，64B 消息，客户端串行 ping-pong（不流水线） |
| 客户端 | 同一份手写 Go 压测器测两边（每协议一个最小客户端） |
| 统计 | 每连接独立 goroutine，建连 + 预热后同时起跑；`ops / 实际耗时` |
| 负载水平 | 各轮 `loadavg` 0.5–1.5（多 agent 共享机，已记录） |

**为什么必须本机测 Go**：任务参考的「Go 典型 50k–100k HTTP / ~1M TCP」是原生 Linux 数字。
本机 WSL2 回环 RTT ≈110 µs（单连接串行 ping-pong 的物理上限约 9k msg/s），
Go 在本机单连接也只有 5–7k。**因此本报告只与「本机实测 Go」比较**，不与文献典型值比较。

---

## 2. 6 协议 × 14 场景：我们 vs Go

### 2.1 Go 基线（本机实测）

| 场景 | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo 64B | 6,663 | 21,422 | 90,638 |
| UDP echo 64B | 6,196 | 23,677 | 60,748 |
| HTTP GET /hello（keep-alive） | 5,227 | 21,053 | 68,943 |
| WebSocket echo 64B | 6,585 | 20,402 | 90,210 |
| MQTT 3.1.1 QoS0 自订阅回环 | 6,581 | —（单连接口径） | — |
| CoAP CON GET（UDP） | 7,315 | —（单连接口径） | — |

### 2.2 我们（每连接一 Fiber，服务端只用 2 个 OS 线程）

| 场景（2 线程公平口径） | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo 64B | 8,220 | 25,296 | 110,501 |
| UDP echo 64B（批量） | 8,614 | 28,781 | 176,202 |
| HTTP GET /hello | 7,124 | 23,074 | 92,875 |
| WebSocket echo 64B | 8,110 | 25,432 | 96,835 |
| MQTT QoS0 回环 | 7,903 | — | — |
| CoAP CON GET | 8,325 | — | — |

### 2.3 倍数表（我们 / Go，>1 表示我们更快）

| 场景 | 1 连接 | 4 连接 | 16 连接 |
|---|---:|---:|---:|
| TCP echo | **1.23×** | **1.18×** | **1.22×** |
| UDP echo | **1.39×** | **1.22×** | **2.90×** |
| HTTP | **1.36×** | **1.10×** | **1.35×** |
| WebSocket | **1.23×** | **1.25×** | **1.07×** |
| MQTT | **1.20×** | — | — |
| CoAP | **1.14×** | — | — |

### 2.4 达标判定

要求：**我们耗时 ≤ 1.1 × Go 耗时**，即吞吐 **≥ 0.909 × Go**。

- **14/14 场景全部达标**，且全部**快于** Go（无一项落在 0.909–1.0 区间）；
- 最差为 WebSocket 16 连接 **1.07×**（仍快 7%），其余 1.10–2.90×；
- 共享机器典型波动 ±5%，最差两项仍在达标侧；
- 服务端只用 **2 个 OS 线程**达到上述数字；与我们自己优化前的「16 线程 / 16 worker」相比，
  线程数降为 1/8 而吞吐持平或更高。

---

## 3. 优化前后（为什么现在能赢）

| 场景 | 优化前（bento 线程模型） | 优化后（每连接一 Fiber） | 变化 |
|---|---:|---:|---:|
| HTTP 2 线程 × 4 连接 | **>120 s 跑不完 4 万请求**（worker 被前两条 keep-alive 独占，后两条饥饿） | 23,074 req/s | 饥饿消失 |
| HTTP 16 连接 | 95,306（16 worker） | 92,875（**2** worker） | 线程数 1/8，吞吐基本持平 |
| WS 16 连接 | 79,367（16 worker） | 96,835（**2** worker） | **+22%** |
| WS 单连接 | 6,869 | 8,110 | **+18%** |
| UDP 16 连接 | 120,526（单线程 poll） | 176,202（recvmmsg/sendmmsg） | **+46%** |
| TCP 16 连接 | 116,595（16 线程） | 110,501（**2** 线程） | −5%（线程数 1/8） |

优化项（`docs/reports/2026-11-01-net-vs-go.md` §4）：
1. `HttpServer::Start()` / `WsServer::Start()` 改为 accept Fiber + 每连接 Fiber（与 Go `net/http` 的 goroutine-per-connection 同构）；
2. HTTP 响应去 `std::ostringstream`（一次 `reserve` + `append` + `to_chars`），请求起始行手写三段切分、header 用 `string_view`；
3. WebSocket 一帧一次 `recv`（16KB 预读）+ 小帧（≤512B）一次 `write`；
4. UDP `recvmmsg`/`sendmmsg` 批量（每批 ≤64 条），把 syscall 从 1/包降到 1/64。

**根因**：优化前瓶颈不是协议编解码，而是**每连接一线程/固定 worker 池**的模型——
连接数 > worker 数时，空闲 keep-alive 连接会饿死后续连接。换成 Fiber 后，
同一套协议代码的吞吐直接超过 Go。

---

## 4. 与 C++ 工业库的对比（muduo / libevent / asio / Seastar）

### 4.1 已有的同机实测（仅队列层，不是网络吞吐）

来自 `docs/reports/2026-11-01-concurrency-survey.md` §3（本机编译 muduo `chenshuo/muduo` HEAD）：

| 实现 | 语义 | ns/条 | 我们 vs 它 |
|---|---|---:|---:|
| **Go2Cpp `FastChannel<1024>`** | MPMC 无锁 ring | **10.73** | 基准 |
| muduo `BlockingQueue`（无界） | mutex + deque + CV | 32.40 | 我们快 **3.02×** |
| muduo `BoundedBlockingQueue(1024)` | mutex + 环形 + CV | 71.16 | 我们快 **6.63×** |

> 这只能证明「消息传递层我们不输 muduo」，**不能**替代网络吞吐对比。

### 4.2 架构与能力对照（定性，非性能）

| 维度 | Go2Cpp | muduo | libevent | asio | Seastar |
|---|---|---|---|---|---|
| 并发模型 | Fiber + GMP（M:N，跨线程可迁移） | one loop per thread（N:1，无跨线程协程） | 事件循环回调（无协程） | 回调 + 可选协程（可配） | share-nothing，每核一线程（N:1 per shard） |
| 每连接成本 | 一个 Fiber（池化栈） | 一个回调对象 + 缓冲 | 一个 event + 缓冲 | 一个 handler + 缓冲 | 一个 continuation 链 |
| 内置协议 | **6 协议**（TCP/UDP/HTTP/WS/MQTT/CoAP）+ SOCKS5/Link/OmniNet | HTTP（简易）、protobuf 示例；无 MQTT/CoAP | 无（纯事件库） | 无（纯 IO 库） | HTTP（server 侧） |
| 背压 | 有（`Chan` + `TrySend`） | 有（`BoundedBlockingQueue`） | 应用自建 | 应用自建 | 有（future/continuation 链） |
| 取消语义 | 有（`Context`） | 无 | 无 | 有（`cancellation_slot`，C++20） | 有（`cancelled_exception`） |
| 跨线程调度 | 有（work stealing） | 无（靠线程间队列） | 无（靠用户 post） | 无（需手动 `post`） | 无（跨 shard 靠显式消息） |
| 官方吞吐数字 | 本文 §2/§5（实测） | **未找到**（README 仅安装说明） | **未找到** | **未找到** | **未找到** |

### 4.3 公开数字缺口（已核实的抓取结果）

| 库 | 本轮抓取 | 结果 |
|---|---|---|
| muduo | `https://raw.githubusercontent.com/chenshuo/muduo/master/README` | 972 B，仅安装/教程链接，**无吞吐数字** |
| libevent | `https://raw.githubusercontent.com/libevent/libevent/master/README.md` | 3.4 KB，介绍 + 构建，**无吞吐数字** |
| asio | `https://raw.githubusercontent.com/chriskohlhoff/asio/master/README` | 152 B，**无吞吐数字** |
| Seastar | `https://raw.githubusercontent.com/scylladb/seastar/master/README.md` | 12.3 KB，设计描述（share-nothing/每核一线程），**无吞吐数字** |
| Seastar（搜索） | Bing 检索「Seastar requests per second per core benchmark」 | 只有介绍性文章，**无可引用的原始基准数字** |

**结论**：这四家**没有官方公开的同口径 req/s 数字**（至少本轮抓取与检索范围内没有）。
按规范「缺的数据标注未测/待补」，本报告不填二手博客数字，把「同机同负载的 muduo/libevent/asio 基准」
列为改进项 P0（§7）。

---

## 5. 弱网可靠性：TCP / UDP / Bento 三方实测（补充维度）

来源 `docs/review/transport-comparison-2026-10-28.md`（netem + prio/u32 只影响探针端口，5 场景 × 3 传输 × 3 轮，
每场景 200 条 × 256B，p50/p95 为单向延迟 ms）：

| 场景 | 传输 | 送达率 | p50 | p95（最大） | 重传均值 |
|---|---|---|---:|---:|---:|
| baseline | TCP | 100.0% | 0.02 | 0.06（0.08） | 0.0 |
| baseline | UDP | 100.0% | 0.01 | 0.03（0.04） | 0.0 |
| baseline | **Bento** | 100.0% | **1.00** | 2.00（2.00） | 0.0 |
| loss 5% delay 50 | TCP | 100.0% | 193.84 | 193.96（281.60） | 0.7 |
| loss 5% delay 50 | UDP | 94.3% | 50.15 | 50.18（50.24） | 0.0 |
| loss 5% delay 50 | Bento | 100.0% | 272.33 | 527.67（848.00） | 105.3 |
| loss 20% delay 200 jitter 50 | TCP | 100.0% | 1143.71 | 1580.04（2366.06） | 4.0 |
| loss 20% delay 200 jitter 50 | UDP | 81.3% | 204.70 | 278.61（281.87） | 0.0 |
| loss 20% delay 200 jitter 50 | Bento | 100.0% | 1842.67 | 3441.67（4587.00） | 142.0 |
| loss 40% delay 100 | TCP | 100.0% | 2729.49 | 2796.25（6920.10） | 4.7 |
| loss 40% delay 100 | UDP | 59.8% | 100.09 | 100.13（100.15） | 0.0 |
| loss 40% delay 100 | Bento | **66.7%** | 5944.50 | 9533.00（9796.00） | 397.0 |
| rate 1mbit loss 10% delay 100 | TCP | 100.0% | 786.75 | 786.81（899.65） | 1.7 |
| rate 1mbit loss 10% delay 100 | UDP | 87.8% | 311.30 | 497.03（502.62） | 0.0 |
| rate 1mbit loss 10% delay 100 | Bento | 100.0% | 1372.00 | 1837.00（1922.00） | 70.3 |

**如实记录的三条差距**：

1. **baseline 下 Bento p50 1.00 ms vs TCP 0.02 ms（50×）**：Bento 探针固定 500ms 收尾 +
   `ack_interval 10ms`/`PTO 200ms`，握手与收尾成本压在延迟上；这不是数据面慢，是**控制面参数**。
2. **重传次数**：Bento 在 5% 丢包场景重传 105 次 vs TCP 0.7 次——我们的 PTO 策略比内核 TCP 激进，
   且没有 SACK/快速重传。
3. **40% 丢包场景 Bento 送达率 66.7%**：根因是 1 轮握手 15s 超时（非数据面丢包），
   其余 2 轮 200/200；属**超时参数**问题，不是协议不可靠。

> 该对比说明：我们在「应用层可靠传输」上**明显不如内核 TCP**，这与 §2 的「回环吞吐赢 Go」
> 是两件不同的事，不能互相替代。

---

## 6. 差距分析

| 项 | 现状 | 差距/根因 |
|---|---|---|
| 单连接吞吐 | 8.2–8.6k msg/s | 受 WSL2 回环 RTT（≈110 µs）限制，物理上限 ~9k；已接近上限，**不是实现问题** |
| 多连接扩展 | 16 连接 92k–176k | 2 线程下已超 Go；进一步需要多核扩展性测试（**未测**） |
| WS 16 连接仅 1.07× | 帧头/掩码处理占 CPU | Go 标准库路径极成熟；我们刚做完「一帧一次 recv」，收益已兑现（+18% 单连接） |
| vs muduo/libevent/asio/Seastar 吞吐 | **无数据** | 未测；这是本报告最大缺口 |
| 应用层可靠传输（Bento） | baseline p50 1.00 ms / 40% 丢包 66.7% | 参数（ack_interval/PTO/握手超时）+ 缺 SACK/快速重传 |
| 协议覆盖 | 6 协议 + 3 扩展 | 与 Go 标准库比：Go 有 TLS/HTTP2/QUIC/gRPC，我们无 TLS/HTTP2/QUIC（**功能缺口**，非性能） |

---

## 7. 改进方向

| 优先级 | 动作 | 预期收益 | 依据 |
|---|---|---|---|
| **P0** | 补「同机同负载」的 muduo/libevent/asio 基准（同一 Go 客户端、同样 64B ping-pong、同样绑核） | 补齐 §4 的空白，让网络模块第一次有 C++ 横评 | 规范 §一强制项 |
| **P1** | Bento PTO/ack_interval 调参 + 加 SACK/快速重传 | 弱网送达率 66.7% → 接近 TCP 的 100% | transport-comparison §2 三条差距 |
| **P1** | Bento 握手/收尾延迟（500ms 固定收尾、15s 握手超时） | baseline p50 1.00 ms → 目标 ≤0.2 ms | 同上 |
| **P2** | 16 连接以上的扩展性（64/256 连接） | 验证 Fiber 调度的多核扩展 | 当前只测到 16 连接 |
| **P2** | HTTP/WS 静态表/零拷贝响应（`sendfile`/`writev` 合并） | 中大型响应吞吐 | 现有优化集中在解析与序列化 |
| **P3** | TLS/HTTP2 协议补齐 | 功能差距（非性能） | §6 末行 |

---

## 8. 未测 / 待补（诚实清单）

| 项 | 状态 | 原因 |
|---|---|---|
| muduo 网络吞吐（req/s） | **未测/待补** | 只测了其队列；官方无公开数字 |
| libevent 网络吞吐 | **未测/待补** | 事件库，无内置协议基准；官方无数字 |
| asio 网络吞吐 | **未测/待补** | 官方 README 仅 152 B，无数字 |
| Seastar 网络吞吐 | **未测/待补** | 官方 README 无数字；Bing 检索无可引用原始基准 |
| 我们 vs Go 的 p99/p999 延迟 | **未测** | 目前只有吞吐对拍（`--correctness` 有断言但无延迟分位） |
| 64/256 连接扩展性 | **未测** | 只做到 16 连接 |
| 真实网络（非回环） | **未测** | WSL2 单机环境，无第二台机器 |
| TLS / HTTP2 / QUIC 对比 | **未测** | 功能未实现，无法对比 |
| CPU 占用/内存占用对比 | **未测** | 现有数据只有吞吐 |

---

## 9. 数据来源清单

| # | 来源 | 用途 |
|---|---|---|
| 1 | `docs/reports/2026-11-01-net-vs-go.md` | §2/§3/§4：6 协议 14 场景、Go 基线、优化前后、倍数表、达标判定 |
| 2 | `docs/reports/2026-11-01-concurrency-survey.md` §3 | §4.1 muduo 队列同机实测 |
| 3 | `docs/review/transport-comparison-2026-10-28.md` | §5 弱网三方实测（TCP/UDP/Bento） |
| 4 | `docs/reports/2026-10-31-http-perf.md` | 协议层细项（HTTP/WS/CoAP 单连接与 p99） |
| 5 | `https://raw.githubusercontent.com/chenshuo/muduo/master/README` | muduo 官方资料（无吞吐数字） |
| 6 | `https://raw.githubusercontent.com/libevent/libevent/master/README.md` | libevent 官方资料（无吞吐数字） |
| 7 | `https://raw.githubusercontent.com/chriskohlhoff/asio/master/README` | asio 官方资料（无吞吐数字） |
| 8 | `https://raw.githubusercontent.com/scylladb/seastar/master/README.md` | Seastar 官方资料（无吞吐数字） |

**工业库公开数据缺口**：muduo / libevent / asio / Seastar 的官方仓库均**未提供**同口径网络吞吐数字，
本轮抓取与检索均未找到可引用的一手基准；已按规范标「未测/待补」，不以二手数字填充。
