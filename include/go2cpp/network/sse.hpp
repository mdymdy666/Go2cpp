#pragma once

/**
 * @file sse.hpp
 * @brief Server-Sent Events（SSE）事件编码、增量解析和流式写入接口。
 * @details 本模块只负责 HTTP/1.1 SSE 的协议边界，不负责连接池、重连调度
 *          或业务事件源。SseStream 复用 ResponseWriter，因而普通线程和
 *          IOManager 管理的 Fiber 都可以使用；SseParser 可用于客户端或网关
 *          对无限长的 SSE 字节流做增量解析。
 *
 * ## 模块定位
 * HTTP/1.1 Server-Sent Events 的协议边界：事件编码、增量解析、流式写入。
 * 不负责重连调度、事件源与业务背压；写入复用 `http::ResponseWriter`，因此
 * 普通线程与 `IOManager` 的 Fiber 都能用。
 *
 * ## 本文件定义什么
 * - `Error` / `Result`：不跨线程抛异常的结果类型；
 * - `Event`：一个 SSE 事件（id/event/data/retry）与它的编码；
 * - `StreamOptions` / `ParserLimits`：写侧与解析侧的上限；
 * - `Parser`：可喂任意分片的增量解析器；
 * - `Stream`：单连接写入器（响应头、事件、心跳注释、关闭）；
 * - `PrepareResponse()` / `LastEventId()`：两个辅助函数。
 *
 * ## 阅读建议
 * 先看 `Event::encode()` 的字段顺序与多行 data 拆分，再看 `Parser::feed()`
 * 的行切分/派发规则，最后看 `Stream::start/send/close` 与心跳 `tick()`。
 *
 * ## 边界与陷阱
 * - **CR-only 行尾需要显式 EOF**：`Parser` 遇到 `\r` 时必须看下一个字节才能
 *   判定是 CR 还是 CRLF；若 `\r` 恰好是流的最后一个字节，`feed()` 只能保守
 *   等待。调用方确认流结束时必须调用 `finish()`：它把末尾 CR 当作 CR-only
 *   行尾，并派发最后一个未以空行闭合的事件。编码侧 `Event::encode()` 会把
 *   CR/CRLF 归一化为 LF，所以该边界只在解析方向；
 * - `Stream` 的锁是 `sync::Mutex`（Fiber/线程混合），因为持锁期间写可能 park；
 * - `Stream` 只保证字节不交错，不承诺业务事件顺序，也不建无界队列。
 */

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::sse {

/** @brief SSE 写入或解析结果类别。 */
enum class Error {
    None,
    InvalidEvent,
    MessageTooLarge,
    Cancelled,
    DeadlineExceeded,
    Closed,
    Io,
    Backpressure,
    Parse,
    ResourceExhausted,
};

/** @brief 不跨线程抛出异常的 SSE 操作结果。 */
struct Result {
    Error error{Error::None};
    int system_error{0};
    std::string message;

    bool ok() const noexcept { return error == Error::None; }
    static Result success() { return {}; }
};

/**
 * @brief 一个待发送或已经解析的 SSE 事件。
 * @details id 和 event 是单行字段，不能包含 CR、LF 或 NUL；data 可以包含
 *          多行，编码时会按 SSE 规范拆成多个 data 字段。retry 表示毫秒级重连
 *          建议，解析器会把它保留为无符号整数。
 *
 * ## 定位
 * `Stream::send()` 的输入，也是 `Parser` 的输出；`websocket` 与它无关，
 * 两者只是并列的应用协议。
 *
 * ## 不变量
 * - `id` / `event` 必须是单行（不含 CR/LF/NUL），否则 `valid()` 返回 false；
 * - `data` 可以多行，编码时按行拆成多个 `data:` 字段；
 * - `retry_ms` 有值才会输出 `retry:` 字段。
 */
struct Event {
    /// 事件 id（单行，不含 CR/LF/NUL）；encode() 写、Parser 解析时填。
    std::string id;
    /// 事件类型（单行）；encode() 写、Parser 解析时填。
    std::string event;
    /// 事件数据（可多行）；encode() 按行拆成多个 data 字段。
    std::string data;
    /// 重连建议（毫秒）；encode() 写 retry 字段，Parser 保留最近值。
    std::optional<std::uint64_t> retry_ms;

    /** @brief 校验事件字段；失败时写入可读原因。 */
    bool valid(std::string* reason = nullptr) const;
    /** @brief 编码成以空行结尾的 SSE 事件；失败时返回错误结果。 */
    Result encode(std::string& output, std::size_t max_bytes = 0) const;
};

/** @brief SSE 注释/心跳的默认限制。 */
struct StreamOptions {
    /// 单条事件上限（**编码后**字节数）；send()/comment() 用它拒绝过大的写入。
    /// 与 ParserLimits::max_line_bytes 的默认值自洽：编码后不超过 1MiB 的事件
    /// 不会产生超过 1MiB 的单行，默认 Parser 一定能解析默认 Stream 能发送的事件。
    std::size_t max_event_bytes{1024 * 1024};
    /// 心跳间隔；tick() 与 refresh_heartbeat_locked() 使用，<=0 表示关闭心跳。
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(15)};
    /// 单次写超时；构造 ResponseWriter 时传入。
    net::Socket::Duration write_timeout{net::Socket::Duration::max()};
    /// 是否加 X-Accel-Buffering: no；PrepareResponse() 读取，避免 nginx 缓冲。
    bool add_accel_buffering_header{true};
};

/** @brief 增量解析器的内存上限。 */
struct ParserLimits {
    /// 单行上限（含 `data: ` 等字段前缀）；feed()/finish() 切行前检查，
    /// 防止无换行的超长行。默认 1MiB 与 StreamOptions::max_event_bytes 对齐，
    /// 避免“默认发送端能编码、默认解析端却拒收”。
    std::size_t max_line_bytes{1024 * 1024};
    /// 单事件上限；consume_line()/dispatch() 累计检查。
    std::size_t max_event_bytes{1024 * 1024};
    /// 未完成行的缓冲上限；feed() 追加前检查。
    std::size_t max_buffer_bytes{2 * 1024 * 1024};
    /// retry 字段上限；parse_retry() 用它做饱和。
    std::uint64_t max_retry_ms{24ULL * 60ULL * 60ULL * 1000ULL};
};

/**
 * @brief SSE 增量解析器。
 * @details 支持任意字节分片、LF/CRLF/CR 换行、UTF-8 BOM、注释、重复 data
 *          字段和 Last-Event-ID。隐式 EOF（不再 feed）不会把没有空行结束的
 *          半个事件强行派发，避免把截断数据误报为完整业务消息；调用方明确
 *          知道流已结束时，应调用 `finish()` 冲刷末尾 CR 行与最后一个事件。
 * @note Parser 是有状态的单消费者对象；多个线程/Fiber 同时 feed 必须由调用方
 *       使用 sync::Mutex 或把输入集中到一个 Fiber，否则会产生数据竞争。
 *
 * ## 定位
 * 客户端/网关用来解析无限长的 SSE 字节流；`test_sse_*` 直接驱动它做边界测试。
 *
 * ## 不变量
 * - 单消费者、有状态：并发 feed 必须由调用方加锁或集中到单 Fiber；
 *   `m_buffer` 只追加，消费用 `m_in_off` 游标推进，每次 feed/finish 结束后
 *   一次性压缩已消费前缀（避免逐行 `erase(0, ...)` 的 O(n²) 搬移）；
 * - 只有空行才派发事件；没有 data 的块只更新 `last_event_id`；
 * - 失败后 `m_failed` 置位，必须 `reset()` 才能复用。
 */
class Parser final {
public:
    explicit Parser(ParserLimits limits = {});

    /** @brief 清空当前事件和未完成字节，保留限制配置。 */
    void reset();
    /**
     * @brief 输入一段网络字节并追加完整事件。
     * @param bytes 本次收到的任意长度字节，可为空。
     * @param events 输出容器，函数只追加本次新完成的事件。
     * @return 成功返回 None；协议或内存限制错误返回 Parse/MessageTooLarge。
     *
     * ## 步骤
     * step1. 检查 `m_failed` 与缓冲区上限，追加本次字节；
     * step2. 首次输入时识别并剥离 UTF-8 BOM；
     * step3. 循环切行（LF / CRLF / CR，CR 在末尾时等待下一字节）；
     * step4. 逐行 `consume_line()` 累积字段，空行触发 `dispatch()` 派发事件。
     *
     * ## 失败语义
     * 超限返回 `MessageTooLarge` 并置 `m_failed`；协议错误返回 `Parse`，需 reset()。
     */
    Result feed(std::string_view bytes, std::vector<Event>& events);
    /**
     * @brief 声明输入已到 EOF：冲刷末尾 CR 行与最后一个未闭合事件。
     * @param events 输出容器，函数只追加本次新完成的事件。
     * @return 成功返回 None；失败语义与 feed() 相同（失败后必须 reset()）。
     *
     * ## 语义
     * - 缓冲末尾的 `\r` 视为 CR-only 行尾，不再等待下一个字节；
     * - **没有空行结尾的最后一行也按 EOF 行尾处理**，并派发累积了 data 的
     *   最后一个事件；这是调用方显式声明 EOF 才有的行为，`feed()` 本身仍
     *   保持“不猜截断数据”的保守策略；
     * - 可重复调用：第二次调用时缓冲与当前事件已清空，直接成功。
     *
     * ## 步骤
     * step1. 压缩已消费前缀；
     * step2. 若仍有未完成行，按 EOF 行尾切出并 `consume_line()`；
     * step3. `dispatch()` 派发最后一个事件（无 data 时只更新 last_event_id）。
     */
    Result finish(std::vector<Event>& events);
    /** @brief 返回最近一次 retry 字段建议。 */
    std::optional<std::uint64_t> retry_after() const noexcept { return m_retry_after; }
    /** @brief 返回服务器最近发送的事件 ID，供重连时填入 Last-Event-ID。 */
    const std::string& last_event_id() const noexcept { return m_last_event_id; }
    const ParserLimits& limits() const noexcept { return m_limits; }

private:
    Result consume_line(std::string line, std::vector<Event>& events);
    Result dispatch(std::vector<Event>& events);
    Result fail(Error error, std::string message);

    /// 解析上限快照；构造时固定，feed()/consume_line() 使用。
    ParserLimits m_limits;
    /// 未消费字节；feed() 追加，消费用 m_in_off 游标，压缩时一次性 erase。
    std::string m_buffer;
    /// m_buffer 中已消费前缀的长度；feed()/finish() 推进，reset() 清零。
    std::size_t m_in_off{0};
    /// 当前事件的 event 字段；空行派发后清空。
    std::string m_event_name;
    /// 当前事件的 id 字段；派发后按需更新 last_event_id。
    std::string m_event_id;
    /// 当前事件累积的 data（多行用 \n 连接）；派发后清空。
    std::string m_data;
    /// 当前事件已累计字节数；每次追加时与上限比较。
    std::size_t m_event_bytes{0};
    /// 当前事件的 retry 值；派发后清空。
    std::optional<std::uint64_t> m_event_retry;
    /// 本轮是否出现过 id 字段；dispatch() 据此决定是否更新 last_event_id。
    bool m_event_id_seen{false};
    /// 最近一次 retry 建议；`retry_after()` 只读。
    std::optional<std::uint64_t> m_retry_after;
    /// 最近一次事件 id；`last_event_id()` 供重连填 Last-Event-ID。
    std::string m_last_event_id;
    /// 是否已处理过 BOM；feed() 首次输入时置位。
    bool m_seen_bom{false};
    /// 解析器失败标记；fail() 置位，reset() 清零。
    bool m_failed{false};
};

/** @brief SSE 流状态；状态只向前迁移，失败后不能复用连接。 */
enum class StreamState { Idle, Open, Closed, Failed };

/**
 * @brief 单连接 SSE 流写入器。
 * @details 依赖 net::Socket、http::ResponseWriter 和 Context；向上层提供响应
 *          头、事件、注释心跳和关闭操作。一个实例内部串行化写入，所以可以由
 *          多个生产者安全调用 send；互斥只保证字节不交错，不承诺业务事件顺序，
 *          生产者仍应控制顺序和业务背压，模块不会
 *          建立无界队列。Context 取消时会标记流并调用 cancel_write 唤醒阻塞写。
 *
 * ## 定位
 * 被服务端 handler 创建并持有 `Ptr`；依赖 `net::Socket`、`http::ResponseWriter`
 * 与 `Context`（取消传播）。客户端侧对应 `Parser`。
 *
 * ## 生命周期与线程模型
 * `std::shared_ptr` 持有；所有写入由 `m_mutex` 串行化，多生产者可安全调用
 * `send()`，但事件顺序由调用方负责。`Context` 取消时标记流并唤醒阻塞写。
 *
 * ## 状态机
 * `Idle` -(start)-> `Open` -(close)-> `Closed`；任一次写失败 -> `Failed`。
 * 只有 `Open` 允许 `send()`/`comment()`/`tick()`；`close()` 在 `Closed`
 * 状态幂等成功，`Failed` 状态不写任何字节并返回 `Error::Closed`。
 * 完整转化表见 `docs/state-machines/Stream.md`。
 *
 * ## 不变量
 * - `state()` 由 `m_failed`、Context 取消标记与 `m_writer.state()` 共同决定；
 * - 进入 `Failed` 后不再尝试写，也不复位（只能重建 Stream）；
 * - 心跳时间点 `m_next_heartbeat` 只在成功写入后刷新。
 */
class Stream final {
public:
    using Ptr = std::shared_ptr<Stream>;

    Stream(net::Socket::Ptr socket, ContextPtr context = {},
           StreamOptions options = {});
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /** @brief 发送 200 和 text/event-stream 响应头。 */
    Result start();
    /** @brief 编码并发送一条事件，失败后流进入 Failed。 */
    Result send(const Event& event);
    /** @brief 发送 SSE 注释，默认内容为 heartbeat。 */
    Result comment(std::string_view text = "heartbeat");
    /** @brief 到达心跳时间时发送注释，否则不写入任何字节。 */
    Result tick();
    /**
     * @brief 发送 chunked 终止块。
     * @note 仅 `Closed` 状态幂等成功；`Failed` 状态返回 `Error::Closed`
     *       （连接已不可复用，不再尝试写终止块），与 `StreamState` 一致。
     */
    Result close();

    StreamState state() const noexcept;
    bool active() const noexcept;
    bool heartbeat_due() const noexcept;
    std::chrono::steady_clock::time_point next_heartbeat() const noexcept;
    const StreamOptions& options() const noexcept { return m_options; }

private:
    /// pimpl：共享取消状态（原子 cancelled 标记），定义在 sse.cpp。
    struct State;
    Result cancellation_result() const;
    Result write_locked(std::string_view encoded);
    void arm_context_callback();
    void disarm_context_callback() noexcept;
    void mark_failed_locked(Result result);
    void refresh_heartbeat_locked();

    /// 底层已连接 socket；构造 ResponseWriter 时传入，写失败时取 last_error。
    net::Socket::Ptr m_socket;
    /// 取消上下文；arm_context_callback() 注册回调，析构时注销。
    ContextPtr m_context;
    /// 写侧配置；构造时固定，send()/tick() 读取。
    StreamOptions m_options;
    /// chunked 写入器；start/send/close 都经它写字节。
    http::ResponseWriter m_writer;
    /// 共享取消状态（Context 回调与写路径共用）；cancelled 为原子量。
    std::shared_ptr<State> m_state;
    // 写入可能在 Fiber 中 park，必须使用 Fiber/线程混合锁，不能使用裸 std::mutex。
    /// 写锁（Fiber/线程混合）；所有写路径持有，state() 也加锁读。
    mutable sync::Mutex m_mutex;
    /// 流失败标记；mark_failed_locked() 置位，state() 据此返回 Failed。
    bool m_failed{false};
    /// 下一次心跳时间点；refresh_heartbeat_locked() 刷新，tick() 比较。
    std::chrono::steady_clock::time_point m_next_heartbeat{};
    /// Context 回调 id；arm_context_callback() 写入、disarm 清零。
    DoneSignal::CallbackId m_callback_id{0};
};

/** @brief 设置 SSE 响应的标准头；不会写 body 或提交 socket。 */
void PrepareResponse(http::Response& response, bool add_accel_buffering_header = true);
/** @brief 读取请求中的 Last-Event-ID；非法换行会返回空字符串。 */
std::string LastEventId(const http::Request& request);

}  // namespace go2cpp::sse

namespace go2cpp {
using SseEvent = sse::Event;
using SseParser = sse::Parser;
using SseStream = sse::Stream;
using SseStreamOptions = sse::StreamOptions;
using SseResult = sse::Result;
using SseError = sse::Error;
}  // namespace go2cpp
