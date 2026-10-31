# 网络 / 应用层 / 缓存 / P2P「还能再优化」审查（只读，2026-11-01）

> 审查方式：**只读代码**，不修改任何实现/测试/文档文件。工作树 WSL `/UserData/CodexWorkSpace/Go2cpp`，
> 构建目录 `.build-bento-wsl`（Release）。本机**没有 `perf`**，定量结论来自：
> ① 可直接运行的既有基准（`bench_net_batch`、`go2cpp_bufio_batch_bench`、`go2cpp_cache_bench`、`p2p_bench`）；
> ② 对热路径的静态计数（遍历次数 / 加锁次数 / 堆分配次数 / 系统调用次数）；
> ③ 仓库既有报告（`2026-11-01-net-vs-go.md`、`2026-11-01-concurrency-opt-review.md`）。
> **凡属估算一律标注「估算」**，不冒充实测。
>
> 上一轮只审了并发模块；本轮覆盖网络/应用/缓存/P2P，**不重复**已落地的 `net-vs-go` 优化项。

## 0. 结论摘要

- 网络"协议层"（HTTP/WS/MQTT/CoAP 的解析与序列化）在 `net-vs-go` 轮已经打磨过一轮；本轮新发现的高价值点集中在
  **四类结构性问题**：
  1. **每包/每帧的固定税**：`Socket::begin_operation` 的 `shared_from_this` + 互斥、`P2PChannel::Recv` 每包 5-7 次加锁、
     每帧一次 `sendto`/`recvfrom`（未批量）；
  2. **每帧 O(全表)**：`StreamTable::PickNext` 每帧两遍遍历整张流表 + 父链 `map::find`；
  3. **每条目/每帧的堆分配**：`Frame::payload` 的 `assign` 拷贝、`KvTable` 每个 key/value 两次 string 分配、
     缓存策略每条目两次堆分配（list 节点 + hash 节点）；
  4. **锁粒度**：`Link::RunOnce` 用一把 `recursive_mutex` 罩住 poll+分发+Tick，发送线程被接收超时阻塞（代码注释自承）。
- 24 个点中，**7 个属「收益高、难度低-中」**，可在不改协议语义的前提下先做；
  **4 个属结构性改造**（流表调度器、帧零拷贝视图、缓存容器侵入式化、IO 定时器 wheel），收益最大但需要成体系的回归。
- 缓存模块的基准显示：LRU/CLOCK 约 31-41 Mops/s（26-32 ns/op），LRU-K 只有 9-14 Mops/s——
  差距主要来自容器（`std::list` + `unordered_map<K, list::iterator>`）的两次堆分配与指针追逐，而非算法本身。

## 1. 已有的本机实测基线（用于收益判断）

`bench_net_batch`（UDP 回环 100000 包 × 64B，5 次中位数）：

| 模式 | 系统调用 | pps | ns/packet | 相对 |
|---|---|---:|---:|---:|
| single（逐包） | 200000 | 164,343 | 6084.8 | 1.00x |
| batch-16 | 12500 | 283,663 | 3525.3 | 1.73x |
| batch-64 | 3124 | 296,163 | 3376.5 | **1.80x** |
| batch-256 | 780 | 295,054 | 3389.2 | 1.80x |

同文件其他两项：

| 场景 | 数字 |
|---|---|
| TCP 小消息逐条 `send` vs 批量 `writev(64)` | 786,614 msg/s (1271 ns/msg) → 10,342,460 msg/s (**97 ns/msg，13.15x**) |
| TCP 64 MiB 文件 `sendfile` vs `read+send` | 3802.6 MiB/s → 2163.1 MiB/s（**1.76x**） |

`go2cpp_bufio_batch_bench`（16B 消息，单连接）：

| 场景 | 吞吐 | syscalls/msg |
|---|---:|---:|
| raw-send（逐条） | 0.70 Mmsg/s | 1.20 |
| batch-64 | 12.32 Mmsg/s | 0.019 |
| batch-256 | 15.86 Mmsg/s | 0.005 |

> 结论：**"一次系统调用发一条消息" 与 "批量发" 之间是 10-20 倍量级**。凡是仍在"每包一次 syscall"的路径
> （P2P、Link、PeerHub），都是本轮最值得动的地方。

`go2cpp_cache_bench`（capacity=500，30000 ops，单线程）：

| 策略 | hotspot Mops/s | scan Mops/s | cdn Mops/s |
|---|---:|---:|---:|
| CLOCK | 35.22 | 35.01 | 25.42 |
| LRU | 31.22 | 33.53 | 24.76 |
| ARC | 27.00 | 24.18 | 12.61 |
| 2Q | 19.68 | 18.90 | 11.88 |
| GDSF | 17.72 | 20.21 | 16.06 |
| W-TinyLFU | 15.18 | 14.95 | 11.98 |
| LRU-K(2) | **9.02** | **10.25** | **10.02** |

## 2. 优化清单（按优先级 = 收益/难度 排序）

评级口径：收益 = 高/中/低（对热路径单次操作的影响）；难度 = 低/中/高（改动面 + 回归风险）。

### P0-1　`Link::RunOnce` 用一把 `recursive_mutex` 罩住「poll + 全部分发 + Tick」，发送线程被接收超时阻塞

- **位置**：`src/bento/net/link.cpp:703-736`（`RunOnce`）、`include/go2cpp/bento/net/link.hpp:625`（`mutable std::recursive_mutex m_mutex`）；
  发送入口同一把锁：`src/bento/net/link.cpp:1895`（`TrySendStream`）、`:1919`（`SendDatagram`）、`:1932`（`SendVideoFrame`）等 24 处。
- **现状**：`RunOnce` 的第一行就是 `std::lock_guard<std::recursive_mutex> lock(m_mutex);`，然后依次执行
  `PollPathMtuErrors → WaitReadable(timeout_ms) → 循环 RecvFrom + HandleFrame（解密/ACK/重组/回调）→ PollPathMtuErrors → Tick`。
  代码注释自己写明："发送线程最多被阻塞一个 timeout_ms；测试通常传 1ms，生产可传 0"。
  即：**一条链路上，接收与发送完全串行**（发送入口 `TrySendStream`/`SendDatagram` 与接收循环抢同一把锁），
  `HandleFrame` 内部还会回调用户（`CallbackDepthGuard`），回调期间锁不释放（递归锁），进一步拉长临界区。
  同一条路径上还有一处每帧分配：`SendBuiltFrame`（`link.cpp:1164-1200`）在需要写 `conn_id`/`kFlagResume` 时
  `stamped.assign(data, data + bytes)`（一次堆分配 + 整帧拷贝），可用成员 scratch 复用。
- **建议**：
  1. 拆两把锁：`m_rx_mutex`（只保护链路会话状态：密钥、重放窗口、通道表）与 `m_tx_mutex`（发送队列）；
  2. 发送改为「单写者 + 无锁 MPSC 队列」：`Send` 只入队并 `eventfd` 唤醒，由接收线程在 `RunOnce` 尾部批量 drain；
  3. 统计计数器（`m_stats`）改原子或按线程累加后合并（见 P0-2 同思路）。
- **收益**：高。消除"发送等接收 poll"的固定延迟（生产 `timeout_ms=0` 时也不再有跨线程争锁）；
  对单链路双向流（视频/文件传输）是直接吞吐收益。估算 P99 延迟下降 >30%，双向吞吐提升 1.2-1.5x（估算）。
- **难度**：中（要保证 Tick/重传在单写者模型下的顺序）。
- **风险**：中。链路状态（密钥、重放窗口）跨线程访问必须重新论证；现有 `recursive_mutex` 也承担了"重入安全"，
  拆分后需覆盖"回调里再次 Send"的场景（否则死锁或乱序）。

### P0-2　`Socket::begin_operation` 让**每次** recv/send 都付 `shared_from_this()` + 互斥锁

- **位置**：`src/network/socket.cpp:212-228`（`begin_operation`）、`include/go2cpp/network/socket.hpp:219-221`；
  调用点：`socket.cpp` 的 `sendv`/`recvv`/`send`/`recv` 每次进入都调 `begin_operation`，退出调 `end_operation`。
- **现状**：
  ```cpp
  bool Socket::begin_operation(int& fd) noexcept {
      std::shared_ptr<Socket> keep_alive;
      keep_alive = shared_from_this();      // 弱引用升强：一次原子 CAS
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_closing || m_fd < 0) { errno = EBADF; return false; }
      if (m_inflight == 0) m_inflight_keepalive = std::move(keep_alive);
      fd = m_fd; ++m_inflight; return true;
  }
  ```
  即**每次系统调用前**：1 次 `shared_from_this`（原子 RMW）+ 1 次互斥加解锁 + 1 次 `shared_ptr` 析构（另一次原子 RMW）。
  `end_operation` 在归零时还要再取锁。对 64B 小包来说，这是 IO 路径上最大的一块"非内核"固定税。
- **建议**：
  1. in-flight 用 `std::atomic<std::uint32_t> m_inflight` + 关闭路径 futex 等待（不需要每操作加锁）；
  2. keep-alive 改「关闭时登记到延迟销毁列表」：`close()` 先把 fd 置 -1 并把自己挂到 manager 的销毁队列，
     由最后一次 `fetch_sub` 归零者负责回收（避免每操作 `shared_from_this`）；
  3. 为已知生命周期由调用方保证的场景（Link/PeerHub 持有成员 Socket）提供 `recv_nolock`/`send_nolock` 变体，
     跳过 keep-alive，仅做 fd 代际校验；
  4. `GO2CPP_SOCKET_DEBUG_RECORD` 已是宏开关，确认关闭时零成本。
- **收益**：高。每次 IO 省 2 次原子 RMW + 1 次互斥（无争用 ~15-25 ns，估算）；64B 消息单包往返可省 3-5%（估算）。
- **难度**：中（关闭与在途操作的竞态是最容易出 UAF 的地方）。
- **风险**：中高。`Socket` 的生命周期语义（`close()` 可并发、fd 代际）必须逐条重新论证，
  建议先在 `Link`/`PeerHub` 的"参数 socket"上试点，再推广。

### P0-3　`StreamTable::PickNext` 每帧遍历**两遍**整张流表，并对每个候选走父链 `map::find`

- **位置**：`src/app/stream_table.cpp:460-500`（`PickNext`）、`:476-495`（`HasReadyAncestor`）、
  `include/go2cpp/app/stream_table.hpp:228`（`StreamMap = std::map<std::uint32_t, std::unique_ptr<Stream>>`）；
  唯一热调用点：`src/app/connection.cpp:515`（`FlushLocked` 的 `while (budget > 0)` 循环，**每帧调一次**）。
- **现状**：
  ```cpp
  Stream* StreamTable::PickNext(std::uint32_t max_payload) {
      for (auto& item : m_streams) { ... FillPending(stream, max_payload); }   // 第一遍：全表
      for (auto& item : m_streams) {                                            // 第二遍：全表
          if (... !IsReady(stream)) continue;
          if (HasReadyAncestor(stream)) continue;                               // 每个候选走父链 find
          if (stream.finish < best->finish || ...) best = &stream;
      }
  }
  ```
  `HasReadyAncestor` 沿 `parent_id` 链每跳一次 `m_streams.find(parent)`（O(log N) 红黑树 + 节点指针追逐）。
  默认 `max_concurrent_streams = 256`：每帧约 **512 次 map 节点迭代 + 最多 256×深度 次 find**。
  估算每帧 3-15 µs 的纯调度开销（估算，取决于缓存命中），而帧本身可能只有几百字节。
- **建议**：
  1. **single-pass**：去掉第一遍，`FillPending` 只在流"变为可发送"时（生产者有数据/窗口更新/父流关闭）触发；
  2. 维护 **ready 集合**：按 `finish` 的最小堆 + 每个流一个 `ready_children` 计数（父只有全部 ready 子发完才可发），
     `PickNext` 取堆顶 O(1)，`ChargeFinish` 后只调整自己的堆位置；
  3. 若保守：至少把 `StreamMap` 换成 flat map（开放寻址 + 连续 `Stream` 存储，去掉 `unique_ptr` 的第二次分配），
     把 `find` 的指针追逐消掉。
- **收益**：高。每帧调度从 O(N) 降到 O(log N)，256 流时估算省 80%+ 的调度时间。
- **难度**：中（堆 + 依赖计数要处理 Open/Close/Reset/Prune 的所有边）。
- **风险**：中。现有 335 条 `app_test_stream_table` 断言里有 WFQ 权重分配、父优先、窗口阻塞等精确期望，
  任何遍历顺序变化都可能改变 `finish` 的平局裁决顺序，需要逐条对齐。

### P0-4　`P2PChannel::Send/Recv` 每包 5-7 次互斥锁 + 每包一次 `MakeFrame` 堆分配

- **位置**：`src/p2p/p2p_channel.cpp:355-390`（`Send`）、`:392-470`（`Recv`）；
  锁点：`:367`（取 peer）、`:382`（stats）、`:396`（入口状态）、`PeerTimedOut()`（`:573`）、`IsRelayed()`（`:475`）、
  `TouchPeer()`（`:562`）、`UpdateRtt()`（`:641`）、`UpdateSequenceGapLocked`（`:412`）。
- **现状**：`Recv` 每收到**一个数据报**，依次：入口 1 次锁 → `MaybeSendKeepalive` → `PeerTimedOut()`（锁）→
  `IsRelayed()`（锁）→ 收包 → `TouchPeer()`（锁）→ 解析 → stats 更新（锁）→ `UpdateSequenceGapLocked`（锁）。
  即每个包 **5-7 次 mutex**，且这些锁全部是同一把 `m_mutex`（非递归），高频小包时是纯争用点。
  另外 `Send` 每包 `MakeFrame(...)` 返回 `std::vector<std::uint8_t>`（一次堆分配 + 一次载荷 memcpy），
  keepalive ACK 同理（`:452`）。
- **建议**：
  1. 状态快照原子化：`state`/`relayed`/`peer` 打包成一个 `std::atomic<std::uint64_t>` 或 seqlock，
     读路径无锁；`last_receive` 用 `atomic<uint64_t>`；
  2. 统计改为 per-instance 原子（`fetch_add(relaxed)`）或"线程本地累加 + 定期 flush"，不要在每包里加锁；
  3. `MakeFrame` 改为复用成员 `m_tx_frame_scratch`（`std::vector<uint8_t>` 预留 `kP2PMaxPayload + 头`），
     每包只 `resize` + 写头 + memcpy 载荷；
  4. 收方向的 `Recv` 可一次 `recvmmsg` 拿多个包后批量更新统计（与 P1-11 联动）。
- **收益**：高。每包锁数 5-7 → 0-1，估算小包吞吐提升 20-40%（估算）。
- **难度**：低-中（状态机改动小，主要是可见性/内存序）。
- **风险**：低-中。`state` 与 `peer` 必须成对读（否则可能拿到"已连接但 peer 为旧值"），建议用 seqlock 或版本号。

### P0-5　`KvTable` 用有序 `vector` 线性查找，且每个 entry 两次 `std::string` 分配

- **位置**：`src/app/kv.cpp:49-56`（`Find`）、`:63-72`（`Set`）、`:74-84`（`Erase`）；
  `include/go2cpp/app/kv.hpp:149`（`std::vector<Entry> m_entries`，`Entry = pair<string,string>`）。
- **现状**：
  ```cpp
  const std::string* KvTable::Find(std::string_view key) const noexcept {
      for (const Entry& entry : m_entries) { if (entry.first == key) return &entry.second; }
      return nullptr;
  }
  ```
  HEADERS 帧的入口 `Connection::HandleHeaders`（`src/app/connection.cpp:618`）先 `KvTable::ParseBinary`
  （每个 entry 构造 2 个 string → 一次 HEADERS 帧 N×2 次分配），随后多次 `table.Get(kAppKindKey)` 等
  线性查找；资源元数据（`ResourceMeta::FromKv`，`src/app/resource.cpp:168-240`）一次要查 5-6 个 key。
- **建议**：
  1. `Entry` 里缓存 64-bit key hash（`Set` 时算一次），`Find` 先比 hash（一次整数比较）再比字符串；
  2. 条目数 >8 时惰性建 `open addressing` 索引（`std::array<uint16_t, 32>` 内联，超了才堆分配）；
  3. `ParseBinary` 可直接用 `string_view` 视图（生命周期 = 解码输入缓冲），避免每个 value 拷贝；
     需要持久化的调用方显式 `Materialize()`。
- **收益**：中高。HEADERS 帧解析省 N×2 次分配 + 查找从 O(N) 字符串比较降到 O(1) hash 比较（估算 2-4x）。
- **难度**：低（不改变对外 API 语义，`entries()` 顺序仍保持）。
- **风险**：低。唯一风险是 `string_view` 模式下的悬垂（调用方持有超出缓冲生命周期的表），
  建议默认仍拷贝，只在 `ParseBinaryView` 这种显式 API 下返回视图。

### P0-6　每帧 `payload.assign` 一次堆分配 + 一次整帧拷贝；`bento_bridge` 甚至编码两遍

- **位置**：`src/app/frame.cpp:149`（`out->payload.assign(data + kFrameHeaderBytes, data + total)`）、
  `src/app/connection.cpp:1473`（`frame.payload.assign(data, data + size)`）、
  `src/app/bento_bridge.cpp:39-51`（`Encode`：先 `EncodeFrame` 分配 A，再 `bento::net::Encode` 分配 B 并 memcpy A）。
- **现状**：`Frame` 的 `payload` 是 `std::vector<std::uint8_t>`（`include/go2cpp/app/frame.hpp:158`）。
  每个入站帧解码都要新分配并拷贝；出站 `EncodeFrame` 再分配一次；桥接到 Bento 帧时**再分配 + 再拷贝一次**。
  对 256KB 的 DATA 帧，等于每帧 2 次内存带宽占用。
- **建议**：
  1. `Frame` 增加"视图模式"：`std::span<const std::uint8_t> payload_view` + `std::vector` 二选一，
     解码时指向连接接收缓冲（缓冲按代际回收，引用计数块）；
  2. 出站 `EncodeFrame(frame, &out)` 复用连接级 `m_tx_scratch`（现有 `AppendEncodedFrame` 已是这个方向，推广到热路径）；
  3. `bento_bridge::Encode` 先算 `kFrameHeaderBytes + payload` 再加 Bento 头，**一次分配直接写两层头**。
- **收益**：中高。每帧省 1-2 次堆分配 + 1-2 次整帧 memcpy（大帧场景直接省一半内存带宽）。
- **难度**：中（`Frame` 语义扩散到很多测试与调用点）。
- **风险**：中。视图生命周期是硬约束，必须有明确的"下一帧到达前有效"契约 + debug 下的失效毒化。

### P1-7　7 个缓存策略都用 `std::list + unordered_map<K, list::iterator>`，每条目两次堆分配 + 两次指针追逐

- **位置**：`include/go2cpp/cache/lru.hpp:38-163`、`lru_k.hpp:57-120`、`two_queue.hpp`、`arc.hpp:271-276`、
  `tiny_lfu.hpp`、`gdsf.hpp`、`clock.hpp`；统一模式见 `lru.hpp`：
  ```cpp
  std::list<Entry> lru_;                                             // 每条目一次 list 节点分配
  std::unordered_map<K, typename List::iterator, Hash, Equal> index_; // 每条目一次 hash 节点分配
  ```
- **现状**：`Put` 一次 = 1 次 list 节点分配 + 1 次 unordered_map 节点分配（rehash 时还要整表搬移）；
  `Get` 命中 = 1 次 hash 查找（指针追逐到 bucket 链）+ 1 次 `splice`（4 次指针写）。
  基准（`go2cpp_cache_bench`）：LRU 31-39 Mops/s，即 26-32 ns/op，其中相当一部分是这两次分配与两次缓存缺失。
- **建议**：
  1. 改**侵入式链表**：`Entry` 内嵌 `prev/next`，用 `std::vector<Entry>` 槽位池 + freelist，`Put` 只从池里取（摊销 O(1)，无每次分配）；
  2. 索引改**开放寻址 flat hash**（power-of-two 容量、线性探测、存 `uint32` 槽位下标），一次 cache line 内完成探测；
  3. 对 ARC/2Q/TinyLFU 的多队列结构，共享同一个槽位池（每条目只分配一次）。
- **收益**：中。估算吞吐 1.3-1.8x（业界同类改造的常见区间）；内存峰值也会下降（省掉 list 节点与 hash 节点各 16-24B）。
- **难度**：中（7 个策略都要改，但模式统一，可先改 LRU 做样板）。
- **风险**：低。缓存模块有 `cache_bench` 的命中率/恢复对拍 + 随机对拍，改容器不改算法语义，回归面可控。

### P1-8　LRU-K 的 history/corral 双表最慢（9-14 Mops/s），每次命中可能跨表搬移

- **位置**：`include/go2cpp/cache/lru_k.hpp:57-120`（构造与 `Get`）、`:196-223`（`MainEntry`/`HistoryEntry` 双 list + 双 map）。
- **现状**：LRU-K(2) 维护 `history_`（corral，容量 = capacity）与 `main_` 两张 list + 两张 map；
  命中时若历史访问次数达标就 `PromoteToMain`（从 history 摘节点、插 main、更新两个索引）。
  基准 hotspot 9.02 Mops/s（约为 LRU 的 1/3.5），但命中率 0.78 vs LRU 0.55——**这是算法换来的命中率，不全是实现问题**。
- **建议**：
  1. 惰性提升：命中只更新 `visit_count`，把"提升"延迟到淘汰时批量做（减少每次 Get 的跨表搬移）；
  2. history 用固定容量环形数组 + hash 索引（corral 本来就是"只保留最近 K 次访问"，天然适合环形）；
  3. 在文档/基准里明确"LRU-K 用吞吐换命中率"，避免调用方误用在高 QPS 路径。
- **收益**：中（估算 1.3-1.5x，且不牺牲命中率）。
- **难度**：中（提升时机变化会影响命中率，需要对拍验证）。
- **风险**：中。命中率可能小幅波动，必须用 `go2cpp_cache_bench` 的 4 个 workload 同时验证。

### P1-9　`SessionTable::Find` 返回 `optional<SessionEntry>`，每次查询拷贝 string + vector

- **位置**：`src/bento/net/session_table.cpp:250-262`；结构见 `include/go2cpp/bento/net/session_table.hpp:98-115`
  （`std::string key` + `std::vector<SessionClient> clients` + 4 个 uint64）。
- **现状**：所有查询（`Find`/`Contains`/`Remove` 相关路径）都在全局 `m_mutex` 内把整个 entry 拷出来，
  包括邀请密钥字符串与 clients 数组；`Contains` 甚至只需要一个 bool。
- **建议**：
  1. 增加 `const SessionEntry* FindPtr(...)`（锁内返回指针，调用方在同一临界区内用完）；
  2. 或返回 `std::shared_ptr<const SessionEntry>`（entry 内部改 shared_ptr，锁外安全）；
  3. `Contains` 走 `m_entries.count()`，不构造 optional。
- **收益**：中。每次查询省 1 次 string + 1 次 vector 分配拷贝（clients 多时更明显）。
- **难度**：低（API 增量，不改现有语义）。
- **风险**：低。指针版必须写清"锁外不得持有"。

### P1-10　`g_session_cache` 用全局 `std::map`，且**每次握手**都全表 `PurgeExpiredSessions`

- **位置**：`src/bento/net/link.cpp:147-156`（`std::mutex g_session_cache_mutex; std::map<std::uint32_t, CachedSession> g_session_cache;`
  + `PurgeExpiredSessions` 全表遍历）、调用点 `:1804-1806`（保存会话）、`:1817-1819`（Resume 查询）、`:1876-1878`、`:1889-1890`。
- **现状**：`CachedSession` 很大（2×nonce + `ReplayWindow` + 指纹），存进 `std::map` 时按值拷贝；
  每次 Save/Resume 都先 `PurgeExpiredSessions(NowMs())` 线性扫全表——O(会话数)，且持有全局锁。
- **建议**：
  1. `std::unordered_map<uint32_t, CachedSession>`（key 就是 uint32，天然适合哈希）；
  2. 过期改**惰性**：查询时只检查自己那条的 `expires_ms`；另设低频（如每 1024 次或每秒一次）的批量清理；
  3. 值改 `std::unique_ptr<CachedSession>` 或 emplace 原地构造，避免 resume 路径的整结构拷贝。
- **收益**：中低（会话数少时低，万级 0-RTT 重连时明显）。
- **难度**：低。
- **风险**：低。清理时机变化不影响正确性（查询已有 expires 校验）。

### P1-11　`PeerHub` 每个控制帧都 `BuildFrame` 新分配 vector，且每包一次 `sendto`

- **位置**：`src/bento/net/peer_hub.cpp:523-540`（`SendControl` 里 `const std::vector<std::uint8_t> frame = BuildFrame(stamped, nullptr, payload, bytes);`）、
  `:487-500`（`SendRawPacket` 每次 `m_socket.SendTo`）、`:501-521`（`SealAndSend` 已用 `m_tx_sealed_scratch`，是正确样板）。
- **现状**：ACK/心跳/MAX_DATA/BARRIER 每个控制帧一次堆分配 + 一次载荷拷贝；`SendRawPacket` 每帧一次系统调用。
  ACK 是高频帧（每个数据帧可能触发一次 ACK），这条路径的分配量不小。
- **建议**：
  1. `SendControl` 复用成员 `m_tx_frame_scratch`（照 `SealAndSend` 的模式），`BuildFrameInto(header, payload, bytes, &scratch)`；
  2. 同一轮 `RunOnce` 内要发的多个帧（ACK + 数据）用 `sendmmsg` 批量（`Socket::send_batch` 已存在，
     `bench_net_batch` 显示 batch-64 = 1.80x）；
  3. `SendRawPacket` 的统计累加合并到调用方，避免每帧多次写 `m_stats`。
- **收益**：中。省每控制帧一次分配；批量后小包吞吐 1.5-1.8x（估算，参考基准）。
- **难度**：低-中（批量发送要处理部分成功）。
- **风险**：低。ACK 丢失语义不变（批量失败时按单帧重试）。

### P1-12　`Link` 每帧一次 `recvfrom` + 每帧一次 `sendto`，未利用 `recvmmsg`/`sendmmsg`

- **位置**：`src/bento/net/link.cpp:714-736`（`RunOnce` 接收循环里每帧一次 `m_socket.RecvFrom`）、
  `:1205-1215`（`SendRawPacket` → `m_socket.SendTo`）。
- **现状**：接收侧虽然一次 `WaitReadable` 后循环最多 `kMaxFramesPerRun` 帧，但每帧仍一次 `recvfrom`；
  发送侧每个帧一次 `sendto`（`SendBuiltFrame` → `SendRawPacket`）。链路层的帧很小（头 + 小载荷），
  syscall 开销占比高。
- **建议**：
  1. 接收：`recvmmsg` 一次收 ≤64 帧到预分配的 iovec 数组，再逐帧 `HandleFrame`（`Socket` 已有 `recv_batch`）；
  2. 发送：把"本 tick 待发的帧"聚合到 `sendmmsg`（每帧一个 `mmsghdr`），失败时按帧回退；
  3. 注意 `Link` 的可靠通道有"每帧一次 `SendNow`"逻辑（`channel.cpp:455-470`），批量要放在通道之上的汇聚点；
  4. `SendBuiltFrame`（`link.cpp:1178-1190`）写 `conn_id`/`kFlagResume` 时的 `stamped.assign` 改用成员 scratch，
     省掉每帧一次分配（与批 1 的其他"复用 scratch"改动同模式）。
- **收益**：中高。基准：UDP 批量 1.80x、TCP writev 13.15x；小帧场景估算 1.3-1.8x。
- **难度**：中（批量发送的部分失败/重传语义）。
- **风险**：中。`sendmmsg` 部分成功需要精确回写每帧状态，否则重传会重复/漏发。

### P1-13　`ResourceProducer` 分块传输是"读进临时 chunk → 再拷进帧 payload"的双缓冲双拷贝

- **位置**：`src/app/resource.cpp:810-865`（`LoadChunk` 的 `m_chunk.assign(bytes, 0)` + `ReadFull`；
  `Produce` 里 `out->payload.insert(..., m_chunk.begin()+offset, ...)`）。
- **现状**：每个资源块（默认 256KB 级）先在 `m_chunk` 里分配 + 清零 + 读入，再整块拷进 `Frame::payload`；
  即每块 1 次分配 + 1 次清零（`assign`）+ 2 次内存写（读 + 拷）。大文件传输时这是纯带宽浪费。
- **建议**：
  1. `Frame` 支持"外部载荷视图"（与 P0-6 同一机制）：payload 直接指向 `m_chunk`，`Produce` 只移动 offset；
  2. 或 `ReadFull` 直接读进 `out->payload` 的预留区（先 `resize(kResourceChunkHeaderBytes + take)`，再读进 `data()+header`）；
  3. `Crc32Of` 可在读入时增量计算，省一次遍历。
- **收益**：中。大文件传输（64 MiB+）估算省 1/3 内存带宽。
- **难度**：低-中（依赖 P0-6 的视图机制；不依赖时也可直接读进 payload）。
- **风险**：低。CRC/分块偏移语义不变。

### P1-14　IO 定时器：每个带 deadline 的等待插两个容器节点（2 次分配）+ 全局 `m_timer_mutex`

- **位置**：`src/network/io.cpp:980-995`（`m_deadline_order.emplace(...)` + `m_deadline_index.emplace(...)`）、
  `:1621-1660`（`expire_timers` 每轮循环取一次 `m_timer_mutex`，且每轮 `Clock::now()`）。
- **现状**：`DeadlineOrder = std::multimap<TimePoint, uint64_t>` + `std::unordered_map<uint64_t, TimerRecord>`：
  每个带超时的 IO 等待要 **2 次节点分配**，插入/删除都持全局定时器锁。
  `expire_timers` 每次循环加锁、取时钟、删两个容器——到期批次多时锁往返次数 = 到期数。
- **建议**：
  1. 分层 **timer wheel**（如 4 层 × 64 槽，O(1) 插入、批量到期），节点内嵌到 `WaitNode`（省掉两次分配）；
  2. 若保留 multimap：把 `TimerRecord` 内嵌进 `WaitNode`，`m_deadline_index` 只是 `unordered_map<uint64, WaitNode*>`；
  3. `expire_timers` 一次加锁取出整批到期节点（而不是每条一次锁），锁外批量 claim。
- **收益**：中。有超时的 IO（几乎所有网络等待都带 deadline）每请求省 2 次分配 + 多次锁往返。
- **难度**：中（wheel 的级联/取消语义要仔细处理）。
- **风险**：中。定时器精度与取消竞态是既有测试重点，需要保留"claim 后校验 outcome"的双重检查。

### P1-15　`VideoChannel` 每帧重组先 `assign(frag_count × kFragmentBytes, 0)`，一次大分配 + 清零

- **位置**：`src/bento/net/channel.cpp:1549-1558`（新帧 `reassembly.data.assign(frag_count * kFragmentBytes, 0)` +
  `received.assign(frag_count, 0)`）、`:1630-1639`（可靠通道同一逻辑）、`:1791`（`DropReassembly`）。
- **现状**：每来一个新视频帧（frame_id 变化）就分配 `frag_count × 1200B` 的缓冲并清零，即使该帧只到了 1 个分片；
  丢帧/乱序时 `m_active` 里可能同时存在多份这种大缓冲（`kMaxReassembly` 上限）。
- **建议**：
  1. 按需增长：收到第 i 个分片时 `resize` 到 `(i+1)*kFragmentBytes`（或用 chunk 链表，只在完整时拼）；
  2. `received` 用 `std::vector<bool>`/位图代替 `vector<uint8_t>`（frag_count 通常 <64）；
  3. 帧缓冲池化（复用刚交付的缓冲）。
- **收益**：中（视频路径；每帧省一次几十 KB 分配 + memset）。
- **难度**：低。
- **风险**：低。重组语义（缺片检测、超时丢弃）不变。

### P1-16　`bento_bridge::Encode` 双重编码：先 `EncodeFrame` 分配，再 Bento `Encode` 分配并整体拷贝

- **位置**：`src/app/bento_bridge.cpp:39-51`。
- **现状**：
  ```cpp
  std::vector<std::uint8_t> app_bytes = EncodeFrame(frame);   // 分配 A + 写 app 帧
  BentoFrame bento; bento.payload = std::move(app_bytes);     // 移动（无拷贝）
  return go2cpp::bento::net::Encode(bento, Format());         // 分配 B + 把 A 整体 memcpy 进 B
  ```
  每帧 2 次分配 + 1 次整帧拷贝。
- **建议**：先算 `kAppHeader + payload` 与 `kBentoHeader` 总长，**一次分配**，先写 Bento 头、再原地写 app 帧头与载荷
  （`AppendEncodedFrame(frame, &out)` 已存在，可复用）。
- **收益**：中（每帧省一次分配 + 一次整帧拷贝）。
- **难度**：低。
- **风险**：低。编码结果字节完全一致（可用 `app_test_frame` 的 round-trip 校验）。

### P2-17　`MemoryTransport` 每 64KB 一块新 `vector`，写端拷入、读端再 `std::copy` 拷出

- **位置**：`src/app/transport.cpp:59-90`（`WriteAll` 里 `m_outbox->chunks.emplace_back(data + offset, data + offset + chunk)`）、
  `:107-140`（`Read` 里 `std::copy(chunk.begin()+head_offset, ..., data+copied)`）。
- **现状**：进程内配对传输每 64KB 一次堆分配 + 一次拷贝；读时再拷一次。加上 `std::mutex` + `condition_variable`
  每次读写各一次加锁与通知（`notify_all` 是广播，读端/写端互相唤醒有惊群）。
- **建议**：
  1. 块池化（固定 64KB slab，引用计数归还），或直接存 `std::shared_ptr<const std::vector>` 让读端零拷贝 view；
  2. `notify_all` → 按方向分成两个 cv（读通知、写通知），避免无谓唤醒；
  3. 背压分支的 `wait_for(100ms)` 改为在关闭时 notify（已有 cv，去掉定时轮询）。
- **收益**：中低（仅进程内传输，主要用于 app 层测试/本地桥接）。
- **难度**：中（引用计数与关闭竞态）。
- **风险**：低。

### P2-18　MQTT 订阅匹配每次 publish 线性扫全部订阅 + 逐段字符串比较

- **位置**：`src/bento/net/mqtt.cpp:184-195`（`BestSubscriptionQos` 遍历 `session.subscriptions()`）、
  `:326-370`（`TopicMatchesFilter` 逐段 `find('/')` + `compare`）。
- **现状**：每条 PUBLISH 对每个订阅做一次通配符匹配（O(订阅数 × 段数)），全部字符串操作在热路径上。
- **建议**：按 `/` 分层的 topic trie（节点存 literal 子节点与 `+`/`#` 指针），publish 时一次下行匹配；
  订阅数少（<8）时保留线性快路径。
- **收益**：中低（订阅多、消息多时才明显）。
- **难度**：中（MQTT 通配符语义：`+` 单层、`#` 多层、`$` 前缀例外，要逐条对齐现状）。
- **风险**：中（通配符语义错会静默丢消息，需保留现有 `TopicMatchesFilter` 作为对照测试）。

### P2-19　`CoapMessage::Encode` 每次整份拷贝 options vector 再排序

- **位置**：`src/bento/net/coap.cpp:384`（`std::vector<CoapOption> options = message.options;` 之后排序/编码）。
- **现状**：CoAP 要求 option 按 number 升序编码，实现每次编码都复制整个 options 数组再 `std::sort`（每个 option 含 `vector<uint8_t>` value，拷贝是深拷贝）。
- **建议**：改为 `std::vector<const CoapOption*>` 排序（只排指针）；或 `AddOption` 时按 number 有序插入（option 数很少，插入成本可忽略）。
- **收益**：低中（每消息省 N 次深拷贝）。
- **难度**：低。
- **风险**：低。

### P2-20　`Socket::send/recv` 单 iovec 也走 `sendmsg/recvmsg`

- **位置**：`src/network/socket.cpp:749-755`（`send` 构造 1 元素 iovec 调 `sendv`，`recv` 同理调 `recvv`）。
- **现状**：单缓冲区场景（绝大多数）也走 `sendmsg/recvmsg`，内核要多解析一次 `msghdr`；`sendmsg` 还带 `MSG_NOSIGNAL` 之外的固定开销。
- **建议**：`count == 1` 时直接 `::send/::recv`（同样带 `MSG_NOSIGNAL`）；
  或提供 `send_fast/recv_fast` 供内部热路径（P2P/Link）使用。
- **收益**：低（每次 syscall 省 ~20-50ns，估算）。
- **难度**：低。
- **风险**：极低（语义等价）。

### P2-21　`Peer` 用 `std::map<uint8_t, unique_ptr<...>>` 存通道，每帧两次 map 查找

- **位置**：`src/bento/net/peer_hub.cpp:166-167`（`std::map<std::uint8_t, std::unique_ptr<ReliableChannel>> reliable;`
  + `unreliable`），查找点 `:862`、`:1043`、`:1118`、`:1142`。
- **现状**：channel 是 `uint8_t`（≤256），却用红黑树；每帧至少 1 次 `find`（收/发各一次）。
- **建议**：`std::array<std::unique_ptr<ReliableChannel>, 256>`（未用槽位为空），O(1) 无指针追逐；
  或 `std::array<ReliableChannel*, 256>` + 惰性构造。
- **收益**：低中（每帧省 1-2 次树查找）。
- **难度**：低。
- **风险**：低。

### P2-22　`OmniNetServer` 每连接一把 `ConnectionState::mutex`，泵/入队/统计都加锁

- **位置**：`src/bento/net/omni_server.cpp:276-280`（`ConnectionState::mutex` 保护 transport/paused/stats）、
  `:351-352`（`table_mutex` + `unordered_map<uint64, shared_ptr<ConnectionState>>`）、`:387`、`:524`、`:832`。
- **现状**：每个连接的每次读写、每次统计快照都要 `state->mutex`；连接表查找要 `table_mutex`。
- **建议**：`stats` 与 `paused` 改原子；出站队列改无锁 MPSC（单消费者泵）；`table_mutex` 改读多写少的
  `shared_mutex` 或分段。
- **收益**：中低（连接数多、每连接流量小时，锁本身比数据大）。
- **难度**：中。
- **风险**：中（暂停/恢复读的语义依赖锁序）。

### P2-23　数据布局：`Peer` 与 `Stream` 的热字段与冷字段混排，轮询要跨多个 cache line

- **位置**：`src/bento/net/peer_hub.cpp:150-210`（`Peer`：state/conn_id/last_rx 与
  `psk[32] + client_nonce[32] + server_nonce[32] + tx_key[32] + rx_key[32] + x25519_private[32] + cached_hello(vector) + cached_hello_ack(vector)`
  混排，单 peer 约 600B+）；`include/go2cpp/app/stream_table.hpp:159-206`（`Stream`：热字段 `state/finish/send_window`
  与冷字段 `opened_at_ms/closed_at_ms/bytes_sent/bytes_received` 混排，且 `std::optional<Frame>` 内联在结构里）。
- **现状**：`PeerHub` 每次 tick 遍历所有 peer 做超时/心跳检查（`PeerTimedOut`/`MaybeHeartbeat`），
  每个 peer 要触碰多个 cache line（热字段分散）；`StreamTable` 每帧遍历流表时同样。
- **建议**：热字段（state/conn_id/last_rx/last_tx/next_*_ms）打包到前 64B；密钥/nonce/缓存向量等冷字段拆到
  `PeerSecrets` 旁路结构（只在握手/加解密时访问）；`Stream` 同理（`finish/state/window` 前置，统计字段另存）。
- **收益**：中（万级 peer / 256 流时，轮询的 cache miss 显著下降）。
- **难度**：低-中（纯结构重排，但 `Peer` 被大量内联代码引用，需要机械修改）。
- **风险**：低（无逻辑变化，编译期即可验证）。

### P2-24　P2P 打洞：每个候选一次 `sendto` 串行发，`pending` 用 vector 线性查

- **位置**：`src/p2p/p2p_channel.cpp:752-805`（`TryDirect`：`for (const Endpoint& target : targets) SendCheckDirect(target, &pending);`
  每 interval 对每个候选发一次 STUN，逐个 syscall）、`:826-880`（`TryRelay` 同理）；
  `pending` 是 `std::vector<std::array<uint8_t, 12>>`，`IsPendingTransaction` 线性查找。
- **现状**：候选 N 个时每轮 N 次 `sendto`；`pending` 最多 `kMaxPendingChecks`，每收到一个 response 线性扫一遍。
  打洞本身是"尽快把所有候选都打一遍"的延迟敏感路径，串行 syscall 会拉长首包时间。
- **建议**：
  1. 所有候选的 check 用**一次 `sendmmsg`** 发出（N 个 `mmsghdr`，同一 socket）；
  2. `pending` 改固定容量环形数组 / 按 transaction_id 哈希的位图，查找 O(1)；
  3. 重试间隔用单一 `next_check`（现状已是），但可以把"发送"和"接收"放到同一次 poll 轮次里（现状是 send 后立刻 recv）。
- **收益**：中（候选多、NAT 严格时，首包时间从 N×syscall 降到 1×syscall）。
- **难度**：中。
- **风险**：低（STUN 事务语义不变，失败的 mmsghdr 单独重试）。

## 3. 收益/难度矩阵

| # | 点 | 收益 | 难度 | 优先级 |
|---|---|---|---|---|
| P0-1 | Link 递归锁罩接收循环 | 高 | 中 | ★★★★★ |
| P0-2 | Socket 每次 IO 的 shared_from_this + 锁 | 高 | 中 | ★★★★★ |
| P0-3 | 流表 PickNext 每帧两遍全表 | 高 | 中 | ★★★★★ |
| P0-4 | P2P 每包 5-7 次锁 + 每包分配 | 高 | 低-中 | ★★★★★ |
| P0-5 | KvTable 线性查找 + 每 entry 两次分配 | 中高 | 低 | ★★★★★ |
| P0-6 | 每帧 payload 拷贝 / 双重编码 | 中高 | 中 | ★★★★☆ |
| P1-7 | 缓存策略 list+hash 双分配 | 中 | 中 | ★★★★☆ |
| P1-8 | LRU-K 双表搬移 | 中 | 中 | ★★★☆☆ |
| P1-9 | SessionTable Find 拷贝 | 中 | 低 | ★★★★☆ |
| P1-10 | g_session_cache 全表 purge | 中低 | 低 | ★★★☆☆ |
| P1-11 | PeerHub 控制帧分配 + 逐包 sendto | 中 | 低-中 | ★★★★☆ |
| P1-12 | Link 逐帧 syscall | 中高 | 中 | ★★★★☆ |
| P1-13 | 资源分块双拷贝 | 中 | 低-中 | ★★★★☆ |
| P1-14 | IO 定时器 2 次分配 + 全局锁 | 中 | 中 | ★★★☆☆ |
| P1-15 | 视频重组每帧大分配 | 中 | 低 | ★★★★☆ |
| P1-16 | bento_bridge 双重编码 | 中 | 低 | ★★★★☆ |
| P2-17 | MemoryTransport 每块分配 + 双拷贝 | 中低 | 中 | ★★☆☆☆ |
| P2-18 | MQTT 订阅线性匹配 | 中低 | 中 | ★★☆☆☆ |
| P2-19 | CoAP options 深拷贝 | 低中 | 低 | ★★★☆☆ |
| P2-20 | 单 iovec 走 sendmsg | 低 | 低 | ★★★☆☆ |
| P2-21 | Peer 通道 map | 低中 | 低 | ★★★☆☆ |
| P2-22 | Omni 每连接 mutex | 中低 | 中 | ★★☆☆☆ |
| P2-23 | Peer/Stream 冷热布局 | 中 | 低-中 | ★★★☆☆ |
| P2-24 | P2P 打洞逐候选 syscall | 中 | 中 | ★★★☆☆ |

## 4. 建议落地顺序（分批，每批可独立验证）

**批 1（纯收益、几乎不改语义，1-2 天）**
P0-5（KvTable hash 加速）、P1-9（SessionTable 零拷贝查询）、P1-10（会话缓存容器+惰性清理）、
P1-15（视频重组按需分配）、P1-16（bento_bridge 单次编码）、P2-19/P2-20/P2-21。

**批 2（每包/每帧固定税，需要并发回归，3-5 天）**
P0-4（P2P 状态原子化 + scratch 复用）、P1-11（PeerHub scratch + 批量）、P1-13（资源单拷贝）、P2-24（打洞批量发送）。

**批 3（结构性，单独立项）**
P0-1（Link rx/tx 分离）、P0-2（Socket 生命周期去锁）、P0-3（流表 ready 堆）、P0-6（Frame 视图零拷贝）、
P1-7/P1-8（缓存容器侵入式化）、P1-12/P1-14（批量收发 + timer wheel）。

**验证口径（每批都要跑）**：
① `app_test_stream_table` / `app_test_frame` / `app_test_kv` / `app_test_resource`（现有 335/2277/98 断言）；
② `bench_net_batch`、`go2cpp_bufio_batch_bench`、`go2cpp_cache_bench`、`p2p_bench` 前后对比；
③ `bento_bench_protocols --correctness`（37 场景 / 237 断言）确保协议字节不变；
④ 如改缓存容器：4 个 workload 的命中率必须逐格一致（改的是容器不是算法）。

## 5. 已确认**不需要**重复优化的点（前次已落地，避免重复劳动）

- HTTP 响应序列化去 `ostringstream`、请求起始行手写切分、header `string_view`
  （`src/network/http.cpp:258-380` 一带）；
- WebSocket 一帧一次 `recv`（16KB 预读）+ 小帧一次 `write`（`src/network/websocket.cpp`）；
- `bufio::Writer` 的 `WriteBatch`/`FlushBatch` + `SocketBatchWriter` 的 `sendv`/`sendmmsg`/`sendfile`
  （`include/go2cpp/network/bufio_socket.hpp`）——**这套能力已经在，缺的是让 P2P/Link/PeerHub 用上**；
- IO 层 fd 条带锁、`epoll_ctl` try_lock 自适应、timer 无锁快速路径（`src/network/io.cpp:2044-2110` 注释）；
- 并发模块（channel/fast_channel/scheduler）的全部 P0 项（见 `2026-11-01-concurrency-opt-review.md`）。

## 6. 局限

1. 本机无 `perf`，无法给出 cache miss / 分支预测的硬数字；所有"每帧 N 次"是静态计数，
   所有百分比是**估算**（已逐条标注）。
2. `app_test_*` 只有功能断言（335/2277/98 条）**没有吞吐基准**，无法量化流表/帧/资源路径的绝对耗时；
   建议后续补 `bench_app_stream_table`（固定 256 流、每帧 PickNext 计时）。
3. `p2p_bench` 只覆盖 STUN/TURN 的协议往返，不覆盖"候选多、NAT 严格"的打洞时延；
   本报告 P2-24 的收益只能按 syscall 数推算。
4. 缓存基准是单线程无锁策略，`ThreadSafeCache` 的实际吞吐还要乘上锁开销（不在本轮范围）。
5. 未审查 `src/network/quic/**`、`http3/**`（第三方 picoquic 适配层，改动收益/风险比不划算）。
