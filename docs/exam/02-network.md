# Go2Cpp 理解力考试 · 网络数据面卷（02 network）

> **考的是「为什么 / 坏在哪 / 数据多少」**，不是背 API 名。
> 每题给出痛点、可复核的源码位置、验收标准与常见错误答案；只答对名词不给分。
> **口径**：答案若引用数字，必须能在下列项目内文档/提交里找到；查不到就说「未实测，只能定性」。
> 参考基线：`docs/reports/asm-http-e2e.md`、`docs/reports/asm-tls.md`、`docs/reports/2026-11-01-net-vs-go.md`、
> `docs/modules/{network,http1,http2,tls,ws,sse}.md`、`docs/quic.md`、`user/docs/02-network.md`，
> 以及提交 `aa4b728`（nginx 三策略）、`c10255c`（TLS SIGPIPE 快路径）、`4b50960`（旧 HTTP 端到端）。
>
> 难度分布：L1 = 4、L2 = 7、L3 = 6、L4 = 3。
> **注意**：网络流传的「HTTP/1.1 pipeline 411k rps」在仓库里**没有任何出处**（全库检索只命中构建日志），
> 可复核的数字是单连接 72.9k 与 8 连接 239.5k；把 411k 当结论写在卷子里视为不合格。

---

## 一、Socket / epoll / Hook

### Q01 【L1 概念】ET 模式下只读一半就回去等通知，会发生什么？

- **痛点**：把 `EPOLLET` 当成「有数据就通知」，漏掉「一次边沿必须读到 EAGAIN」这条约束，线上表现为某些连接永久不再被唤醒。
- **追查位置**：
  - `src/network/io.cpp:1531`（注释：EPOLLET 下本轮边沿已被消费，同一 fd 还有等待者时必须重新 MOD）
  - `src/network/io.cpp:634`（`EPOLLET 下同一 fd 还有等待者时必须强制 MOD 重新产生边沿`）
- **验收标准**（答到这些算过）：
  1. ET 只在「状态变化」时产生一次边沿，不是水平触发；没读到 EAGAIN，内核不会因为你「还剩数据」再报一次。
  2. 一次事件到来必须循环 `recv` 到 `EAGAIN`；读一半就返回的代价是该 fd 在下一批数据到来前彻底静默。
  3. 本实现的 epoll 注册带 `EPOLLONESHOT`（`src/network/io.cpp:273` 的字段表写明），所以**每次被唤醒后都要重新 arm**，否则「一次就绪」之后再无事件。
  4. 同一 fd 上可能有多个等待者，重新 MOD 是为了让下一个等待者还能拿到边沿，而不是让第一个把事件吃掉。
- **常见错误答案**：「内核发现没读完会再通知一次」「ET 只是性能选项，语义和 LT 一样」「读到 0 才算读完」。

### Q02 【L2 陷阱】`accept()` 为什么默认给新连接开 `TCP_NODELAY`？关掉会怎样？

- **痛点**：把 Nagle 当「省带宽的无害选项」，不知道它和延迟 ACK 组合会把小包往返压到约 40 ms。
- **追查位置**：
  - `src/network/socket.cpp:486`（`accept` 成功后按进程开关自动 `SetTcpNoDelay`）
  - `src/network/socket.cpp:74`（`accept()` 新连接是否自动开启 TCP_NODELAY 的进程级开关）
- **验收标准**：
  1. 说清机制：Nagle 会等「未确认的小包」被 ACK 才发下一个小包，对端延迟 ACK 最多等 ~40 ms，于是第二个小包被硬等。
  2. 本库在 `Socket::accept()` 里默认开（`SetTcpNoDelayDefault()` 为真时），业务不用自己设。
  3. `SetTcpNoDelayDefault(false)` 只用于对比实验，不能进生产；`docs/reports/2026-10-31-http-perf.md` §6.1 记录了这个 40 ms 现象。
  4. 知道 `TCP_CORK` 与 `TCP_NODELAY` 互斥：头体合包要临时 CORK，合完必须 uncork（`src/bento/net/http_server.cpp:100` 注释）。
- **常见错误答案**：「Nagle 只影响带宽不影响延迟」「默认是关的，要业务自己开」。

### Q03 【L2 陷阱】一个 Fiber 正在 `recv`，另一个 Fiber 调 `close()`，为什么不会 UAF？

- **痛点**：以为 `close` 是原子的、会打断阻塞调用；或者反过来，以为加一把互斥锁就解决了。
- **追查位置**：
  - `src/network/socket.cpp:224`（`begin_operation` 用一次 `fetch_add` 做在途计数）
  - `src/network/socket.cpp:273`（`end_operation` 归零时才把 keepalive 移出并延迟关闭）
- **验收标准**：
  1. `close()` 先把 closing 置位，再等 `m_inflight` 归零，最后才真正 `::close`；正在 `recv` 的 Fiber 持有一次计数，所以 fd 不会被提前关掉/复用。
  2. 计数只用一次原子 RMW（`fetch_add`），不是「每包一次 `shared_from_this` + 互斥锁」——后者是审查里被点名的固定税（`docs/reports/2026-11-01-network-app-opt-review.md` P0-2）。
  3. 归零者在锁内把 `m_inflight_keepalive` 移出，避免每包一次原子提升。
  4. `Socket::fd()` 是瞬时值、不进在途保护，拿到后可能被 close/复用（ABA），**不能跨操作持有**。
- **常见错误答案**：「给 recv 加锁就行」「close 会唤醒 recv 并返回 0」「fd 只要不 close 就一直有效」。

### Q04 【L1 概念】`Parser::feed` 的 `consumed` 到底是什么？pipeline 的第二条请求为什么必须用 `take_remaining()`？

- **痛点**：把 `consumed` 当「已消费偏移」，于是第二条请求的字节被丢掉，pipelining 直接坏掉。
- **追查位置**：
  - `src/network/http.cpp:404`（`consumed` 取 `m_buffer.size()`，即本次喂入字节数）
  - `src/network/http.cpp:409`（`take_remaining()` 用 `swap` 把剩余字节交出去并 reset）
- **验收标准**：
  1. `consumed` 返回的是**本次喂入的字节数**，不是「已解析到的偏移」；`http.hpp` 里「consumed 之前已交付」是过期注释。
  2. 一次 `recv` 可能含多条请求；parser 只交付第一条，剩余字节必须 `take_remaining()` 取走，作为下一条的输入。
  3. `serve_connection` 的主循环正是这么做的（`src/network/http.cpp:716`：`pending = parser.take_remaining()`），这也是 pipelining 能工作的前提。
  4. 判断「有没有下一条」看 `pending` 是否为空，不要靠猜缓冲区。
- **常见错误答案**：「consumed 是偏移，直接 erase 前 consumed 字节」「再 feed 一个空串就能拿到下一条」。

### Q05 【L2 陷阱】Hook 里 `shutdown` 从来没被接管，那「对端关闭」靠什么发现？

- **痛点**：旧文档写「22 个 HookOp 覆盖 shutdown」，实际没有；依赖 shutdown 事件等关闭会永久阻塞。
- **追查位置**：
  - `user/docs/02-network.md:289`（`shutdown` **从未被 hook**，旧文档误记）
  - `user/docs/02-network.md:327`（`SetOpEnabled` 是全局状态，必须 RAII 恢复）
- **验收标准**：
  1. 22 个 HookOp 里没有 `shutdown`；对端关闭检测只能靠 `read` 返回 0（EOF）。
  2. `http::Server::serve_connection` 不设 recv/write 超时，连接级超时由上层负责；如果业务等 shutdown 事件，会挂住。
  3. Hook 开关是进程级全局：`SetOpEnabled(op,false)` 之后忘了恢复，后续所有 Fiber 会静默退化（不 park、阻塞 worker）。
  4. 测试里必须用 RAII 包装恢复，不能靠人工记得。
- **常见错误答案**：「shutdown 被 hook 了，会唤醒等待者」「Hook 是每连接独立的，关掉不影响别人」。

### Q06 【L2 概念】默认 poller 线程模型与 worker 自己 poll，分别适合什么场景？

- **痛点**：只知道「epoll 很快」，不知道跨线程唤醒一次往返 54–56 µs，也不知道高并发小包该换模型。
- **追查位置**：
  - `src/network/io.cpp:2178`（`set_io_idle_poller` 注入空闲 worker 的 poll 回调）
  - `src/network/io.cpp:1971`（`wake_io_batch` 批量唤醒）
- **验收标准**：
  1. 默认模型：独立 poller 线程发现就绪 → `wake_io` 把 G 放回 P 队列，跨线程唤醒 **54–56 µs**（WSL2 实测，`user/docs/02-network.md:60`）。
  2. 另一模型：空闲 worker 自己 `poll`（`set_io_idle_poller`），没有跨线程唤醒，适合高并发小包，与 Go `netpoll` 同构。
  3. 批量唤醒（`wake_io_batch`）把多个就绪任务一次投递，减少跨线程次数。
  4. 选型判据是「唤醒次数 × 54 µs」与「worker 空转 poll 的 CPU」之间的权衡。
- **常见错误答案**：「epoll 只有一个线程，没有唤醒成本」「worker 自己 poll 一定更快」。

### Q07 【L3 陷阱】`sendfile(2)` 为什么也要处理 SIGPIPE？静态文件路径上最容易漏什么？

- **痛点**：只记得 `send/sendmsg` 加了 `MSG_NOSIGNAL`，忘了 `sendfile` 没有这个参数。
- **追查位置**：
  - `src/network/socket.cpp:41`（注释：`sendfile(2)` 没有 `MSG_NOSIGNAL`，对端关闭后继续发会收到 SIGPIPE）
  - `src/network/http.cpp:829`（文件体发送：`open` + `sendfile` 循环）
- **验收标准**：
  1. `sendfile` 没有 flags 参数，无法带 `MSG_NOSIGNAL`；对端关闭后继续发会触发 SIGPIPE，默认动作是杀进程。
  2. 正确做法是进程/线程级屏蔽（或忽略）+ 把 `EPIPE` 当写错误返回，而不是靠「不会发生」。
  3. 静态文件路径还要处理：部分写循环（`sendfile` 可能只写一部分）、HEAD 不发 body、`Content-Length` 由 `file.length` 补齐。
  4. `strace` 证据：1 MiB 由**一次** `sendfile` 完成，用户态没有 `read` 文件内容（`docs/reports/asm-http-e2e.md:100`）。
- **常见错误答案**：「sendfile 在内核里发，不会产生信号」「对端关闭会返回 0，不用管」。

---

## 二、HTTP/1.1 与测量方法

### Q08 【L3 陷阱】pipeline 为什么能把单连接从 8.9k 顶到 72.9k？三个 nginx 策略各解决什么？

- **痛点**：只会说「pipeline 省 RTT」，说不出服务端为 pipeline 做的三件事，也解释不了 8 连接 239.5k 的来源。
- **追查位置**：
  - `src/bento/net/http_server.cpp:505`（偷自 nginx `ngx_http_write_filter` 的 postpone_output 注释）
  - `src/bento/net/http_server.cpp:71`（`TCP_CORK` 只对「头 + 大文件」划算，阈值 16 KiB）
  - `src/bento/net/http_server.cpp:99`（CORK 打开/关闭的实现）
- **验收标准**：
  1. 数字口径：单连接串行 8.9k → pipeline=8 **53.7k**（6.0×）→ pipeline=16 **72.9k**（8.2×）；8 连接 pipeline=8 **239.5k**（`docs/reports/asm-http-e2e.md:127`）。原因是绕开了 WSL2 回环 ~110 µs 的 RTT，不是 CPU 变快。
  2. **postpone_output**：同一次 `recv` 里解析出的 N 条响应先攒在用户态（`kWriteBatchBytes = 16 KiB`），攒到上限/必须阻塞读/遇到文件体才一次性写出，把 N 次 `send`、N 个 TCP 段压成 1 次。
  3. **TCP_CORK**：把「响应头 + 文件体」括起来合包；只在 ≥16 KiB 文件启用，1 KiB/10 KiB 档实测回退 6–10%（`src/bento/net/http_server.cpp:71-73`），因为两次 `setsockopt` 抵不过省下的一个段。
  4. **pipeline 批量 flush**：`bufio` 的 `FlushBatch()` 用一次 `writev` 把头+体两段写出，而不是拼字符串再写。
  5. 三者解决的是不同层的固定税：每响应一次 syscall（批量 flush）、每响应一个 TCP 段（CORK）、每响应一次内核唤醒（postpone）。
- **常见错误答案**：「pipeline 就是把请求攒一起发，服务端没变化」「pipeline 一定更快，任何响应都该 CORK」。**特别注意**：背「411k rps」但说不出出处的，按未验证处理——仓库文档/提交里没有这个数字。

### Q09 【L4 陷阱】「0.45–0.65× nginx」错在哪？为什么同口径变成 1.02–2.97×？

- **痛点**：把测量方法的错误当成性能结论，对外宣称「比 nginx 慢一半」或「优化后翻倍」，两种都错。
- **追查位置**：
  - `docs/reports/asm-http-e2e.md:172`（nginx 1.24.0，2 worker，**未绑核**，affinity 0-11）
  - `docs/reports/asm-http-e2e.md:235`（报告自己承认：nginx 未绑核，跨引擎对比是既有口径，数据有噪声）
  - 提交 `aa4b728`（同口径 vs nginx **1.02–2.97×**；旧 0.45–0.65× 系 **nginx 未绑核 + 负载污染**）
- **验收标准**：
  1. 旧口径不公平：cpp/Go 服务端 `taskset -c 2,3`，nginx 却跑在 0-11 任意核上，还会被 loadgen（`taskset -c 4-9`）抢核。
  2. 跨轮负载污染有直接证据：`docs/reports/asm-http-e2e.md:208-210` 记录同一轮 nginx 10 KiB/32 冲到 80k、复测回落到 50k，说明同机有其它任务在跑；报告只能用「同一时间窗口重测」补救。
  3. 旧结论「0.45–0.65×」出自提交 `4b50960` 的口径；`aa4b728` 用同口径（双方绑核、同轮次、记录 loadavg）重测得到 **1.02–2.97×**。
  4. 结论要落到方法上：跨引擎对比必须「双方绑核 + 同一轮次 + 记录 loadavg」，否则数字只能当噪声。
- **常见错误答案**：「优化了 nginx 三策略所以快了 2 倍」（主因是修测量）、「0.45–0.65× 说明我们就是慢」。

### Q10 【L3 陷阱】`max_requests=100` 是 bug 还是设计？keep-alive 压测到 100 个请求就掉线怎么解释？

- **痛点**：压测脚本跑到第 101 个请求连接被关，误判为连接泄漏或 keep-alive 坏了。
- **追查位置**：
  - `src/network/http.cpp:1419`（`max_requests` 是本连接最多服务的请求数，防单连接饿死其它连接）
  - `src/network/http.cpp:1465`（主循环上限 `max_requests`）
- **验收标准**：
  1. 这是**公平性保护**：没有它，一个 keep-alive 连接可以长期占用 worker，其它连接排不上。
  2. 默认 100；超过后服务端主动断开，客户端应重连（或把压测改成多连接）。
  3. 与 pipeline 的关系：pipeline 只影响「一次读进来几条」，不影响 `max_requests` 计数。
  4. 如果业务需要长连接，显式传更大的 `max_requests`，并自己承担公平性风险。
- **常见错误答案**：「100 是硬编码 bug」「pipeline 会把 100 个请求一次算完所以不会断」。

### Q11 【L2 概念】`ResponseWriter::flush()` 为什么「不写字节」？SSE 该用什么？

- **痛点**：名字叫 flush，以为是「把缓冲推出去」，结果 SSE 客户端收不到事件。
- **追查位置**：
  - `user/docs/02-network.md:299`（`ResponseWriter::flush()` 不写字节，只检查可写，历史命名）
  - `src/network/sse.cpp:149`（`retry:` 字段输出）
- **验收标准**：
  1. `flush()` 只检查连接是否可写，不写任何字节；真正写出用 `write(chunk)` 或 `close()`。
  2. `ResponseWriter::start()` 强制 chunked、丢弃 `Content-Length`；`Response::file` 在 chunked 路径上必须丢弃，否则长度语义冲突（`src/network/http.cpp:767`）。
  3. SSE 用 `sse::Stream::send(Event)` + `tick()` 维持心跳；带 `retry:` 控制客户端重连间隔（`src/network/sse.cpp:71` 解析、`:150` 输出）。
  4. SSE `Parser` 是有状态单消费者，并发 `feed()` 必须调用方加锁。
- **常见错误答案**：「flush 之后客户端就能收到」「SSE 就是 chunked，不用管 retry/id」。

---

## 三、HTTP/2 / HTTP/3 / TLS

### Q12 【L3 陷阱】HTTP/2 的 half-closed(remote) 到底还能不能发 DATA？状态机漏了哪一步会 RST？

- **痛点**：把 half-closed 当「已关闭」，或者反过来在 closed 之后继续发，被对端 RST_STREAM。
- **追查位置**：
  - `src/network/http2/stream_state.cpp:49`（`RefreshFromEndFlags`：两个 bool 重算七状态）
  - `src/network/http2/stream_state.cpp:191`（`ReceiveHeaders/ReceiveData` 的对端方向转移）
- **验收标准**：
  1. 状态不是直接赋值：`local_end` / `remote_end` 两个 bool 决定 `open` / `half-closed(local)` / `half-closed(remote)` / `closed`（`stream_state.cpp:49-59`）。
  2. `half-closed(remote)` = 对端已发 END_STREAM，**本端仍可发**；`half-closed(local)` 反之。
  3. 所有事件方法是**预检语义**：非法转移返回 `kInvalidTransition` 且不改任何字段，所以 `CanSendData()` 之后再 `SendData()` 不会出现「校验失败但 END_STREAM 已被吃掉」。
  4. `kClosed` 之后所有事件返回 `kStreamClosed`；在 closed 流上发 DATA 会被对端 RST（本端也应早拒）。
- **常见错误答案**：「half-closed 就是关了，不能再发」「状态机只是枚举，随便赋值」。

### Q13 【L3 陷阱】并发流超限时，为什么必须「先解完 HPACK 再回 RST_STREAM」？

- **痛点**：为了省事直接丢弃超限的 HEADERS 帧，结果 HPACK 动态表两边不同步，后续所有 header 解析全错。
- **追查位置**：
  - `src/network/http2/connection.cpp:1197`（注释：此时 HPACK 已解码完毕，动态表保持同步，可以直接拒绝该流）
  - `src/network/http2/connection.cpp:1200`（回 `RST_STREAM(REFUSED_STREAM)`）
- **验收标准**：
  1. HPACK 是**有状态压缩**：只要对端认为这条 header block 已被处理，动态表就必须更新；跳过解码会让后续索引整体偏移。
  2. 正确顺序：解完 header block → 判断并发数超限 → 回 `REFUSED_STREAM` → **不把流放进流表**。
  3. 客户端侧在**分配流号之前**检查对端 `MAX_CONCURRENT_STREAMS`，被拒绝的尝试不消耗流号（`connection.cpp:498`）。
  4. `OpenStreamCount()` 只统计 open + 两个 half-closed，排除 closed。
- **常见错误答案**：「超限就直接丢帧，省内存」「RST 之后再补解 HPACK 也行」。

### Q14 【L3 陷阱】为什么要两道 header 上限？只设 `max_header_list_size` 会漏什么？

- **痛点**：以为「解压后限制总大小」就够了，结果被 CONTINUATION 泛滥或单片 10 MB HEADERS 打爆。
- **追查位置**：
  - `src/network/http2/connection.cpp:1116`（单片 HEADERS 不走 AssembleHeaderBlock，必须独立再检查一次压缩块上限）
  - `src/network/http2/connection.cpp:1135`（解压后 header list 上限，防 HPACK 解压炸弹）
- **验收标准**：
  1. 第一道 `max_header_block_size`（64 KiB）防 CONTINUATION 泛滥：压缩块本身撑爆内存；接收时必须**边拼边累计**，不能等收完再判。
  2. 第二道 `max_header_list_size`（64 KiB）防 HPACK 解压炸弹：压缩块很小、解压后极大（每项按 `name+value+32` 计）。
  3. 单片 HEADERS 不经过 `AssembleHeaderBlock`，所以必须**独立**再检查一次，否则一个 10 MB 单帧可绕过第一道闸。
  4. 超限是连接错误，不是流错误。
- **常见错误答案**：「只设一个总上限就够」「单片 HEADERS 也走同一个拼装函数」。

### Q15 【L4 陷阱】HTTP/2 与 HTTP/3 的分工是什么？「QUIC 不比 TCP 快」的边界在哪？

- **痛点**：把 HTTP/3 当 HTTP/2 的升级替代，或者以为「UDP 上的 QUIC 一定更快」；被追问「快多少」时编数字。
- **追查位置**：
  - `docs/modules/http2.md:36`（范围：h2c 与客户端 h2 over TLS；明确不做 QUIC/HTTP3）
  - `docs/quic.md:127`（HTTP/3 能力边界：无 0-RTT、无 push、无迁移，QPACK 只用静态表）
- **验收标准**：
  1. 分工：HTTP/2 解决**应用层**队头阻塞（多路复用），但底层仍是单条 TCP，丢包时 TCP 层 HOL 仍在；HTTP/3 把可靠性/拥塞控制搬到用户态 QUIC，消除 TCP 层 HOL 与「换网要重连」。
  2. 本项目的 HTTP/3 是 **picoquic 薄封装**：默认 OFF、无 ctest、只有客户端、无 0-RTT/push/迁移、QPACK 不用动态表（`docs/quic.md:127-134`）。它不是自研 QUIC，旧的自实现已在 2026-10-18 删除（`docs/quic.md:189-199`）。
  3. 「QUIC 不比 TCP 快」的边界：无丢包、低 RTT、单流大块时，QUIC 的用户态每包处理与更大头部反而更贵；优势在丢包、换网（连接迁移）、多流并发不互相阻塞。
  4. **诚实结论 + 一个真实的坑**：`docs/quic.md:228` 只说「无 ctest、验收只在 /tmp」，**但实测是存在的，只是没归档进 docs/**——证据在提交 `340d02a`（11-02 09:08）的提交信息里：
     「QUIC vs TCP 弱网公平对照（内核 netem 同注入器）——丢包型 p99 20-47x QUIC 碾压 / 高 RTT 吞吐 1/3 / 抖动 4-5x 冗余重传」。
     另有 `ab9ddb7` 的诚实结论「不比 TCP 快（p50 打平靠 busy_poll 2 核，p99 更差）」。
     **答这题的正解是**：说出「有实测但只在提交信息/`/tmp`，没进 docs/reports」——这正是本项目被反复批评的证据链断裂；同时要指出两条结论并不矛盾：
     丢包场景 QUIC 赢（p99 20-47×）、无丢包低 RTT 场景 QUIC 输（p50 靠 busy_poll 才打平）。
     只背「不比 TCP 快」或只背「20-47× 碾压」都算不合格。
- **常见错误答案**：「HTTP/3 用 UDP 所以一定快」「QUIC 就是加密的 TCP」「我们有 HTTP/3 所以比 nginx 强」。

### Q16 【L4 陷阱】TLS 写路径为什么能省掉 40 万次 `sigprocmask`？只恢复掩码为什么不够？

- **痛点**：只知道「屏蔽 SIGPIPE」，不知道旧实现每次写要 4 次信号系统调用，也不知道恢复掩码的瞬间会投递 pending 信号。
- **追查位置**：
  - `src/network/http2/tls_transport.cpp:51`（自定义 socket BIO 写走 `send(MSG_NOSIGNAL)`，从源头不产生 SIGPIPE）
  - `src/network/http2/tls_transport.cpp:185`（`ScopedSigpipeBlock` 的三级快路径）
- **验收标准**：
  1. 旧实现每次 `Write` 固定 4 次信号系统调用：`pthread_sigmask(SIG_BLOCK)` → `SSL_write` → `sigpending`+`sigtimedwait` → `pthread_sigmask(SIG_SETMASK)`。
  2. 快路径三级：①已挂 `MSG_NOSIGNAL` BIO → `required=false` 直接返回（0 次系统调用）；②进程已 `SIG_IGN` → 也跳过；③只有兜底才 `sigprocmask`，且只有失败（`m_failed`）才 `sigpending`/`sigtimedwait`。
  3. 实测：40 万次小包写（其中 TLS 20 万次）只剩 **15 次** `rt_sigprocmask`、`rt_sigpending`/`rt_sigtimedwait` 为 0（`docs/reports/asm-tls.md:151-162`）；按旧口径本该至少 20 万 + 20 万 + 40 万次。信号系统调用降了 4 个数量级。
  4. 为什么不能只恢复掩码：失败的 `write` 会让 SIGPIPE 处于「本线程 pending」，恢复掩码的瞬间投递并杀进程（`docs/modules/tls.md:191`）。必须先消费/确认没有 pending 再恢复。
- **常见错误答案**：「`signal(SIGPIPE, SIG_IGN)` 一把梭就完事」「屏蔽了就不会有 pending 信号」。

### Q17 【L1 概念】`TlsTransport` 默认校验对端证书吗？关掉它同时关了什么？

- **痛点**：把安全默认记反（以为默认不校验），或者以为 `verify_peer=false` 只是「跳过链校验」。
- **追查位置**：
  - `include/go2cpp/network/http2/tls_transport.hpp:22`（默认校验对端证书，`verify_peer = true`）
  - `user/docs/02-network.md:184`（默认 true，审计后从 false 改过来的 fail-closed）
- **验收标准**：
  1. 默认 `verify_peer = true`，校验证书链 + 主机名；这是审计后从 `false` 改过来的。
  2. `verify_peer=false` 会**同时**关掉链校验和主机名校验，只用于自签名本地测试。
  3. 空 `server_name` 不设 SNI，也不做主机名匹配。
  4. TLS 只有客户端：没有 `SSL_accept` 路径、没有会话复用/0-RTT/ticket。
- **常见错误答案**：「默认不校验，方便测试」「关掉只影响链，主机名还会校验」。

### Q18 【L4 陷阱】HTTP/2 核心实现曾经不在 `go2cpp_network` 里，加入时为什么是并库而不是新建目标？

- **痛点**：只看当前代码，不知道它曾缺库；也答不出「修好之后还剩什么隐患」。
- **追查位置**：
  - `cmake/Go2CppModules.cmake:217-224`（五个 `.cpp` 已并入 `go2cpp_network`，注释原文：
    「HTTP/2 协议核心：此前只在测试目标里编译，导出库缺这些符号，生产使用需自行加源文件」）
  - `docs/modules/http2.md` §6（当时记录的缺符号问题）
  - `user/docs/02-network.md:290,331,340`（**已被本次修复作废，仅作历史记录**）
- **验收标准**（答到这些算过）：
  1. 说清「曾经缺库」的表现形态：**未定义符号**（不是找不到头文件），且加 `-lgo2cpp_http2` 无效——没有这个目标。
  2. 说清修复方式：把 `huffman/hpack/frame/stream_state/connection` 五个 `.cpp` **并入 `go2cpp_network` 的源列表**，
     而不是新建 `go2cpp_http2` 库——因为它与 `http2/tls_transport.cpp` 同属一个传输层，拆开会造成符号分散。
  3. 指出加进去之后**仍未解决**的两件：`hpack.cpp` 的 unused-function 告警（阻塞零警告目标）；
     HTTP/2 服务端与 h2-over-TLS 全链路缺对端（nginx/nghttpd）验证。
  4. 能指出这条漂移的教训：文档记的是「修复前状态」，代码修好后文档没改 →
     后来人会照着文档写出错误的构建命令、或误判「这个功能不可用」。
- **常见错误答案**：「加 `-lgo2cpp_http2` 就好」「undefined symbol 是 ABI 不兼容」
  —— 都是当年的答案，现在连这个库目标都不存在。

### Q19 【L2 概念】WebSocket 的析构会关闭 fd 吗？读写方向各能几个线程？

- **痛点**：以为 `WebSocket` 拥有连接、析构即关闭；实际只 abort transport，fd 由调用方管。
- **追查位置**：
  - `src/network/websocket.cpp:554`（`abort_transport()`：只中止，不 close fd）
  - `user/docs/02-network.md:301`（所有权：析构只 abort，不 close fd；读单线程、写多线程）
- **验收标准**：
  1. `Accept/Connect` 拿的是 `Socket::Ptr`，WebSocket 不拥有 fd；析构只 `abort_transport()`，调用方负责 close。
  2. 读方向单线程（`m_read_mutex`），写方向可多线程。
  3. `auto_pong` 默认 true；帧/消息上限默认 16 MiB / 64 MiB；`validate_utf8` 默认 true。
  4. 控制帧限制：FIN 必须置位、载荷 ≤125，否则协议错误（`src/network/websocket.cpp:672`）。
- **常见错误答案**：「析构会 close fd」「读写都只能一个线程」。

### Q20 【L1 概念】MQTT / CoAP / SOCKS5 在哪个模块？为什么在 `network/` 下找不到？

- **痛点**：在 `src/network/` 里翻半天，或者以为「网络模块=全部协议」。
- **追查位置**：
  - `user/docs/02-network.md:28`（MQTT 3.1.1、CoAP、SOCKS5 在 `bento::net`，需 `GO2CPP_WITH_BENTO=ON`）
  - `src/bento/net/mqtt.cpp`（真实实现所在）
- **验收标准**：
  1. MQTT/CoAP/SOCKS5 以及 HTTP 服务端的一部分在 `bento::net`（`src/bento/net/{mqtt,coap,socks5}.cpp`），不在 `go2cpp::network` 命名空间。
  2. 打开 `GO2CPP_WITH_BENTO=ON` 才能用；默认 OFF。
  3. `network/` 只管 Socket/IO/Hook/HTTP1/HTTP2/TLS/WS/SSE/QUIC 薄封装。
  4. 改协议前先 `grep -rn "namespace go2cpp::net" include/`，别在错误目录里找。
- **常见错误答案**：「都在 network，bento 只是上层应用」「MQTT 没实现」。

---

## 难度分布自检

| 难度 | 题号 | 数量 |
|---|---|---:|
| L1 | Q01 / Q04 / Q17 / Q20 | 4 |
| L2 | Q02 / Q03 / Q05 / Q06 / Q11 / Q18 / Q19 | 7 |
| L3 | Q07 / Q08 / Q10 / Q12 / Q13 / Q14 | 6 |
| L4 | Q09 / Q15 / Q16 | 3 |
