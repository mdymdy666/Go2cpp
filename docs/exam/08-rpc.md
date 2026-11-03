# Go2Cpp 理解力考试 · RPC 卷（08 rpc / 5 题）

> 覆盖：`GO2CPP_RPC_SERVICE` 的 ADL 展开、普通/流式 handler 的线程模型、客户端超时与服务端执行、
> 流式「背压」的真实位置、TCP 与 WebSocket 传输的边界，以及当前工作树里 RPC-over-picoquic 的消失。
> 出题依据：`user/docs/08-rpc.md`、`docs/modules/rpc.md`、`docs/quic.md`、`docs/testing.md`、
> `docs/reports/reuse-audit.md`、`AI_COLLAB.md`，以及**当前工作树源码**。
> 难度分布：L1 = 0、L2 = 0、L3 = 2、L4 = 3。
> **本卷数字均需自测，文档数字仅作历史对照。** 文档 §3.3 的「64 帧背压」与 §3.2 的
> `max_stream_frames = 1024` 自相矛盾，以源码为准（Q03 专考）。

---

## 一、注册与调用形态

### Q01 【L3 陷阱】`GO2CPP_RPC_SERVICE` 为什么必须写在服务类所在命名空间？宏最多支持几个方法？

- **痛点**：把宏当普通模板注册，写在全局或另一个命名空间，结果编译期静态断言；或照着注释
  「最多 8 个方法」设计，实际宏支持 12 个。
- **追查位置**：
  - `include/go2cpp/rpc/registry.hpp:25-27`（设计说明：宏展开成 `RpcMethods(ServiceType*)` 重载，
    `AddService` 用**无限定调用**找到它，靠 ADL）
  - `include/go2cpp/rpc/registry.hpp:476`（兜底 `RpcMethods(...)`，返回 `NoMethods`）
  - `include/go2cpp/rpc/registry.hpp:510-520`（`AddService`：`static_assert(!is_const_v<Service>)`；
    找不到方法表时用 `kDependentFalse` 给出「no rpc method table」）
  - `include/go2cpp/rpc/registry.hpp:592`（注释写「最多 8 个方法」）
  - `include/go2cpp/rpc/registry.hpp:603-675`（实际 `ENTRY_1..ENTRY_12` + `PICK`，支持 **12** 个）
  - `include/go2cpp/rpc/registry.hpp:671-675`（`#define GO2CPP_RPC_SERVICE` 生成
    `inline auto RpcMethods(ServiceType*)`）
- **验收标准**：
  1. 说清 ADL 机制：宏在服务类所在命名空间生成 `RpcMethods(Service*)`；`AddService` 里
     `RpcMethods(service)` 是无限定调用，只查实参类型的关联命名空间，全局命名空间不在其中。
  2. 说出写错位置的现象：找不到精确重载 → 匹配 `RpcMethods(...)` → `NoMethods` →
     `static_assert` 报「no rpc method table…」；不是链接错误。
  3. 说出实际容量是 12（`ENTRY_12` + `PICK` 的 12 个占位），注释里的 8 已过时；第 13 个方法会在
     预处理阶段报参数个数错误。
  4. 说出宏的其它约束：方法名必须是合法标识符（用 `#MethodName` 做线上名）；不能直接注册重载
     （`&Service::M` 有歧义）；服务类型必须非 const；要自定义线上名只能手写
     `Method<&Service::M>("Other")` + 显式 `Register`。
  5. 说出注册表在 `Listen` 时 `Freeze()`，之后再 `Register` 返回 false；查找路径无锁的前提是
     「冻结后只读」。
- **怎么自证**：`cmake --build .build-bento-wsl -j"$(nproc)" --target go2cpp_rpc_protocol_test && ./.build-bento-wsl/go2cpp_rpc_protocol_test`
  （注册/重名/非法名/冻结/5 种 handler 签名）。再写两个编译探针：把宏写在全局、服务类在
  `namespace demo` → 期望静态断言；写 13 个方法 → 期望预处理错误。把编译器完整报错贴进报告。
- **常见错误答案**：「宏就是特化，写哪都行」「最多 8 个方法」「ADL 会去全局找，所以全局也能注册」
  「重载方法可以用同一个名字注册」——零分。

---

## 二、线程模型与超时

### Q02 【L4 陷阱】客户端 5 ms 超时返回后，服务端在干什么？为什么同连接的下一个请求会排队？

- **痛点**：把「客户端超时」当成「服务端取消」；以为 Cancel 帧能中断普通 handler；不知道普通
  handler 跑在**读线程**上，慢 handler 会把同连接后续请求全部堵住。
- **追查位置**：
  - `src/rpc/server.cpp:401-410`（`HandleRequest` 快路径：非流式 `Process(request)` **在读线程直接执行**；
    只有流式才入队 worker）
  - `src/rpc/server.cpp:342-370`（`ReaderLoop`：读帧 → `HandleFrame` → 同一循环里执行 handler）
  - `src/rpc/server.cpp:428-445`（`HandleCancel` 只把 `shared_ptr<atomic<bool>>` 置 true）
  - `src/rpc/server.cpp:478-523`（`Process`：调用 `binding.invoke`，普通 handler 拿不到
    `ServerStream`，也没有检查 `request->cancelled`）
  - `src/rpc/client.cpp:275-300`（`Wait`：先自旋 100 µs，超时后 `RemovePending` + `SendCancel`，
    返回 `kDeadlineExceeded`）
  - `src/rpc/client.cpp:480-505`（`HandleResponse`：找不到 pending 就丢弃——迟到响应不会串到下一次）
  - `src/rpc/server.cpp:285-292`、`src/rpc/server.cpp:609-625`（`Join` 等 reader/worker；
    `Stop()` 逐个 `Join`，在 handler 里调 `Stop()` 会等自己）
- **验收标准**：
  1. 说清超时是**客户端单侧**决定：客户端删等待项、发 Cancel；服务端普通 handler 继续跑完，
     结果写回时客户端已无等待者，直接丢弃。
  2. 说出 Cancel 的真实作用域：只有流式 handler 能通过 `ServerStream::cancelled()` 观察；
     普通 handler 没有这个入口，也不检查取消标志。
  3. 推导同连接排队：handler 占着 reader 线程，后续 `kRequest` 的 `Read`/`Process` 都排在它后面；
     `Ping` 也在同一 reader 线程处理，所以慢 handler 会同时抬高心跳/超时。
  4. 说出「超时后恢复」测试为什么能过：`User.Slow` 只睡 100 ms，客户端 5 ms 返回后，下一个
     `User.Touch` 等到 handler 跑完仍能在 5 s 内成功；这不是取消生效，只是等到了。
  5. 说出 `Stop()` 的死锁条件：在 handler（reader 线程）里调 `Server::Stop()` → `Stop` 对当前
     连接 `Join()` 自己 → `std::system_error`/死锁；正确做法是另起线程或只置停止标志。
- **自证**：`ctest --test-dir .build-bento-wsl -R '^go2cpp_rpc_test$' --output-on-failure`，
  它包含 5 ms 超时 + 恢复 + 20 次连续超时。再加探针：服务端 `User.Slow` 睡 300 ms，客户端
  `timeout=5ms`；超时后立刻 `Ping()`，测 `Ping` 的返回时间 → 期望 ≈ 剩余 handler 时间（约 295 ms），
  证明 reader 被占。另起一条连接再 `Ping` → 期望立刻返回，证明阻塞是**每连接**的。
- **常见错误答案**：「客户端超时后服务端会收到取消并立刻停止 handler」
  「Cancel 帧能中断任何 handler」「每个请求一个线程，慢 handler 不影响别人」——零分。

---

## 三、流式与传输

### Q03 【L4 陷阱】流式 RPC 的「背压」到底在哪一侧？服务端 `SendSerialized` 会阻塞吗？

- **痛点**：照文档以为有 64 帧的协议窗口、服务端 Send 会阻塞等客户端消费；实际没有窗口/credit，
  服务端只管往 socket 写，客户端缓冲超限后**自己把流判死并 Cancel**。
- **追查位置**：
  - `include/go2cpp/rpc/client.hpp:134-135`（`max_stream_frames = 1024`、`max_stream_bytes = 16 MiB`
    的默认值）
  - `src/rpc/client.cpp:532-556`（`HandleStreamData`：`frames.size() >= max_frames` 或
    `buffered_bytes + payload > max_bytes` → `ended=true`、`kResourceExhausted`、`RemoveStream` +
    `SendCancel`）
  - `src/rpc/server.cpp:327-332`（`SendStreamData` 只做编码 + `SendPayload`，没有任何窗口检查）
  - `src/rpc/server.cpp:296-318`（`SendPayload` 只在 `m_write_mutex` 下 `WriteAll`；阻塞点是
    socket/TCP 发送缓冲，不是应用层背压）
  - `user/docs/08-rpc.md:120`（`max_stream_frames = 1024`）
  - `user/docs/08-rpc.md:144-145`（同一文档又写「64 帧背压上限，超过后服务端 Send 会阻塞或失败」）
  - `tests/rpc/test_rpc_stream.cpp:128-153`（测试显式 `options.max_stream_frames = 64`，断言
    `kResourceExhausted` 且 `received <= 64`）
- **验收标准**：
  1. 说清没有协议级流控：帧格式里没有 window/credit/ACK；服务端 `SendSerialized` 不会等客户端读。
  2. 说清真正的背压位置：客户端接收队列（默认 1024 帧 / 16 MiB）；超限时客户端单方面结束流、
     丢缓冲、发 Cancel；服务端下一次 `Send` 可能仍成功（直到 socket 满），但数据会被丢弃。
  3. 解释文档矛盾：`64` 来自测试里显式设置的 `max_stream_frames`，不是默认值；默认是 1024。
     「服务端 Send 会阻塞」也不成立，阻塞只可能发生在 TCP 发送缓冲满时。
  4. 说出对使用者的影响：慢消费者不会让服务端降速，只会让客户端丢流；需要真背压必须在应用层
     做 ack/credit，或把 `max_stream_frames` 调大并接受内存增长。
  5. 说出取消传播：客户端超限发 `kCancel`，服务端 `HandleCancel` 置标志，流式 handler 通过
     `ServerStream::cancelled()` 观察；普通 handler 无此能力。
- **怎么自证**：`ctest --test-dir .build-bento-wsl -R '^go2cpp_rpc_stream_test$' --output-on-failure`。
  再改探针：服务端每 1 ms 推 1 帧、客户端 `Next()` 前睡 2 s；把 `max_stream_frames` 设为 8，
  打印客户端 `Finish` 的 status 与服务端 `cancelled()` 计数 → 期望客户端 `kResourceExhausted`、
  服务端观察到 cancel；把 `max_stream_frames` 设为 100000，同样睡 2 s → 期望不丢流但内存上涨。
  用 `/proc/<pid>/status` 的 VmRSS 记录前后差值，报你测到的数。
- **常见错误答案**：「64 帧是协议窗口」「服务端 Send 会阻塞等客户端消费」「背压由 TCP 自动保证」
  「`max_stream_frames` 是服务端配置」——零分。

### Q04 【L3 陷阱】WebSocket 上「一条消息 = 一帧」，为什么 `Read` 还是不能假设一次拿到整帧？

- **痛点**：把 WS 的消息边界当成 RPC 帧边界，写一个「一次 `Read` 解一帧」的传输实现；或以为
  TCP 与 WS 的 `Transport` 语义完全一样。
- **追查位置**：
  - `include/go2cpp/rpc/websocket_transport.hpp:8-16`（适配规则：`WriteAll` 把这段字节作为**一条**
    二进制消息；`Read` 先消费当前消息剩余字节，读空再取下一条；帧层看到的仍是连续字节流）
  - `include/go2cpp/rpc/websocket_transport.hpp:121-160`（`Read` 的 `memcpy` 只拷
    `min(available, size)`；消息读空才 `read_message()`；空消息跳过）
  - `src/rpc/transport.cpp:274-300`（`TcpTransport::Read` 直接 `recv`，返回内核给多少算多少；
    EAGAIN 时 `poll` 1 s）
  - `include/go2cpp/rpc/transport.hpp:70-75`（`Transport` 契约：`WriteAll` 全写或失败；
    `Read` 返回值/0/-1 的语义；`Close` 唤醒阻塞读）
  - `src/rpc/client.cpp:440-455`（客户端 `FrameCodec::Feed` 处理半包/粘包，不依赖一次读满）
- **验收标准**：
  1. 说清 WS 的写侧保证：`WriteAll(data,size)` 发一条二进制消息；上层每次写一整帧，所以
     正常路径「一条消息 = 一帧」。
  2. 说清读侧不保证：`Read(buf,size)` 在 `size` 小于消息剩余长度时只返回一部分；调用方缓冲小、
     或未来一次写入多帧时，消息边界与帧边界不再一一对应。
  3. 说清 TCP 的差异：纯字节流，可能一次 `Read` 拿到半帧、也可能拿到多帧；两者都必须靠
     `FrameCodec` 的半包/粘包状态机。
  4. 说出 WS 的额外边界：文本消息也被当作字节流接受；`Close` 时 `read_message` 返回 Closed →
     `Read` 返回 0（EOF）；`max_message_payload` 默认 64 MiB，大于 RPC 的 4 MiB 帧上限。
  5. 说出线程约束：`Read` 只允许一个线程；`WriteAll` 由上层 `m_write_mutex` 串行化，
     不是靠 WS 自身的写锁。
- **自证**：`ctest --test-dir .build-bento-wsl -R '^go2cpp_rpc_websocket_test$' --output-on-failure`
  （真实 WS 握手 + 调用 + 流 + 心跳 + 关闭）。再写探针：把 `WebSocketTransport` 的 `Read` 缓冲
  设为 1 字节，喂一个完整 `kRequest` 帧 → 期望 `FrameCodec::Feed` 分多次才吐出一帧；
  再用 `TcpTransport` 一次写入两个帧 → 期望一次 `Feed` 吐出两帧。
- **常见错误答案**：「WS 是消息流，所以一次 Read 一定是一整条消息」「WS 不会粘包，所以不需要
  FrameCodec」「TCP 和 WS 的 Transport 可以互换而无需改上层」——零分。

---

## 四、集成史与陷阱

### Q05 【L4 陷阱】`ctest -N` 里还有 `go2cpp_rpc_picoquic_test`，源码里却找不到 `transport_picoquic.cpp`——哪个是真的？

- **痛点**：把构建目录当成源码事实；或者反过来，以为「ctest 里有就说明功能还在」，照着不存在的
  API 写代码。这是本项目「文档/构建产物不可信」的典型样本。
- **追查位置**：
  - `CMakeLists.txt:92-94`（只有一行 `# RPC over picoquic（QUIC 传输…）：见` 的**悬空注释**，
    下一行直接是存储模块；没有 `include(cmake/Go2CppPicoquic.cmake)`）
  - `src/rpc/`（只有 `client/protocol/registry/server/status/transport` 六个 .cpp，没有
    `transport_picoquic.cpp`）
  - `tests/rpc/`（只有 `bench_rpc`、`test_rpc`、`test_rpc_protocol`、`test_rpc_stream`、
    `test_rpc_websocket`，没有 `test_rpc_picoquic.cpp`）
  - `docs/testing.md:869-872`（历史记录：`go2cpp_rpc_picoquic_test` 独立复跑仍失败：
    `mux_reused=0`、256 KiB 往返、尾部 **`double free`**，不属当轮修复范围）
  - `docs/reports/reuse-audit.md:257`（把 T12.2 描述为
    `include/go2cpp/rpc/transport_picoquic.hpp` + `.cpp`（1397 行），客户端复用 T12.1、
    服务端自写 picoquic 泵）
  - `.build-bento-wsl/CTestTestfile.cmake:499`（残留的 `add_test(go2cpp_rpc_picoquic_test …)`）
- **验收标准**：
  1. 说出当前工作树的事实：RPC 只有 `TcpTransport` 和 `WebSocketTransport` 两种实现；
     RPC-over-QUIC 的源码与 CMake 入口都不在树里。
  2. 解释构建目录为什么还有：`.build-bento-wsl` 是在迁移代码存在时配置的；代码被删除/回退后
     没有重新 configure，所以 `CTestTestfile.cmake` 和二进制仍是旧的。`ctest -N` 读的是构建目录，
     不是源码。
  3. 说出历史坑：那次迁移的测试特征 `mux_reused=0`、256 KiB 往返、尾部 double free；这是
     审计/测试文档记录的失败，不是「已修好」。
  4. 说出正确判断顺序：先 `git status`/`ls`/`grep -rn` 确认源码，再看 `CMakeLists.txt` 是否
     include 对应 cmake，最后才看构建目录；任何「ctest 里有」都不等于「源码里有」。
  5. 指出如果今天要重做 RPC-over-QUIC，入口应是 `net::quic::PicoquicConnection` +
     新的 `rpc::Transport` 适配层，且必须先补齐服务端驱动（reuse-audit 的建议：抽
     `PicoquicServer`），不能直接复活旧文件。
- **怎么自证**：
  ```bash
  cd /UserData/CodexWorkSpace/Go2cpp
  ls src/rpc tests/rpc
  grep -rn 'transport_picoquic\|Go2CppPicoquic' --exclude-dir='.build*' . || echo 'NO SOURCE'
  grep -n 'picoquic' CMakeLists.txt
  ./.build-bento-wsl/go2cpp_rpc_picoquic_test --help 2>&1 | head
  ```
  期望：前三条显示源码/CMake 都没有；最后一条旧二进制仍能启动（证明构建目录是残留）。
  再把 `docs/testing.md:869-872` 的失败特征抄进报告，注明「历史记录，当前无源码可复现」。
- **常见错误答案**：「ctest 里有这个测试，所以功能存在」「文档写了 1397 行，代码就在某个分支」
  「`.build-bento-wsl` 是当前构建，可以代表 HEAD」——零分。
