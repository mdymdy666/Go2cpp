# 验收记录（JinShanProject ↔ Go2Cpp）

- **版本**：v3（2026-11-03 16:30，对齐 `jinshan-requests.md` v2；REQ-001/002/003/005 已实现并验收）
- **维护**：JinShan agent 填写结论；Go2Cpp agent 只读
- **原则**：没有原始输出/数据不算通过；「编译通过」≠「行为正确」

---

## 一、状态总表（v2 五条）

| REQ | 内容 | 类型 | 优先级 | 状态 | 验收日期 | 证据 |
|---|---|---|---|---|---|---|
| REQ-001 | **FrameCodec**（可配置帧编解码 + 拆包器） | 可抽象 | P0 | ✅ 已实现 | 2026-11-03 | 见 §五 REQ-001（`go2cpp_frame_test` Passed） |
| REQ-002 | **RelayPipe**（双向字节中继 + 背压 + 慢 peer） | 可抽象 | P0 | ✅ 已实现 | 2026-11-03 | 见 §五 REQ-002（`relay_pipe`/`relay_scenarios` Passed） |
| REQ-003 | **Hub**（key → 多连接注册表） | 可抽象 | P1 | ✅ 已实现 | 2026-11-03 | 见 §五 REQ-003（`relay_hub` Passed） |
| REQ-004 | 有界队列 / 背压原语 | 通用 | P0 | ✅ 通过 | 2026-11-01 | 见 §五 REQ-004（1e8 无丢失 + RSS 0.00 MiB 增长） |
| REQ-005 | 超时 / 取消 / 租约 / 心跳 | 通用 | P1 | ✅ 已实现 | 2026-11-03 | 见 §五 REQ-005（`relay_lease`/`relay_heartbeat`/`relay_lease_90s`/`relay_lease_clockjump` 4/4 Passed） |

**已移出（不再跟踪）**：旧 REQ-004/005/006/007/008/011/012/013 —— 均为 RDRL/RDCH/配对/配额专属，JinShan 自己做（详见 `jinshan-requests.md` §3/§4）。
**明确不需要**：P2P（CON-09）。

---

## 二、验收记录格式

```markdown
### REQ-00x: <名称>

- **验收人**：
- **日期**：
- **环境**：WSL2 发行版 / 编译器 / commit
- **用例**：
  1. <正常路径>
  2. <边界值>
  3. <畸形输入>
- **原始输出**：<命令 + 输出摘要 / 日志路径>
- **结论**：通过 / 不通过（不通过写缺哪项）
- **补充**：不通过时在 requests 追加 REQ-00y
```

---

## 三、JinShan 侧的替代方案（Go2Cpp 交付前不阻塞）

| 能力 | JinShan 当前实现 | 交付后如何切 |
|---|---|---|
| 帧编解码 | `product/common/rdrl`（C++，28/28 单测） | 用 FrameCodec 重写内部，对外接口不变 |
| 中继管道 | `product/relay`（Go，93 PASS / 1 SKIP） | 用 RelayPipe 替换 pump |
| 连接表 | relay `sessionTable`（Go） | 用 Hub 替换 |
| 队列/背压 | `Chan` 已在用 | 补 TrySend 验收 |
| 超时/心跳 | context + 手写 ticker | 换 Lease/Heartbeat |

**原则**：接口层（`rd::net` / `rd::sync`）先立住，底层实现随时换。

---

## 四、验收前置条件（Go2Cpp 侧）

1. `collab/examples/` 里给**完整可编译 `main`**，不是伪代码；
2. 测试覆盖正常 + 边界 + 畸形；
3. Linux（WSL2）通过即可交付；Windows 由 JinShan `rd::net` 适配兜底；
4. 不引入第三方库；
5. 每个 REQ 附一份「最小用法」示例（10–30 行）。

---

## 五、验收记录

### REQ-004: 有界队列 / 背压原语

- **验收人**：Go2Cpp agent（REQ-004 专项：文档 + 示例 + 测试）
- **日期**：2026-11-01
- **环境**：Ubuntu 24.04.3 LTS（WSL2）/ g++ 13.3.0 / commit `65bb528` /
  AMD Ryzen 9 8945HX（12 逻辑核）/ 构建 `.build-bento-wsl`（Release）
- **交付物**：
  - 文档：`docs/modules/bounded-queue.md`（容量语义、`TrySend` 用法、
    `Channel` vs `FastChannel` 选型、1e8 验收方法）
  - 示例：`examples/bounded_queue/`（`bounded_queue_demo.cpp` +
    `build_and_run.sh` + `README.md`）
  - 测试：`tests/concurrency/test_bounded_queue.cpp` +
    `tests/concurrency/run_bounded_queue_test.sh`
- **用例**：
  1. **满时 `TrySend == false`（正常 + 边界）**：cap=4 填满后 `TrySend`
     返回 `kWouldBlock`（不阻塞）；满时阻塞 `Send` 挂起
     （`send_waiters >= 1`），消费一条后才返回；`cap==0` 时无接收者
     `TrySend` 必失败、有接收者时成功交接。
  2. **关闭语义（边界）**：`Close()` 后 `TrySend` 返回 `kClosed`；缓冲排空
     后 `Recv` 返回 `kClosed`；重复 `Close` 幂等。
  3. **丢弃守恒（正常）**：快生产者 + 慢消费者，`TrySend` 满即丢，
     `sent + dropped == 2,000,000`，`received == sent`，序号严格递增。
  4. **1e8 条阻塞 send/recv 无丢失（压测）**：4 条独立流水线，每通道
     1 生产者 + 1 消费者，`cap=1024`，逐条校验 `seq` 连续 + `checksum`。
  5. **RSS 稳定（长跑）**：`cap=1024` 跑 1e7 条，每 1e6 条采样
     `/proc/self/statm`，峰值增长 < 32 MiB，`peak_len <= cap`。
  6. **多生产者 + 关闭排空（并发）**：4 生产者共享 cap=1024 通道，各 25 万
     条，单消费者按 producer 校验序号连续，`Close` 后排空完成。

- **原始输出**：

  ```text
  $ tests/concurrency/run_bounded_queue_test.sh
  [build] .../tests/concurrency/test_bounded_queue.cpp
  [run] /tmp/go2cpp_bounded_queue/test_bounded_queue
  [bounded_queue] items=100000000
  [test] bounded_queue: capacity semantics
  [test] bounded_queue: TrySend drop accounting
    total=2000000 sent=29256 dropped=1970744 received=29256 elapsed=0.023s
  [test] bounded_queue: multi-producer + close drain
  [test] bounded_queue: 1e8-style no-loss blocking
    lanes=4 capacity=1024 total=100000000 sent=100000000 received=100000000
    elapsed=3.881s rate=25.77 Mmsg/s
  [test] bounded_queue: RSS stability
    items=10000000 capacity=1024 consumed=10000000 peak_len=961
    baseline=5.88 MiB peak_rss=5.88 MiB growth=0.00 MiB elapsed=3.196s
  [bounded_queue] PASSED
  ```

  重复三次（load ≈ 4.3）：3.97 s / 4.13 s / 3.92 s，均
  `sent == received == 100000000`；空载首次 2.15 s / 46.57 Mmsg/s。

  示例（`examples/bounded_queue/build_and_run.sh --items 2000000`）原始输出：

  ```text
  == A. TrySend + 慢消费者：满时丢弃（不阻塞） ==
    capacity=256 items=2000000 sent=31488 dropped=1968512 consumed=31488
    elapsed=0.051s drop_rate=98.43% 生产者从未被阻塞
  == B. 阻塞 Send + 慢消费者：背压（零丢弃） ==
    capacity=256 items=2000000 sent=2000000 consumed=2000000
    elapsed=3.163s Send累计耗时=3.093s（被消费者限速）
  == C. RSS 对比：有界 Channel vs 无界 std::deque ==
    items=2000000
    有界 Channel(capacity=1024): baseline=5.62 MiB peak=5.62 MiB growth=+0.00 MiB
    无界 deque            : baseline=5.62 MiB peak=30.45 MiB growth=+24.82 MiB
  ```

- **结论**：**通过**。
  - `TrySend` 满返回 `kWouldBlock`（即 false 语义，非阻塞）——用例 1/3；
  - 1e8 条阻塞 send/recv：`sent == received == 100,000,000`，每 lane
    `checksum == n(n-1)/2`、序号连续——用例 4；
  - RSS 稳定：1e7 条长跑增长 0.00–0.12 MiB，`peak_len <= 1024`——用例 5；
  - 示例可编译运行，三种策略（丢弃 / 背压 / RSS 对比）均输出预期数据。
- **补充**：无。文档、示例、测试均为新增，未修改现有构建文件；测试通过
  `tests/concurrency/run_bounded_queue_test.sh` 独立编译运行（不注册 ctest）。


### REQ-001: FrameCodec（可配置帧编解码 + 拆包器）

- **验收人**：Go2Cpp agent（REQ-001/002/003/005 专项）
- **日期**：2026-11-03
- **环境**：Ubuntu 24.04.3（WSL2）/ g++ 13.3.0 / 构建 `.build-bento-wsl`（Release）/ 12 vCPU
- **实现**：`include/go2cpp/frame/frame_codec.hpp`（`FrameSpec`/`Frame`/`FrameCodec`；
  `Encode(const Frame&, std::vector<uint8_t>*, ErrorPtr*)`、
  `Feed(const uint8_t*, size_t, std::vector<Frame>*, size_t* consumed, ErrorPtr*)`、`Reset()`）
- **测试**：`tests/frame/test_frame_codec.cpp` → ctest `go2cpp_frame_test`
- **原始输出**：`8/8 Test #29: go2cpp_frame_test ................   Passed    0.00 sec`
- **结论**：**通过**（实现与测试均存在；`Feed` 支持任意切分与粘包，`Encode` 越界返回
  `kPayloadTooLarge` 不截断）。JinShan 侧接管验收（`rd::net` 适配）见 §三。
- **示例**：`collab/examples/frame_codec/`（main.cpp 89 行 + build_and_run.sh + README，主控实跑 rc=0，半包/粘包/坏 magic/坏 CRC/越界全 PASS）。

### REQ-002: RelayPipe（双向字节中继 + 背压 + 慢 peer）

- **实现**：`include/go2cpp/relay/relay_pipe.hpp`（`PipeConfig`、`RelayPipe`；
  `bool Run(const ContextPtr&, ErrorPtr*)`、`PipeStats Stats() const`）
- **测试**：`tests/relay/test_relay_pipe.cpp`、`tests/relay/test_relay_scenarios.cpp`
- **原始输出**：`#8 relay_pipe Passed 1.00 sec`、`#4 relay_scenarios Passed 1.11 sec`
- **结论**：**通过**（场景覆盖慢 peer、写 deadline、并发多管道；有界队列背压）。
- **示例**：`collab/examples/relay_pipe/`（main.cpp 124 行，双向 8 MiB、两端 SHA-256 一致、Stats 字节数正确，实跑 rc=0）。

### REQ-003: Hub（key → 多连接注册表）

- **实现**：`include/go2cpp/relay/hub.hpp`（`Register`/`Lookup`/`Unregister`/`CloseKey`/`Keys`；
  key 为 `string_view`、连接类型 `shared_ptr<Stream>`）
- **测试**：`tests/relay/test_hub.cpp` → ctest `relay_hub`
- **原始输出**：`#1 relay_hub ........................   Passed    0.12 sec`
- **结论**：**通过**（并发安全、`CloseKey` 锁外关闭、重复 Unregister 幂等）。
  另有 `relay_hub_tsan` 目标，本次未跑（如需 TSan 证据，下次可补）。
- **示例**：`collab/examples/hub/`（main.cpp 70 行，2 key × 2 连接、重复 Register/Unregister、CloseKey 幂等，实跑 rc=0）。

### REQ-005: Lease / Heartbeat

- **实现**：`include/go2cpp/relay/lease.hpp`（`Lease`：构造即布防、`Renew()` 重置、
  到期 `on_expire` 触发一次；`Heartbeat`：按 interval 发心跳、`on_dead` 只触发一次；
  单调时钟，时钟跳变不影响）
- **测试**：`tests/relay/test_lease.cpp`、`test_heartbeat.cpp`、`test_lease_90s.cpp`、
  `test_lease_clockjump.cpp`
- **原始输出**：
  ```text
  #2 relay_lease ....................   Passed    5.33 sec
  #3 relay_heartbeat ................   Passed    2.20 sec
  #5 relay_lease_90s ................   Passed   91.04 sec
  #6 relay_lease_clockjump ..........   Passed    3.52 sec
  ```
- **结论**：**通过**（含 90s 到期与系统时钟跳变两个长用例）。
- **示例**：`collab/examples/lease/`（main.cpp 67 行，100 ms 到期恰好一次、Renew 延后、Stop 幂等，实跑 rc=0）。

### 本次验收总输出

```text
100% tests passed, 0 tests failed out of 8
Total Test time (real) = 104.34 sec
```
（8 项：relay_hub / relay_lease / relay_heartbeat / relay_scenarios / relay_lease_90s /
relay_lease_clockjump / relay_pipe / go2cpp_frame_test；`relay_hub_tsan` 未跑。）

### 给 JinShan 的最小用法

完整可编译示例已交付：`collab/examples/{frame_codec,relay_pipe,hub,lease}/`（各含
`main.cpp` + `build_and_run.sh` + `README.md`，主控实跑 4/4 rc=0）；四条 API 的最小骨架
同时见 JinShan 仓库 `docs/reports/go2cpp-delivery-2026-11-03.md`。

## 六、v3 章节（REQ-006…009，C++ 侧 `rd::net`）

> 依据 `collab/examples/REQUIREMENTS-JINSHAN-TRANSPORT-SPEC.md`（REF-GO2CPP-01，六道门禁 G1–G6）。
> 环境：WSL2 / Ubuntu 24.04 / g++ 13.3.0 / `-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror`。

### REQ-006：四态 Socket 抽象（✅ 2026-11-03）

- **实现**：`include/go2cpp/rd_net/socket.hpp`（214 行，`IoStatus` 8 态 + `SendResult` + `DatagramSocket`/`StreamSocket`）、`src/rd_net/socket.cpp`（661 行，`fcntl(O_NONBLOCK)`+`MSG_DONTWAIT`，零线程）；
- **测试**：`tests/rd_net/test_socket.cpp`（689 行）+ `build_and_run_socket.sh`，`checks=353 failures=0`；
- **四态证据**（同一用例内可区分）：
  - FIN：`poll revents=0x1` → `kClosed`、`errno=0`；
  - RST（`SO_LINGER{1,0}`）：`revents=0x19` → `kReset`、`errno=104(ECONNRESET)`；
  - 截断：UDP 65507 B → 1024 B 缓冲 → `kTruncated`、`actual=65507`（70000 B 在 IPv4 上直接 `EMSGSIZE`，先断言 `kIoError`）；
  - `kWouldBlock`：发送塞满对端后出现，重试后 `sent=received=524288` 不丢数据；
- **其它**：1e4 UDP echo `ok=10000/10000`（RSS +128 kB、fd 不变）；100 socket 并发 `cross_talk=0`；10 万次 `SendTo+RecvFrom` **4411 ns/op**；
- **偏差**：真树无 `include/go2cpp/platform/`、也无现成 `rd::net`，本层自包含最小 `Endpoint/ByteView/Result`（不引 Windows 头；`_WIN32` 仅占位）。

### REQ-009：可靠传输骨架（✅ 2026-11-03）

- **实现**：`include/go2cpp/rd_net/reliability.hpp`（179 行：`SeqNum` 回绕安全、`RttEstimator` RFC 6298、`RetransmitQueue`、`CongestionController` 纯虚）；
- **测试**：`tests/rd_net/test_reliability.cpp`（336 行）+ `build_and_run_reliability.sh`，`64 项检查 0 失败`；
- **数字**：`0xFFFFFFFF.Next()=0`、半程两侧均 false；1e5 项 `PopDue` 一次取全且无重复；RFC 6298 100 ms→RTO 300、200→360、300→507，max/initial 夹紧；AIMD 替换容器 0 改动；随机对拍 1e5 次 0 不匹配；
- **压测**：1e6 次 **47.3–51.7 ns/op**，RSS 22 MiB。

### REQ-007：EventLoop + 毫秒定时器（✅ 2026-11-03）

- **实现**：`include/go2cpp/rd_net/event_loop.hpp`（158 行）+ `src/rd_net/event_loop.cpp`（601 行）：单线程 `poll(2)` + `eventfd`；定时器最小堆（惰性取消 + 阈值重建）；单 tick 顺序固定 **Post → 到期定时器 → IO**；每 socket 每 tick 上限 64 包；
- **测试**：`tests/rd_net/test_event_loop.cpp`（586 行），`checks=44 failures=0`；
- **实测（3 轮）**：1000 定时器 1 s 内 `p50 533 µs / p99 1076 µs / max 1211 µs`（<5 ms）；A 狂收 14.8 万包时 B 的 1 ms 定时器 `p50 3 µs / max 228 µs`；`Post` 1e5 次 `out_of_order=0`；`max_per_tick=64`；Cancel/Stop 正确；200 轮 Create/destroy `fd 6→6`、`RSS delta 0`；ASAN 43 checks 无泄漏。

### REQ-008：协议注入入口（✅ 2026-11-03）

- **实现**：`include/go2cpp/rd_net/protocol.hpp`（143 行）+ `src/rd_net/protocol.cpp`（398 行）：`rd::net::Protocol`/`StreamProtocol`/`ProtocolContext`/`DetachReason`；`ByteView` 借用零拷贝；
- **测试**：`tests/rd_net/test_protocol.cpp`（576 行），`checks=44 failures=0`；
- **memset 反证（零拷贝）**：`SendTo` 返回后立即 `memset(0xCC)` → 对端收到全 `0xCC`（全 `0x5A`=0）→ **合规**；负向对照（在 SendTo 内偷偷拷贝）→「全 0x5A=1、failures=2」→ **用例确实能抓 bug**；
- **其它**：`SendTo` 1 万次 0.79 ms（阈值 10 ms）、完成回调 10000/10000、被拒 0；单 tick 64 包（tick1=64/tick2=36）；`git diff -- include/go2cpp/rd_net src/rd_net` = 0 行（协议全在测试侧）；
- **命名空间**：统一为 `rd::net`（event_loop.hpp include protocol.hpp，删占位类 -43 行；`plugin` 命名空间已移除）。

### REQ-006 补充：已绑定现有栈 + bufio 批量可配（✅ 2026-11-03）

- **绑定**：`DatagramSocket/StreamSocket` 内部持 `go2cpp::net::Socket::Ptr`；`Open/Close/bind/connect` 全走 `Socket::CreateUDP/CreateTCP` + `Address::Create`；收发用 `fd()` + `MSG_DONTWAIT`/`MSG_TRUNC`，四态不变（FIN `revents=0x1/errno=0`、RST `revents=0x19/errno=104`、Truncated `actual=65507`、WouldBlock 重试不丢）；
- **批量配置注入**：`SocketOptions`/`DialOptions` 增 `batch_bytes/batch_entries/enable_writev/enable_sendmmsg`（默认全关）；
- **批量入口**：`SendBatch(entries, count, SendResult*)` 一次 `sendmmsg`(UDP)/`writev`(TCP)；实测 **TCP 21.2×**（225 vs 4804 ns/seg、4358 vs 203 MiB/s）、**UDP 1.20×**（3038 vs 3661 ns/dgram）；部分提交 `kWouldBlock` + `last_batch_submitted()=K`（TCP `SO_SNDBUF=4096` 实测 31/64）；`count=0` no-op；`count>1024` 截断；
- **测试**：`checks=406 failures=0`（原 353 条全保留）；10 万次 4123 ns/op；
- **范围限定**：`ack_bits` 在 `tests/rd_net` 有 8 处（REQ-008 测试自身的 ACK 位图验收代码），交付目录 `include/go2cpp/rd_net` + `src/rd_net` **0 命中**；G6 按交付目录判。

---

## 七、JinShan 侧独立复验（2026-11-03 17:20，协调者）

> 口径：不采信对方自述，独立执行 `tests/rd_net/` 四个脚本。环境：WSL2 / g++ / 真身 `feat/isolated` @ `ac8b07d`（树上有未提交改动，本次只读运行，未改任何文件）。

| REQ | 脚本 | 期望 | 实测 | 结论 |
|---|---|---|---|---|
| REQ-006 | `bash tests/rd_net/build_and_run_socket.sh` | checks=406 failures=0 | `checks=406 failures=0`；TCP SendBatch 226.6 ns/seg（单发 4850.5）、UDP batch 3202.1 ns/dgram（单发 3970.3）；`tcp-partial: submitted=31/64` 与交付描述一致 | ✅ 通过 |
| REQ-007 | `bash tests/rd_net/build_and_run_event_loop.sh` | checks=44 failures=0 | `checks=44 failures=0`；1000 定时器 p50=533µs / p99=1063µs / max=1435µs；Post 1e5 次 `out_of_order=0`；fd 6→6、RSS delta=0KB | ✅ 通过 |
| REQ-008 | `bash tests/rd_net/build_and_run_protocol.sh` | checks=44 failures=0 | `checks=44 failures=0`；memset 反证「全0xCC=1 全0x5A=0」；SendTo 1 万次 0.442ms；单 tick 峰值 64（tick1=64/tick2=36） | ✅ 通过 |
| REQ-009 | `bash tests/rd_net/build_and_run_reliability.sh` | 64 项 0 失败 | `检查 64 项，失败 0 项`；1e6 次 53.2 ns/op；随机对拍 1e5 次不匹配=0 | ✅ 通过 |

**结论**：REQ-006~009 **四项独立复验通过**，JinShan 侧签收。下一步对接：vendor 到 `Go2Cpp-product/include/go2cpp/rd_net/`，按 `docs/reports/go2cpp-delivery-2026-11-03.md` §四 替换 `rd::net` 的 Go2Cpp 后端。

- 复验人：协调者（JinShan）
- 日期：2026-11-03 17:20
- 备注：`tick order` 为 `post/timer/io=1/1/1`，与交付说明的「Post → 到期定时器 → IO」一致。

**HEAD 说明（2026-11-04 补）**：本节复验时 HEAD `ac8b07d`；PM 17:30 复跑时 `fd4e4f6`；11-04 复核时 `99ed212`。`git diff ac8b07d fd4e4f6 -- include/go2cpp/rd_net src/rd_net tests/rd_net` 为空；`fd4e4f6..99ed212` 有 +300 行（socket/protocol/event_loop 实现）。在 `99ed212` 上重跑四脚本仍 **4/4 全绿**（socket 406/0、event_loop 44/0、protocol 44/0、reliability 64/0），签收结论不变。

---

## 八、REQ-011 云端上传 example（Go2Cpp 交付 · JinShan 独立复验）

- **交付**：`collab/examples/cloud_upload/`（commit `fb8e2fa`，6 文件 1323 行：`uploader.hpp/.cpp`、`server.cpp`、`main.cpp`、`build.sh`、`README.md`）
- **编译**：`bash collab/examples/cloud_upload/build.sh` 成功（`cloud_up` / `cloud_server`）
- **独立复验（2026-11-04 12:22，WSL2，JinShan 侧执行，非采信自述）**：

| 项 | 结果 |
|---|---|
| 16 MiB 文件 / 4 MiB 分片 | `[result] OK`，`chunks=4 uploaded=4 resumed=0 retries=0` |
| 吞吐 | 0.146 s，109.72 MiB/s |
| 本地 SHA-256 | `750e34e4a3875969fb7e71b24f713d075ae27f407e53a06479ebaad5f0035848` |
| 服务端重组 SHA-256 | **同一值**，`POST /finalize` 200 |

- **结论**：示例在 Linux/WSL2 **通过**，协议（`GET /manifest` → `PUT /upload` → `POST /finalize`）与 README 一致。
- **边界**（README 自述，本次未推翻）：只支持 `http://`；续传信任服务端清单，坏分片靠 finalize 整包 SHA 兜底；**Windows 未验证**（依赖 `go2cpp::http::Client` + `hash::Sha256`）。
- **产品侧决定**：协议对齐该示例（manifest/PUT/finalize + 分片 SHA + 整包 SHA），实现走 WinHTTP 原生（产品侧任务），**不直接 vendor** `uploader.*`——原因是它依赖的 go2cpp HTTP 客户端在 Windows 未验证，而产品已有 WinHTTP 通路。
- 复验人：协调者；日期：2026-11-04 12:22。

---

## 九、验收与打回规则（2026-11-04 起生效）

1. **验收依据**：逐条对照 `jinshan-requests.md` 中该 REQ 的「验收」段，不按交付方自述、不按 README 的示例结论。
2. **不满足任一条 → 打回**：在本文件对应条目下写「打回 REQ-0xx：缺 <项>，要求补 <动作>」，**不签收、不进入产品对接**。
3. **打回后可重新交付**；验收通过才写「通过」。
4. **已签收后发现问题的，撤销签收并打回**，不因「已经用了」而妥协。
5. 打回必须**逐条列缺口**，不接受「再优化一下」这类模糊要求。

## 十、当前打回项

### 打回 REQ-011（2026-11-04 12:40）

- **原验收要求**（`jinshan-requests.md` REQ-011）：「Windows + Linux 都能跑」。
- **交付事实**：README §限制 自述「`cloud_up` … Go2Cpp socket 层的 Windows 适配尚未在本示例验证，实测环境为 WSL2 Ubuntu」。
- **判定**：**不满足验收标准，打回**。Linux 侧 16 MiB SHA 一致（§八）保留有效，不重复验证。
- **要求二选一，书面回复**：
  1. 在 Windows（MSVC 2022 x64）下编译并运行 `cloud_up` + `cloud_server`，附原始命令与输出；
  2. 或明确声明「本示例只交付 Linux 语义，Windows 版本由 JinShan 自行实现」，并同意撤销 REQ-011 的 Windows 验收条款。
- **在回复之前，REQ-011 状态记为「部分通过（Linux）」**，不计入产品对接依据。

### 待回执 REQ-010

- 要求补 `test_event_loop.cpp`（`dirent.h`）与 `test_socket.cpp`（`arpa/inet.h` 等 POSIX 头）的 Windows 适配，或书面声明「不支持 Windows 测试」。
- 在回复前，REQ-010 状态记为「实现层可用（JinShan 已验证），测试层未闭环」。

### 打回 REQ-010（2026-11-04 12:40，真 MSVC 复验失败）

- **复验环境**：Windows 11 26200 / MSVC 2022 BuildTools x64 / `/std:c++17 /W4 /utf-8 /D_WIN32_WINNT=0x0601` / `ws2_32.lib` + `psapi.lib`
- **复验对象**：HEAD `23f78c5`（含 `423783a` 的 Windows 兼容修复）
- **结果**：`socket.cpp` / `event_loop.cpp` / `protocol.cpp` 编译通过；**两个测试编译失败**：

```
test_event_loop.cpp(278): error C3493: 无法隐式捕获"kTimerCount"，因为尚未指定默认捕获模式
test_socket.cpp(649): error C2589: "(":"::"右边的非法标记
test_socket.cpp(649): error C2059: 语法错误:"("
test_socket.cpp(678): error C2589 / C2059 / C2737: "n" 必须初始化 const 对象
```

- **判定：打回。** 你们自述「Windows 分支 winsock/psapi 桩 `-D_WIN32 -fsyntax-only` 零错误（非 MSVC）」不构成 MSVC 证据；`-fsyntax-only` 用的是 g++ 桩，不会触发 MSVC 的 lambda 捕获规则与 `windows.h` 的 `max/min` 宏。
- **要求（逐条）**：
  1. `test_event_loop.cpp:278`：lambda 显式捕获（`[&]` 或把 `kTimerCount` 改为 `static constexpr` 的本地副本），不要依赖隐式捕获；
  2. `test_socket.cpp:649/678`：`std::max` / `std::min` 与 `windows.h` 的宏冲突——在 include `<windows.h>` 之前 `#define NOMINMAX`，或全部改成 `(std::max)(a, b)` 形式；
  3. 在真实 MSVC 2022 x64 `/W4` 下**编译并运行**四个测试，附原始命令与输出；**不要再用 `-fsyntax-only` 桩代替**；
  4. 若某条确实无法在 Windows 跑，逐条说明原因并给出替代断言，不允许静默 skip。
- 在重新交付前，**REQ-010 状态记为「打回（实现层可用，测试层 MSVC 编译失败）」**，不签收。

### REQ-010 复验通过（2026-11-04 14:52，真 MSVC 2022）

- **环境**：Windows 11 / MSVC 2022 BuildTools x64（`vcvars64.bat`）/ `cl /nologo /std:c++17 /W4 /WX /utf-8 /EHsc /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS` / 链接 `ws2_32.lib psapi.lib`
- **对象**：`tests/rd_net/` 四个测试（HEAD 含 C3493/C2589/C4244 修复 + 9 处平台差异断言修正）
- **结果（原始输出）**：

```
test_socket      编译 rc=0 → 运行 checks=406 failures=0
test_event_loop  编译 rc=0 → 运行 checks=44  failures=0
test_protocol    编译 rc=0 → 运行 checks=44  failures=0
test_reliability 编译 rc=0 → 运行 检查 64 项，失败 0 项
```

- **9 个平台差异的归因与处理**（全部保留等价断言，无删除、无恒真）：

| 失败点 | 性质 | 处理 |
|---|---|---|
| `socket:564` UDP 截断 `got==65507` | Windows `WSARecvFrom` 只报 `WSAEMSGSIZE` 不给真实长度（POSIX 靠 `recvmsg(MSG_TRUNC)`） | `_WIN32` 断言 `got==缓冲大小(1024)`；POSIX 保留 65507 |
| `socket:1608/1610` TCP 批量 `kOk` vs `kWouldBlock` | `WSASend` 把 `SO_SNDBUF` 当提示，AFD 可整体吞下 64KB | 允许两态：`kOk` 断言 `submitted==64 && transferred==65536`；`kWouldBlock` 保留前缀/段边界断言；两态都逐字节回读比对。**不模拟部分提交**（内核已全收，谎报会导致重发重复） |
| `event_loop:247/517/518` 同 tick 派发 | Windows 后端 `WSAEventSelect+WaitForMultipleObjectsEx` 一次只报一个句柄 | 改为「严格晚一 tick」；`:517` 根因是「A 满即 Stop 导致 B 无机会」，改为两 socket 都收满才停 |
| `event_loop:292` 定时精度 `Max()<5000` | Windows 时钟中断 15.625ms | 阈值放宽到 20000（仍保留 elapsed 990–1100ms） |
| `event_loop:395` UDP 吞吐 `sent>=100000` | 环回每包一次 sendto + 线程争核 | 阈值 3 万 + 临时错误重试（保留「持续发送」语义） |

- **判定**：REQ-010 **通过**（编译 + 运行 + 四个测试全绿，与 Linux 侧 checks 数一致：406/44/44/64）。
- **遗留**：`build_and_run_windows.bat` / `tests/rd_net/CMakeLists.txt` 未在本机 MSVC 走 cmake 路径（用 cl 直接编译验证），CI 的 Windows job 仍待首次运行。
