# Go2Cpp 理解力考试 · Bento 传输卷（03 bento）

> **考的是「为什么自研 / 哪一层坏 / 数据多少」**。
> 参考基线：`user/docs/03-bento.md`、`docs/modules/bento-{foundation,transport,session,security,scenarios}.md`、
> `docs/audit/audit-sack.md`、`docs/audit/audit-heartbeat-backoff.md`、`docs/reports/2026-11-01-network-app-opt-review.md`、
> `docs/reports/asm-tls.md` §4–§6，以及提交 `5a2c0fa`（Link 收发分离）、`475cc15`（多队列 + PeerHub FEC）、
> `aa4b728`（HTTP 三策略，本卷不考）。
>
> 难度分布：L1 = 3、L2 = 7、L3 = 7、L4 = 3。
> **诚实缺口**：弱网对比是 **2026-10-28 基线**，之后加了 SACK/快速重传/BBR/FEC 但**没有重跑**；
> 卷内引用该表必须带日期限定，否则视为不合格。

---

## 一、为什么要自研传输

### Q01 【L1 概念】已经有 TCP，为什么还要在 UDP 上自研一条可靠传输？

- **痛点**：只会说「UDP 快」，说不出 TCP 在弱网下具体坏在哪、以及为什么应用层需要三种通道语义。
- **追查位置**：
  - `user/docs/03-bento.md:13`（TCP 在弱网被「队头阻塞 + 丢包即降速」拖死，裸 UDP 又没有可靠性）
  - `user/docs/03-bento.md:16`（三种通道语义：STREAM / DATAGRAM / VIDEO；会话按 `conn_id` 识别）
- **验收标准**：
  1. TCP 的**传输层队头阻塞**：一条连接上一个丢包会卡住后面所有字节，哪怕它们属于不同消息/不同流。
  2. **丢包即降速**：CUBIC 把丢包当拥塞信号做乘性减，随机无线丢包会让窗口无谓缩水。
  3. **连接与四元组绑定**：NAT 重绑定/换网后 TCP 连接死掉；Bento 按 `conn_id` 识别会话，换端口可续。
  4. 应用确实需要混合语义：操控要可靠有序（STREAM），状态/位置要最新值可丢（DATAGRAM），视频要分片 + 可选 FEC（VIDEO）。
  5. 边界：它是 JinShan relay 场景的传输底座，**不是通用 RPC 框架，也不兼容 WebRTC**（`user/docs/03-bento.md:24`）。
- **常见错误答案**：「UDP 比 TCP 快」「TCP 也能多路复用所以不需要」。

### Q02 【L3 陷阱】BBR 与 CUBIC 的判据差在哪？为什么高丢包链路上 BBR 不会像 CUBIC 那样崩？

- **痛点**：把 BBR 当「更大的 CUBIC」，或者以为 BBR 对丢包完全免疫。
- **追查位置**：
  - `src/bento/net/congestion.cpp:57`（BBR 相位：`kStartup / kDrain / kProbeBw / kProbeRtt`）
  - `include/go2cpp/bento/net/congestion.hpp:44`（`CongestionAlgorithm::kBbr` 默认，保留 `kNewReno` 对照）
- **验收标准**：
  1. CUBIC 的判据是**丢包**：一旦丢包就乘性减窗，随机丢包（非拥塞）会被误判。
  2. BBR 的判据是**模型**：测量 bottleneck bandwidth 与 min RTT，用 `BDP = bw × min_rtt` 决定 inflight，用 pacing 控制发送速率；丢包不再直接触发减窗。
  3. 相位语义：Startup 指数增、Drain 排空队列、ProbeBW 周期性探测带宽、ProbeRTT 探测最小 RTT。
  4. 本实现是 **BBRv1 最小子集**：没有 ack aggregation、没有包级精确 round 判定、没有完整 `inflight_hi` 探测（`user/docs/03-bento.md:345`）。
  5. 仍然要承认 BBR 不是万灵药：竞争公平性、深队列下的 RTT 膨胀是已知问题（仓库未实测，只能定性）。
- **常见错误答案**：「BBR 不会丢包」「BBR 就是无脑加大窗口」「BBR 是 TCP 的开关」。

### Q03 【L3 取舍】FEC 与重传怎么选？什么丢包率下 FEC 才划算？

- **痛点**：以为「加 FEC 一定更好」，不知道 FEC 是带宽换延迟，低丢包时纯亏。
- **追查位置**：
  - `user/docs/03-bento.md:344`（FEC 默认关闭；开启是「带宽换延迟」；`k+m <= 255`）
  - `src/bento/net/fec.cpp:55`（`ValidShape`：XOR 只允许 m=1，RS 受 GF(256) 约束）
- **验收标准**：
  1. 机制差异：重传要等一个 RTT 才知道丢了；FEC 用固定冗余（XOR 开销 1/k，RS 可恢复任意 m 片）在**同一 RTT 内**恢复，代价是恒定带宽。
  2. 划算区间：低丢包（~1% 以下）冗余几乎全浪费；中等丢包 + 高 RTT（如 5–10% + 100 ms 以上）FEC 收益最大；高丢包（20%+）FEC 也补不过来，仍要重传兜底。
  3. 项目默认 `enabled=false`，`video_fec_group_size=0`（关）；组大小 2..8，组内分片零填充到 `max_len`（`src/bento/net/channel.cpp:1550` 的 XOR 实现按 `max_len` 补齐）。
  4. 已有实测只有 **PeerHub 5% 丢包重传 -90.3%**（提交 `475cc15`，517 checks）；仓库**没有** FEC on/off 的吞吐-延迟曲线，所以「什么丢包率开始划算」的精确拐点**未实测，只能定性**。
- **常见错误答案**：「FEC 越多越好」「FEC 可以替代重传」「FEC 不占带宽」。

### Q04 【L4 陷阱】0-RTT 省下的那一轮 RTT，代价是什么？重放窗口为什么不能关？

- **痛点**：只记住「0-RTT 快」，说不出重放攻击面，也不知道本项目为什么只给纯 PSK 会话开。
- **追查位置**：
  - `src/bento/net/link.cpp:2483`（`ExportSessionTicket`：导出会话票据）
  - `src/bento/net/link.cpp:2521`（`Resume`：用票据恢复）
  - `user/docs/03-bento.md:95`（0-RTT 恢复只对纯 PSK 会话开放；X25519 会话不导出票据）
- **验收标准**：
  1. 收益：客户端第一个包就能带应用数据，省 1-RTT；对短连接/高 RTT 场景收益最大。
  2. 代价：0-RTT 数据可被**重放**——攻击者抓包后重发，服务端可能重复执行非幂等操作（下单、扣款）。协议层无法区分「重放」与「真的又发了一次」。
  3. 本实现的防线：握手时间戳 + 重放窗口（`handshake_replay_window_ms` 默认 30000）+ 票据生命周期（`session_ticket_lifetime_ms` 默认 60000），窗口外/过期票据拒绝。
  4. 为什么只给纯 PSK：X25519 会话导出票据会把「前向保密」静默降级成长期密钥，所以直接不导出（`user/docs/03-bento.md:96`）。
  5. 结论：0-RTT 适合幂等请求（GET/状态上报），非幂等操作必须由应用层去重。
- **常见错误答案**：「0-RTT 没有代价」「重放窗口可以关掉换性能」「X25519 也能 0-RTT，只是慢一点」。

---

## 二、帧格式与可靠性

### Q05 【L1 概念】16 字节帧头里每个字段解决什么问题？为什么不是 12 字节？

- **痛点**：把 Bento 的 16B 头与 tunnel 的 12B 头搞混，或者答不出 `conn_id` 与 `dep_seq` 的用途。
- **追查位置**：
  - `include/go2cpp/bento/net/protocol.hpp:70`（`kFrameHeaderBytes = 16`，v0.2 起含 4B `conn_id`）
  - `include/go2cpp/bento/net/protocol.hpp:169`（`struct FrameHeader` 的字段顺序）
- **验收标准**：
  1. 字段：`ver(1) / type(1) / chan(1) / flags(1) / conn_id(4) / seq(4) / dep_seq(4)`，共 16B，大端。
  2. `conn_id` 是 P2 新增：接收端按它路由，不按源 IP:port；NAT 重绑定后会话可续（`include/go2cpp/bento/net/protocol.hpp:19-21`）。
  3. `dep_seq` 是「依赖的不可靠 seq」，0 表示无依赖；用于因果屏障，保证操控/视频不越序。
  4. 12B 是 tunnel 的 `TUNL` 帧（`magic+version+type+flags+length`），与 Bento 无关；Bento 旧 12B 线格式已不兼容（`user/docs/03-bento.md:353`）。
- **常见错误答案**：「Bento 头是 12 字节」「`conn_id` 就是 UDP 源端口」。

### Q06 【L4 陷阱】Link 收发分离到底改了什么？1405 ms → 0.045 ms 是网络抖动还是锁？

- **痛点**：把 p99 的 3 万倍改善归因于「网络变好」或「换了个 mutex」，说不出真正的临界区。
- **追查位置**：
  - `src/bento/net/link.cpp:828`（`RunOnce` 阶段 1 锁外 poll，注释写明基线里发送线程被整段 poll 挡住、p99 达到 timeout 量级）
  - `src/bento/net/link.cpp:1776`（`SendBuiltFrame` 只入队，真正的 seal/sendto 由 `DrainTx` 在锁外完成）
  - `include/go2cpp/bento/net/link.hpp:791`（`m_tx_mutex` 只保护队列本身，不保护链路状态）
- **验收标准**：
  1. 旧结构：`RunOnce` 第一行 `lock_guard<recursive_mutex>` 罩住 `poll + 全部 HandleFrame + Tick`，发送入口抢同一把锁 → 发送线程最多被阻塞一个 `timeout_ms`（review P0-1，`docs/reports/2026-11-01-network-app-opt-review.md:72-80`）。
  2. 新结构：poll 在锁外；`Send*` 只把帧塞进 `m_tx_queue` 并置 `m_tx_pending`；由 `DrainTx`（单 drainer，`src/bento/net/link.cpp:1800`）在锁外做 seal + `sendto`；接收路径改成「锁内入队事件、锁外派发回调」。
  3. 实测：发送 p99 **1405 ms → 0.045 ms（3.1 万倍）**，1e4 包吞吐 **7.4×**（提交 `5a2c0fa`）。错因是 **`recursive_mutex` 罩住接收循环**，不是网络。
  4. 队列满时按 UDP 语义丢弃并 `++m_stats.send_errors`（`src/bento/net/link.cpp:1781-1784`），不能无限堆。
  5. 附带效果：回调不再持锁执行，用户回调里 `Send*`/`Close` 仍安全（靠「锁内入队、锁外派发」而不是可重入锁）。
- **常见错误答案**：「把 recursive_mutex 换成普通 mutex 就行」（会自死锁）、「p99 是 WSL2 调度抖动」、「7.4× 是压测误差」。

### Q07 【L3 陷阱】Link 的 `SO_REUSEPORT` 多队列，为什么单 peer 场景「没有收益」？

- **痛点**：以为多队列必然线性扩展，把「已证无收益」的结论当失败。
- **追查位置**：
  - `src/bento/net/link.cpp:316`（`SO_REUSEPORT` 建多队列 socket）
  - `src/bento/net/link.cpp:667`（按 `rx_queues` 建队列）
  - `src/bento/net/link.cpp:574`（`recvmmsg` 批量收包）
- **验收标准**：
  1. 多队列解决的是**收包 syscall 的核间扩展**：每队列一个 fd、内核按四元组哈希分流，配合 `recvmmsg` 一次收多包。
  2. 单 peer 时协议处理仍在 `RunOnce` 单线程：解帧/ACK/重组/回调都串行，收包 syscall 根本不是瓶颈 → **收益为零**（提交 `475cc15` 标题明确「单 peer 无收益已证」）。
  3. 收益场景是 **PeerHub 多 peer**：多个 peer 可并行收包，再按 `conn_id` 分发。
  4. `rx_threads` 只把 poll+recvmmsg 摊到多核，**协议处理与回调仍在 `RunOnce` 调用线程**（`user/docs/03-bento.md:75`）。
- **常见错误答案**：「多队列一定提升吞吐」「rx_threads 拉满就线性扩展」。

### Q08 【L3 组合】PeerHub 的「多队列 + FEC」是怎么组合的？FEC 挂在哪一层？

- **痛点**：以为 FEC 在 Hub 层统一做，或者以为 PeerHub 也走单 peer 的 Link 路径。
- **追查位置**：
  - `src/bento/net/peer_hub.cpp:328`（`m_config.fec.enabled` 时把 `FecConfig` 灌给 `reliable` 通道）
  - `src/bento/net/peer_hub.cpp:1557`（收到 `kFec` 帧后按 peer 分发到对应通道）
- **验收标准**：
  1. 拓扑：一个 UDP socket 承载 N 个 peer，按 `conn_id` 路由；每 peer 独立密钥与重放窗口。
  2. FEC 挂在 **per-peer 的可靠/视频通道**上（`ReliableChannel::Config.fec` / `VideoChannel`），不是 Hub 全局一层；Hub 只负责把配置下灌。
  3. 多队列（`SO_REUSEPORT + recvmmsg`）负责收包并行，FEC 负责丢包恢复，两者解决不同层的问题。
  4. 实测：PeerHub 在 5% 丢包下重传 **-90.3%**（提交 `475cc15`，517 checks）；3 peer × 50 STREAM + 50 DATAGRAM 隔离，307 KB/80 ms ≈ **30.7 Mbit/s**（`user/docs/03-bento.md:331`）。
  5. 边界：PeerHub 仍未接 0-RTT、VIDEO/FEC 的完整矩阵、PMTUD（`docs/modules/bento-scenarios.md:347` 记录）。
- **常见错误答案**：「FEC 在 Hub 层统一编码」「PeerHub 就是多个 Link 实例」。

### Q09 【L4 陷阱】弱网基线 2026-10-28 的数据还能直接引用吗？40% 丢包 66.7% 送达率怎么解释？

- **痛点**：把旧基线当现状，或者反过来把「已修但未复测」说成「已解决」。
- **追查位置**：
  - `user/docs/03-bento.md:309`（口径提醒：netem 对比是 **2026-10-28 基线**，之后补了 SACK/快速重传/BBR/FEC，**尚未重跑**）
  - `user/docs/03-bento.md:321`（如实记录的三条差距：baseline p50 1.00 ms、5% 丢包重传 105 次、40% 丢包 66.7%）
- **验收标准**：
  1. 表里 5 个场景：baseline / 5%+50ms / 20%+200ms±50 / 40%+100ms / 1Mbit+10%+100ms；每场景 200 条 × 256B。
  2. 40% 丢包送达率 66.7% 的根因是**一轮握手 15 s 超时**（其余两轮 200/200），不是数据面丢包。
  3. 之后用 `handshake_copies=2`、16 轮握手、SACK 扩展、快速重传、BBR 针对性修改过，但**没有重跑同口径对比** → 引用必须带「2026-10-28 基线」限定。
  4. baseline 下 Bento p50 1.00 ms vs TCP 0.02 ms，差在「500 ms 固定收尾 + ack_interval/PTO 参数」，不是数据面（`user/docs/03-bento.md:321`）。
  5. 正确表述：「旧基线在 40% 丢包下 66.7%，根因已定位并改过，但**新数据未实测**」。
- **常见错误答案**：「Bento 在 40% 丢包下就是 66.7%」（过时）、「已经修好了所以旧数据仍有效」。

### Q10 【L3 陷阱】SACK 扩展为什么在加密会话里恒降级？降级后损失什么？

- **痛点**：以为「开了 `enable_sack_ext` 就有多块 SACK」，不知道加密会话拿不到 caps。
- **追查位置**：
  - `include/go2cpp/bento/net/protocol.hpp:96`（`kHelloCapsBytes = kHelloBaseBytes + 2`，caps 追加在 12B 之后）
  - `include/go2cpp/bento/net/protocol.hpp:101`（`kCapSackExt = 0x0001`，bit0）
- **验收标准**：
  1. 协商机制：HELLO/HELLO_ACK 载荷 12B 之后可选追加 2B caps；双方都置位才启用，任一端关就退回 32 位位图，线格式与旧协议逐字节一致。
  2. 降级原因：PSK 加密会话的 HELLO body 长度固定（旧端严格校验 46/78B），追加 caps 会破坏旧端校验 → 加密会话**强制降级**（`docs/audit/audit-sack.md:48`、`:176`）。
  3. 数据帧捎带的 ACK **永远 8B**（帧头 flags 无空闲位），扩展只走纯 ACK 帧。
  4. 降级损失：只有 32 位位图，乱序窗口 >32 的已收项无法告诉对端，PTO 会重复重传；块数上限也只有 16（近端 15 + 最远 1）。
- **常见错误答案**：「加密会话也能用扩展 SACK」「改个常量就能开」。

### Q11 【L2 概念】握手为什么是 16 轮 × 每轮 2 份？失败概率怎么算？

- **痛点**：觉得「重传 16 轮」是暴力堆参数，不知道它和丢包率的定量关系。
- **追查位置**：
  - `include/go2cpp/bento/net/link.hpp:99`（`hello_max_attempts = 16`）
  - `include/go2cpp/bento/net/link.hpp:103`（`handshake_copies = 2`，每轮冗余份数）
- **验收标准**：
  1. 参数：16 轮、每轮 2 份、PTO 从 200 ms 指数退避到 2 s（`user/docs/03-bento.md:91`）。
  2. 每轮 2 份独立丢失概率 `p²`；32 份全丢约 `p^32`，40% 丢包下也已可忽略。
  3. 代价：最坏握手耗时随 PTO 退避累加（约十几秒），这是 40% 丢包旧基线里「一轮 15 s 超时」的来源。
  4. 与「快速重传」的分工：握手阶段没有后续包可触发重复 ACK，只能靠冗余 + 退避。
- **常见错误答案**：「重传次数越多越好」「握手丢包靠 SACK 恢复」。

### Q12 【L2 陷阱】回调里能不能调 `Send*`？能不能睡 100 ms？

- **痛点**：把回调当普通线程函数，在里面做重活，把整条链路收包卡死。
- **追查位置**：
  - `include/go2cpp/bento/net/link.hpp:802`（回调事件：锁内入队、锁外派发，避免 `recursive_mutex`）
  - `user/docs/03-bento.md:77`（回调在 `RunOnce` 内、锁外派发；可以继续 `Send*`/`Close`；必须快速返回）
- **验收标准**：
  1. 回调在 `RunOnce` 调用线程、状态锁外执行；可以调 `Send*`/`Close`（不会自死锁）。
  2. **不能阻塞**：回调里跑长任务会阻塞整条链路的收包与定时器（ACK/PTO/心跳）。
  3. `Close`/`RemovePeer` 的销毁延迟到安全点，回调返回后才真正释放。
  4. 需要跨线程投递用 `ConcurrentSend` 或自己的队列。
- **常见错误答案**：「回调里可以随便 sleep」「回调持锁所以不能调 Send」。

---

## 三、安全 / 性能 / 边界

### Q13 【L1 概念】Bento 默认加密吗？没有 PSK 会怎样？

- **痛点**：以为默认明文，或者忽略返回值让程序「看起来能跑」但所有发送被拒。
- **追查位置**：
  - `include/go2cpp/bento/net/link.hpp:121`（`require_encryption = true` 默认）
  - `user/docs/03-bento.md:340`（无 32B PSK 时 `Open/Connect` 直接失败 `kEncryptionRequired`；监听端拒收明文 HELLO 防降级）
- **验收标准**：
  1. 默认 `require_encryption=true`；没有 32B PSK 时 `Open/Connect` 返回 `SecurityError::kEncryptionRequired`。
  2. 监听端拒收明文 HELLO，防止攻击者把连接降级成明文。
  3. 老代码忽略返回值仍然「能跑」，但所有发送都会被安全策略拒绝——这是最危险的静默失败。
  4. 测试要么两端配同一 PSK，要么显式 `require_encryption=false`。
- **常见错误答案**：「默认明文，加密是可选项」「没 PSK 会自动生成一个」。

### Q14 【L2 概念】DATAGRAM 通道的 DropStale 与 `dep_seq` 是什么关系？

- **痛点**：以为 DATAGRAM 也会重传，或者不知道 `dep_seq` 保的是什么序。
- **追查位置**：
  - `include/go2cpp/bento/net/channel.hpp:20`（`seq > applied_seq`，旧包一律 DropStale；带 `dep_seq` 的帧在依赖被越过时也丢弃）
  - `include/go2cpp/bento/net/channel.hpp:625`（`UnreliableChannel`）
- **验收标准**：
  1. DATAGRAM 不可靠、不重传：单调 `seq` 去旧，只保留最新值。
  2. 乱序缓存上限可配（0 = 关闭缓存，退回纯 DropStale，`include/go2cpp/bento/net/channel.hpp:629`）。
  3. `dep_seq` 表示「依赖的不可靠 seq」：依赖帧还没到/已被越过时，本帧按 DropStale 丢弃，保证 0 乱序的因果语义。
  4. 与 STREAM 的区别：STREAM 丢包会重传、保证有序；DATAGRAM 丢包就丢，语义是「最新值优先」。
- **常见错误答案**：「DATAGRAM 也会重传只是慢」「dep_seq 是 TCP 的序号」。

### Q15 【L3 取舍】静态 PSK 与 X25519 各解决什么？为什么 0-RTT 只给纯 PSK？

- **痛点**：以为 X25519 只是「更安全的 PSK」，不知道它和 0-RTT 互斥。
- **追查位置**：
  - `include/go2cpp/bento/net/link.hpp:136`（`enable_x25519 = true` 默认）
  - `user/docs/03-bento.md:95`（0-RTT 恢复只对纯 PSK 开放；X25519 会话不导出票据）
- **验收标准**：
  1. 静态 PSK：认证 + 加密，**无前向保密**——长期密钥泄露则历史流量可解；好处是握手简单、可 0-RTT。
  2. X25519：临时密钥交换，提供**前向保密**；代价是多一轮、且票据不能复用（否则前向保密被静默降级）。
  3. 本实现默认 `enable_x25519=true`，失败不降级（`AI_COLLAB.md:111`）。
  4. 0-RTT 票据只对纯 PSK 会话导出；X25519 会话想 0-RTT 就必须放弃前向保密，所以直接不给。
- **常见错误答案**：「X25519 更安全所以默认关掉省性能」「PSK 也有前向保密」。

### Q16 【L3 陷阱】自研 ChaCha20-Poly1305 为什么在 1 MB 上比 OpenSSL 慢 13.41×？

- **痛点**：以为「C++ 写得更紧凑就能追上」，不知道差距是 SIMD 造成的物理下限。
- **追查位置**：
  - `docs/reports/asm-tls.md:29`（256B/1KB/16KB/1MB = 1.41×/4.10×/11.93×/**13.41×**，不达标）
  - `docs/reports/asm-tls.md:190`（`grep -cE 'v[a-z0-9]+'` 统计：全文件 **0 条 SIMD 指令**）
- **验收标准**：
  1. 分档：≤64B 自研 **0.52×**（更快，因为 EVP 每次 `EncryptInit_ex` 有 ~300–500 ns 固定开销）；≥256B 被反超，1 MB 到 13.41×。
  2. 根因：自研是标量 C（`Poly1305Block` 每 16B 块 143 指令 ≈ 8.9 指令/字节），OpenSSL 是 AVX512 手写汇编（~5.1 GB/s vs 自研 ~380 MB/s）。
  3. 自研封顶 ≈380 MB/s 是 ChaCha20(830 MB/s) 与 Poly1305(700 MB/s) 串联的必然结果（`docs/reports/asm-tls.md:237-239`）。
  4. 边界：这条自研 AEAD **不在 TLS 路径上**（TLS 用 OpenSSL），只在 Bento PSK 里用；TLS 传输层 vs OpenSSL 全部 <1.1×（握手 1.070×）。
  5. 结论：不是调参能解决的，需要 AVX2/AVX512 向量化。
- **常见错误答案**：「优化一下循环就能追上」「13.41× 是因为编译选项没开 -O3」。

### Q17 【L2 概念】PMTUD 为什么默认关闭？1200/1500 这两个数怎么来的？

- **痛点**：以为 MTU 越大越好，或者不知道关闭时用的保守值。
- **追查位置**：
  - `user/docs/03-bento.md:351`（PMTUD 默认关闭；保守 MTU 1200，最大 1500，跨 IPv4/IPv6 不混用）
  - `tests/bento/PROTOCOL.md`（PMTUD 探针帧 `kPmtudProbe/kPmtudAck` 定义）
- **验收标准**：
  1. PMTUD 需要 DF 位 + 探针（`kPmtudProbe`/`kPmtudAck`），探测失败要靠超时重试，实现复杂且依赖路径行为。
  2. 默认关闭时用保守 1200（IPv6 最小 MTU 1280 减去隧道开销的安全值），最大 1500。
  3. 跨 IPv4/IPv6 不混用 MTU；STREAM 帧 >1300B 自动分片（`user/docs/03-bento.md:66`）。
  4. 不开 PMTUD 的代价：MTU 更大时无法利用，但不会因为黑洞丢包。
- **常见错误答案**：「MTU 一律 1500」「PMTUD 开着一定更好」。

### Q18 【L2 陷阱】视频通道每来一个新帧就分配几十 KB，问题在哪？

- **痛点**：只看到「能跑」，没注意每帧一次大分配 + 清零在高帧率下是纯浪费。
- **追查位置**：
  - `src/bento/net/channel.cpp:203`（`assembly.received.assign(frag_count, 0)`，每帧按分片数分配）
  - `include/go2cpp/bento/net/channel.hpp:753`（`kMaxFecGroupSize = 8`，FEC 组上限）
- **验收标准**：
  1. 现象：新 `frame_id` 到来就 `assign(frag_count × kFragmentBytes, 0)`，即使只到了 1 个分片；丢帧/乱序时 `m_active` 里可能同时存在多份大缓冲（受 `kMaxReassembly` 限制）。
  2. 代价：每帧一次几十 KB 分配 + memset，高帧率下是纯内存带宽浪费（review P1-15）。
  3. 改进方向：按需增长（收到第 i 片才 resize）、`received` 用位图、帧缓冲池化。
  4. FEC 组大小 2..8，组内分片零填充到 `max_len`；FEC 只对 VIDEO/可靠通道生效。
- **常见错误答案**：「分配一次没关系，反正会复用」。

### Q19 【L2 概念】`rx_threads > 0` 能线性扩展吞吐吗？瓶颈在哪？

- **痛点**：以为多收包线程就能多核扩展，忽略协议处理仍是单线程。
- **追查位置**：
  - `user/docs/03-bento.md:75`（`rx_threads > 0` 只把 poll+recvmmsg 摊到多核，**协议处理与回调仍在 `RunOnce` 调用线程**）
  - `user/docs/03-bento.md:347`（单线程 `RunOnce` 是吞吐上限；要再扩需按 `conn_id`/通道分片，未做）
- **验收标准**：
  1. `rx_threads` 只并行「收包系统调用」，解帧/ACK/重组/回调仍串行。
  2. 因此吞吐上限由 `RunOnce` 单线程决定；收包 syscall 省下来的时间只在多队列场景有意义。
  3. 回调线程语义不变：`on_*` 仍在 `RunOnce` 调用线程执行。
  4. 要真扩展需要按 `conn_id`/通道分片到多线程（项目未做）。
- **常见错误答案**：「rx_threads 拉满就线性扩展」「回调会跑到收包线程上」。

### Q20 【L2 概念】Link 能连多个 peer 吗？1:N 场景该用什么？

- **痛点**：想把 Link 改成多 peer，或者以为 PeerHub 只是「多个 Link 的容器」。
- **追查位置**：
  - `user/docs/03-bento.md:338`（`Link` 是单 peer；监听方只接受第一个握手成功的来源；1:N 必须用 `PeerHub`）
  - `user/docs/03-bento.md:331`（PeerHub：3 peer × 50 STREAM + 50 DATAGRAM 隔离，≈30.7 Mbit/s）
- **验收标准**：
  1. Link 单 peer 假设散落在 2000+ 行实现里；监听方只接受第一个握手成功的来源。
  2. PeerHub 是**独立组件**：一个 UDP socket、按 `conn_id` 路由、per-peer 密钥/重放窗口/通道表。
  3. 两条 UDP 代码路径并存是有意的，避免把 Link 改成多 peer 引发全量回归。
  4. 1:N 的容量参考：3 peer 各 50 STREAM + 50 DATAGRAM 隔离，307 KB / 80 ms ≈ 30.7 Mbit/s（旧口径）。
- **常见错误答案**：「Link 天然支持多 peer」「PeerHub 就是 vector<Link>」。

---

## 难度分布自检

| 难度 | 题号 | 数量 |
|---|---|---:|
| L1 | Q01 / Q05 / Q13 | 3 |
| L2 | Q11 / Q12 / Q14 / Q17 / Q18 / Q19 / Q20 | 7 |
| L3 | Q02 / Q03 / Q07 / Q08 / Q10 / Q15 / Q16 | 7 |
| L4 | Q04 / Q06 / Q09 | 3 |

> 标了「未实测」的两处：Q03 的 FEC 拐点（只有 5% 丢包 -90.3% 一个点，没有曲线）、Q04 的 0-RTT 重放
> 攻击面（协议层防线可读，但没有攻防实测）。Q09 引用旧基线时必须带 2026-10-28 限定。
