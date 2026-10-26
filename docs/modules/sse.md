# 模块文档：sse（Server-Sent Events）

> **状态：现行**。基线 `HEAD 2ba2f4e`（2026-10-26 核对；`src/`、`include/` 相对 HEAD 无改动，本次只改文档）。
> 本文是 SSE 的**唯一现行文档**：事件编码、增量解析、流式写入、断线/重连边界与测试集中在这里。
> 旧文档 `docs/sse.md` 已并入本文并在其顶部加了合并标注；`docs/state-machines/Stream.md`
> 保留为 `Stream` 状态机明细（已标注为本文配套）。API、默认值与行号以本文和当前代码为准。
> 引用约定：`file:line` 指当前工作树。

**结论：SSE 是独立模块，不在 HTTP 层内部。** 源码为
`include/go2cpp/network/sse.hpp` + `src/network/sse.cpp`，命名空间 `go2cpp::sse`，
编入 `go2cpp_network`；根路径 `include/go2cpp/sse.hpp`、`sse.h` 与
`include/go2cpp/network/sse.h` 只是 3 行兼容转发头。它**复用** HTTP 层的
`http::ResponseWriter` 和 `http::Request`，但协议逻辑（编码/解析/流状态）完全独立。

---

## 1. 概述与职责

SSE 模块提供 HTTP/1.1 `text/event-stream` 的三块能力：

- `Event`：一条事件的校验与线格式编码；
- `Parser`：对无限长字节流做增量解析，输出完整事件；
- `Stream`：单连接写入器（响应头、事件、注释心跳、chunked 关闭）。

它不负责连接池、重连调度、事件源、路由鉴权或跨连接事件存储；这些应由上层
NetService/业务实现（`include/go2cpp/network/sse.hpp:5-36`）。

| 做 | 不做 |
|---|---|
| 事件编码（id/event/retry/data）与多行 data 拆分 | 不自动重连、不保存事件、不做 Last-Event-ID 回放 |
| 任意字节分片的增量解析（BOM/LF/CRLF/CR/注释） | 不解析 HTTP 报文；请求/响应头仍由 HTTP 层负责 |
| chunked 流式写入、心跳注释、Context 取消、背压反馈 | 不建无界队列、不保证并发生产者的事件顺序 |
| 解析侧/写入侧内存上限 | 不提供 TLS、HTTP/2/3 推送、双向通信（用 WebSocket） |

依赖：`net::Socket`、`http::ResponseWriter`、`http::Request/Response`、
`Context`（取消传播）、`sync::Mutex`（Fiber/线程混合锁）。

---

## 2. 文件布局

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/network/sse.hpp` | `Error/Result`、`Event`、`StreamOptions`、`ParserLimits`、`Parser`、`Stream`、辅助函数 | 341 行 |
| `src/network/sse.cpp` | 编码、解析三件套、`Stream` 写路径与取消状态 | 808 行 |
| `include/go2cpp/sse.hpp`、`sse.h`、`network/sse.h` | 兼容转发头 | 各 3 行 |
| `tests/network/test_sse.cpp` | 编码、解析、流生命周期、取消、并发、跨线程取消 | 318 行 |
| `example/network/sse_basic_demo.cpp` | 基础示例 | 69 行 |
| `example/network/sse_demo.cpp` | 普通线程中的流写入、心跳、Context 取消 | 115 行 |
| `example/network/sse_full.cpp` | 8 个纯解析场景 | 219 行 |
| `docs/state-machines/Stream.md` | `Stream` 的 Idle/Open/Closed/Failed 状态表 | 43 行 |

构建：`src/network/sse.cpp` 编入 `go2cpp_network`（`cmake/Go2CppModules.cmake:53`），
无额外第三方依赖；`go2cpp` 命名空间下的 `SseEvent`/`SseParser`/`SseStream` 等别名在
`sse.hpp:334-341`。

---

## 3. API

### 3.1 结果与事件

| 类型/API | 语义 | 位置 |
|---|---|---|
| `enum class Error` | `InvalidEvent/MessageTooLarge/Cancelled/DeadlineExceeded/Closed/Io/Backpressure/Parse/ResourceExhausted` | `sse.hpp:55-66` |
| `struct Result` | `error`、`system_error`、`message`、`ok()`、`success()` | `:69-76` |
| `struct Event` | `id`、`event`、`data`、`retry_ms`（`optional<uint64_t>`） | `:93-107` |
| `bool Event::valid(std::string* reason = nullptr)` | 校验 `id`/`event` 不含 CR/LF/NUL | 声明 `:104`；实现 `sse.cpp:105-115` |
| `Result Event::encode(std::string& output, std::size_t max_bytes = 0)` | 编码成以空行结尾的 SSE 事件；`max_bytes=0` 表示本层不设上限 | 声明 `:106`；实现 `:130-190` |

事件字段不变量（`sse.hpp:88-92`）：`id`/`event` 必须是单行（不含 CR、LF、NUL），
否则 `valid()` 失败；`data` 可以多行，编码时按行拆成多个 `data:` 字段；
`retry_ms` 有值才输出 `retry:`。

### 3.2 写侧配置与 `Stream`

| 类型/API | 语义 | 位置 |
|---|---|---|
| `struct StreamOptions` | `max_event_bytes`（默认 1 MiB）、`heartbeat_interval`（默认 15s）、`write_timeout`（默认无限）、`add_accel_buffering_header`（默认 true） | `sse.hpp:110-121` |
| `Stream(net::Socket::Ptr, ContextPtr = {}, StreamOptions = {})` | 构造 chunked 写入器并注册取消回调 | 声明 `:269-270`；实现 `sse.cpp:495-504` |
| `Result start()` | 写 200 + `text/event-stream` 头，`Idle -> Open` | 声明 `:276`；实现 `:618-643` |
| `Result send(const Event&)` | 编码并写一条事件；失败进入 `Failed` | 声明 `:278`；实现 `:653-664` |
| `Result comment(std::string_view text = "heartbeat")` | 写 SSE 注释（默认 `: heartbeat`） | 声明 `:280`；实现 `:666-700` |
| `Result tick()` | 心跳到期才写 `: heartbeat\n\n`，否则不写字节 | 声明 `:282`；实现 `:702-712` |
| `Result close()` | 写 chunked 终止块；`Closed` 幂等，`Failed` 返回 `Closed` | 声明 `:288`；实现 `:723-741` |
| `StreamState state()` / `bool active()` | 状态查询；`active()` 等价于 `Open` | 声明 `:290-291`；实现 `:743-765` |
| `bool heartbeat_due()` / `next_heartbeat()` | 心跳是否到期/下一时间点 | 声明 `:292-293`；实现 `:767-784` |
| `const StreamOptions& options()` | 只读配置 | `:294` |

`StreamState` 四态（`sse.hpp:236`）：`Idle -> Open -> Closed/Failed`，状态只向前迁移，
`Failed` 粘性、只能重建对象（`docs/state-machines/Stream.md:10-26`）。

### 3.3 解析侧与辅助函数

| 类型/API | 语义 | 位置 |
|---|---|---|
| `struct ParserLimits` | `max_line_bytes`（1 MiB）、`max_event_bytes`（1 MiB）、`max_buffer_bytes`（2 MiB）、`max_retry_ms`（24h） | `sse.hpp:124-135` |
| `Parser(ParserLimits = {})` | 构造单消费者解析器 | 声明 `:158`；实现 `sse.cpp:192` |
| `void reset()` | 清空事件/缓冲/失败标记，保留限制；会清 `last_event_id` | 声明 `:161`；实现 `:194-207` |
| `Result feed(std::string_view, std::vector<Event>&)` | 追加字节并输出本次新完成的事件 | 声明 `:177`；实现 `:320-419` |
| `Result finish(std::vector<Event>&)` | 显式 EOF：冲刷末尾 CR 行并派发最后一个未闭合事件 | 声明 `:195`；实现 `:421-471` |
| `optional<uint64_t> retry_after()` | 最近一次 `retry` 字段建议 | `:197` |
| `const std::string& last_event_id()` | 最近事件 ID，供重连填 `Last-Event-ID` | `:199` |
| `void PrepareResponse(http::Response&, bool add_accel = true)` | 设 SSE 标准响应头，不写 body/不提交 socket | 声明 `:328`；实现 `sse.cpp:786-798` |
| `std::string LastEventId(const http::Request&)` | 读取 `Last-Event-ID`；含 CR/LF/NUL 返回空 | 声明 `:330`；实现 `:800-806` |

`Parser` 是**有状态单消费者**对象；多线程/多 Fiber 并发 `feed()` 必须由调用方用
`sync::Mutex` 串行化（`sse.hpp:138-155`）。

---

## 4. 设计

### 4.1 事件编码

`Event::encode()`（`sse.cpp:130-190`）的步骤：

1. 校验 `id`/`event`；失败返回 `InvalidEvent` 并清空 `output`（`:132-136`）；
2. `data` 的 CR、CRLF 统一归一化为 LF（`normalize_newlines`，`:48-63`、`:138-139`）；
3. 按固定顺序输出：`id:` → `event:` → `retry:`（缺省字段跳过，`:143-151`）；
4. `data` 按行拆成多个 `data:` 字段（`:153-172`）：
   - 空 data 输出 `data:\n`（注意没有空格，`:154-155`）；
   - 非空每行输出 `data: <line>\n`（`:160-165`）；
   - data 以换行结尾时额外输出一行空 `data: \n`（`:167-170`）；
5. 末尾补空行（`:173-174`）；
6. `max_bytes != 0` 且编码后超限 → `MessageTooLarge`（`:175-178`）；
7. `bad_alloc` → `ResourceExhausted`，其它异常 → `InvalidEvent`，都不向调用方抛异常
   （`:181-189`）。

线格式示例（`docs/sse.md:31-37` 的原始例子）：

```text
id: order-42
event: updated
data: status=ready
data: version=3

```

**边界**：`data` 内含 NUL 不会被 `valid()` 拒绝（只检查 `id`/`event`）；如果需要
杜绝 NUL，应在业务层自行校验。`encode()` 先完整构造字符串再检查上限，因此
`max_bytes` 只限制输出大小，不限制构造过程中的峰值内存。

### 4.2 增量解析

`Parser::feed()`（`sse.cpp:320-419`）：

1. **失败短路**：`m_failed` 为真时返回 `Parse`，必须 `reset()`（`:324-326`）；
2. **缓冲**：一次性丢弃上次已消费前缀，再检查 `max_buffer_bytes`，追加本次字节
   （`:328-336`）；用 `m_in_off` 游标避免逐行 `erase(0,...)` 的 O(n²)（`:351-352`）；
3. **BOM**：首次输入识别 `EF BB BF`；BOM 可能跨两次 feed 到达，不足 3 字节且是
   BOM 前缀时先等待（`:338-349`）；
4. **切行**：扫描 LF、CRLF、CR；`\r` 在缓冲区末尾时无法判定是 CR 还是 CRLF，
   保留等待下一字节（`:353-377`）；单行超过 `max_line_bytes` → `MessageTooLarge`
   （`:373-380`）；
5. **字段行**：`consume_line()`（`:256-306`）：
   - 空行 → `dispatch()`；
   - `:` 开头 → 注释，丢弃；
   - 按第一个 `:` 拆 field/value，value 去掉一个前导空格；无冒号时 field=整行、
     value 为空；
   - `data` 追加 value + `\n`；`event` 覆盖；`id` 覆盖并**立即**更新
     `m_last_event_id`（即使事件还没派发，重连也能拿到最新游标，`:285-293`）；
   - `retry` 只接受纯数字，超过 `max_retry_ms` 饱和到上限（`parse_retry`，
     `:71-94`、`:294-300`）；
   - 未知字段忽略；
   - 累计 `m_event_bytes` 超过上限 → `MessageTooLarge`（`:301-304`）。
6. **派发**：`dispatch()`（`:214-245`）只在空行触发；按 WHATWG 语义，只有出现过
   `data` 才产出 `Event`，没有 data 的块只更新 `last_event_id` 并清状态（`:214-225`）；
   data 末尾的 `\n` 去掉一个后作为 `Event::data`（`:227-229`）；派发后清空当前事件。

**EOF 语义**：`feed()` 不会猜测「没有空行结尾的半个事件」；调用方确认流结束时必须
调用 `finish()`（`sse.cpp:421-471`）：把末尾 `\r` 当作 CR-only 行尾、切出未完成行，
再 `dispatch()` 最后一个事件（`:431-447`）。可重复调用，第二次为空操作。这个设计
避免把截断数据误报为完整业务消息（`sse.hpp:29-33`、`:140-142`）。

**失败语义**：`feed()`/`finish()` 失败后 `m_failed=true`，后续调用返回 `Parse`，
必须 `reset()` 才能复用（`sse.cpp:209-212`、`:324-326`、`:422-424`）；`bad_alloc`
返回 `ResourceExhausted`（`:396-406`、`:448-458`）。

### 4.3 流式写入与背压

`Stream` 复用 `http::ResponseWriter` 输出 chunked 响应：

- `start()` 构造 200 响应，`PrepareResponse()` 设置
  `Content-Type: text/event-stream`、`Cache-Control: no-cache`、
  `Connection: keep-alive`，删除 `Content-Length`/`Transfer-Encoding`，默认加
  `X-Accel-Buffering: no`（`sse.cpp:786-798`）；
- `ResponseWriter::start()` 写头成功才进入 `HeadersCommitted`（即 `Open`）；
  头写失败置 `m_failed`（`sse.cpp:637-640`）；
- `send()` 在**锁外**编码（避免持锁做内存分配），再取 `m_mutex`、检查
  `HeadersCommitted`，最后 `write_locked()`（`:653-664`）；
- 所有写路径由 `m_mutex`（`sync::Mutex`）串行化；多生产者可安全调用 `send()`，
  但只保证 chunk/事件字节不交错，**不保证业务事件顺序**（`sse.hpp:250-253`）；
- 没有内部队列：写失败立即 `mark_failed_locked()`，后续 `send()`/`comment()`/
  `tick()` 返回 `Closed`，不会继续排队（`sse.cpp:551-556`、`:660-661`、`:696-697`）。

`write_locked()`（`sse.cpp:566-604`）的失败分类：

1. 写前检查 Context 取消 → `Cancelled`/`DeadlineExceeded`（`:567-570`）；
2. `ResponseWriter::write()` 返回 false 后再次检查取消（`write` 可能被取消唤醒，
   `:574-579`）；
3. 读 `socket->last_error()`；`EAGAIN`/`EWOULDBLOCK`/`ETIMEDOUT` 记为背压（`:580-583`）；
4. `ResponseWriter::write()` 只返回 bool，本地 `poll` 超时不会写 `SO_ERROR`；
   因此额外用「本次写耗时 ≥ `write_timeout`」判定慢消费者，置 `ETIMEDOUT`
   （`:584-594`）；
5. 返回 `Backpressure` 或 `Io`，并置 `m_failed`（`:595-600`）；
6. 写成功刷新心跳时间点（`:602-603`）。

**心跳**：`tick()` 是显式调度点，不占常驻线程；`heartbeat_interval <= 0` 关闭心跳；
到期写 `: heartbeat\n\n`（`:702-712`）。`heartbeat_due()` 只在 `Open` 且
`now >= m_next_heartbeat` 时为真（`:767-775`）。`m_next_heartbeat` 只在成功写入后刷新
（`:558-564`），所以慢消费者不会让心跳堆积。

**取消**：构造时用 `Context::Done().AddCallback()` 注册回调（`weak_ptr<State>`），
回调只做原子置位与 `socket->cancel_write()`，不持有 `Stream` 锁（`sse.cpp:512-528`）。
`cancellation_result()` 用 Context 的 `Err()` 区分 `DeadlineExceeded` 与 `Cancelled`
（`:537-549`）。`close()`/析构在**锁外**注销回调，避免与取消回调互等
（`:723-741`、`:506-510`）。

### 4.4 状态与取消的准确语义

`state()`（`sse.cpp:743-763`）的优先级是：`m_failed` → Context 取消 → `ResponseWriter`
状态。因此：

- `start()` 成功后为 `Open`；
- `close()` 成功后为 `Closed`；
- 写失败后 `m_failed=true`，为 `Failed`（粘性）；
- **仅** Context 取消（没有触发写失败）时，`state()` 返回 `Closed`，不是 `Failed`；
- 取消后如果又调用 `send()`/`tick()`，写路径返回取消并 `mark_failed_locked()`，
  此后状态才变为 `Failed`。

`docs/state-machines/Stream.md:23` 把「Context 取消回调」直接写成 `Open -> Failed`，
这是简化说法；以本节和 `sse.cpp:743-763` 为准。

### 4.5 断线与重连

模块本身**不做自动重连**，也不保存事件（`sse.hpp:5-8`）：

- 服务端应为可恢复事件设置 `Event::id`；
- 客户端重连时把 `Parser::last_event_id()` 放进 HTTP `Last-Event-ID` 请求头；
- 服务端用 `sse::LastEventId(request)` 读取它，再从业务事件日志恢复；
- `Parser::retry_after()` 只是服务端建议的重连间隔（毫秒），不构成自动重试保证；
- 连接中断时，客户端应调用 `Parser::finish()` 处理已收到的最后一行/最后一个事件，
  然后 `reset()` 开始新连接；不调用 `finish()` 时，未以空行闭合的事件不会被派发。

```cpp
go2cpp::http::Request request = go2cpp::http::NewRequest(
    go2cpp::http::Method::Get, "/events");
request.header.set("Accept", "text/event-stream");
request.header.set("Last-Event-ID", parser.last_event_id());
```

以上请求构造是使用方职责，SSE 模块只提供 `LastEventId()` 读取辅助函数。

---

## 5. 边界与已知限制

- **HTTP/1.1 only**：依赖 chunked 响应；代理可能缓冲，默认写
  `X-Accel-Buffering: no`，但网关仍需关闭缓冲；HTTP/2/3 推送未实现。
- **无 TLS**：SSE 只接收已连接的 `net::Socket`；`https` 由调用方在传输层解决。
- **无自动重连/事件存储**：`retry`/`Last-Event-ID` 只是协议字段，重连策略与事件日志
  由业务负责；没有跨连接回放、无限重放或 Exactly-Once。
- **单消费者解析器**：`Parser` 并发 `feed()` 是数据竞争；必须加锁或集中到一个 Fiber。
- **写入顺序**：`Stream` 只保证字节不交错，不保证多生产者的业务顺序；需要严格顺序
  时在上层排队。
- **无无界队列/背压**：写失败即 `Failed`，调用方应停止生产并关闭连接，避免慢客户端
  耗尽内存；`write_timeout` 默认无限，生产应设有限值。
- **`max_event_bytes=0`**：`StreamOptions` 的 0 表示不设本层上限；`ParserLimits` 的
  上限字段没有「0 表示无限」的统一约定（`max_line_bytes=0` 会让任何非空行超限），
  不要混用。
- **CR-only 行尾**：`feed()` 遇到缓冲区末尾的 `\r` 会等待；流结束时必须 `finish()`，
  否则最后一个 CR 行/事件可能留在缓冲里（`sse.cpp:353-377`、`:431-447`）。
- **BOM**：只在流首次输入时识别；中途出现的 BOM 会作为普通内容。
- **`id` 立即生效**：解析到 `id:` 行就更新 `last_event_id`，不等空行；这是断线重连
  的刻意设计，但也意味着「未派发事件的 id」也会成为游标。
- **取消不回滚**：Context 取消只让后续写失败并唤醒等待；已经写给客户端的数据不会
  撤回，事务语义应在发送前完成本地校验（`docs/sse.md:89` 的原始边界）。
- **对象所有权**：`Stream` 不关闭 Socket；`close()` 只写 chunked 终止块。销毁
  `Stream` 不会自动 `close()`，需要显式调用。
- **`state()` 是 `noexcept`**：内部捕获异常，调度器停止等无法 park 的场景按
  `Failed` 返回（`sse.cpp:743-763`）。
- **`send()` 先编码后取锁**：编码失败不会改变流状态；只有写失败才进入 `Failed`。

---

## 6. 测试

### 6.1 自动化用例

`tests/network/test_sse.cpp`（318 行）由 `run_sse_tests()` 执行
（`:310-318`），包含六组：

| 用例 | 覆盖点 | 位置 |
|---|---|---|
| `test_event_encoding` | 字段顺序、多行 data、空事件 `data:\n\n`、无限上限、非法 id/event、超限 retry、`PrepareResponse` 头、`LastEventId` 换行拒绝 | `:60-100` |
| `test_incremental_parser` | BOM、CRLF、注释、重复 data、retry、Last-Event-ID、断线前 id、未知字段、空 id 清除、行上限、失败后必须 reset | `:106-144` |
| `test_stream_lifecycle` | socketpair 上写响应头、chunked 事件、心跳 `tick()`、close 幂等、关闭后 send 失败 | `:150-180` |
| `test_context_cancellation` | 取消后 `send()` 返回 `Cancelled`/`Closed` | `:186-203` |
| `test_concurrent_producers` | 8 线程 × 20 事件并发写，全部成功且事件不交错 | `:210-275` |
| `test_cross_thread_socket_cancel` | 跨线程 `cancel_read()` 让阻塞 recv 返回 `ECANCELED` | `:282-302` |

注册：`tests/test_main.cpp:101` 在 `sse` 过滤器下调用 `run_sse_tests()`；
`cmake/Go2CppTests.cmake:42` 把 `test_sse.cpp` 编进 `go2cpp_tests`，
`go2cpp_tests` 注册为 ctest（`:216`）。运行：

```bash
GO2CPP_TEST_FILTER=sse ./build/go2cpp_tests
# 或
ctest --test-dir build -R go2cpp_tests --output-on-failure
```

### 6.2 示例

- `example/network/sse_basic_demo.cpp`（69 行）：最小事件发送。
- `example/network/sse_demo.cpp`（115 行）：普通线程中的流写入、注释心跳、仅更新
  游标的元数据事件、Context 取消。
- `example/network/sse_full.cpp`（219 行）：8 个纯解析场景——基本事件、重复 data、
  event/id/retry、任意字节分片、换行变体/BOM、注释、半个事件不派发、encode 往返
  （文件头 `:8-18`）。
- 以上在 `cmake/Go2CppTests.cmake:1009-1011` 登记。

### 6.3 覆盖缺口

- `Parser::finish()` 的 EOF 冲刷、末尾 CR 行、最后一个未闭合事件没有自动化用例；
- 没有 `max_buffer_bytes`、`max_event_bytes`、`max_retry_ms` 饱和的边界测试；
- 没有 `DeadlineExceeded`（只测了 `Cancelled`）、真实慢消费者 `Backpressure`、
  `write_timeout` 判定路径的测试；
- 没有真实 HTTP 服务器/浏览器 EventSource 的端到端互通测试；
- 测试均用 `socketpair(AF_UNIX)`，不覆盖代理缓冲、TCP 半关闭等真实网络行为。

---

## 7. 使用示例

```cpp
#include "go2cpp/network/sse.hpp"

void serve(go2cpp::net::Socket::Ptr socket, go2cpp::ContextPtr context) {
    go2cpp::sse::StreamOptions options;
    options.heartbeat_interval = std::chrono::seconds(15);
    options.write_timeout = std::chrono::seconds(5);
    options.max_event_bytes = 64 * 1024;

    go2cpp::sse::Stream stream(socket, context, options);
    if (!stream.start().ok()) return;

    go2cpp::sse::Event event;
    event.id = "order-42";
    event.event = "updated";
    event.data = "status=ready\nversion=3";
    if (!stream.send(event).ok()) return;  // Failed 后不要继续生产

    if (stream.heartbeat_due()) (void)stream.tick();
    (void)stream.close();
}
```

---

## 8. 相关文档

- `docs/modules/ws.md`：WebSocket，另一条建立在已连接 Socket 上的应用协议。
- `docs/state-machines/Stream.md`：`Stream` 状态机；§4.4 修正了取消状态的简化说法。
- `docs/sse.md`：已合并的旧使用说明（顶部有标注）。
- `docs/network.md`：`ResponseWriter` chunked 输出与网络层总览。
- `example/network/sse_full.cpp`：解析器边界的可运行示例。
