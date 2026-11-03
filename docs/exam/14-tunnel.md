# Go2Cpp 理解力考试 · 公网穿透卷（14 tunnel）

> **考的是「为什么这样分层 / 故障怎么定位 / 数据多少」**。
> 参考基线：`docs/modules/tunnel.md`（设计/协议/对照 frp·rathole）、`user/docs/14-tunnel.md`（交接文档）、
> `docs/audit/audit-heartbeat-backoff.md`（复用的 Backoff 真实坑）、提交 `f95c688`（tunnel 工业级：控制/数据分离 +
> 多路复用 + PSK 挑战应答 + 心跳 + 退避 + drain + metrics，p50 1.96× 直连 / 153 检查）。
>
> 难度分布：L1 = 4、L2 = 6、L3 = 7、L4 = 3。
> 口径提醒：本模块**没有做过 objdump/汇编级审计**，瓶颈在系统调用与线程调度（`user/docs/14-tunnel.md:268`）；
> 所有性能数字都是 WSL2 本机回环、8 核口径，不能外推到公网。

---

## 一、协议与握手

### Q01 【L3 陷阱】控制连接与数据连接为什么要分开？单连接多路复用不是更简单吗？

- **痛点**：把 `--data-conns` 当过度设计，或者反过来以为分离是为了「负载均衡」。
- **追查位置**：
  - `docs/modules/tunnel.md:54`（默认单连接 + 流多路复用，与 rathole 默认同构；`--data-conns N` 额外建 N 条数据连接）
  - `src/tunnel/client.cpp:317`（`data_conns <= 0` 时只建控制连接；否则循环建 N 条）
- **验收标准**：
  1. 单连接时控制帧（PING/GOAWAY/注册）与数据帧共享同一条 TCP 的发送缓冲：大流量会把 PING/GOAWAY 排在数据后面，心跳延迟、drain 变慢。
  2. 分离后：控制连接只负责注册、心跳、GOAWAY；数据连接只承载 `OPEN/DATA/CLOSE`，服务端按**轮询**把新流分配到数据连接。
  3. 代价：多 N 条 TCP + N 个心跳（`docs/modules/tunnel.md:63`），不是免费的。
  4. 定位：这是 frp 的 work connection / rathole 的 data channel 的等价物；默认 0 = 单连接，绝大多数场景够用。
- **常见错误答案**：「分离是为了负载均衡/多核加速」「单连接一定更好，分离是多余复杂度」。

### Q02 【L1 概念】12 字节帧头里每个字段干什么？为什么 DATA 的 body ≤ 65532？

- **痛点**：把 tunnel 的 12B 头与 Bento 的 16B 头搞混，或者不知道 65532 从哪来。
- **追查位置**：
  - `docs/modules/tunnel.md:85`（12 字节帧头布局：magic "TUNL" / version / type / flags / length，全部小端）
  - `src/tunnel/mux.cpp:111`（`frame::FrameCodec codec(MakeTunnelFrameSpec(with_checksum))`，复用 FrameCodec 处理半包/粘包）
- **验收标准**：
  1. 字段：`magic(4) / version(1) / type(1) / flags(2) / length(4)`，payload 紧随其后。
  2. `type`：1 HELLO / 2 HELLO_ACK / 3 OPEN / 4 DATA / 5 CLOSE / 6 PING / 7 PONG / 8 CHALLENGE / 9 AUTH / 10 GOAWAY。
  3. `DATA` 的 body ≤ 65532 是 **payload 内再带 `stream_id(4)`** 后的余量（长度字段 4B 能表达更大，但协议主动限小，避免单帧占满发送缓冲）。
  4. 半包/粘包由 `FrameCodec` 处理；握手阶段一次收到多帧不能丢（`include/go2cpp/tunnel/mux.hpp:70`）。
- **常见错误答案**：「头是 16 字节」「65532 是 UDP 限制」。

### Q03 【L2 概念】这里的「多路复用」与 yamux/smux 有什么相同、什么不同？

- **痛点**：直接说「就是 yamux」，忽略缺流控、缺优先级、每流多线程这些实质差异。
- **追查位置**：
  - `docs/modules/tunnel.md:214`（一条 TCP 上按 `stream_id` 复用，12B 帧头；等价 yamux/smux 的会话层）
  - `docs/modules/tunnel.md:220`（限流/背压：RelayPipe 有界队列 256 KiB + 每流接收队列 8 MiB + 每隧道 max-streams）
- **验收标准**：
  1. 相同：一条 TCP 上按 `stream_id` 复用；`OPEN/DATA/CLOSE` 生命周期；心跳 + GOAWAY 关停。
  2. 不同：**没有 per-stream credit 窗口**（用每流 8 MiB 有界队列 + 溢出关流代替）；没有优先级/权重；GOAWAY 只有 reason + retry_after，没有 yamux 的 last-stream-id 语义。
  3. 不同：每流 5 条线程（2 读 + 2 写 + watchdog），yamux 是单 goroutine 状态机；线程数随并发线性增长。
  4. 所以它是「够用的会话层」，不是通用 mux 库。
- **常见错误答案**：「就是 yamux 的 C++ 移植」「多路复用一定需要窗口」。

### Q04 【L4 陷阱】PSK 挑战应答的完整流程是什么？为什么 token 明文从不上线？

- **痛点**：把顺序记成「先 HELLO 再鉴权」，或者以为 AUTH 是「把 token 发过去比对」。
- **追查位置**：
  - `src/tunnel/protocol.cpp:474`（`DerivePsk`：`SHA-256(token)` → 32B PSK）
  - `src/tunnel/protocol.cpp:483`（`MakeChallenge`：16B 随机挑战）
  - `src/tunnel/protocol.cpp:520`（`VerifyAuthTag`：用 PSK 解 AUTH tag）
- **验收标准**：
  1. 顺序：`S→C CHALLENGE(16B)` → `C→S AUTH(16B tag)` → `C→S HELLO` → `S→C HELLO_ACK`。**AUTH 在 HELLO 之前**，攻击者拿不到任何隧道信息（子域名/URL/本地端口）。
  2. 密钥派生：`PSK = SHA-256(token)`（32B）；`tag = Seal(KDF(PSK), nonce=challenge[0..12), aad=challenge)`（`docs/modules/tunnel.md:105`、`:117`）。
  3. token 明文**从不上线**：线上只有随机挑战与 AEAD tag；知道 tag 也反推不出 token。
  4. 挑战每次随机 → 抓包无法重放；无 token 时服务端发全零挑战、客户端回全零 tag（客户端不需要猜服务端有没有开鉴权）。
  5. 失败语义：明确拒绝（token 错/版本错）返回状态码，客户端**不进入退避重试**。
- **常见错误答案**：「客户端把 token 发过去，服务端字符串比较」「先 HELLO 再鉴权，省一次往返」。

### Q05 【L3 陷阱】为什么不用静态密钥/固定 nonce？静态密钥的具体风险是什么？

- **痛点**：觉得「反正有 token，静态密钥更简单」，不知道重放与长期密钥泄露的后果。
- **追查位置**：
  - `src/tunnel/protocol.cpp:474`（每次从 token 派生 PSK，而不是把 PSK 硬编码在帧里）
  - `src/tunnel/protocol.cpp:483`（挑战随机生成，保证每次 AUTH 不同）
- **验收标准**：
  1. 静态密钥 + 固定 nonce：抓一次 AUTH 就能无限重放，冒充任意客户端注册隧道（子域名抢占）。
  2. 挑战应答让每次 AUTH 的 nonce/aad 都不同；服务端只接受「本次挑战」算出的 tag，过期挑战无效。
  3. token 轮换 = PSK 轮换，不需要改协议字段。
  4. 仍未解决的边界：token 本身是长期共享秘密，没有 per-token ACL/配额，知道 token 的人可以顶掉同子域名（`docs/modules/tunnel.md:264`）。
- **常见错误答案**：「静态密钥更简单，安全性差不多」「nonce 用时间戳就行」。

### Q06 【L1 概念】PING 15 s / idle 90 s 各防什么？为什么是 6 倍关系？

- **痛点**：把 90 s 当成 TCP keepalive，或者不知道心跳线程独立于数据线程。
- **追查位置**：
  - `src/tunnel/mux.cpp:609`（心跳循环：`interval = max(1000, ping_interval_ms)`）
  - `src/tunnel/mux.cpp:622`（`now - last > idle_timeout_ms` 判定死亡）
  - `src/tunnel/mux.cpp:628`（`SendPayload(FrameType::kPing, EncodeNonce(nonce))`）
- **验收标准**：
  1. PING 每 15 s 一次；90 s 内没有任何入站帧（含 PONG/数据）判定连接死亡。
  2. 90 s = 6 个心跳周期，容忍连续丢失约 5 个 PING/PONG，避免一次抖动就断。
  3. 心跳线程独立于数据线程：数据连接也可以有自己的心跳（N 条数据连接 = N 份心跳）。
  4. PING/PONG 带 8B nonce，防止把旧 PONG 当新响应。
- **常见错误答案**：「90 s 是 TCP keepalive 的默认值」「心跳和数据在同一个线程，所以大流量会误判」。

---

## 二、可靠性 / 可观测 / 性能

### Q07 【L3 陷阱】重连退避为什么必须带抖动？「错误 token 不重试」又是为了什么？

- **痛点**：以为抖动只是「加点随机」，不知道封顶处会堆积；或者把「不重试」当 bug。
- **追查位置**：
  - `src/tunnel/client.cpp:170`（`MakeBackoff`：`reconnect_min_ms=500`、`reconnect_max_ms=15000`、`jitter=0.2`）
  - `src/tunnel/client.cpp:372`（重连主循环用同一个 `Backoff` 取延迟）
- **验收标准**：
  1. 退避范围 0.5 s → 15 s，±20% 抖动（`docs/modules/tunnel.md:217`）。
  2. 抖动的目的：多客户端同时断线后不要同一刻一起重连（重连风暴）。
  3. 真实坑：`bento::Backoff` 的旧实现先算 `nominal ± span` 再 `min(..., max)`，到上限时一半样本被压到 30000，直方图第 10 桶占 **54.7%**，抖动等于白加；修复后是 9798–10160（≈10%/桶）（`docs/audit/audit-heartbeat-backoff.md:95-104`）。
  4. 明确拒绝（token 错/版本错）**不重试**：重试只会刷日志，且永远不会成功；只有网络类错误才退避重连。
- **常见错误答案**：「一直重试总能连上」「抖动随便加，反正有上限」。

### Q08 【L3 陷阱】`SIGTERM` 之后为什么要 GOAWAY + drain？直接退出会怎样？

- **痛点**：把优雅关闭当「多余流程」，结果在途 HTTP 响应被截断，客户端看到 502/半截 body。
- **追查位置**：
  - `src/tunnel/server.cpp:519`（`BeginDrain`：`m_draining.exchange(true)` 幂等入口）
  - `src/tunnel/server.cpp:537`（对所有会话 `SendGoAway(1, 0)`）
  - `src/tunnel/server.cpp:544`（按 `drain_timeout_ms`（默认 5000）轮询 `streams_active()==0`）
- **验收标准**：
  1. 流程：`SIGTERM` → `BeginDrain()` → 每条会话发 `GOAWAY(reason=1, retry_after=0)` → 轮询在途流数 → 归零或超时（5 s）→ 断开。
  2. 客户端收到 GOAWAY 后**不再重连**，否则 drain 白做。
  3. drain 期间 `/__tunnel__/status` 的 `draining` 为 true，可观测。
  4. 不做 drain 的后果：正在传输的响应被 TCP RST/FIN 截断，浏览器看到半截内容或 502。
- **常见错误答案**：「直接 kill -9 更快」「GOAWAY 只是通知，没有实际作用」。

### Q09 【L3 陷阱】每流 8 MiB 接收队列溢出时，为什么关流而不是关连接？

- **痛点**：以为背压就是「队列开大点」，不知道溢出策略决定了故障半径。
- **追查位置**：
  - `include/go2cpp/tunnel/mux.hpp:106`（`recv_queue_cap = 8U * 1024U * 1024U`，每流独立有界队列）
  - `src/tunnel/mux.cpp:521`（`FailStream(data.stream_id, "recv queue overflow")`）
- **验收标准**：
  1. 每条流有独立的有界队列（默认 8 MiB）；下游极慢时该流先满。
  2. 溢出策略是**关该流**（`FailStream`），不是关整条 TCP：保护其它流与整条连接。
  3. 接收线程不阻塞在单条流上；`RelayPipe` 另有 256 KiB 有界队列做二级背压。
  4. 未实现的是 per-stream credit 窗口（真正的流控），所以只能靠「有限队列 + 溢出关流」兜底（`docs/modules/tunnel.md:259`）。
- **常见错误答案**：「队列越大越好」「溢出就关整条连接」。

### Q10 【L3 陷阱】`/__tunnel__/status` 的字段分别能定位什么故障？只看 streams 够吗？

- **痛点**：把状态页当「监控装饰」，线上出事时不知道看哪个字段。
- **追查位置**：
  - `src/tunnel/server.cpp:588`（`BuildStatusJson`：server/version/public_port/draining/connections_total/tunnels[]）
  - `src/tunnel/server.cpp:610`（每隧道 `streams / streams_total / data_connections / bytes_in / bytes_out / online_ms / draining`）
- **验收标准**（逐个说能定位什么）：
  1. `streams`：当前并发是否打满 `max_streams_per_tunnel`（256）；打满表现为新请求 503/排队。
  2. `streams_total`：只增不涨说明流泄漏（CLOSE 没到/响应完成检测失败）；涨得飞快说明短连接风暴。
  3. `data_connections`：控制连接还在、数据连接掉了 → 新流分配不到通道，表现为「隧道在线但请求全 502」。
  4. `bytes_in` / `bytes_out`：单向不动 = 半关闭语义错/后端不回包；两边都动但很小 = 背压。
  5. `online_ms`：频繁归零 = 重连风暴（退避失效或 token 错）。
  6. `draining`：已发 GOAWAY 却还在接新流 = drain 逻辑没拦住入口。
  7. `peer` / `local`：路由错配（连错本地端口/NAT 后地址变了）。
- **常见错误答案**：「看 streams 就行」「status 只是给 Prometheus 抓的」。

### Q11 【L4 陷阱】p50 1.96× 直连说明隧道「只慢一倍」吗？488 MiB/s 又该怎么解读？

- **痛点**：把短连接延迟比当通用结论，或者把 488 MiB/s 当「没有损耗」。
- **追查位置**：
  - `docs/modules/tunnel.md:233`（短连接延迟 p50：直连 0.685 ms → 隧道 1.344 ms，**1.96×**）
  - `docs/modules/tunnel.md:236`（8 MiB 下载吞吐：直连 1312 MiB/s → 隧道 488 MiB/s，**0.37×**）
- **验收标准**：
  1. 1.96× 是**短连接**（每请求新建 TCP）的 p50；keep-alive p50 是 0.291 → 0.869 ms（**2.99×**），不是同一个指标。
  2. 固定开销来源：两端各 5 条 RelayPipe 线程创建 + 4 次跨线程队列唤醒 ≈ 0.6 ms（`docs/modules/tunnel.md:239-241`）。
  3. 外推边界：公网 RTT ≥ 10 ms 时这 0.6 ms 占比 < 6%，所以「1.96×」只在本机回环这种亚毫秒基线下成立。
  4. 吞吐 488 MiB/s 是直连的 0.37×，绝对损耗大；但已高于 1 Gbps 网卡上限（119 MiB/s），说明单机回环口径下吞吐不是瓶颈。
  5. 结论：不能对外说「隧道只慢 2 倍」——延迟看场景、吞吐看基线，两个指标不能混。
- **常见错误答案**：「隧道只慢 2 倍」「488 MiB/s 说明没损耗」「1.96× 是公网实测」。

### Q12 【L2 陷阱】路径前缀模式为什么强制 `Connection: close`？这是 bug 吗？

- **痛点**：把「一请求一条流」当实现缺陷，不知道是路由模型的必然结果。
- **追查位置**：
  - `docs/modules/tunnel.md:251`（路径前缀模式强制 `Connection: close`：服务端只解析第一个请求头）
  - `user/docs/14-tunnel.md:273`（keep-alive 上的后续请求无法再按前缀路由）
- **验收标准**：
  1. 路径模式按 URL 前缀（`/shop/`）路由到隧道；keep-alive 的后续请求可能带不同前缀，服务端已无法回头改路由。
  2. 所以一请求一条隧道流，代价是多一次 OPEN/握手；这是设计取舍，不是 bug。
  3. 子域名模式（`Host: shop.domain`）没有这个限制，可以 keep-alive。
  4. 正式部署推荐子域名模式。
- **常见错误答案**：「keep-alive 坏了，应该修」「强制 close 只是懒」。

---

## 三、资源 / 边界

### Q13 【L2 概念】每条流 5 条线程，32 并发就是 160 条——这个模型的上限在哪？

- **痛点**：以为服务端是固定线程池，不知道线程数随隧道数与并发线性增长。
- **追查位置**：
  - `docs/modules/tunnel.md:256`（每流 5 条线程：2 读 + 2 写 + watchdog；32 并发约 160 线程）
  - `user/docs/14-tunnel.md:62`（服务端线程模型：2 条 accept 循环 + 每控制连接 1 等待线程 + 每公网连接 1 RelayPipe，外加 MuxSession 2 条）
- **验收标准**：
  1. 线程数 = accept 循环 + 控制连接等待线程 + 每流 RelayPipe（5 条）+ MuxSession（2 条），随在线隧道数与并发请求数增长。
  2. 32 并发 ≈ 160 线程；建议单隧道并发 ≤ 256。
  3. 更高并发需要给 RelayPipe 加线程池（属于 relay 模块，未做）。
  4. 代价不是「内存」，是调度与栈（每线程默认栈 8 MiB 虚拟地址，实际 RSS 小）。
- **常见错误答案**：「固定线程池，和并发无关」「线程多就是内存泄漏」。

### Q14 【L3 陷阱】响应完成检测只认 `Content-Length`，chunked 响应会怎样？

- **痛点**：以为所有 HTTP 响应都能被正确判定「结束」，结果 chunked 流把隧道流占死。
- **追查位置**：
  - `docs/modules/tunnel.md:260`（只认 `Content-Length`；chunked 依赖客户端关连接收尾；路径模式下无 CL 的响应会走到 90 s 心跳超时）
  - `user/docs/14-tunnel.md:281`（响应完成检测只认 Content-Length）
- **验收标准**：
  1. 有 `Content-Length`：按长度判定响应结束，关闭隧道流。
  2. `Transfer-Encoding: chunked`：实现靠客户端关闭连接来收尾（curl/浏览器都会关）；服务端不解析 chunked 终止块。
  3. 路径模式下既无 CL 又不是「客户端会关」的响应，会占着流直到 90 s idle 超时，表现为「请求很慢/连接不释放」。
  4. 修复方向：解析 chunked 的 `0\r\n\r\n` 终止块，或显式要求后端给 CL。
- **常见错误答案**：「chunked 也能正确检测」「超时是网络问题」。

### Q15 【L1 概念】隧道里为什么不做 TLS？PSK 不是加密吗？

- **痛点**：把「PSK 鉴权」当「传输加密」，或者以为不做 TLS 是偷懒。
- **追查位置**：
  - `docs/modules/tunnel.md:222`（TLS 传输加密 ❌：PSK 只做鉴权，隧道里是明文 HTTP）
  - `docs/modules/tunnel.md:263`（敏感场景在 VPS 前置 nginx/caddy 终结 TLS）
- **验收标准**：
  1. PSK/挑战应答只做**鉴权**（证明你有 token），不做传输加密；隧道内是明文 HTTP。
  2. 不做自签 TLS 的理由：浏览器端仍要装证书，破坏「任何设备打开就能用」的产品定位。
  3. 正确部署：公网侧前置 nginx/caddy 终结 TLS，隧道只跑内网/回环。
  4. 这与 frp 默认（无 TLS 时）一致，不是本项目独有的偷懒。
- **常见错误答案**：「PSK 就是加密传输」「不做 TLS 所以不安全，不能用」。

### Q16 【L2 陷阱】知道 token 的人能把同子域名的隧道顶掉，这是漏洞还是设计？

- **痛点**：把「子域名抢占」当安全漏洞报，或者反过来不知道它的风险。
- **追查位置**：
  - `docs/modules/tunnel.md:264`（知道 token 的人可以用同一子域名顶掉现有隧道，设计如此）
  - `docs/modules/tunnel.md:224`（同名子域名以**最新注册**为准，重连友好）
- **验收标准**：
  1. 设计动机：客户端崩溃重启后要能立刻用回同一个子域名，否则 URL 变了。
  2. 风险：多人共享一个 token 时，任何人可以顶掉别人的隧道；没有 per-token ACL/配额。
  3. 隔离方案：每人独立 token + 配额存储（未实现）；或用不同 `--domain`。
  4. 这不是「鉴权绕过」——仍然是「有 token 的人」，只是 token 的粒度太粗。
- **常见错误答案**：「这是漏洞，应该拒绝重复注册」「无所谓，反正有 token」。

### Q17 【L4 陷阱】在事件回调里调 `TunnelClient::Stop()` 会怎样？为什么仍然不该这么做？

- **痛点**：把 `Stop()` 当「发个停止信号就返回」，不知道它要 join 工作线程。
- **追查位置**：
  - `user/docs/14-tunnel.md:317`（`Stop()` 返回后保证没有工作线程再访问对象；在回调里调 `Stop()` 会自 join 死锁）
  - `include/go2cpp/tunnel/client.hpp:122`（`Stop()`：置停 + join；返回后无线程访问本对象）
- **验收标准**：
  1. `Stop()` 语义：置停止标志 → join 所有工作线程 → 返回；返回后对象可安全销毁。
  2. 事件回调运行在工作线程上；在回调里调 `Stop()` 不会永久死锁——`client.cpp:83-98` 有 `get_id()==this_thread::get_id()` 判断，命中时改为 `detach()`（self-detach）。真实后果是：该线程被 detach、失去 join 语义，对象析构后若它仍回调即 UAF。
  3. 正确做法：回调里只置标志/投递消息，由外部线程调 `Stop()`。
  4. `Stop()` 之后不能再 `Start()` 同一个对象（线程已 join，状态未复位）。
- **常见错误答案**：「Stop 是异步的，回调里随便调」「加个锁就不会死锁」。

### Q18 【L2 概念】`--data-conns N` 的心跳是几份？服务端怎么把新流分到数据连接？

- **痛点**：以为数据连接只是「多开几条 TCP」，不知道服务端要轮询分配、每条连接各有心跳。
- **追查位置**：
  - `docs/modules/tunnel.md:57`（服务端按轮询把新流分配到数据连接）
  - `docs/modules/tunnel.md:63`（代价：多 N 条 TCP + N 个心跳）
- **验收标准**：
  1. N 条数据连接各自跑心跳（15 s PING / 90 s idle），所以是 N 份心跳流量。
  2. 服务端维护数据连接列表，新流按**轮询**分配；控制连接不承载 OPEN/DATA/CLOSE。
  3. 客户端 `data_connections()` 可查实际建立了几条；`data_conns=0` 表示只用控制连接。
  4. 数据连接断开不会让隧道立即下线（控制连接还在），但新流会分不到通道 → status 的 `data_connections` 掉。
- **常见错误答案**：「数据连接不需要心跳」「服务端随便挑一条」。

### Q19 【L1 概念】`with_checksum` 为什么必须两端一致？打开后多多少开销？

- **痛点**：单端打开，结果两端对帧边界的理解不一致，直接解析失败。
- **追查位置**：
  - `user/docs/14-tunnel.md:319`（`with_checksum` 两端必须一致，默认关；打开会多 4 B/帧）
  - `src/tunnel/mux.cpp:111`（`MakeTunnelFrameSpec(with_checksum)`：帧规格由配置决定）
- **验收标准**：
  1. 打开后在帧尾追加 CRC，帧长与解析规则都变；两端不一致 → 长度/边界错位，连接直接失败。
  2. 默认关闭；打开多 4 B/帧，小 DATA 帧开销比例不可忽略。
  3. 校验只防传输损坏，不防篡改（没有密钥），安全仍靠 PSK 鉴权 + 上层 TLS。
  4. 配置来自两端命令行，必须一起改（部署脚本要检查）。
- **常见错误答案**：「可以只在一端开，兼容」「CRC 能防篡改」。

### Q20 【L2 概念】tunnel 能水平扩展成多 VPS 集群吗？重启会丢什么？

- **痛点**：以为「隧道表在内存」只是实现细节，不知道它决定了部署形态。
- **追查位置**：
  - `docs/modules/tunnel.md:267`（单进程单实例：隧道表在内存里，重启即丢失；不支持多 VPS 集群共享路由表）
  - `user/docs/14-tunnel.md:287`（单进程单实例）
- **验收标准**：
  1. 单进程单实例；隧道注册表在内存，进程重启后所有隧道要客户端重连才能恢复。
  2. 多 VPS 时各自持有一份路由表，同一个子域名落到不同 VPS 会 404；没有共享存储/gossip。
  3. 要扩需要：外部路由表（Redis/etcd）+ 一致的子域名分配 + 健康检查（均未实现）。
  4. 重启时 drain 能减少影响，但不能避免「客户端必须重连」。
- **常见错误答案**：「多起几个实例就水平扩展了」「隧道表会持久化」。

---

## 难度分布自检

| 难度 | 题号 | 数量 |
|---|---|---:|
| L1 | Q02 / Q06 / Q15 / Q19 | 4 |
| L2 | Q03 / Q12 / Q13 / Q16 / Q18 / Q20 | 6 |
| L3 | Q01 / Q05 / Q07 / Q08 / Q09 / Q10 / Q14 | 7 |
| L4 | Q04 / Q11 / Q17 | 3 |

> 本卷**没有**「未实测」标记的题：所有数字来自 `docs/modules/tunnel.md` §6 与 153 项检查的功能验证；
> 唯一的口径限制是「WSL2 本机回环、8 核」，已在卷首与 Q11 写明，不能外推到公网。
