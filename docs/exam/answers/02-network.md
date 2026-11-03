# 02 network 答卷（20 题）

> 环境：WSL2 / 12 vCPU / GCC 13.3。未跑 nginx 对比（见 Q08/Q09 的「没答出」）。行号均现场 `sed` 核对。

## Q01

**答**：ET 只在「状态变化」时产生一次边沿；没读到 EAGAIN，内核不会因为「还剩数据」再报一次。一次事件必须循环 `recv` 到 EAGAIN；读一半就返回，该 fd 在下一批数据到来前彻底静默。本实现注册带 `EPOLLONESHOT`，每次被唤醒后必须重新 arm；同一 fd 可能有多个等待者，重新 MOD 是为了让下一个等待者还能拿到边沿，而不是让第一个把事件吃掉。

**代码证据**：
```text
src/network/io.cpp:273   * | `m_backend` | epoll 实例（`EPOLLONESHOT`） |
src/network/io.cpp:1531  // EPOLLET：本轮边沿已被消费。同一 fd 还有等待者时，必须让内核
src/network/io.cpp:634   /// @brief EPOLLET 下同一 fd 还有等待者时必须强制 MOD 重新产生边沿。
```

**自证**：未写独立 epoll 复现；功能面 `ctest -R go2cpp_scheduler_smoke` passed（同进程 IO/hook 用例）。

**我没答出的**：没有用裸 epoll 写「读一半就停」的静默复现。

## Q02

**答**：Nagle 会等「未确认的小包」被 ACK 才发下一个小包，对端延迟 ACK 最多 ~40 ms，于是第二个小包被硬等；本库在 `Socket::accept()` 里按进程开关默认开 `TCP_NODELAY`。`SetTcpNoDelayDefault(false)` 只用于 A/B 对比。`TCP_CORK` 与 `TCP_NODELAY` 互斥：头体合包要临时 CORK，合完必须 uncork。

**代码证据**：
```text
src/network/socket.cpp:74   /// @brief `accept()` 新连接是否自动开启 TCP_NODELAY 的进程级开关。
src/network/socket.cpp:76   std::atomic<bool> g_tcp_nodelay_default{true};
src/network/socket.cpp:485-486  TcpNoDelayDefault() → (void)SetTcpNoDelay(accepted);
src/bento/net/http_server.cpp:100  // TCP_CORK 与 TCP_NODELAY 互斥...
```

**自证**：未做 40 ms 延迟复现（需禁用 NODELAY 的小包往返）；仅代码核对。

**我没答出的**：没有实测 Nagle + delayed ACK 的 40 ms。

## Q03

**答**：`close()` 先置 closing，再等 `m_inflight` 归零，最后才 `::close`；正在 `recv` 的 Fiber 持一次 `fetch_add` 计数，fd 不会被提前关/复用。计数只用一次原子 RMW；归零者在锁内把 `m_inflight_keepalive` 移出，避免每包一次原子提升。`Socket::fd()` 是瞬时值、不进在途保护，不能跨操作持有（ABA）。

**代码证据**：
```text
src/network/socket.cpp:224  bool Socket::begin_operation(int& fd) noexcept {
src/network/socket.cpp:225  const std::size_t prev = m_inflight.fetch_add(1, ...);
src/network/socket.cpp:272-274  // step1. m_inflight.fetch_sub(1)... step2. 归零时在 m_mutex 下把 m_inflight_keepalive 移到局部变量
```

**自证**：`ctest -R go2cpp_channel_concurrency` 等不直接覆盖；以代码路径为准。

**我没答出的**：没有写「recv 中 close」的 UAF 压测。

## Q04

**答**：`consumed` 是本次喂入的字节数（解析完成时取 `m_buffer.size()`），不是「已解析偏移」；一次 `recv` 可能含多条请求，parser 只交付第一条，剩余字节必须 `take_remaining()`（swap + reset）取走作为下一条输入，`serve_connection` 主循环正是这么做的。判断有没有下一条看 `pending` 是否为空。

**代码证据**：
```text
src/network/http.cpp:404  const auto consumed = m_buffer.size();
src/network/http.cpp:409  std::string Parser::take_remaining() { std::string result; result.swap(m_buffer); reset(); return result; }
src/network/http.cpp:716  pending = parser.take_remaining();
```

**自证**：`ctest -R 'http_e2e|go2cpp_http'` 未单跑；pipelines 逻辑以 716 行为准。

**我没答出的**：没有单独跑 pipeline 用例（`test_http_e2e`）。

## Q05

**答**：`HookOp` 枚举里**有** `kShutdown`（196 行），但 hook 的 dispatch 没有对应 park 分支——它只出现在 `IsNonDisableable()`（1762 行）的硬拒绝列表（close/shutdown/fcntl/dup/dup2 共 5 个），即「不可关闭、也不接管」，所以对端关闭只能靠 `read` 返回 0（EOF）。旧文档「22 个 HookOp 覆盖 shutdown」不准确：枚举有位、语义为空。Hook 开关是进程级全局，`SetOpEnabled(op,false)` 之后忘恢复会让后续 Fiber 静默退化，测试要用 RAII。

**代码证据**：
```text
include/go2cpp/network/hook.hpp:196  kShutdown,
src/network/hook.cpp:1761-1765
  1761  case HookOp::kClose:
  1762  case HookOp::kShutdown:
  1763  case HookOp::kFcntl:  ...  // 「不可关闭操作」
include/go2cpp/network/hook.hpp:279  bool SetOpEnabled(HookOp op, bool enabled) noexcept; // 全局
```

**自证**：`grep -n "case HookOp::kShutdown" src/network/hook.cpp` 只命中 1762（IsNonDisableable），无 dispatch 分支。

**我没答出的**：没有在 Hook=ON 构建下用 shutdown 事件做挂起复现。

## Q06

**答**：默认模型是独立 poller 线程发现就绪 → `wake_io` 把 G 放回 P 队列（跨线程唤醒一次往返约 54–56 µs，历史 WSL2 数据）；另一模型 `set_io_idle_poller` 让空闲 worker 自己在 M 上 poll，没有跨线程唤醒，适合高并发小包（Go netpoll 同构）。`wake_io_batch` 把多个就绪任务一次投递，减少跨线程次数。选型判据是「唤醒次数 × 54 µs」与「worker 空转 poll 的 CPU」的权衡。

**代码证据**：
```text
src/network/io.cpp:2178  (void)m_scheduler.set_io_idle_poller(...)
src/network/io.cpp:1971  (void)m_scheduler->wake_io_batch(tasks);
```

**自证**：未跑 poller 模型对比基准。

**我没答出的**：没有实测两种模型的延迟/CPU 差。

## Q07

**答**：`sendfile(2)` 没有 flags 参数，无法带 `MSG_NOSIGNAL`；对端关闭后继续发会向调用线程投递 SIGPIPE，默认动作是杀进程。库用 `ScopedSigpipeBlock` 做线程级阻塞，并在恢复掩码前 `sigtimedwait` 消费 pending，否则恢复瞬间仍会按默认动作投递。静态文件路径还要处理部分写循环、HEAD 不发 body、`Content-Length` 由 `file.length` 补齐；chunked 路径必须丢弃 `file`（长度语义冲突）。

**代码证据**：
```text
src/network/socket.cpp:41  /// @details `sendfile(2)` 没有 `MSG_NOSIGNAL` 参数...
src/network/socket.cpp:1058-1059  const ssize_t sent = ::sendfile(fd, in_fd, offset, count - total);
src/network/http.cpp:829  /// 文件体发送：open + sendfile 循环...
src/network/http.cpp:767  // chunked 流没有 sendfile 路径：文件体必须丢弃...
```

**自证**：`ctest -R go2cpp_http` 未单跑；用 `strace -c -e trace=sendfile` 复核需要真压测（未做）。

**我没答出的**：没有 strace 验证 1 MiB 一次 sendfile。

## Q08

**答**：pipeline 把「一次 recv 里的 N 条响应」在用户态攒批（`kWriteBatchBytes = 16 KiB`），攒到上限/必须阻塞读/遇到文件体才一次性写出，把 N 次 send、N 个 TCP 段压成 1 次；`TCP_CORK` 把「响应头 + 大文件」括起来合包，只在 ≥16 KiB 文件启用（小文件两次 setsockopt 抵不过省段）；`bufio::FlushBatch` 用一次 `writev` 把头+体两段写出。三者分别解决「每响应一次 syscall」「每响应一个 TCP 段」「每响应一次内核唤醒」。**仓库里没有 411k rps 这个数**，可复核的是文档口径下的 72.9k/8 连接 239.5k；本机未重跑。

**代码证据**：
```text
src/bento/net/http_server.cpp:505  // 偷自 nginx ngx_http_write_filter 的 postpone_output...
src/bento/net/http_server.cpp:509  constexpr std::size_t kWriteBatchBytes = 16 * 1024;
src/bento/net/http_server.cpp:71-73  // TCP_CORK 只对「头 + 大文件」划算...16 KiB 以上才启用
src/bento/net/http_server.cpp:99  #if defined(TCP_CORK)
```

**自证**：未跑；`asm-http-e2e.md` 的数字只作历史对照，本机没有 net_perf_server 的负载脚本复跑。

**我没答出的**：没有本机 pipeline 吞吐实测（时间/负载生成器未备）。

## Q09

**答**：「0.45–0.65×」是测量口径错误：cpp/Go 服务端 `taskset -c 2,3`，nginx 却跑在 0-11 任意核上，还会被 loadgen（`taskset -c 4-9`）抢核；跨轮还有负载污染（同一轮 nginx 10 KiB/32 冲到 80k、复测回落 50k）。`aa4b728` 用同口径（双方绑核、同轮次、记录 loadavg）重测得 1.02–2.97×。结论要落到方法：跨引擎对比必须双方绑核 + 同轮次 + 记录 loadavg。本机未复测 nginx。

**代码证据**：`nginx.conf` 与压测脚本在 `tests/network/`（`net_perf_server.cpp`、`http_loadgen.go`），本机未启动 nginx。

**自证**：未跑。

**我没答出的**：没有本机 nginx 同口径对比数据；这一题只能给方法论与历史证据链，不能给实测倍数。

## Q10

**答**：`max_requests` 是**公平性保护**，不是 bug：没有它，一条 keep-alive 连接可以长期占住 worker；默认 100，超过后服务端主动断开，客户端应重连（或压测改多连接）。pipeline 只影响「一次读进来几条」，不影响计数。需要长连接就显式传更大的 `max_requests` 并自担公平性风险。

**代码证据**（题面 `http.cpp:1465` 有误，主循环在 **1462**）：
```text
src/network/http.cpp:1419  * @param max_requests 本连接最多服务的请求数（防单连接饿死其它连接）。
src/network/http.cpp:1462  for (std::size_t i = 0; i < max_requests && !m_stopped...; ++i) {
src/network/http.cpp:1446  * - **`max_requests` 是公平性保护**...
```
> **题面错误**：Q10 的 `src/network/http.cpp:1465` 是循环体内的 `do {`，不是主循环上限；正确位置是 **1462**（本卷埋点）。

**自证**：未跑 101 请求掉线的 keep-alive 压测。

**我没答出的**：没有本机复现第 101 个请求被断开。

## Q11

**答**：`ResponseWriter::flush()` 只检查连接是否可写，不写任何字节（历史命名）；真正写出用 `write(chunk)` 或 `close()`。`start()` 强制 chunked 并丢弃 `Content-Length`；chunked 路径上 `Response::file` 必须丢弃。SSE 用 `sse::Stream::send(Event)` + `tick()` 心跳，`retry:` 控制客户端重连间隔；`Parser` 是有状态单消费者，并发 `feed()` 必须调用方加锁。

**代码证据**：
```text
src/network/http.cpp:813  bool ResponseWriter::flush() noexcept { return m_socket && m_state == StreamState::HeadersCommitted; }
src/network/http.cpp:810-812  // flush() 只用于查询"当前是否仍可写"，不执行任何系统调用；叫 flush 是历史命名（R8）
src/network/sse.cpp:149-150  if (retry_ms.has_value()) encoded.append("retry: ")...
src/network/http.cpp:767-769  // chunked 流没有 sendfile 路径：文件体必须丢弃
```

**自证**：`grep -n "retry" src/network/sse.cpp` 命中 149/150；未跑 SSE 端到端。

**我没答出的**：没有跑 `test_sse` 的 200 事件流。

## Q12

**答**：状态不是直接赋值：`RefreshFromEndFlags()` 用 `local_end`/`remote_end` 两个 bool 重算 open/half-closed(local)/half-closed(remote)/closed。`half-closed(remote)` = 对端已发 END_STREAM，**本端仍可发**；反过来亦然。所有事件方法是预检语义：非法转移返回 `kInvalidTransition` 且不改字段，所以 `CanSendData()` 之后再 `SendData()` 不会出现「校验失败但 END_STREAM 已被吃掉」。`kClosed` 之后所有事件返回 `kStreamClosed`。

**代码证据**：
```text
src/network/http2/stream_state.cpp:49-51  void StreamStateMachine::RefreshFromEndFlags() { if (m_local_end && m_remote_end) m_state = kClosed;
src/network/http2/stream_state.cpp:191  StreamEventResult StreamStateMachine::ReceiveHeaders(bool end_stream)
src/network/http2/stream_state.cpp:139  // step2. `CanSendData()` 为假 -> `kInvalidTransition`
```

**自证**：`ctest -R 'http2_state_desync_repro|http2_boundary'` 未跑；代码路径为准。

**我没答出的**：没有跑 HTTP/2 状态机的边界用例。

## Q13

**答**：HPACK 是有状态压缩：只要对端认为 header block 已被处理，动态表就必须更新；跳过解码会让后续索引整体偏移。正确顺序是「解完 header block → 判断并发超限 → 回 `RST_STREAM(REFUSED_STREAM)` → 不把流放进流表」。客户端侧在分配流号之前检查对端 `MAX_CONCURRENT_STREAMS`，被拒绝的尝试不消耗流号。`OpenStreamCount()` 只统计 open + 两个 half-closed。

**代码证据**：
```text
src/network/http2/connection.cpp:1197  // 注意此时 HPACK 已解码完毕，动态表保持同步，可以直接拒绝该流。
src/network/http2/connection.cpp:1200-1201  status = WriteFrame(MakeRstStream(headers.stream_id, ErrorCode::kRefusedStream), error);
src/network/http2/connection.cpp:501-502  if (enforce... && OpenStreamCount() >= m_peer_max_concurrent_streams)
```

**自证**：未跑并发超限用例。

**我没答出的**：没有构造 HPACK 动态表错位复现。

## Q14

**答**：两道闸：① `max_header_block_size`（压缩块上限，防 CONTINUATION 泛滥/单片 10 MB HEADERS），接收时边拼边累计；② `max_header_list_size`（解压后按 `name+value+32` 累计，防 HPACK 解压炸弹）。单片 HEADERS 不走 `AssembleHeaderBlock`，所以必须**独立**再检查一次压缩块上限，否则大单帧可绕过第一道闸。超限是连接错误。

**代码证据**：
```text
src/network/http2/connection.cpp:1117-1120  // 单片 HEADERS 走的是另一条路径（不经 AssembleHeaderBlock），所以这里必须独立再检查一次上限
src/network/http2/connection.cpp:1135-1140  // 第二道闸：解压后的大小...header_list_size += name.size()+value.size()+32
```

**自证**：未跑超大 header 用例。

**我没答出的**：没有构造 10 MB 单帧 HEADERS 验证绕过/拦截。

## Q15

**答**：HTTP/2 解决应用层队头阻塞（多路复用），但底层仍是单条 TCP，丢包时 TCP 层 HOL 仍在；HTTP/3 把可靠性/拥塞控制搬到用户态 QUIC，消除 TCP 层 HOL 与「换网要重连」。本项目 HTTP/3 是 picoquic 薄封装：默认 OFF、无 ctest、只有客户端、无 0-RTT/push/迁移、QPACK 用静态表。**「QUIC 不比 TCP 快」有实测但只在提交信息/`/tmp`**：`ab9ddb7` 说 p50 打平靠 busy_poll 2 核、p99 更差；`340d02a` 说丢包型 p99 20–47× QUIC 碾压、高 RTT 吞吐 1/3。两条不矛盾：丢包场景 QUIC 赢，无丢包低 RTT 场景 QUIC 输。

**代码证据**：`src/network/http3/client.cpp`（picoquic，默认不编）；`cmake/Go2CppModules.cmake` 中 `GO2CPP_WITH_PICOQUIC` 默认 OFF。

**自证**：未跑 QUIC；`grep -rn picoquic CMakeLists.txt cmake/` 可确认默认 OFF。

**我没答出的**：没有本机 QUIC vs TCP 实测。

## Q16

**答**：旧实现每次 `Write` 固定 4 次信号系统调用（阻塞 → SSL_write → sigpending/sigtimedwait → 恢复掩码）。快路径三级：①已挂 `MSG_NOSIGNAL` BIO → `required=false` 直接返回（0 syscall）；②进程已 `SIG_IGN` → 也跳过；③只有兜底才 `sigprocmask`，且只有失败才 `sigpending/sigtimedwait`。不能只恢复掩码：失败的 write 会让 SIGPIPE 处于本线程 pending，恢复瞬间投递并杀进程，必须先消费/确认没有 pending。

**代码证据**：
```text
src/network/http2/tls_transport.cpp:51  // 自定义 socket BIO：写走 send(MSG_NOSIGNAL)
src/network/http2/tls_transport.cpp:63  written = ::send(fd, data, len, MSG_NOSIGNAL);
src/network/http2/tls_transport.cpp:182-185  // 1. 进程已 SIG_IGN...2. SSL_* 成功 → 跳过 sigpending...class ScopedSigpipeBlock
src/network/http2/tls_transport.cpp:214  while (::sigtimedwait(&m_blocked, &info, &zero) == SIGPIPE)
```

**自证**：未跑 `strace -c` 统计 `rt_sigprocmask`（需要 TLS 压测脚本）。

**我没答出的**：没有本机 40 万次写的 strace 计数。

## Q17

**答**：默认 `verify_peer = true`，校验证书链 + 主机名（IP 字面量走 `X509_VERIFY_PARAM_set1_ip_asc`）；这是审计后从 false 改过来的 fail-closed。`verify_peer=false` 会**同时**关掉链校验与主机名校验，只用于自签本地测试。空 `server_name` 不设 SNI，也不做主机名匹配。TLS 只有客户端：没有 `SSL_accept`、没有会话复用/0-RTT/ticket。

**代码证据**：
```text
include/go2cpp/network/http2/tls_transport.hpp:22  * - **默认校验对端证书**（`verify_peer = true`）...
include/go2cpp/network/http2/tls_transport.hpp:102  * @param verify_peer 是否校验证书链与主机名，**默认 true（安全默认）**；
include/go2cpp/network/http2/tls_transport.hpp:121  Socket::Ptr socket, const std::string& server_name, bool verify_peer = true,
```

**自证**：代码核对；未起 TLS 服务端（库无 SSL_accept）。

**我没答出的**：没有自签证书 + 主机名不匹配的握手失败实验。

## Q18

**答**：**题面依据已过时**。`cmake/Go2CppModules.cmake` 的 `go2cpp_network` 源列表已经包含 `http2/{huffman,hpack,frame,stream_state,connection}.cpp`（206–224 行），注释明确写「此前只在测试目标里编译，导出库缺这些符号」。所以现在链接 `go2cpp_network` 就有 HTTP/2 符号，不再需要自己加源文件；仍然没有 `go2cpp_http2` 这个库目标，所以 `-lgo2cpp_http2` 依然会失败。文档 §7.2 的「未定义符号」是历史状态。

**代码证据**：
```text
cmake/Go2CppModules.cmake:206  add_library(go2cpp_network
cmake/Go2CppModules.cmake:218-224
  218  # HTTP/2 协议核心：此前只在测试目标里编译，导出库缺这些符号，生产
  219  # 使用需自行加源文件（见 docs/modules/http2.md §6）。
  220  .../huffman.cpp ... hpack.cpp ... frame.cpp ... stream_state.cpp ... connection.cpp)
```
> **题面/文档错误**：Q18 声称「五个 .cpp 只在测试 target 编译」，与当前 CMake 不符（已加入导出库）。这是本卷除 Q10 外的第二处需要指出的过期描述。

**自证**：`grep -rn "http2" cmake/*.cmake` 命中 206-224；`nm -D .build-bento-wsl/libgo2cpp_network.so | grep -c http2::` 可进一步验证（未跑）。

**我没答出的**：没有用 `nm` 确认导出符号。

## Q19

**答**：`Accept/Connect` 拿 `Socket::Ptr`，WebSocket 不拥有 fd；析构只 `abort_transport()`（代际取消唤醒读写），不 close fd，调用方负责 close。读方向单线程（`m_read_mutex`），写方向可多线程。`auto_pong` 默认 true；帧/消息上限 16 MiB/64 MiB；`validate_utf8` 默认 true。控制帧要求 FIN=1 且载荷 ≤125，否则协议错误。

**代码证据**：
```text
src/network/websocket.cpp:554-557  void WebSocket::abort_transport() ... // 不在这里直接 close fd，避免破坏仍由调用方共享的 Socket 所有权
src/network/websocket.cpp:856  std::lock_guard<sync::Mutex> lock(m_read_mutex);
src/network/websocket.cpp:672  if ((control && (!frame.fin || length > 125)) || length > m_config.max_frame_payload)
```

**自证**：`ctest -R 'websocket'` 未跑；代码为准。

**我没答出的**：没有跑 RFC 6455 的控制帧边界用例。

## Q20

**答**：MQTT 3.1.1、CoAP、SOCKS5 在 `bento::net`（`src/bento/net/{mqtt,coap,socks5}.cpp`），需要 `GO2CPP_WITH_BENTO=ON`，默认 OFF；`go2cpp::network` 只管 Socket/IO/Hook/HTTP1/HTTP2/TLS/WS/SSE/QUIC 薄封装。改协议前先 `grep -rn "namespace go2cpp::net" include/`，别在错误目录里找。

**代码证据**：
```text
src/bento/net/mqtt.cpp（48 KB，存在）
src/bento/net/coap.cpp（33 KB）
src/bento/net/socks5.cpp（46 KB）
```

**自证**：`ls -la src/bento/net/{mqtt,coap,socks5}.cpp` 三个文件均存在。

**我没答出的**：没有构建/跑 MQTT 服务端用例。
