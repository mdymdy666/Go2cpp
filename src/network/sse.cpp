/**
 * @file sse.cpp
 * @brief SSE 事件编码、增量解析和流式发送实现。
 *
 * ## 模块定位
 * `sse.hpp` 的实现：事件编码、增量解析与流式写入。IO 全部经
 * `http::ResponseWriter` / `Socket`，本文件不感知调度器。
 *
 * ## 本文件定义什么
 * - 匿名命名空间里的纯工具：换行归一化、retry 解析、行/字段校验；
 * - `Event::valid()` / `Event::encode()`：编码侧的校验与序列化；
 * - `Parser::feed()` / `consume_line()` / `dispatch()`：解析侧三件套；
 * - `Stream` 的写路径：`start()` / `send()` / `comment()` / `tick()` / `close()`。
 *
 * ## 阅读建议
 * 先看 `Parser::feed()` 的 BOM/切行逻辑，再看 `consume_line()` 的字段分派，
 * 最后看 `Stream::write_locked()` 如何把「取消/背压/IO 失败」归一成 Result。
 *
 * ## 边界与陷阱
 * - **CR-only 换行是确定性设计缺口，不是 flaky**：`feed()` 遇到缓冲末尾的 `\r`
 *   只能等待下一个字节才能判定 CR/CRLF；若流在此结束，该行与事件永不派发
 *   （缺 EOF 冲刷接口 `finish()`）。编码侧会把 CR/CRLF 归一成 LF，缺口仅在解析侧；
 * - `Parser` 失败后必须 `reset()`；继续 feed 只会重复返回 Parse 错误；
 * - `Stream` 的失败是**单向**的：`mark_failed_locked()` 之后不再尝试写。
 */

#include "go2cpp/network/sse.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <utility>

namespace go2cpp::sse {
namespace {

bool contains_line_break_or_nul(std::string_view value) noexcept {
    return value.find('\r') != std::string_view::npos ||
           value.find('\n') != std::string_view::npos ||
           value.find('\0') != std::string_view::npos;
}

std::string normalize_newlines(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '\r') {
            if (index + 1 < value.size() && value[index + 1] == '\n') {
                ++index;
            }
            result.push_back('\n');
        } else {
            result.push_back(current);
        }
    }
    return result;
}

void set_reason(std::string* reason, std::string value) {
    if (reason != nullptr) {
        *reason = std::move(value);
    }
}

bool parse_retry(std::string_view value, std::uint64_t maximum,
                 std::uint64_t& output) noexcept {
    if (value.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            parsed = maximum;
            break;
        }
        parsed = parsed * 10 + digit;
        if (parsed > maximum) {
            parsed = maximum;
            break;
        }
    }
    output = parsed;
    return true;
}

Result invalid_result(Error error, std::string message) {
    Result result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

}  // namespace

bool Event::valid(std::string* reason) const {
    if (contains_line_break_or_nul(id)) {
        set_reason(reason, "SSE id 不能包含 CR、LF 或 NUL");
        return false;
    }
    if (contains_line_break_or_nul(event)) {
        set_reason(reason, "SSE event 不能包含 CR、LF 或 NUL");
        return false;
    }
    return true;
}

/**
 * @brief 把一个事件编码成 SSE 线格式（以空行结尾）。
 *
 * ## 步骤
 * step1. 校验 id/event 不含 CR/LF/NUL；
 * step2. data 的换行归一化成 LF（CR、CRLF 都变 LF）；
 * step3. 依次输出 `id:` / `event:` / `retry:`（缺省字段跳过）；
 * step4. data 按行拆成多个 `data:` 字段（空 data 也输出 `data:`）；
 * step5. 末尾补空行；超过 `max_bytes` 返回 `MessageTooLarge`。
 *
 * ## 异常安全
 * 内部捕获 `bad_alloc` 返回 `ResourceExhausted`，不向调用方抛异常。
 */
Result Event::encode(std::string& output, std::size_t max_bytes) const {
    try {
    std::string reason;
    if (!valid(&reason)) {
        output.clear();
        return invalid_result(Error::InvalidEvent, std::move(reason));
    }

    // step2: 统一成 LF，避免下游把 CR 当内容。
    std::string normalized = normalize_newlines(data);
    // step1: 先编码，失败直接返回（流状态不变）。
    std::string encoded;
    encoded.reserve(id.size() + event.size() + normalized.size() + 32);
    if (!id.empty()) {
        encoded.append("id: ").append(id).append("\n");
    }
    if (!event.empty()) {
        encoded.append("event: ").append(event).append("\n");
    }
    if (retry_ms.has_value()) {
        encoded.append("retry: ").append(std::to_string(*retry_ms)).append("\n");
    }

    // 空 data 也输出 data:，让编码结果可被其他 SSE 实现识别为一个空事件。
    if (normalized.empty()) {
        encoded.append("data:\n");
    } else {
        std::size_t begin = 0;
        for (;;) {
            const std::size_t end = normalized.find('\n', begin);
            encoded.append("data: ");
            if (end == std::string::npos) {
                encoded.append(normalized, begin, std::string::npos).append("\n");
                break;
            }
            encoded.append(normalized, begin, end - begin).append("\n");
            begin = end + 1;
            if (begin == normalized.size()) {
                encoded.append("data: \n");
                break;
            }
        }
    }
    // step5: 事件以空行结束（SSE 的派发边界）。
    encoded.append("\n");
    if (max_bytes != 0 && encoded.size() > max_bytes) {
        output.clear();
        return invalid_result(Error::MessageTooLarge, "SSE 事件超过配置的最大字节数");
    }
        output = std::move(encoded);
        return Result::success();
    } catch (const std::bad_alloc&) {
        output.clear();
        Result result;
        result.error = Error::ResourceExhausted;
        return result;
    } catch (...) {
        output.clear();
        return invalid_result(Error::InvalidEvent, "SSE 事件编码失败");
    }
}

Parser::Parser(ParserLimits limits) : m_limits(std::move(limits)) {}

void Parser::reset() {
    m_buffer.clear();
    m_event_name.clear();
    m_event_id.clear();
    m_data.clear();
    m_event_bytes = 0;
    m_event_retry.reset();
    m_event_id_seen = false;
    m_retry_after.reset();
    m_last_event_id.clear();
    m_seen_bom = false;
    m_failed = false;
}

Result Parser::fail(Error error, std::string message) {
    m_failed = true;
    return invalid_result(error, std::move(message));
}

Result Parser::dispatch(std::vector<Event>& events) {
    // 按 WHATWG 语义，只有收到 data 字段才派发事件；但 id 仍然会更新。
    if (m_data.empty()) {
        if (m_event_id_seen) {
            m_last_event_id = m_event_id;
        }
        m_event_name.clear();
        m_event_retry.reset();
        m_event_id_seen = false;
        m_event_bytes = 0;
        return Result::success();
    }

    if (m_data.back() == '\n') {
        m_data.pop_back();
    }
    Event event;
    event.id = m_event_id;
    event.event = m_event_name;
    event.data = m_data;
    event.retry_ms = m_event_retry;
    if (m_event_id_seen) {
        m_last_event_id = m_event_id;
    }
    events.push_back(std::move(event));
    m_event_name.clear();
    m_data.clear();
    m_event_retry.reset();
    m_event_id_seen = false;
    m_event_bytes = 0;
    return Result::success();
}

/**
 * @brief 解析一行 SSE 字段并累积到当前事件。
 *
 * ## 步骤
 * step1. 空行 -> 交给 `dispatch()` 派发事件；
 * step2. `:` 开头 -> 注释行，直接丢弃；
 * step3. 按第一个 `:` 拆成 field/value（value 去掉一个前导空格）；
 * step4. 分派 `event` / `data` / `id` / `retry`，并累计事件字节数。
 */
Result Parser::consume_line(std::string line, std::vector<Event>& events) {
    if (line.empty()) {
        return dispatch(events);
    }
    if (line.front() == ':') {
        // 注释用于心跳，不应进入业务事件。
        return Result::success();
    }

    const std::size_t colon = line.find(':');
    std::string_view field(line);
    std::string_view value;
    if (colon == std::string::npos) {
        value = {};
    } else {
        field = std::string_view(line).substr(0, colon);
        value = std::string_view(line).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "data") {
        m_data.append(value);
        m_data.push_back('\n');
        m_event_bytes += value.size() + 1;
    } else if (field == "event") {
        m_event_name.assign(value);
        m_event_bytes += value.size();
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            m_event_id.assign(value);
            m_event_id_seen = true;
            // Last-Event-ID 是解析状态，不应等到空行才发布；连接在一条
            // id 行后中断时，重连仍必须携带最新游标。
            m_last_event_id = m_event_id;
            m_event_bytes += value.size();
        }
    } else if (field == "retry") {
        std::uint64_t retry = 0;
        if (parse_retry(value, m_limits.max_retry_ms, retry)) {
            m_retry_after = retry;
            m_event_retry = retry;
        }
    }
    if (m_limits.max_event_bytes != 0 &&
        (m_event_bytes > m_limits.max_event_bytes || m_data.size() > m_limits.max_event_bytes)) {
        return fail(Error::MessageTooLarge, "SSE 事件超过解析器内存上限");
    }
    return Result::success();
}

/**
 * @brief 喂入一段字节并派发所有完整事件。
 *
 * ## 步骤
 * step1. 检查失败标记与缓冲上限，追加本次字节；
 * step2. 首次输入时识别/剥离 UTF-8 BOM（可能跨两次 feed）；
 * step3. 循环找行尾（LF / CRLF / CR），`\r` 在末尾时保留等待下一字节；
 * step4. 每行交给 `consume_line()`，空行触发 `dispatch()` 追加到 events。
 *
 * ## 失败语义
 * 超限 -> `MessageTooLarge` 并置 `m_failed`；异常统一转成 `Parse`/`ResourceExhausted`。
 */
Result Parser::feed(std::string_view bytes, std::vector<Event>& events) {
    std::vector<Event> pending_events;
    try {
    // step1: 失败后必须 reset，不能继续喂()。
    if (m_failed) {
        return invalid_result(Error::Parse, "SSE 解析器已经失败，必须 reset 后重用");
    }
    if (m_buffer.size() > m_limits.max_buffer_bytes ||
        bytes.size() > m_limits.max_buffer_bytes - m_buffer.size()) {
        return fail(Error::MessageTooLarge, "SSE 未完成行超过缓冲区上限");
    }
    m_buffer.append(bytes);

    // step2: BOM 可能分两次到达，不足 3 字节时先等待。
    if (!m_seen_bom) {
        static constexpr std::string_view bom("\xEF\xBB\xBF", 3);
        if (m_buffer.size() < bom.size() && bom.substr(0, m_buffer.size()) == m_buffer) {
            return Result::success();
        }
        if (m_buffer.size() >= bom.size() &&
            std::string_view(m_buffer.data(), bom.size()) == bom) {
            m_buffer.erase(0, bom.size());
        }
        m_seen_bom = true;
    }

    for (;;) {
        // step3: 找行尾；CR 在末尾时 break，等下一个字节判定 CR/CRLF。
        std::size_t position = std::string::npos;
        std::size_t ending_size = 0;
        for (std::size_t index = 0; index < m_buffer.size(); ++index) {
            if (m_buffer[index] == '\n') {
                position = index;
                ending_size = 1;
                break;
            }
            if (m_buffer[index] == '\r') {
                if (index + 1 == m_buffer.size()) {
                    break;
                }
                position = index;
                ending_size = (m_buffer[index + 1] == '\n') ? 2 : 1;
                break;
            }
        }
        if (position == std::string::npos) {
            if (m_buffer.size() > m_limits.max_line_bytes) {
                return fail(Error::MessageTooLarge, "SSE 行超过最大长度");
            }
            break;
        }
        if (position > m_limits.max_line_bytes) {
            return fail(Error::MessageTooLarge, "SSE 行超过最大长度");
        }
        std::string line = m_buffer.substr(0, position);
        m_buffer.erase(0, position + ending_size);
        const Result result = consume_line(std::move(line), pending_events);
        if (!result.ok()) {
            return result;
        }
    }
        events.insert(events.end(), std::make_move_iterator(pending_events.begin()),
                      std::make_move_iterator(pending_events.end()));
        return Result::success();
    } catch (const std::bad_alloc&) {
        m_failed = true;
        std::string().swap(m_buffer);
        std::string().swap(m_event_name);
        std::string().swap(m_event_id);
        std::string().swap(m_data);
        m_event_bytes = 0;
        Result result;
        result.error = Error::ResourceExhausted;
        return result;
    } catch (...) {
        m_failed = true;
        std::string().swap(m_buffer);
        std::string().swap(m_event_name);
        std::string().swap(m_event_id);
        std::string().swap(m_data);
        m_event_bytes = 0;
        Result result;
        result.error = Error::Parse;
        return result;
    }
}

/**
 * @brief `sse::Stream` 的跨线程取消状态。
 *
 * ## 定位
 * `Stream` 的私有共享状态：Context 取消回调通过 `weak_ptr` 观察它，写路径
 * 通过 `cancelled` 判断是否继续；只保存取消标记与用于唤醒写等待的 socket。
 *
 * ## 状态机
 * 只有两态：`Running`（`cancelled == false`）与 `Cancelled`（`cancelled == true`，
 * 置真后不复位）。`Stream` 自身的 Idle/Open/Closed/Failed 见
 * `docs/state-machines/Stream.md`。
 *
 * ## 不变量
 * - `cancelled` 只从 false 变 true；
 * - `socket` 可为空（构造早期），使用前必须判空；
 * - 回调只做原子置位与 `cancel_write()`，不持有 Stream 锁。
 */
struct Stream::State {
    std::atomic<bool> cancelled{false}; ///< 是否已取消；Context 回调置真，写路径/state() 读取
    net::Socket::Ptr socket;            ///< 用于 cancel_write 的 socket 副本；构造时设置，回调判空后使用
};

Stream::Stream(net::Socket::Ptr socket, ContextPtr context, StreamOptions options)
    : m_socket(std::move(socket)),
      m_context(std::move(context)),
      m_options(std::move(options)),
      m_writer(m_socket, m_options.write_timeout),
      m_state(std::make_shared<State>()) {
    m_state->socket = m_socket;
    m_next_heartbeat = std::chrono::steady_clock::time_point::max();
    arm_context_callback();
}

Stream::~Stream() {
    // step4: 注销回调放在锁外，避免与取消回调互等。
    disarm_context_callback();
    m_state.reset();
}

void Stream::arm_context_callback() {
    if (!m_context || !m_state) {
        return;
    }
    const std::weak_ptr<State> weak_state = m_state;
    m_callback_id = m_context->Done().AddCallback([weak_state] {
        const auto state = weak_state.lock();
        if (!state) {
            return;
        }
        state->cancelled.store(true, std::memory_order_release);
        if (state->socket) {
            // cancel_write 只唤醒当前写等待，不把 Socket 的所有权交给回调。
            state->socket->cancel_write();
        }
    });
}

void Stream::disarm_context_callback() noexcept {
    if (m_context && m_callback_id != 0) {
        m_context->Done().RemoveCallback(m_callback_id);
        m_callback_id = 0;
    }
}

Result Stream::cancellation_result() const {
    if (!m_state || m_state->cancelled.load(std::memory_order_acquire) ||
        (m_context && m_context->IsDone())) {
        if (m_context) {
            const ErrorPtr error = m_context->Err();
            if (error && Is(error, DeadlineExceededError())) {
                return invalid_result(Error::DeadlineExceeded, "SSE Context 已到期");
            }
        }
        return invalid_result(Error::Cancelled, "SSE Context 已取消");
    }
    return Result::success();
}

void Stream::mark_failed_locked(Result result) {
    (void)result;
    // ResponseWriter 的写失败不可恢复；禁止后续生产者继续向同一连接写入，
    // 避免把半个 chunk 当成下一条完整事件。
    m_failed = true;
}

void Stream::refresh_heartbeat_locked() {
    if (m_options.heartbeat_interval > std::chrono::milliseconds::zero()) {
        m_next_heartbeat = std::chrono::steady_clock::now() + m_options.heartbeat_interval;
    } else {
        m_next_heartbeat = std::chrono::steady_clock::time_point::max();
    }
}

Result Stream::write_locked(std::string_view encoded) {
    const Result cancelled = cancellation_result();
    if (!cancelled.ok()) {
        return cancelled;
    }
    if (!m_writer.write(encoded)) {
        const Result cancelled_after_write = cancellation_result();
        if (!cancelled_after_write.ok()) {
            mark_failed_locked(cancelled_after_write);
            return cancelled_after_write;
        }
        const int system_error = m_socket ? m_socket->last_error() : EBADF;
        const bool backpressure = system_error == EAGAIN ||
                                   system_error == EWOULDBLOCK ||
                                   system_error == ETIMEDOUT;
        Result result = invalid_result(backpressure ? Error::Backpressure : Error::Io,
                                       backpressure ? "SSE 慢消费者导致写入超时"
                                                     : "SSE Socket 写入失败");
        result.system_error = system_error;
        mark_failed_locked(result);
        return result;
    }
    refresh_heartbeat_locked();
    return Result::success();
}

/**
 * @brief 发送 SSE 响应头，把流从 Idle 迁到 Open。
 *
 * ## 步骤
 * step1. 取写锁并校验状态（未启动、未失败、未取消）；
 * step2. 构造 200 + text/event-stream 响应；
 * step3. `ResponseWriter::start()` 写头；失败置 `m_failed` 并返回 Io；
 * step4. 刷新心跳时间点。
 *
 * ## 边界
 * socket 无效时不置 `m_failed`（还没写任何字节，换 socket 后可重新 start）。
 */
Result Stream::start() {
    std::lock_guard<sync::Mutex> lock(m_mutex);
    // step1: 只有 Idle 能启动。
    if (m_failed || m_writer.state() != http::StreamState::Idle) {
        return invalid_result(Error::Closed, "SSE 流已经启动或关闭");
    }
    const Result cancelled = cancellation_result();
    if (!cancelled.ok()) {
        return cancelled;
    }
    if (!m_socket || !m_socket->valid()) {
        // 已知偏差（R6）：这里**不**置 m_failed —— 还没有写过任何字节，
        // 流仍处于 Idle。调用方看到 Error::Io 后应丢弃该 Stream；若之后
        // 换成有效 socket，仍可重新 start。真正写失败才会进入 Failed。
        return invalid_result(Error::Io, "SSE 没有有效 Socket");
    }
    // step2: 组装标准 SSE 响应头。
    http::Response response;
    PrepareResponse(response, m_options.add_accel_buffering_header);
    if (!m_writer.start(std::move(response))) {
        m_failed = true;
        return invalid_result(Error::Io, "SSE 响应头写入失败");
    }
    refresh_heartbeat_locked();
    return Result::success();
}

/**
 * @brief 编码并发送一条事件。
 *
 * ## 步骤
 * step1. 在锁外完成编码（避免持锁做内存分配/格式化）；
 * step2. 取写锁并校验流处于 Open；
 * step3. `write_locked()` 写字节：成功则刷新心跳时间点，失败则标记 Failed。
 */
Result Stream::send(const Event& event) {
    std::string encoded;
    const Result encoded_result = event.encode(encoded, m_options.max_event_bytes);
    if (!encoded_result.ok()) {
        return encoded_result;
    }
    std::lock_guard<sync::Mutex> lock(m_mutex);
    if (m_failed || m_writer.state() != http::StreamState::HeadersCommitted) {
        return invalid_result(Error::Closed, "SSE 流尚未启动或已经关闭");
    }
    return write_locked(encoded);
}

Result Stream::comment(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return invalid_result(Error::InvalidEvent, "SSE 注释不能包含 NUL");
    }
    const std::string normalized = normalize_newlines(text);
    std::string encoded;
    if (normalized.empty()) {
        encoded = ":\n\n";
    } else {
        std::size_t begin = 0;
        for (;;) {
            const std::size_t end = normalized.find('\n', begin);
            encoded.append(": ");
            if (end == std::string::npos) {
                encoded.append(normalized, begin, std::string::npos).append("\n");
                break;
            }
            encoded.append(normalized, begin, end - begin).append("\n");
            begin = end + 1;
            if (begin == normalized.size()) {
                encoded.append(": \n");
                break;
            }
        }
        encoded.append("\n");
    }
    if (m_options.max_event_bytes != 0 && encoded.size() > m_options.max_event_bytes) {
        return invalid_result(Error::MessageTooLarge, "SSE 注释超过配置的最大字节数");
    }
    std::lock_guard<sync::Mutex> lock(m_mutex);
    if (m_failed || m_writer.state() != http::StreamState::HeadersCommitted) {
        return invalid_result(Error::Closed, "SSE 流尚未启动或已经关闭");
    }
    return write_locked(encoded);
}

Result Stream::tick() {
    std::lock_guard<sync::Mutex> lock(m_mutex);
    if (m_failed || m_writer.state() != http::StreamState::HeadersCommitted) {
        return invalid_result(Error::Closed, "SSE 流尚未启动或已经关闭");
    }
    if (m_options.heartbeat_interval <= std::chrono::milliseconds::zero() ||
        std::chrono::steady_clock::now() < m_next_heartbeat) {
        return Result::success();
    }
    return write_locked(": heartbeat\n\n");
}

/**
 * @brief 发送 chunked 终止块并注销 Context 回调。
 *
 * ## 步骤
 * step1. 取写锁；已 Closed 直接幂等成功；
 * step2. 非 Open 状态返回 Closed；
 * step3. `m_writer.close()` 写终止块，失败置 Failed；
 * step4. 出锁后注销 Context 回调（注销可能等待回调完成，不能在锁内做）。
 */
Result Stream::close() {
    {
        std::lock_guard<sync::Mutex> lock(m_mutex);
        if (m_writer.state() == http::StreamState::Closed) {
            return Result::success();
        }
        if (m_failed || m_writer.state() != http::StreamState::HeadersCommitted) {
            return invalid_result(Error::Closed, "SSE 流尚未启动或写入已经失败");
        }
        if (!m_writer.close()) {
            m_failed = true;
            return invalid_result(Error::Io, "SSE 终止块写入失败");
        }
    }
    // RemoveCallback 可能需要等待正在执行的取消回调；不要在流锁内调用，
    // 这样 Context 回调可以安全地唤醒 Socket。
    disarm_context_callback();
    return Result::success();
}

StreamState Stream::state() const noexcept {
    try {
        std::lock_guard<sync::Mutex> lock(m_mutex);
        if (m_failed) {
            return StreamState::Failed;
        }
        if (m_state && m_state->cancelled.load(std::memory_order_acquire)) {
            return StreamState::Closed;
        }
        switch (m_writer.state()) {
            case http::StreamState::Idle: return StreamState::Idle;
            case http::StreamState::HeadersCommitted: return StreamState::Open;
            case http::StreamState::Closed: return StreamState::Closed;
            case http::StreamState::Failed: return StreamState::Failed;
        }
    } catch (...) {
        // 查询接口是 noexcept；调度器停止期间无法 park 时按失败处理，
        // 不能让 C++ 异常穿过 noexcept 导致进程 terminate。
    }
    return StreamState::Failed;
}

bool Stream::active() const noexcept { return state() == StreamState::Open; }

bool Stream::heartbeat_due() const noexcept {
    try {
        std::lock_guard<sync::Mutex> lock(m_mutex);
        return m_writer.state() == http::StreamState::HeadersCommitted &&
               std::chrono::steady_clock::now() >= m_next_heartbeat;
    } catch (...) {
        return false;
    }
}

std::chrono::steady_clock::time_point Stream::next_heartbeat() const noexcept {
    try {
        std::lock_guard<sync::Mutex> lock(m_mutex);
        return m_next_heartbeat;
    } catch (...) {
        return std::chrono::steady_clock::time_point::max();
    }
}

void PrepareResponse(http::Response& response, bool add_accel_buffering_header) {
    response.status = http::Status::OK;
    response.close = false;
    response.body.clear();
    response.header.set("Content-Type", "text/event-stream");
    response.header.set("Cache-Control", "no-cache");
    response.header.set("Connection", "keep-alive");
    response.header.erase("Content-Length");
    response.header.erase("Transfer-Encoding");
    if (add_accel_buffering_header) {
        response.header.set("X-Accel-Buffering", "no");
    }
}

std::string LastEventId(const http::Request& request) {
    const std::string value = request.header.get("Last-Event-ID");
    if (contains_line_break_or_nul(value)) {
        return {};
    }
    return value;
}

}  // namespace go2cpp::sse
