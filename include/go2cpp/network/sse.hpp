#pragma once

/**
 * @file sse.hpp
 * @brief Server-Sent Events（SSE）事件编码、增量解析和流式写入接口。
 * @details 本模块只负责 HTTP/1.1 SSE 的协议边界，不负责连接池、重连调度
 *          或业务事件源。SseStream 复用 ResponseWriter，因而普通线程和
 *          IOManager 管理的 Fiber 都可以使用；SseParser 可用于客户端或网关
 *          对无限长的 SSE 字节流做增量解析。
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
 */
struct Event {
    std::string id;
    std::string event;
    std::string data;
    std::optional<std::uint64_t> retry_ms;

    /** @brief 校验事件字段；失败时写入可读原因。 */
    bool valid(std::string* reason = nullptr) const;
    /** @brief 编码成以空行结尾的 SSE 事件；失败时返回错误结果。 */
    Result encode(std::string& output, std::size_t max_bytes = 0) const;
};

/** @brief SSE 注释/心跳的默认限制。 */
struct StreamOptions {
    std::size_t max_event_bytes{1024 * 1024};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(15)};
    net::Socket::Duration write_timeout{net::Socket::Duration::max()};
    bool add_accel_buffering_header{true};
};

/** @brief 增量解析器的内存上限。 */
struct ParserLimits {
    std::size_t max_line_bytes{64 * 1024};
    std::size_t max_event_bytes{1024 * 1024};
    std::size_t max_buffer_bytes{2 * 1024 * 1024};
    std::uint64_t max_retry_ms{24ULL * 60ULL * 60ULL * 1000ULL};
};

/**
 * @brief SSE 增量解析器。
 * @details 支持任意字节分片、LF/CRLF/CR 换行、UTF-8 BOM、注释、重复 data
 *          字段和 Last-Event-ID。EOF 不会把没有空行结束的半个事件强行派发，
 *          这样可以避免把截断数据误报为完整业务消息。
 * @note Parser 是有状态的单消费者对象；多个线程/Fiber 同时 feed 必须由调用方
 *       使用 sync::Mutex 或把输入集中到一个 Fiber，否则会产生数据竞争。
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
     */
    Result feed(std::string_view bytes, std::vector<Event>& events);
    /** @brief 返回最近一次 retry 字段建议。 */
    std::optional<std::uint64_t> retry_after() const noexcept { return m_retry_after; }
    /** @brief 返回服务器最近发送的事件 ID，供重连时填入 Last-Event-ID。 */
    const std::string& last_event_id() const noexcept { return m_last_event_id; }
    const ParserLimits& limits() const noexcept { return m_limits; }

private:
    Result consume_line(std::string line, std::vector<Event>& events);
    Result dispatch(std::vector<Event>& events);
    Result fail(Error error, std::string message);

    ParserLimits m_limits;
    std::string m_buffer;
    std::string m_event_name;
    std::string m_event_id;
    std::string m_data;
    std::size_t m_event_bytes{0};
    std::optional<std::uint64_t> m_event_retry;
    bool m_event_id_seen{false};
    std::optional<std::uint64_t> m_retry_after;
    std::string m_last_event_id;
    bool m_seen_bom{false};
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
    /** @brief 发送 chunked 终止块；重复 close 幂等成功。 */
    Result close();

    StreamState state() const noexcept;
    bool active() const noexcept;
    bool heartbeat_due() const noexcept;
    std::chrono::steady_clock::time_point next_heartbeat() const noexcept;
    const StreamOptions& options() const noexcept { return m_options; }

private:
    struct State;
    Result cancellation_result() const;
    Result write_locked(std::string_view encoded);
    void arm_context_callback();
    void disarm_context_callback() noexcept;
    void mark_failed_locked(Result result);
    void refresh_heartbeat_locked();

    net::Socket::Ptr m_socket;
    ContextPtr m_context;
    StreamOptions m_options;
    http::ResponseWriter m_writer;
    std::shared_ptr<State> m_state;
    // 写入可能在 Fiber 中 park，必须使用 Fiber/线程混合锁，不能使用裸 std::mutex。
    mutable sync::Mutex m_mutex;
    bool m_failed{false};
    std::chrono::steady_clock::time_point m_next_heartbeat{};
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
