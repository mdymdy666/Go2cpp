# 14 tunnel 答卷（20 题）

> 环境：WSL2 / 12 vCPU。tunnel 未做 asm 审计；本卷没有重跑 1.96×/488 MiB/s 基准（见 Q11「没答出」）。
> 源码行号全部 `sed` 核对；题面 Q07 的描述与行号不严格对应，已在题内标注。

## Q01

**答**：单连接时控制帧（PING/GOAWAY/注册）与数据帧共享同一条 TCP 的发送缓冲：大流量会把 PING/GOAWAY 排在数据后面，心跳延迟、drain 变慢。`--data-conns N` 后控制连接只负责注册、心跳、GOAWAY，数据连接只承载 `OPEN/DATA/CLOSE`，服务端按**轮询**把新流分配到数据连接。代价是 N 条 TCP + N 个心跳，不是免费的；默认 0（单连接）够用。这是 frp work connection / rathole data channel 的等价物。

**代码证据**：
```text
src/tunnel/client.cpp:316-318
  316  void TunnelClient::StartDataConnections() {
  317      if (m_config.data_conns <= 0) { return; }
  320      bool expected = false;
```
服务端侧分配见 `src/tunnel/server.cpp` 的 data session 轮询（`m_tunnels_mutex` 内的 index 轮转）。

**自证**：`grep -n "data_conns" src/tunnel/client.cpp` 命中 316-318；未起 N=2 数据连接实测。

**我没答出的**：没有实测「单连接大流量下 PING 延迟 / drain 变慢」的具体数值。

## Q02

**答**：12B 头：`magic(4) "TUNL" / version(1) / type(1) / flags(2) / length(4)`，小端，payload 紧随其后。`type`：HELLO/HELLO_ACK/OPEN/DATA/CLOSE/PING/PONG/CHALLENGE/AUTH/GOAWAY。DATA body ≤65532 是「payload 内再带 `stream_id(4)`」后的余量（长度字段 4B 能表达更大，但协议主动限小，避免单帧占满发送缓冲）。半包/粘包由 `frame::FrameCodec` 处理。

**代码证据**：
```text
src/tunnel/mux.cpp:111  frame::FrameCodec codec(MakeTunnelFrameSpec(with_checksum));
include/go2cpp/tunnel/mux.hpp:70  // 握手阶段一次收到多帧不能丢（FrameReader）
```

**自证**：`ctest -R tunnel` 未单跑；协议常量在 `include/go2cpp/tunnel/protocol.hpp`（`kMaxDataBody = 65532`）。

**我没答出的**：没有抓包核对 12B 偏移。

## Q03

**答**：相同：一条 TCP 上按 `stream_id` 复用，`OPEN/DATA/CLOSE` 生命周期，心跳 + GOAWAY 关停。不同：**没有 per-stream credit 窗口**（用每流 8 MiB 有界队列 + 溢出关流代替）；没有优先级/权重；GOAWAY 只有 reason + retry_after，没有 yamux 的 last-stream-id 语义；每流 5 条线程（yamux 是单 goroutine 状态机）。所以它是「够用的会话层」，不是通用 mux 库。

**代码证据**：
```text
include/go2cpp/tunnel/mux.hpp:106  std::size_t recv_queue_cap = 8U * 1024U * 1024U;
src/tunnel/mux.cpp:521  FailStream(data.stream_id, "recv queue overflow");
```

**自证**：代码核对；未与 yamux/smux 做互操作测试。

**我没答出的**：没有实测流数上限/线程增长曲线。

## Q04

**答**：顺序 `S→C CHALLENGE(16B)` → `C→S AUTH(16B tag)` → `C→S HELLO` → `S→C HELLO_ACK`；**AUTH 在 HELLO 之前**，攻击者拿不到任何隧道信息。PSK=`SHA-256(token)`；tag=`Seal(KDF(PSK), nonce=challenge[0..12), aad=challenge)`。token 明文从不上线，线上只有随机挑战与 AEAD tag。挑战每次随机 → 抓包无法重放。无 token 时服务端发全零挑战、客户端回全零 tag（客户端不需要猜服务端有没有开鉴权）。明确拒绝不进入退避重试。

**代码证据**：
```text
src/tunnel/protocol.cpp:474  Psk DerivePsk(const std::string& secret) { ... Sha256::Digest(secret) ...
src/tunnel/protocol.cpp:483  std::vector<std::uint8_t> MakeChallenge() { ... SecureRandomBytes(challenge.data(), 16) ...
src/tunnel/protocol.cpp:520  bool VerifyAuthTag(const Psk& psk, const std::uint8_t* challenge, const std::uint8_t* tag)
```
> **题面错误（本卷埋点）**：Q07 说 `src/tunnel/client.cpp:372` 是「重连主循环用同一个 Backoff 取延迟」；372 实际是 `RunOnce()` 调用，真正 `backoff.Next()` 在 **391** 行（`sed -n '391p'` 可见 `delay_ms = backoff.Next();`）。Q04 本身行号无误。

**自证**：`sed -n '470,486p' src/tunnel/protocol.cpp` 显示 DerivePsk/MakeChallenge 的 SHA-256 与 SecureRandomBytes。

**我没答出的**：没有写「错误 token → 不重试」的端到端用例。

## Q05

**答**：静态密钥 + 固定 nonce：抓一次 AUTH 就能无限重放，冒充任意客户端注册隧道（子域名抢占）。挑战应答让每次 AUTH 的 nonce/aad 都不同；服务端只接受「本次挑战」算出的 tag，过期挑战无效。token 轮换 = PSK 轮换，不需要改协议字段。仍未解决：token 是长期共享秘密，没有 per-token ACL/配额，知道 token 的人可顶掉同子域名。

**代码证据**：
```text
src/tunnel/protocol.cpp:474  DerivePsk 每次从 token 派生（不是把 PSK 硬编码在帧里）
src/tunnel/protocol.cpp:483  MakeChallenge 每次随机
```

**自证**：代码核对；未做抓包重放实验。

**我没答出的**：没有重放攻击的实测。

## Q06

**答**：PING 每 15 s 一次；90 s 内没有任何入站帧（含 PONG/数据）判定连接死亡。90 s = 6 个心跳周期，容忍连续丢失约 5 个 PING/PONG，避免一次抖动就断。心跳线程独立于数据线程；N 条数据连接 = N 份心跳。PING/PONG 带 8B nonce，防止把旧 PONG 当新响应。

**代码证据**：
```text
src/tunnel/mux.cpp:608-609  void MuxSession::HeartbeatLoop() { const int interval = std::max(1000, m_config.ping_interval_ms);
src/tunnel/mux.cpp:622-624  if (last != 0 && idle_timeout_ms > 0 && now - last > idle_timeout_ms) { Shutdown("heartbeat timeout");
src/tunnel/mux.cpp:627-628  const std::uint64_t nonce = m_ping_nonce.fetch_add(1); SendPayload(FrameType::kPing, EncodeNonce(nonce));
```

**自证**：`sed -n '608,630p'` 核对三处；未做心跳断链实验。

**我没答出的**：没有实测 90s 判定时延与 PING 丢失容忍。

## Q07

**答**：退避 0.5 s → 15 s，±20% 抖动；抖动避免多客户端同时断线后同一刻一起重连（重连风暴）。真实坑：`bento::Backoff` 旧实现先算 `nominal ± span` 再 `min(..., max)`，到上限时一半样本被压到 30000，直方图第 10 桶占 54.7%，抖动等于白加；修复后 9798–10160（≈10%/桶）。明确拒绝（token 错/版本错）不重试：重试只会刷日志且永远不会成功，只有网络类错误才退避重连。

**代码证据**：
```text
src/tunnel/client.cpp:170  bento::Backoff TunnelClient::MakeBackoff() const { ... base_ms = max(100, reconnect_min_ms) ...
src/tunnel/client.cpp:369  bento::Backoff backoff = MakeBackoff();
src/tunnel/client.cpp:391  delay_ms = backoff.Next();
```
> 题面写的 `client.cpp:372` 是 `RunOnce()`，不是取延迟处；正确是 391。

**自证**：`sed -n '368,392p'` 见 369/391；未做多客户端同时断线的重连直方图。

**我没答出的**：没有复跑 Backoff 抖动直方图。

## Q08

**答**：`SIGTERM` → `BeginDrain()`（`m_draining.exchange(true)` 幂等）→ 对所有会话 `SendGoAway(1, 0)` → 按 `drain_timeout_ms`（默认 5000）轮询 `streams_active()==0` → 归零或超时后断开。客户端收到 GOAWAY 后不再重连，否则 drain 白做。drain 期间 status 的 `draining=true`。不做 drain 的后果：在途响应被 RST/FIN 截断，浏览器看到半截/502。

**代码证据**：
```text
src/tunnel/server.cpp:518-520  void TunnelServer::BeginDrain() { if (m_draining.exchange(true, ...)) return;
src/tunnel/server.cpp:536-537  for (...) { (void)session->SendGoAway(1, 0); }
src/tunnel/server.cpp:541-544  deadline = now + max(0, drain_timeout_ms); while (now < deadline) { ... }
```

**自证**：代码核对；未对真实 server 发 SIGTERM 观察在途流。

**我没答出的**：没有实测「drain 中 32 并发请求全部完成」的用例。

## Q09

**答**：每条流有独立有界队列（默认 8 MiB）；下游极慢时该流先满。溢出策略是**关该流**（`FailStream("recv queue overflow")`），不是关整条 TCP：保护其它流与整条连接。接收线程不阻塞在单条流上；`RelayPipe` 另有 256 KiB 有界队列做二级背压。未实现 per-stream credit 窗口，只能靠「有限队列 + 溢出关流」兜底。

**代码证据**：
```text
include/go2cpp/tunnel/mux.hpp:105-106  /// 单流接收队列上限（字节）；溢出即关闭该流。默认 8 MiB。
src/tunnel/mux.cpp:520-521  if (!stream->PushData(data.body, data.length)) { FailStream(data.stream_id, "recv queue overflow"); }
```

**自证**：代码核对；未注入慢客户端验证只关单流。

**我没答出的**：没有做慢下游的溢出实验。

## Q10

**答**：`streams`：当前并发是否打满 `max_streams_per_tunnel`（256），打满表现为新请求 503/排队；`streams_total`：只增不涨=流泄漏（CLOSE 没到/响应完成检测失败），涨得飞快=短连接风暴；`data_connections`：控制还在、数据掉了→新流分配不到通道，表现为「隧道在线但请求全 502」；`bytes_in/out`：单向不动=半关闭语义错/后端不回包，两边都动但很小=背压；`online_ms`：频繁归零=重连风暴；`draining`：已发 GOAWAY 还在接新流=drain 没拦住入口；`peer/local`：路由错配。只看 streams 不够。

**代码证据**：
```text
src/tunnel/server.cpp:588  std::string TunnelServer::BuildStatusJson() const {
src/tunnel/server.cpp:607-613  streams_total / data_connections / bytes_in / bytes_out / online_ms / draining
```

**自证**：`curl /__tunnel__/status` 在本机 8080 隧道在线时返回过 `"subdomain":"shop"`（前一轮环境验证）；本轮未重跑。

**我没答出的**：没有逐个字段做故障注入（例如杀掉数据连接看 status）。

## Q11

**答**：1.96× 是**短连接**（每请求新建 TCP）p50；keep-alive p50 是 2.99×，不是同一指标。固定开销来自两端各 5 条 RelayPipe 线程创建 + 4 次跨线程队列唤醒 ≈0.6 ms；公网 RTT ≥10 ms 时占比 <6%。488 MiB/s 是直连 0.37×，绝对损耗大，但已高于 1 Gbps 网卡上限（119 MiB/s），说明单机回环口径下吞吐不是瓶颈。结论：不能对外说「隧道只慢 2 倍」——延迟看场景、吞吐看基线，两个指标不能混。**本项目未做 asm 审计**，瓶颈在系统调用与线程调度。

**代码证据**：`src/tunnel/mux.cpp` 的 RelayPipe 使用；`include/go2cpp/tunnel/mux.hpp` 队列参数。

**自证**：未重跑；`docs/modules/tunnel.md` §6 的数字只作历史对照。

**我没答出的**：没有本机 1.96×/488 MiB/s 复测（需要直连/隧道双基线 + 300 次短连接）。

## Q12

**答**：路径模式按 URL 前缀（`/shop/`）路由到隧道；keep-alive 的后续请求可能带不同前缀，服务端已无法回头改路由，所以一请求一条隧道流、强制 `Connection: close`。代价是多一次 OPEN/握手，这是设计取舍不是 bug。子域名模式（`Host: shop.domain`）没有这个限制，可以 keep-alive；正式部署推荐子域名模式。

**代码证据**：`src/tunnel/server.cpp` 路径路由与响应完成检测（只解析首个请求头）；`include/go2cpp/tunnel/server.hpp` 的 `strip_prefix`。

**自证**：代码核对；未用 keep-alive 客户端打两条不同前缀请求。

**我没答出的**：没有实测路径模式下第二条请求的表现。

## Q13

**答**：线程数 = 2 条 accept 循环 + 每控制连接 1 条等待线程 + 每流 RelayPipe（5 条：2 读 + 2 写 + watchdog）+ MuxSession 2 条，随在线隧道数与并发请求数增长。32 并发 ≈160 线程；建议单隧道并发 ≤256；更高并发要给 RelayPipe 加线程池（属于 relay，未做）。代价主要是调度与栈（每线程 8 MiB 虚拟地址，RSS 小）。

**代码证据**：
```text
include/go2cpp/tunnel/mux.hpp:105-108  recv_queue_cap / idle_timeout_ms / ping_interval_ms
src/tunnel/server.cpp:541-544  drain 轮询（每条会话一个后台线程）
```

**自证**：未测 32 并发线程数；可 `ls /proc/<pid>/task | wc -l` 复核（未做）。

**我没答出的**：没有实测线程数随并发增长。

## Q14

**答**：有 `Content-Length`：按长度判定响应结束，关闭隧道流。`Transfer-Encoding: chunked`：实现靠客户端关闭连接收尾（curl/浏览器都会关），服务端不解析 chunked 终止块。路径模式下既无 CL 又不是「客户端会关」的响应，会占着流直到 90 s idle 超时，表现为「请求很慢/连接不释放」。修复方向：解析 `0\r\n\r\n` 终止块，或要求后端给 CL。

**代码证据**：`src/tunnel/server.cpp` 的响应完成检测（按 `Content-Length` 计字节）；`include/go2cpp/tunnel/mux.hpp` 的 `idle_timeout_ms=90000`。

**自证**：代码核对；未用 chunked 后端复现 90s 占用。

**我没答出的**：没有实测 chunked 响应在路径模式下的流占用。

## Q15

**答**：PSK/挑战应答只做**鉴权**（证明你有 token），不做传输加密；隧道内是明文 HTTP。不做自签 TLS 的理由：浏览器端仍要装证书，破坏「任何设备打开就能用」的产品定位。正确部署：公网侧前置 nginx/caddy 终结 TLS，隧道只跑内网/回环。这与 frp 默认（无 TLS 时）一致。

**代码证据**：
```text
src/tunnel/protocol.cpp:474-480  DerivePsk 只派生鉴权密钥；数据面（mux.cpp 的 DATA 帧）无加密
include/go2cpp/tunnel/server.hpp  TunnelServerConfig（没有 tls 字段）
```

**自证**：代码核对；未抓包确认 DATA 明文。

**我没答出的**：没有抓包证明隧道内明文。

## Q16

**答**：设计动机：客户端崩溃重启后要能立刻用回同一个子域名，否则 URL 变了；同名子域名以**最新注册**为准。风险：多人共享一个 token 时任何人可顶掉别人的隧道；没有 per-token ACL/配额。隔离方案：每人独立 token + 配额存储（未实现），或用不同 `--domain`。这不是「鉴权绕过」——仍然是「有 token 的人」，只是 token 粒度太粗。

**代码证据**：`src/tunnel/server.cpp` 的隧道表按子域名插入（同名覆盖最新）；`TunnelServerConfig::token` 全局单 token。

**自证**：代码核对；未做双客户端同子域名抢占实验。

**我没答出的**：没有实测抢占后旧连接的行为。

## Q17

**答**：`Stop()` 语义：置停止标志 → join 所有工作线程 → 返回；返回后对象可安全销毁。事件回调运行在工作线程上；在回调里调 `Stop()` 等于让线程 join 自己 → 永久阻塞。正确做法：回调里只置标志/投递消息，由外部线程调 `Stop()`。`Stop()` 之后不能再 `Start()` 同一对象。

**代码证据**：
```text
include/go2cpp/tunnel/client.hpp:121-122  /// 停止并等待后台线程与本地转发线程退出。 void Stop();
src/tunnel/client.cpp 的 RunLoop 在工作线程；Stop 置 m_running=false 后 join
```

**自证**：代码核对；未写回调自 join 的死锁复现（会真挂，需要 timeout 兜底）。

**我没答出的**：没有跑死锁复现。

## Q18

**答**：N 条数据连接各自跑心跳（15 s PING / 90 s idle），所以是 N 份心跳流量。服务端维护数据连接列表，新流按**轮询**分配；控制连接不承载 OPEN/DATA/CLOSE。客户端 `data_connections()` 可查实际建立了几条；`data_conns=0` 表示只用控制连接。数据连接断开不会让隧道立即下线（控制连接还在），但新流会分不到通道 → status 的 `data_connections` 掉。

**代码证据**：
```text
src/tunnel/client.cpp:316-318  StartDataConnections()（data_conns <= 0 直接返回）
include/go2cpp/tunnel/client.hpp  data_connections() 访问器
```

**自证**：代码核对；未起 `--data-conns 2` 验证轮询。

**我没答出的**：没有实测 N=2 时新流分配比例。

## Q19

**答**：打开后在帧尾追加 CRC，帧长与解析规则都变；两端不一致 → 长度/边界错位，连接直接失败。默认关闭；打开多 4 B/帧，小 DATA 帧开销比例不可忽略。校验只防传输损坏，不防篡改（没有密钥），安全仍靠 PSK 鉴权 + 上层 TLS。配置来自两端命令行，必须一起改（部署脚本要检查）。

**代码证据**：
```text
src/tunnel/mux.cpp:111  frame::FrameCodec codec(MakeTunnelFrameSpec(with_checksum));
include/go2cpp/tunnel/protocol.hpp  MakeTunnelFrameSpec(bool with_checksum)
```

**自证**：代码核对；未做单端开启的错位复现。

**我没答出的**：没有实测「一端开一端关」的失败形态。

## Q20

**答**：单进程单实例；隧道注册表在内存，进程重启后所有隧道要客户端重连才能恢复。多 VPS 时各自持一份路由表，同一个子域名落到不同 VPS 会 404；没有共享存储/gossip。要扩需要：外部路由表（Redis/etcd）+ 一致的子域名分配 + 健康检查（均未实现）。重启时 drain 能减少影响，但不能避免「客户端必须重连」。

**代码证据**：
```text
src/tunnel/server.cpp:588-613  BuildStatusJson 从内存 m_tunnels 生成
include/go2cpp/tunnel/server.hpp  tunnels() 返回内存快照
```

**自证**：代码核对；未做多实例实验。

**我没答出的**：没有多 VPS 部署验证。
