# 模块文档：relay-pipe（RelayPipe 双向字节中继）

> **状态：现行（2026-11-01）**。对应 `collab/jinshan-requests.md` **REQ-002**。
> 引用约定：`file:line` 均指当前工作树；代码注释优先于本文，本文只解释设计取舍与验收数据。

---

## 1. 概述与职责

`RelayPipe` 是**通用双向字节中继**：把两条已经配对好的双向流（A、B）互相透传，
直到两端 EOF、某一方向失败或上下文取消。它只搬字节，**不解析、不重排、不改写**。

| 做 | 不做 |
|---|---|
| A ↔ B 双向透传，各自独立读写 | 不做配对/路由（JinShan 的 Hub 负责） |
| 有界队列 + 背压，内存不随流量增长 | 不解析 RDRL/RDCH 等协议语义 |
| 写 deadline、慢 peer 隔离、统计 | 不做重试、加密、压缩、帧重组 |
| 可接 Socket / TLS / 内存管道（`Stream` 抽象） | 不拥有连接生命周期（只借用指针） |

定位：位于 `go2cpp::streams`（Read/Write/Close 原语）之上、业务 relay 之下；
依赖 `go2cpp::Error`（错误值）与 `go2cpp::Context`（取消传播），不依赖 Bento/App。

---

## 2. 文件布局

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/relay/relay_pipe.hpp` | `Stream`、`PipeConfig`、`PipeStats`、`ErrTimeout()`、`RelayPipe` | 244 行 |
| `src/relay/relay_pipe.cpp` | 有界队列、双方向读/写线程、deadline 看门狗、统计与取消 | 850 行 |
| `tests/relay/test_relay_pipe.cpp` | 8 组行为/随机/压测（自带 main） | 587 行 |
| `tests/relay/relay_test_support.hpp` | 内存 `TestStream`、确定性模式、`AsyncRun` | 334 行 |
| `cmake/Go2CppRelayPipe.cmake` | `go2cpp_relay_pipe` 库 + `relay_pipe` ctest 接入 | 73 行 |

构建入口：顶层 `CMakeLists.txt` 在 `Go2CppModules.cmake` 之后直接 include
`cmake/Go2CppRelayPipe.cmake`，创建独立的 `go2cpp_relay_pipe` 目标；Hub/Lease
走 `cmake/Go2CppRelayHubLease.cmake` 的 `go2cpp_relay` 目标，两者互不覆盖。
之所以不并进同一个库：relay 目录由两条并行工作流维护，独立目标可以避免
「Hub 源码尚在编辑时拖垮 RelayPipe 编译」；合并时把 `relay_pipe.cpp` 加进
`go2cpp_relay` 即可（见该 CMake 文件顶部注释）。

---

## 3. API 表

### 3.1 `Stream`（`relay_pipe.hpp:136`）

```cpp
class Stream : public streams::Reader, public streams::Writer, public streams::Closer {
 public:
  ~Stream() override = default;
};
```

直接复用项目已有的三个流接口（`ReadResult`/`WriteResult` 保留 Go 的
「允许 `count > 0` 同时带错误」契约）。任何实现这三件事的类都可以零成本适配：
socket、TLS、内存管道、测试桩。

**必须满足的三条契约**（否则超时/取消无法收回线程）：

1. `Read` 阻塞到至少 1 字节、EOF 或错误；允许 `count > 0` 同时带错误；
2. `Write` 允许短写（`count < size` 且无错误），RelayPipe 会循环写满；
3. `Close` 幂等、可从其它线程调用，且调用后阻塞中的 `Read`/`Write` 必须尽快返回；
   `Close` 自身不得无限阻塞。

### 3.2 `PipeConfig` / `PipeStats`

| 字段 | 默认 | 语义 |
|---|---|---|
| `copy_buf` | 32768 | 单次 Read 缓冲，也是单个 chunk 上限 |
| `queue_cap` | 262144 | 队列容量（按字节），满则背压 |
| `write_timeout` | 30000 ms | 单 chunk 写 deadline + 队列持续满预算 |

归一化：`copy_buf==0 → 32 KiB`，`queue_cap==0 → 256 KiB`，
`write_timeout<=0 → 30 s`；`queue_cap < copy_buf` 时提升到 `copy_buf`。

`PipeStats` 四个字段：`a_to_b`、`b_to_a`、`dropped`、`timeouts`（见 §6）。

### 3.3 `RelayPipe`

| 方法 | 语义 |
|---|---|
| `RelayPipe(Stream* a, Stream* b, PipeConfig cfg)` | 绑定两端、归一化配置；不启动线程 |
| `bool Run(const ContextPtr& ctx, ErrorPtr* err)` | 阻塞转发，直到两端 EOF / 失败 / 取消 |
| `bool Run(ErrorPtr* err)` | 使用 `Context::Background()` 的便捷重载 |
| `PipeStats Stats() const` | 原子读快照，运行期可调用 |
| `const PipeConfig& Config() const` | 归一化后的配置 |

`Run` 返回 `true` 仅当**两个方向都干净 EOF**；任一方向失败/超时/取消返回 `false`，
`*err` 是第一条错误。`Run` 只允许调用一次；重复/并发调用返回
`relay: Run already called`。

与 Go 签名的 C++ 适配：Go 的 `error` 对应 `ErrorPtr`，因此输出参数是
`ErrorPtr*`；`context.Context` 对应 `ContextPtr`（`Context` 不可拷贝）。
另有哨兵 `ErrTimeout()`，实现返回的是 `Wrap(ErrTimeout(), "relay: a->b write")`
这类包装值，用 `Is(err, ErrTimeout())` 判断，不要比较文本。

---

## 4. 设计

### 4.1 线程模型

```
A --Read--> [reader A→B] --> bounded queue --> [writer A→B] --Write--> B
A <--Write-- [writer B→A] <-- bounded queue <-- [reader B→A] <--Read-- B
```

- 两条方向各 2 条工作线程（读 + 写），共 4 条；
- 另 1 条低频看门狗线程（5 ms 轮询）检查写 deadline；
- 方向之间只共享原子统计与取消标志，队列/错误/生命周期独立；
- 一条方向阻塞在 `Push` 或 `Write` 不会占用另一方向的线程。

### 4.2 有界队列与背压

`ChunkQueue`（`relay_pipe.cpp` 内）按**字节**计容量：`m_bytes + chunk.size() > capacity`
就阻塞等待。内存上界是 `queue_cap + 一个正在写的 chunk + copy_buf`，不会随流量增长。

慢 peer 判定用的是**「队列持续满 + 一个队列容量在预算内没有排空」**：

- 首次发现队列满时记录 `m_stall_start`；
- 只要后续 `Push` 还需要等待，就沿用同一个起点；
- 期间消费者每 `Pop` 一个 chunk，累计 `m_drained_since_stall`；
- 已排空 ≥ `queue_cap` → 说明链路仍在持续排水，重置计时（健康的低速链路
  只是队列长期满，不应被误杀）；
- 持续满 ≥ `write_timeout` 且排空量不足一个容量 → `kStalled`，读线程关闭目的端、
  方向失败、`timeouts+1`；
- 消费者排空队列（`Pop` 后为空）或生产者一次 `Push` 完全没等待 → 清除计时。

为什么不能只用「单次等待时长」：一个每 30 ms 只读 4 KiB 的 peer 会让每次等待
都在 30 ms 内成功，单次永远不超时；但队列持续满，实际吞吐只有 133 KiB/s。
为什么还要看「排空一个容量」：100 Mbps 的长传也会让 256 KiB 队列长期满，但
它 20 ms 就能排空一个容量，远小于 30 s 预算，不应该被断开。

举例：`queue_cap=32 KiB`、`write_timeout=100 ms`，peer 每 30 ms 读 4 KiB。
一个容量需要 240 ms 才能排空，超过 100 ms 预算 → 约 100 ms 后判定为慢 peer
（测试 2 三次实测 100/101/101 ms）。

### 4.3 写 deadline

- 每个 chunk 一个绝对 deadline（开始写该 chunk 的时刻 + `write_timeout`）；
- chunk 内多次短写共用同一 deadline，避免「每次短写重新计时」让慢 drip 写无限延长；
- 写线程每次 `Write` 前登记 `(active, deadline)`，看门狗发现
  `deadline <= now` 时：标记 `timed_out`、`timeouts+1`、关闭**目的端**；
- 写线程在 `EndWrite` 看到 `timed_out` 后走失败路径，记录错误、丢弃未写数据。

`timeouts` 每个方向最多计一次（`Direction::NoteTimeout` 的 CAS），避免
「写超时」与「队列停滞」同时发生被重复统计。

### 4.4 慢 peer 隔离与方向独立性

失败路径只做**该方向**需要的关闭：

| 事件 | 关闭谁 | 统计 |
|---|---|---|
| 写 deadline / 队列停滞 | 该方向**目的端**（慢 peer） | `timeouts+1`，未写字节进 `dropped` |
| 写失败（非超时） | 该方向目的端 | 错误进 `err`，未写字节进 `dropped` |
| 读失败 | 不关闭目的端 | 错误进 `err` |
| 读线程正阻塞在 `Read` 时方向失败 | 额外关闭该方向**源端**（否则线程收不回） | — |
| `ctx` 取消 | 强制关闭两端 | 返回取消原因 |

因此慢 peer 场景下：目的端被关闭、健康源端保持打开；另一条 RelayPipe 完全不受影响
（测试 2 同时跑一条慢管道和一条健康管道验证）。**限制**：如果另一方向正从被关闭的
目的端读数据，它会因为 `Read` 失败而结束——`Stream` 只有完整 `Close`，没有半关闭
接口，这一点无法在接口层避免；但 RelayPipe 不会主动去关闭健康方向的目的端。

### 4.5 取消、所有权与生命周期

- `RelayPipe` 不拥有 `a`/`b`，只借用；调用方保证它们在 `Run` 返回前存活。
- 正常 EOF **不关闭**任何 Stream（半关闭语义留给调用方）。
- `ctx` 取消回调只捕获共享状态（`shared_ptr<RelayPipeState>`），只置原子标志 +
  通知条件变量，不访问 `Stream`；`Run` 线程负责 `ForceCloseAll()`，避免
  「回调在别的线程里关闭用户对象」的时序问题。
- `Run` 返回前 join 所有线程、停止看门狗、注销回调；重复调用被拒绝。
- 析构时若未调用 `Run` 不会启动线程；调用方不得在 `Run` 未返回时析构。

---

## 5. 接入示例（Socket）

```cpp
class SocketStream final : public go2cpp::relay::Stream {
 public:
  explicit SocketStream(go2cpp::net::Socket socket) : m_socket(std::move(socket)) {}

  go2cpp::streams::ReadResult Read(char* buffer, std::size_t size) override {
    const auto result = m_socket.recv(buffer, size);
    if (result.transferred == 0) {
      return {0, go2cpp::streams::IoEof()};        // 对端关闭 → EOF
    }
    if (!result.ok()) {
      return {0, go2cpp::NewError("socket recv errno=" +
                                  std::to_string(result.error))};
    }
    return {static_cast<std::size_t>(result.transferred), {}};
  }
  go2cpp::streams::WriteResult Write(const char* data, std::size_t size) override {
    const auto result = m_socket.send(data, size);
    if (!result.ok()) {
      return {0, go2cpp::NewError("socket send errno=" +
                                  std::to_string(result.error))};
    }
    return {static_cast<std::size_t>(result.transferred), {}};
  }
  go2cpp::ErrorPtr Close() override {
    // 先 shutdown(SHUT_RDWR) 再 close，确保能打断另一线程的阻塞 write
    m_socket.shutdown(SHUT_RDWR);
    m_socket.close();
    return {};
  }

 private:
  go2cpp::net::Socket m_socket;
};

SocketStream a(server_data_socket), b(viewer_data_socket);
go2cpp::relay::RelayPipe pipe(&a, &b, go2cpp::relay::PipeConfig{});
go2cpp::ErrorPtr err;
if (!pipe.Run(ctx, &err)) {
  // err 是第一条失败；pipe.Stats() 给 dropped/timeouts
}
```

> 上面的 `SocketResult` 映射是适配层职责：流式套接字上 `recv` 返回
> `transferred == 0` 表示对端关闭，必须转成 `streams::IoEof()`；`errno` 转成
> `ErrorPtr`。`Socket::send` 允许短写，正好符合 `Stream::Write` 契约。

内存管道只需实现同一组三个方法；`tests/relay/relay_test_support.hpp` 的
`TestStream` 是完整参考（支持 EOF、阻塞写、慢写、短写、空闲等待与并发 Close）。

---

## 6. 统计口径

| 字段 | 含义 | 注意 |
|---|---|---|
| `a_to_b` | A→B 成功写出的字节数 | 按每次 `Write` 的实际 `count` 累加 |
| `b_to_a` | B→A 成功写出的字节数 | 同上 |
| `dropped` | 已读出但未写出的字节数 | 含队列里被丢弃的 chunk 与超时 chunk 的剩余部分 |
| `timeouts` | deadline 事件数 | 写超时 + 队列持续满；每个方向最多一次 |

干净 EOF 的 `dropped=0`、`timeouts=0`；取消路径可能把队列里未写出的数据计入
`dropped`，这是「已读出但未交付」的真实口径。

---

## 7. 测试

### 7.1 运行方式

```bash
# 构建（WSL 树）
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl --target go2cpp_relay_pipe_test -j8

# 直接运行
./.build-bento-wsl/go2cpp_relay_pipe_test

# ctest（标签 relay）
ctest --test-dir .build-bento-wsl -R '^relay_pipe$' --output-on-failure
```

测试是独立的 `main`，失败返回 1 并打印 `文件:行号`；不并入 `go2cpp_tests`。

### 7.2 覆盖矩阵（8 组，全部通过）

| # | 用例 | 覆盖点 |
|---|---|---|
| 1 | 双向 1e7 字节 | byte-exact、SHA-256 两端一致、干净 EOF 不关闭 Stream |
| 2 | 慢 peer → 队列满 | 持续满判定、只关目的端、健康源端保持打开、另一条管道并行完成 |
| 3 | 写 deadline | 50 ms deadline 触发、`timeouts==1`、目的端关闭、`dropped>0` |
| 4 | 4 条管道并发 | 4 组不同种子同时跑，摘要逐组匹配，互不串流 |
| 5 | 1e8 字节压测 | 双向各 1e8、计数/摘要精确、吞吐与内存记录 |
| 6 | 40 轮随机 | 随机规模 0–300 KB、copy_buf 512/4K/32K、队列 1–6 倍、随机短写/慢写 |
| 7 | Context 取消 | 空闲连接被取消、回调唤醒、两端强制关闭、线程回收 |
| 8 | API 边界 | 零配置归一化、空 Stream、重复 Run、已取消 Context |

### 7.3 实测输出（WSL2，Release，2026-11-01）

```
[test] 1. 双向 1e7 字节 byte-exact
  2 x 10000000 bytes, 49 ms, 389.3 MiB/s
[test] 2. 慢 peer → 队列满 → 只断该端
  slow error: relay: a->b queue stalled: relay: timeout
  slow: a_to_b=12288 b_to_a=65536 dropped=40960 timeouts=1, 101 ms
[test] 3. 写 deadline → Timeouts+1
  deadline error: relay: a->b write: relay: timeout
  timeouts=1 dropped=65536, 52 ms
[test] 4. 4 条管道并发互不串流
  4 pipes x 1000000 bytes/direction all byte-exact
[test] 6. 40 轮随机规模/队列/短写/慢写
  40/40 rounds with data, 5840199 bytes total
[test] 7. Context 取消 → 关闭两端并回收线程
  canceled, both endpoints closed
[test] 8. API 边界
  config/empty/already-run/canceled all rejected as expected
[test] 5. 1e8 字节压测（双向各 1e8）
  2 x 100000000 bytes, 525 ms, 363.3 MiB/s (digest oracle 568 ms)
=== relay pipe: all checks passed ===
```

多轮连跑（同一 WSL2 机器，Release，12 vCPU）：
低负载（load≈2–4）时 1e7 双向 47–50 ms（389–406 MiB/s）、1e8 双向 446–577 ms
（330–428 MiB/s）；共享构建把 load 推到 ≈10 时，1e7 为 135 ms、1e8 为 960 ms。
RSS 稳定在 6.2–6.8 MiB（`/usr/bin/time -v`）。SHA-256 是纯标量实现（约 400 MiB/s），
吞吐瓶颈主要在摘要与内存拷贝，不是中继逻辑；测试进程的队列内存始终不超过
`queue_cap`（1e8 用例没有缓存 200 MB 数据）。

ASan/TSan 另做了一次独立构建验证（2026-11-01）：

```bash
g++ -std=c++17 -O1 -g -fsanitize=address -Iinclude -Itests/relay \
    tests/relay/test_relay_pipe.cpp src/relay/relay_pipe.cpp \
    -L.build-bento-wsl -lgo2cpp_context -lgo2cpp_error -lgo2cpp_hash \
    -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -pthread -o /tmp/relay_pipe_asan
ASAN_OPTIONS=detect_leaks=0 /tmp/relay_pipe_asan     # 全过
# TSan 版本同理加 -fsanitize=thread，用 setarch --addr-no-randomize 运行，全过
```

---

## 8. 与 JinShan REQ-002 的对照

| 需求 | 实现 |
|---|---|
| `PipeConfig{CopyBuf,QueueCap,WriteTimeout}` | 同名 C++ 结构，默认 32 KiB/256 KiB/30 s |
| `PipeStats{AtoB,BtoA,Dropped,Timeouts}` | 同名 C++ 结构 + 原子快照 |
| `NewRelayPipe(a,b,cfg)` | 构造函数 |
| `Run(ctx) error` | `Run(const ContextPtr&, ErrorPtr*)` |
| 有界队列 + 背压 | `ChunkQueue` 字节容量；满则阻塞，持续满超预算失败 |
| 慢 peer 只断自身 | 只关该方向目的端；健康源端/其它管道不受影响 |
| 写超时 → 关闭 + Timeouts+1 | 看门狗关目的端、`timeouts+1` |
| 不解析/不重排/不改写 | 只做 `Read`→`Write`，无任何内容处理 |
| 双向 1e7 byte-exact | 测试 1，两端 SHA-256 与期望一致 |
| 慢读 → 队列满 → 只断该端 | 测试 2，另跑健康管道验证隔离 |
| 写 deadline 30 s | 默认 30 s；测试用 50 ms 加速验证 |
| 4 条并发互不串流 | 测试 4 |

---

## 9. 边界与已知限制

1. **Stream 只有完整 `Close`**：没有 `CloseWrite`/`CloseRead`。超时/失败时关闭目的端
   会连带让「从该端读」的反方向结束；RelayPipe 不会主动关闭健康方向的另一目的端，
   但无法提供 TCP 半关闭语义。需要半关闭时应在适配层把 `Close` 实现成 `shutdown(SHUT_WR)`。
2. **依赖 `Close` 打断阻塞 I/O**：Linux 上仅 `close(fd)` 不保证打断另一线程的阻塞
   `write`，适配层应先 `shutdown`；`Stream` 契约已写明。若 `Close` 不返回，`Run` 无法
   回收线程（这是接口契约问题，不是中继逻辑）。
3. **每方向一个写线程**：单条方向内写入是串行的（保持字节顺序）；吞吐上限受
   `copy_buf`、队列容量与 `Write` 系统调用次数影响。1e8 实测约 428 MiB/s（含 SHA 计算）。
4. **看门狗 5 ms 轮询**：timeout 判定有最多 5 ms 误差；默认 30 s 下可忽略。
5. **不重试**：写失败直接结束该方向；重连/重试由上层（JinShan Hub）决定。
6. **`Run` 单次**：不支持同一对象重复 Run；需要重启时新建 `RelayPipe`。
7. **统计非事务快照**：`Stats()` 逐字段原子读，运行期四个数可能不是同一瞬间，但
   单调递增；`Run` 返回后是最终值。
8. **TSan 未注册为 ctest 目标**：本次用独立 `-fsanitize=thread` 构建跑过全部用例，
   但构建目录没有常驻的 TSan 目标；并发正确性主要由 4 管道并发、40 轮随机、
   慢 peer/取消场景、ASan/TSan 手动验证与代码审查覆盖。需要常驻回归时可在
   `cmake/Go2CppRelayPipe.cmake` 增加一个 `relay_pipe_tsan` 目标。

---

## 10. 变更记录

### 2026-11-01：初版 RelayPipe

- 新增 `include/go2cpp/relay/relay_pipe.hpp`、`src/relay/relay_pipe.cpp`；
- 有界字节队列 + 持续满慢 peer 判定 + 写 deadline 看门狗 + 每方向独立失败；
- 8 组测试：1e7/1e8 byte-exact、慢 peer、写超时、4 管道并发、40 轮随机、取消、API 边界；
- CMake：独立目标 `go2cpp_relay_pipe` 与 `relay_pipe` ctest（Hub/Lease 另用
  `go2cpp_relay`，互不覆盖）；
- 实测：低负载 1e7 双向 47–50 ms、1e8 双向 446–577 ms；load≈10 时
  1e7 135 ms、1e8 960 ms；RSS 6.2–6.8 MiB；ASan/TSan 独立构建全过；
- 已知限制见 §9，未提交（多 agent 共享工作树）。
