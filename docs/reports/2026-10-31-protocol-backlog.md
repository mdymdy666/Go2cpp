# 协议遗留清单（6 协议 + OmniNetServer M1）

> 日期：2026-10-31 · 基线：`7fc4952`（基准）+ `419786e`（TCP_NODELAY 修复）
> 范围：HTTP/SSE、WebSocket、LinkServer、MQTT 3.1.1、CoAP、SOCKS5、OmniNetServer M1。
> 性质：**已知问题与优化项清单**，不是缺陷报告替换。每项都给出证据、影响、
> 建议方案与验收口径，便于后续 agent 直接接力。
>
> 状态字段：`OPEN`（未处理）/ `PARTIAL`（部分修复，未复测）/ `WATCH`（环境相关，观察）。
> 优先级：P0 = 数据不可引用/验收阻断；P1 = 正确性或明显性能缺陷且路径明确；
> P2 = 可观测性/体验；P3 = 环境相关、低收益。

## 0. 摘要

| ID | 优先级 | 问题 | 影响 | 状态 |
|---|---|---|---|---|
| B-01 | **P0** | SOCKS5 建连 p99 51.7 ms 未定位 | 建连延迟尾部不可承诺 | PARTIAL（已补 NODELAY，未复测） |
| B-02 | P1 | raw accept 路径缺 `TCP_NODELAY`（tcp_proxy / debug_server） | 小包往返被 Nagle+延迟 ACK 拖慢 | OPEN |
| B-03 | P1 | CoAP 客户端随机种子默认固定 | 多客户端「假重复」，返回上一响应 | OPEN |
| B-04 | P2 | 大包（≥4 KB）走 bufio 回退 19% | 全局开 bufio 会伤大响应 | OPEN（需路由策略） |
| B-05 | P3 | UDP 批量仅约 1.4× | 收益低于预期，非缺陷 | WATCH |
| B-06 | P1 | 协议层未接 `bufio`（适配层已有） | 小包吞吐被 syscall 支配 | OPEN |
| B-07 | P2 | OmniNetServer TCP `accepted` 恒 0 | 统一指标不完整 | OPEN |

建议顺序：**B-01（复测/定位）→ B-02（小修）→ B-03（正确性）→ B-06 + B-04（一起做）→
B-07 → B-05（观察）**。B-01 的复测必须独占低负载窗口，否则会被 B-05/B-06 的基准污染。

## B-01 SOCKS5 建连 p99 51.7 ms（P0，PARTIAL）

**现象**

基准批次（`7fc4952`）实测：45 conn/s，p50 831.94 µs，p99 **51,682 µs**，
p999 51,896 µs。p50 与 p99 差 62 倍，约 2% 的连接出现 ~50 ms 停顿；中继带宽
911 MB/s 不受影响。这是六协议里唯一「吞吐被非协议因素主导」的指标。

**证据**

- 数据：[六协议基准报告](2026-10-31-http-perf.md) §3.1、§3.2、§8（第 1 条）；
- 复现：`taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf --only=socks5`
  （需 `loadavg < 2`，本次复核时机器 loadavg 13.7，未重测）；
- 代码：`src/bento/net/socks5.cpp` 的 `AcceptReady()` / `Poll()` 单线程事件循环。

**已做**

`419786e` 在 accept 后补了 `TCP_NODELAY`（`src/bento/net/socks5.cpp`，注释明确写
「基准实测建连 p99 51.7 ms 同源」），与 HTTP/WS 的 accept 路径一致。**但没有修复后的
A/B 复测**，所以不能把 51.7 ms 当作当前结论，也不能当作已解决。

**影响**

- 引用 SOCKS5 建连性能时必须带 p99 说明，不能只报 45 conn/s；
- 若用于生产代理，尾部延迟会影响短连接业务（HTTP 代理、内网穿透）；
- 中继吞吐不受影响，不要把它当成中继问题。

**建议方案**

1. **先复测**：低负载（`loadavg < 2`、`taskset -c 2,3`）跑 3 轮 `--only=socks5`，
   同时跑 `--nodelay --only=socks5` 的 A/B，确认 51.7 ms 是否消失；
2. 若仍存在：用 `ss -tinp` 抓现场（`Send-Q`/`notsent`/`unacked`）、`strace -c -f`
   看 `poll` 与 `accept` 次数、`perf sched` 看线程调度；重点怀疑：
   - `poll` 的超时粒度与 accept 批量（是否一次只 accept 一条）；
   - 握手三包（greeting/request/reply）是否被拆成多次 `poll` 唤醒；
   - 连接建立后首次写与 `TCP_QUICKACK` 的交互（客户端侧）；
   - 线程被其他连接的中继大块拷贝抢占（单线程 poll 的固有尾延迟）。
3. 若确认是单线程模型的结构问题：给出上限（如 p99 ≤ 5 ms）或改多线程 accept /
   每连接中继线程；在改动前先把根因写进报告。

**验收**

- 低负载 3 轮 p99 ≤ 5 ms，或给出可复现的根因与「单线程 poll 上限」的量化解释；
- 报告更新时标注 loadavg 与是否启用 `--nodelay`；
- 基准输出保留 p50/p99/p999，不接受只报吞吐。

## B-02 raw accept 缺 `TCP_NODELAY`（P1，OPEN）

**现象**

`419786e` 修的是 `net::Socket::accept()` 路径（进程级默认开关
`Socket::SetTcpNoDelayDefault()`，`src/network/socket.cpp`），但仓库里仍有直接调用
裸 `::accept` 的路径没有补：

| 文件 | 位置 | 现状 |
|---|---|---|
| `src/bento/net/tcp_proxy.cpp` | `::accept`（约 291 行） | 未设 `TCP_NODELAY` |
| `src/debug/debug_server.cpp` | `::accept`（约 514 行） | 未设 `TCP_NODELAY` |
| `src/bento/net/tcp_transport.cpp` | `::accept` → `Adopt()` | **已覆盖**：`Adopt()` 内 `SetNoDelay(fd)` |

> 任务清单里提到的 `tcp_transport` 经核实**不是缺口**：accept 返回后走
> `TcpTransport::Adopt()`，其中调用了 `SetNoDelay`（`tcp_transport.cpp` 约 522–529 行）。

**证据**

- `TCP_NODELAY` 的默认开关只在 `Socket` 类内部生效（`src/network/socket.cpp` 的
  `g_tcp_nodelay_default`），裸 `::accept` 不会经过它；
- 历史实测（`419786e`）：小包往返 44 ms → 0.15 ms（292×），说明该路径的收益是
  真实的，不是理论优化。

**影响**

- `tcp_proxy` 承载的代理流量多为小包（握手、ACK、控制帧），可能被 Nagle + 延迟
  ACK 压成 ~40 ms/轮；
- `debug_server` 是调试面，流量小但交互式命令对延迟敏感，且与主路径行为不一致。

**建议方案**

- 两处 accept 成功后调用 `go2cpp::net::Socket::SetTcpNoDelay(fd)`（或先判断
  `TcpNoDelayDefault()`，与 `419786e` 的写法保持一致）；
- 补一条回归：扩展 `bench_protocols --nodelay` 的隔离场景，或在 `test_tcp_proxy` /
  `test_debug_server` 里用 `getsockopt(TCP_NODELAY)` 断言；
- 注意 `debug_server` 的 fd 生命周期：accept 后立即设，失败只记日志不阻断。

**验收**

- `ss -tin` 显示两条路径的连接 `nodelay` 为 1；
- A/B 小包延迟下降（目标与 `419786e` 同量级）；
- 不改变现有错误处理与连接语义。

## B-03 CoAP 随机种子固定（P1，OPEN）

**现象**

`CoapClientConfig::random_seed` 默认 `0x636f61705f636c69`（`"coap_cli"` 的字节），
两个新客户端在同一请求序列下会生成相同的 `(Message ID, Token)`。服务端按
`(Message ID, Token)` 去重，**命中后重发上一个响应、不再调 handler**，于是出现
「PUT 返回了上一个 GET 的 2.05」这类假重复。

**证据**

- `include/go2cpp/bento/net/coap.hpp` 约 427 行：`random_seed = 0x636f61705f636c69ull`；
- 基准报告 §6.2 记录了该现象与规避方式（基准用 `NowNs() ^ 常量` 显式换种子）；
- 服务端去重缓存默认 8 条（`CoapServerConfig`），没有 247 s EXCHANGE_LIFETIME 窗口，
  去重命中是「按缓存内容」而不是严格时间窗，进一步放大了假重复概率。

**影响**

- 正确性风险：多客户端/多实例共存时，响应可能串到另一个请求；
- 只在同种子、同请求序列下复现，单客户端测试发现不了；
- 属于「默认可复现」与「生产正确」的取舍，不是编码错误。

**建议方案**

1. 默认种子改为熵/时间初始化：`NowNs() ^ (pid << 16) ^ counter`，保留
   `CoapClientConfig::random_seed` 作为测试注入点（显式设置时覆盖默认）；
2. 或在 `CoapClient` 构造时检测「同进程内同种子」并输出 warning（不改变行为）；
3. 文档在头文件注释与模块文档里写明「多实例必须换种子」。

**验收**

- 两个默认配置的客户端并发请求，各自收到自己的响应（服务端 handler 各调一次）；
- 单测覆盖「同种子双客户端」与「显式种子可复现」两条路径；
- 默认值变更记入 changelog（行为变更）。

## B-04 大包走 bufio 回退 19%（P2，OPEN，需路由策略）

**现象**

写路径三档对比（4096 B 报文，回环 TCP）：

| 档位 | 吞吐 | 相对单写 |
|---|---|---|
| 单写 `send` | 151,121 ops/s | 1.00× |
| `bufio::Writer(4 KB, kHybrid)` | 122,721 ops/s | **0.81×** |
| `bufio::Writer + FlushBatch(64)` | 146,702 ops/s | 0.97× |

小包（64–120 B）则提升 21–33×。分界线是「报文长度 ≥ 缓冲容量」：此时 bufio 退化成
「直写 + 一次内存拷贝」，系统调用数仍是 1/msg。

**证据**

- 报告 §4「写路径三档对比」与结论 1/2；
- 基准代码：`tests/bento/bench_protocols.cpp` 的 `BufioWrite` / `BufioBatchWrite`
  （约 3801–3886 行）；
- 适配层：`include/go2cpp/network/bufio_socket.hpp` 的 `SocketBatchWriter`。

**影响**

- 若按 B-06 全局给协议层接 bufio 而不做长度路由，16 KB HTTP 响应、大 WS 帧、
  大 MQTT payload 会回退 ~19%；
- 这不是 bufio 的 bug，是「固定缓冲 + 无旁路」的策略问题。

**建议方案**

1. 在写路径入口按长度分流：`len >= capacity` 直接 `send`/`writev`（或 `sendfile`），
   小包进 `bufio::Writer`；
2. 或给 `bufio::Writer` 增加「单条 ≥ 容量时自动直写」策略（保持 API 兼容）；
3. 阈值可配（默认 4 KB），并暴露统计（直写次数/缓冲次数）；
4. 与 B-06 一起落地：先有旁路，再给协议层开 bufio。

**验收**

- 4096 B 档 ≥ 单写 1.0×（目标 1.05×）；
- 64–120 B 档保持 ≥ 20×；
- 回归表纳入 `bench_protocols`，避免后续改动悄悄回退。

## B-05 UDP 批量约 1.4×（P3，WATCH）

**现象**

回环上 UDP 的 `sendmmsg` 批量相对单条 `sendto` 的增益约 **1.4×**（历史批次观察；
标准基准报告没有单列 UDP 批量行，只有 TCP 的 writev 三档）。同一适配层在 TCP 上是
21–33×，差距来自协议语义：UDP 每个段仍是独立数据报。

**证据**

- `include/go2cpp/network/bufio_socket.hpp` 的 UDP 模式：`WriteBatch` → `Socket::send_batch`
  → `sendmmsg`，注释明确「每个段一条数据报」；
- `src/network/socket.cpp` 的 `send_batch_impl`（约 931–953 行）与部分成功语义；
- 报告 §4 结论 3：UDP 批量必须用 `datagram=true`，与 TCP 的 `writev` 语义不同。

**原因假设**

- 每个数据报仍有独立的 IP/UDP 头与内核路径开销，`sendmmsg` 只省「系统调用次数」，
  不省「数据报处理」；
- 回环接收端内核缓冲与单核调度先饱和（Link 不可靠通道不节流时到达率会跌到个位数）；
- 没有真实带宽/多队列，回环上的 1.4× 不能外推到物理网卡。

**影响**

- CoAP、Link 不可靠通道的批量优化收益有限，不要按 TCP 的 21–33× 预期；
- 报告里引用 UDP 批量时必须带「回环、环境上限」限定。

**建议方案**

- 保持 WATCH，不投入优先开发；
- 若后续要提升：优先做「应用层聚合」（同一 peer 的多个小消息合成一条数据报，
  受 MTU 约束），而不是继续压 syscall 次数；
- 跨机/多队列环境重测后再决定是否值得做。

**验收**

- 给出跨机数据，或明确写「回环环境上限 1.4×，不作为优化目标」；
- 不阻塞 B-06 的 TCP 路径。

## B-06 协议层未接 `bufio`（P1，OPEN）

**现象**

6 个协议服务端（`src/bento/net/http_server.cpp`、`ws_server.cpp`、`link_server.cpp`、
`mqtt.cpp`、`coap.cpp`、`socks5.cpp`）仍逐条 `send` / `send_to`，没有使用
`include/go2cpp/network/bufio_socket.hpp` 的 `SocketReader` / `SocketBatchWriter`。
适配层已实现且基准已证明收益（小包 21–33×、syscall/msg 从 1.0 降到 1/32~1/64）。

**证据**

- `grep -rn 'bufio' src/bento/net/*.cpp include/go2cpp/bento/net/*.hpp` 无命中；
- 适配层文档与实现：`include/go2cpp/network/bufio_socket.hpp`（TCP `writev`、UDP
  `sendmmsg`、文件 `sendfile`，含 `send_calls`/`writev_calls`/`sendmmsg_calls` 计数）；
- 基准 §4：HTTP 120 B 23.1×、WS 64 B 27.2×、MQTT 96 B 21.5×、CoAP 64 B 27.7×；
- 当前 6 协议吞吐（HTTP 7.7k、WS 5.4k、MQTT 8.8k、CoAP 9.4k）都受单条 syscall +
  往返支配，与「内存路由 4.88M ops/s」形成 550 倍差距。

**影响**

- 这是当前最大的「已证明、未落地」优化项；
- SSE 事件、WS 帧、MQTT PUBLISH、CoAP 响应都是小包，收益直接；
- 不接 bufio 时，继续优化协议解析/路由的边际收益很低。

**建议方案**

分阶段，每阶段单独跑 bench 回归（先做 B-04 的旁路，避免大包回退）：

1. **HTTP/WS**：响应/帧写路径接 `SocketBatchWriter`（4 KB、`kHybrid`），SSE 单独
   走 flush 策略；保留关闭开关，便于 A/B；
2. **MQTT**：broker 的 outbound 批量 `FlushBatch`（`max_batch=64`），按连接聚合；
3. **CoAP**：UDP `SocketBatchWriter(datagram=true)`，注意每段一条数据报；
4. **Link**：可靠通道批量入队，不可靠通道保持发送节流（批量不解决丢包）；
5. 每步记录 `send_calls/msg`，确认批量化真的生效（不能只看吞吐）。

**验收**

- 小包场景吞吐 ≥ 2×（保守目标；基准上限 21–33×）；
- 大包（≥4 KB）不回退（依赖 B-04）；
- 新增回归：`send_calls/msg < 0.1`（小包档）；
- 6 协议正确性场景全过。

## B-07 OmniNetServer TCP `accepted` 恒 0（P2，OPEN）

**现象**

`NetStats.accepted` 在 TCP 视图（`AsHttpServer`）恒为 0；UDP（LinkServer peer 计数）
与 Memory 正常。`active` 只表示「HttpServer 正在处理的连接数」，keep-alive 空闲连接
不计。

**证据**

- `src/bento/net/omni_server.cpp` 约 751–753 行：注释「HttpServer 未暴露累计连接数；
  accepted 保持 0（M2 在连接层补计数）」；
- `include/go2cpp/bento/net/omni_server.hpp` 的 `NetStats` 注释（约 293–297 行）；
- `HttpServer` 目前只提供 `Stats()` 的活动连接/请求计数，没有累计 accept 数。

**影响**

- 统一入口的指标不完整：无法做「累计连接数 / 拒绝数 / 连接寿命」类 SLA 与告警；
- 与 UDP 视图口径不一致，使用方容易误判；
- 不影响数据面正确性（37 场景全过）。

**建议方案**

1. 在 `HttpServer` 的 accept 循环里加原子 `accepted_total`（成功 accept 即 +1），
   通过 `Stats()` 或新方法暴露；
2. Omni 的 TCP 分支转发该值；`rejected_limit` 同步补（accept 队列满/上限拒绝）；
3. 若不想改 HttpServer 公共接口：在 Omni 包装层用「每次 `active` 增长」估算，
   但会漏掉快速建连/断开，**不推荐**；
4. 补单测：TCP 场景建立 N 条连接后 `accepted == N`（含 keep-alive 复用只算一次）。

**验收**

- TCP 场景 `accepted` 等于实际建连数（含 keep-alive 复用）；
- UDP/Memory 口径不变；
- 单测 + Omni 基准场景覆盖。

## 附录 A：复现与测量纪律

```sh
# 正确性（37 场景 / 238 断言）
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --correctness

# SOCKS5 复测（B-01；必须低负载，记录 loadavg）
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf --only=socks5
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --nodelay   # 含 SOCKS5 A/B

# 写路径（B-04 / B-06）
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf --only=write
```

测量纪律：

- 每次记录 `loadavg`（程序会打印）与是否 `taskset`；共享机器上 loadavg > 4 的
  性能数据不作为结论；
- 延迟必须报 p50/p99/p999，不报均值；
- 不可靠通道（Link DATAGRAM、UDP 批量）必须带到达率，否则测的是丢包率；
- 任何「优化」都要有 syscall/msg 或 `send_calls` 证据，不能只看吞吐。

## 附录 B：不在本清单范围

- TLS/DTLS、HTTP/2、MQTT 5.0、CoAP Observe/Blockwise（协议实现本身未提供）；
- 跨机 RTT/丢包/带宽数据（全部为回环）；
- handler 内部耗时拆解、SSE 长连接持续推流吞吐；
- OmniNetServer M2/M3/M4 的 WS、Raw 嗅探、TLS（设计稿
  [omni-net-design.md](../research/omni-net-design.md) 已列，不在本次收尾范围）。

相关文档：[协议一览](../modules/bento-servers.md)、
[六协议基准报告](2026-10-31-http-perf.md)、
[Bento 路线图](../modules/bento-roadmap.md)。
