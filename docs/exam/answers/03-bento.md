# 03 bento 答卷（20 题）

> 环境：WSL2 / 12 vCPU。Bento 默认 OFF，测试用 `.build-bento-wsl`（已开 BENTO）。
> 性能数字凡来自 docs/提交的，均标注「历史/提交口径」，**未当成本机实测**。

## Q01

**答**：TCP 在弱网的问题有三层：① 传输层队头阻塞——一个丢包卡住后面所有字节，哪怕属于不同消息；② 丢包即降速——CUBIC 把丢包当拥塞信号做乘性减，随机无线丢包让窗口无谓缩水；③ 连接与四元组绑定——NAT 重绑定/换网后 TCP 连接死掉。Bento 在 UDP 上重建可靠层：会话按 `conn_id` 识别（换端口可续），三种通道语义（STREAM 可靠有序 / DATAGRAM 最新值可丢 / VIDEO 分片+FEC）。边界：它是 JinShan relay 的传输底座，不是通用 RPC 框架，也不兼容 WebRTC。

**代码证据**：
```text
include/go2cpp/bento/net/protocol.hpp:19-21  // conn_id 是 P2 新增字段：接收端按它路由，而不是按源 IP:port...
include/go2cpp/bento/net/channel.hpp:20-22  // UnreliableChannel（DATAGRAM）：每通道独立单调 seq；接收端只接受 seq > applied_seq
```

**自证**：`ctest -R '^bento_'` 子集（见下）passed。

**我没答出的**：没有本机弱网（netem）对比 TCP/UDP/Bento 三传输。

## Q02

**答**：CUBIC 判据是丢包（乘性减窗）；BBR 判据是模型：测瓶颈带宽与 min RTT，用 `BDP=bw×min_rtt` 决定 inflight，用 pacing 控制速率，丢包不再直接减窗。相位：Startup 指数增、Drain 排空队列、ProbeBW 周期探测、ProbeRTT 探最小 RTT。本实现是 BBRv1 最小子集：没有 ack aggregation、没有包级 round 判定、没有完整 inflight_hi。BBR 不是万灵药：竞争公平性、深队列 RTT 膨胀是已知问题（仓库未实测）。

**代码证据**：
```text
src/bento/net/congestion.cpp:57-61  case BbrState::kStartup: return "STARTUP"; ... kDrain ... kProbeBw
include/go2cpp/bento/net/congestion.hpp:41-44  enum class CongestionAlgorithm { kNone, kNewReno, kBbr=2 /*默认*/ };
```

**自证**：`ctest -R bento_congestion`（若注册）未单跑；功能子集见末尾。

**我没答出的**：没有 BBR vs NewReno 的同链路吞吐/丢包对比。

## Q03

**答**：重传要等一个 RTT 才知道丢了；FEC 用固定冗余（XOR 开销 1/k；RS 可恢复任意 m 片）在同一 RTT 内恢复，代价是恒定带宽。低丢包（~1% 以下）冗余几乎全浪费；中等丢包 + 高 RTT（5–10% + 100 ms 以上）收益最大；20%+ 仍要重传兜底。默认 `enabled=false`、`video_fec_group_size=0`；组大小 2..8，组内分片零填充到 `max_len`。**仓库只有 5% 丢包重传 -90.3% 一个点（提交 475cc15），没有 FEC on/off 曲线**——精确拐点未实测，只能定性。

**代码证据**：
```text
src/bento/net/fec.cpp:55-59  bool ValidShape(data_shards, parity_shards, kind) { if (data_shards < 2 || > 64) return false; ...
src/bento/net/channel.cpp:1548-1553  std::vector<std::uint8_t> parity(max_len, 0); ... parity[j] ^= data[offset + j];
include/go2cpp/bento/net/channel.hpp:753  static constexpr std::size_t kMaxFecGroupSize = 8;
```

**自证**：未跑 FEC on/off 吞吐；`ctest -R bento_fec` 未单跑。

**我没答出的**：FEC 拐点实测（没有曲线）。

## Q04

**答**：0-RTT 让客户端第一个包就带应用数据，省 1-RTT，短连接/高 RTT 收益最大。代价是 0-RTT 数据可被重放：攻击者抓包重发，服务端可能重复执行非幂等操作；协议层无法区分「重放」与「真的又发了一次」。本实现防线：握手时间戳 + `handshake_replay_window_ms`（默认 30000）+ `session_ticket_lifetime_ms`（默认 60000）；窗口外/过期拒绝。只给纯 PSK：X25519 会话导出票据会把前向保密静默降级，所以 `ExportSessionTicket` 直接拒绝 X25519 会话。

**代码证据**：
```text
src/bento/net/link.cpp:2483-2486  ExportSessionTicket() ... // X25519 会话的密钥混入了临时共享密钥，无法从票据安全恢复，因此直接拒绝导出
src/bento/net/link.cpp:2521-2525  bool Link::Resume(...) { if (!m_secure_enabled) ++tickets_rejected;
include/go2cpp/bento/net/link.hpp:138  std::uint32_t handshake_replay_window_ms = 30000;
```

**自证**：未做 0-RTT 重放攻击实验（题面也标注「无攻防实测」）。

**我没答出的**：没有重放窗口的攻防验证。

## Q05

**答**：16B 头字段：`ver(1)/type(1)/chan(1)/flags(1)/conn_id(4)/seq(4)/dep_seq(4)`，大端。`conn_id` 是 P2 新增：接收端按它路由，不按源 IP:port，NAT 重绑定后会话可续。`dep_seq` 是「依赖的不可靠 seq」，0 表示无依赖，用于因果屏障。为什么不是 12B：v0.2 起多出 4B `conn_id`；12B 是 tunnel 的 `TUNL` 帧（magic+version+type+flags+length），与 Bento 无关，旧 12B 线格式已不兼容。

**代码证据**：
```text
include/go2cpp/bento/net/protocol.hpp:67-70  kProtocolVersion = 2; inline constexpr std::size_t kFrameHeaderBytes = 16;
include/go2cpp/bento/net/protocol.hpp:169-173  struct FrameHeader { version; type; channel; flags; ... }
include/go2cpp/bento/net/protocol.hpp:15-17  | 4 | conn_id | u32 | ... | 8 | seq | ... | 12 | dep_seq | ...
```

**自证**：`grep -n kFrameHeaderBytes include/go2cpp/bento/net/protocol.hpp` → 70:16。

**我没答出的**：没有解析抓包验证字段偏移。

## Q06

**答**：旧结构 `RunOnce` 第一行 `lock_guard<recursive_mutex>` 罩住 `poll + 全部 HandleFrame + Tick`，发送入口抢同一把锁 → 发送线程最多被阻塞一个 `timeout_ms`（p99 达 1405 ms 量级）。新结构：poll 在锁外；`Send*` 只把帧塞进 `m_tx_queue` 并置 `m_tx_pending`；`DrainTx` 在锁外 seal+`sendto`；接收路径「锁内入队事件、锁外派发回调」。错因是锁罩接收循环，不是网络。队列满按 UDP 语义丢弃并 `++send_errors`。

**代码证据**：
```text
src/bento/net/link.cpp:828-831  int Link::RunOnce(int timeout_ms) { // 阶段 1（锁外）：poll... 基线里发送线程会被整段 poll 挡住，p99 达到 timeout 的量级
src/bento/net/link.cpp:1776  // 入队而不是直接发送：真正的 seal/sendto 由 DrainTx() 在锁外完成。
src/bento/net/link.cpp:1781-1783  if (m_tx_queue.size() >= kMaxTxQueue) { ++m_stats.send_errors; return false; }
include/go2cpp/bento/net/link.hpp:791  // m_tx_mutex 只保护队列本身，不保护链路状态
```

**自证**：未复跑 1405 ms→0.045 ms（需要提交 5a2c0fa 前后的对照构建）。

**我没答出的**：没有本机 p99 对照（只有代码结构证据）。

## Q07

**答**：多队列解决的是**收包 syscall 的核间扩展**：每队列一个 fd、内核按四元组哈希分流，配合 `recvmmsg` 一次收多包。单 peer 时协议处理仍在 `RunOnce` 单线程（解帧/ACK/重组/回调串行），收包 syscall 不是瓶颈 → 收益为零（提交 475cc15 明确「单 peer 无收益已证」）。收益场景是 PeerHub 多 peer；`rx_threads` 只把 poll+recvmmsg 摊到多核，协议处理与回调仍在 `RunOnce` 调用线程。

**代码证据**（题面 `link.cpp:577` 有误，`recvmmsg` 在 **574**）：
```text
src/bento/net/link.cpp:313-316  if (reuse_port) { ... setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, ...)
src/bento/net/link.cpp:667  std::uint32_t wanted_queues = m_config.rx_queues;
src/bento/net/link.cpp:574  received = ::recvmmsg(fd, messages, ...);
```
> **题面错误**：Q07 的 `src/bento/net/link.cpp:577` 是 `break;`，`recvmmsg` 实际在 **574**（本卷埋点）。

**自证**：`sed -n '573,578p'` 输出 574 行 `recvmmsg`。

**我没答出的**：没有单 peer vs PeerHub 的吞吐对照。

## Q08

**答**：拓扑是一个 UDP socket 承载 N 个 peer，按 `conn_id` 路由，每 peer 独立密钥与重放窗口。FEC 挂在 **per-peer 的可靠/视频通道**（`ReliableChannel::Config.fec` / `VideoChannel`），Hub 只负责把 `m_config.fec` 下灌给 `reliable`；收到 `kFec` 帧后按 peer/channel 分发到对应通道。多队列负责收包并行，FEC 负责丢包恢复，两层不同。

**代码证据**：
```text
src/bento/net/peer_hub.cpp:328-329  if (m_config.fec.enabled) { m_config.reliable.fec = m_config.fec; }
src/bento/net/peer_hub.cpp:1557-1560  case FrameType::kFec: { ... const auto found = peer.reliable.find(view.header.channel);
```

**自证**：`ctest -R bento_peer_hub` 未单跑；代码路径为准。

**我没答出的**：没有本机 5% 丢包重传 -90.3% 的复跑。

## Q09

**答**：不能直接引用。那是 **2026-10-28 基线**（5 场景 × 3 传输 × 3 轮，每场景 200 条 × 256B）；之后补了 `handshake_copies=2`、16 轮握手、SACK 扩展、快速重传、BBR，但**没有重跑同口径对比**，引用必须带日期限定。40% 丢包 66.7% 送达率的根因是**一轮握手 15 s 超时**（其余两轮 200/200），不是数据面；baseline p50 1.00 ms vs TCP 0.02 ms 的差距在「500 ms 固定收尾 + ack_interval/PTO 参数」。

**代码证据**：
```text
include/go2cpp/bento/net/link.hpp:97-99  // 10 轮在 40% 丢包下不够稳（3 轮对照实测出现 1 次 15s 超时）；配合 handshake_copies 后 16 轮...
include/go2cpp/bento/net/link.hpp:101-103  // 40% 丢包下单包到达率 60%，2 份把单轮成功率从 0.6 提到 0.84
```

**自证**：未跑 netem（需要 CAP_NET_ADMIN，当前容器无权限）。

**我没答出的**：没有本机弱网重跑；只能引用历史基线并说明其边界。

## Q10

**答**：协商机制：HELLO/HELLO_ACK 基础 12B，之后可选追加 2B caps（`kCapSackExt=0x0001`）；双方都置位才启用，任一端关就退回 32 位位图，线格式与旧协议一致。加密会话恒降级的原因：PSK 加密的 HELLO body 长度固定（旧端严格校验 46/78B），追加 caps 会破坏旧端校验，所以强制降级。数据帧捎带的 ACK 永远 8B（帧头 flags 无空闲位），扩展只走纯 ACK 帧。降级损失：32 位位图 + 最多 16 块（近端 15 + 最远 1），乱序窗口 >32 的已收项无法告知对端，PTO 会重复重传。

**代码证据**：
```text
include/go2cpp/bento/net/protocol.hpp:93-101  kHelloBaseBytes=12; kHelloCapsBytes=14; kCapSackExt=0x0001;
include/go2cpp/bento/net/link.hpp:105-107  // SACK 多块扩展...加密握手（PSK）的 HELLO body 长度固定...
```

**自证**：未跑加密会话 SACK 降级用例；代码注释与协议常量一致。

**我没答出的**：没有构造加密 HELLO 验证降级分支。

## Q11

**答**：16 轮 × 每轮 2 份、PTO 从 200 ms 指数退避到 2 s。每轮 2 份独立丢失概率 `p²`，32 份全丢约 `p^32`，40% 丢包下已可忽略（单轮成功率 0.6→0.84）。代价是最坏握手耗时随 PTO 退避累加（十几秒），这正是 40% 丢包旧基线「一轮 15 s 超时」的来源。握手阶段没有后续包触发重复 ACK，只能靠冗余+退避；快速重传帮不上。

**代码证据**：
```text
include/go2cpp/bento/net/link.hpp:95-99  hello_initial_pto_ms=200; hello_max_pto_ms=2000; hello_max_attempts=16;
include/go2cpp/bento/net/link.hpp:100-103  handshake_copies=2; // 2 份把单轮成功率从 0.6 提到 0.84
```

**自证**：未做 40% 丢包握手成功率统计。

**我没答出的**：没有实测握手耗时分布。

## Q12

**答**：可以调 `Send*`/`Close`：回调在 `RunOnce` 调用线程、状态锁外执行（「锁内入队、锁外派发」，不是靠可重入锁），所以不会自死锁。**不能阻塞**：回调里跑长任务会阻塞整条链路的收包与定时器（ACK/PTO/心跳）。`Close`/`RemovePeer` 的销毁延迟到安全点，回调返回后才真正释放。跨线程投递用 `ConcurrentSend` 或自己的队列。

**代码证据**：
```text
include/go2cpp/bento/net/link.hpp:802  // ---- 回调事件（锁内入队、锁外派发；避免 recursive_mutex） ----
```

**自证**：未写「回调 sleep 100 ms」的链路卡死复现。

**我没答出的**：没有实测回调阻塞对 ACK/PTO 的影响。

## Q13

**答**：默认加密：`require_encryption = true`；没有 32B PSK 时 `Open/Connect` 直接失败并返回 `SecurityError::kEncryptionRequired`；监听端拒收明文 HELLO，防降级。老代码忽略返回值仍「能跑」，但所有发送被安全策略拒绝——最危险的静默失败。测试要么两端配同一 PSK，要么显式 `require_encryption=false`。

**代码证据**：
```text
include/go2cpp/bento/net/link.hpp:121  bool require_encryption = true;
include/go2cpp/bento/net/link.hpp:117-118  // 监听方丢弃明文 HELLO 且不回 ACK（拒绝降级尝试）；
```

**自证**：`ctest -R '^bento_(link_loopback|crypto)'` 见末尾（若通过）。

**我没答出的**：没有在无 PSK 下验证 `Open()` 的具体错误码。

## Q14

**答**：DATAGRAM 不可靠、不重传：每通道单调 seq，接收端只接受 `seq > applied_seq`，旧包 DropStale。`dep_seq` 表示「依赖的不可靠 seq」：依赖未满足时帧进 Relation pending，依赖满足后若自身 seq 已被更新帧越过则按 DropStale 丢弃，保证 0 乱序的因果语义。乱序缓存上限可配（0 = 关闭缓存退回纯 DropStale）。与 STREAM 的区别：STREAM 丢包重传保序，DATAGRAM 丢包就丢、最新值优先。

**代码证据**：
```text
include/go2cpp/bento/net/channel.hpp:19-22  // 接收端只接受 seq > applied_seq，旧包一律 DropStale。带 dep_seq 的帧在依赖未满足时进入 Relation 的 pending 队列...
include/go2cpp/bento/net/channel.hpp:629-632  // 乱序缓存上限（帧）。0 = 关闭缓存，退回纯 DropStale...
```

**自证**：未跑 DATAGRAM 乱序/抖动用例。

**我没答出的**：没有构造 dep_seq 越序场景。

## Q15

**答**：静态 PSK：认证+加密，**无前向保密**（长期密钥泄露则历史流量可解），好处是握手简单、可 0-RTT。X25519：临时密钥交换，提供前向保密，代价是多一轮且票据不能复用（否则前向保密被静默降级）。本实现默认 `enable_x25519=true`，失败不降级。0-RTT 票据只对纯 PSK 导出；X25519 会话想 0-RTT 就必须放弃前向保密，所以直接不给。

**代码证据**：
```text
include/go2cpp/bento/net/link.hpp:133-136  // false = 纯 PSK 模式...0-RTT 会话票据按静态 PSK 重派生密钥，因此只有纯 PSK 会话允许导出/缓存票据
include/go2cpp/bento/net/link.hpp:136  bool enable_x25519 = true;
```

**自证**：代码核对。

**我没答出的**：没有做 X25519 会话导出票据被拒的用例。

## Q16

**答**：差距是 SIMD 造成的物理下限：自研是标量 C（Poly1305 每 16B 块约 143 指令 ≈ 8.9 指令/字节），OpenSSL 是 AVX512 手写汇编（约 5.1 GB/s vs 自研约 380 MB/s）。分档：≤64B 自研更快（EVP 每次 `EncryptInit_ex` 有 ~300–500 ns 固定开销），≥256B 被反超，1 MB 到 13.41×。自研封顶约 380 MB/s 是 ChaCha20(830 MB/s) 与 Poly1305(700 MB/s) 串联的必然结果。这条 AEAD 不在 TLS 路径上（TLS 用 OpenSSL），只在 Bento PSK 里用。不是调参能解决，需要向量化。

**代码证据**：
```text
src/bento/net/crypto.cpp（标量实现；git status 显示其有未提交修改，本次未动）
```
> 说明：docs/reports/asm-tls.md 的 13.41×/0 SIMD 是历史测量，本机未复跑。

**自证**：未跑 `objdump -d` 统计 SIMD；未跑 AEAD 分档基准。

**我没答出的**：没有本机 ChaCha20-Poly1305 vs OpenSSL 的吞吐分档。

## Q17

**答**：PMTUD 需要 DF 位 + 探针（`kPmtudProbe/kPmtudAck`），探测失败要靠超时重试，依赖路径行为，所以默认关闭。关闭时用保守 1200（IPv6 最小 MTU 1280 减隧道开销的安全值），最大 1500；跨 IPv4/IPv6 不混用。STREAM 帧 >1300B 自动分片（`kMaxFramePayload=1300`）。不开 PMTUD 的代价：大 MTU 无法利用，但不会黑洞丢包。

**代码证据**：
```text
include/go2cpp/bento/net/protocol.hpp:103-105  // 单帧载荷上限。留出 16B 头 + 8B ACK 后仍低于 1500B...kMaxFramePayload = 1300;
include/go2cpp/bento/net/link.hpp:127  bool enable_pmtud = false;（默认关）
```

**自证**：`grep -n enable_pmtud include/go2cpp/bento/net/link.hpp` → 默认 false。

**我没答出的**：没有跨 MTU 路径的黑洞实验。

## Q18

**答**：新 `frame_id` 到来就 `assembly.data.assign(frag_count × kStreamFragmentBytes, 0)` + `received.assign(frag_count, 0)`，即使只到 1 个分片；丢帧/乱序时 `m_active` 里可能同时存在多份大缓冲（受 `kMaxReassembly` 限制）。代价是每帧一次几十 KB 分配 + memset，高帧率下纯内存带宽浪费（review P1-15）。改进方向：按需增长（收到第 i 片才 resize）、`received` 用位图、帧缓冲池化。FEC 组大小 2..8，组内零填充到 `max_len`。

**代码证据**：
```text
src/bento/net/channel.cpp:202-203  assembly.data.assign(frag_count * kStreamFragmentBytes, 0); assembly.received.assign(frag_count, 0);
include/go2cpp/bento/net/channel.hpp:753  kMaxFecGroupSize = 8;
```

**自证**：代码核对；未用 heaptrack 量化分配。

**我没答出的**：没有测高帧率下的分配/带宽开销。

## Q19

**答**：不能线性扩展。`rx_threads` 只并行「收包系统调用」，解帧/ACK/重组/回调仍在 `RunOnce` 调用线程，所以吞吐上限由单线程 `RunOnce` 决定；回调线程语义不变。多队列省下的 syscall 时间只在多 peer 场景有意义。要真扩展需按 `conn_id`/通道分片到多线程（未做）。

**代码证据**：
```text
src/bento/net/link.cpp:667  wanted_queues = m_config.rx_queues; // 只影响收包 socket 数
src/bento/net/link.cpp:574  recvmmsg(...); // 收包批量
```

**自证**：未测 rx_queues=1/2/4 的吞吐曲线。

**我没答出的**：没有本机多队列扩展性数据。

## Q20

**答**：Link 是单 peer：监听方只接受第一个握手成功的来源；1:N 必须用 `PeerHub`（独立组件：一个 UDP socket、按 `conn_id` 路由、per-peer 密钥/重放窗口/通道表）。两条 UDP 代码路径并存是有意的，避免把 Link 改成多 peer 引发 2000+ 行全量回归。1:N 容量参考是旧口径（3 peer × 50 STREAM + 50 DATAGRAM，约 30.7 Mbit/s），不能当承诺。

**代码证据**：
```text
src/bento/net/peer_hub.cpp:328  FEC 配置下灌到 reliable 通道
include/go2cpp/bento/net/link.hpp（Link 单 peer；PeerHub 是独立类）
```

**自证**：未跑 1 被控 N 主控场景。

**我没答出的**：没有本机 PeerHub 吞吐复测。
