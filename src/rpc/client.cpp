/**
 * @file client.cpp
 * @brief RPC 客户端实现：读线程 + call_id 分派 + 超时/取消/流。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/client.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/frame/frame_codec.hpp"

namespace go2cpp::rpc {
namespace {

using Clock = std::chrono::steady_clock;

/// @brief 由客户端选项构造帧格式。
frame::FrameSpec MakeFrameSpecFromOptions(const ClientOptions& options) {
    int max_payload = static_cast<int>(options.max_payload_bytes);
    if (options.max_payload_bytes > static_cast<std::size_t>(1024) * 1024 * 1024) {
        max_payload = 1024 * 1024 * 1024;
    }
    frame::FrameSpec spec = frame::MakeFrameSpec(frame::MakeMagic("G2RP"), kProtocolVersion,
                                                 max_payload);
    if (!options.checksum) {
        spec.checksum = nullptr;
    }
    return spec;
}

}  // namespace

namespace detail {

/**
 * @brief 一条客户端连接：读线程、写锁、等待表、流表。
 */
class Connection : public std::enable_shared_from_this<Connection> {
   public:
    /// @brief 建立连接对象并启动读线程。
    static std::shared_ptr<Connection> Create(std::unique_ptr<Transport> transport,
                                              const ClientOptions& options, Status* status);

    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// @brief 分配下一个调用 id。
    std::uint64_t NextId() noexcept { return m_next_id.fetch_add(1, std::memory_order_relaxed); }

    /// @brief 登记等待中的调用（必须在发送请求前）。
    Status RegisterPending(const std::shared_ptr<CallState>& state);

    /// @brief 移除等待中的调用。
    void RemovePending(std::uint64_t call_id);

    /// @brief 发送请求帧。
    Status SendRequest(std::uint64_t call_id, std::string_view method,
                       const reflect::Buffer& args, bool streaming);

    /// @brief 发送取消帧。
    void SendCancel(std::uint64_t call_id);

    /// @brief 发送心跳。
    Status SendPing(std::uint64_t call_id);

    /// @brief 等待调用完成。
    Status Wait(const std::shared_ptr<CallState>& state, std::chrono::milliseconds timeout);

    /// @brief 取消调用（本地唤醒 + 发 Cancel 帧）。
    bool CancelCall(std::uint64_t call_id);

    /// @brief 登记流。
    Status RegisterStream(const std::shared_ptr<StreamState>& state);

    /// @brief 移除流。
    void RemoveStream(std::uint64_t call_id);

    /// @brief 取流的下一条消息。
    bool NextStreamFrame(const std::shared_ptr<StreamState>& state, reflect::Buffer* out,
                         Status* status, std::chrono::milliseconds timeout);

    /// @brief 等待流结束。
    Status FinishStream(const std::shared_ptr<StreamState>& state,
                        std::chrono::milliseconds timeout);

    /// @brief 取消流。
    bool CancelStream(std::uint64_t call_id);

    /// @brief 关闭连接（幂等）。
    void Close();

    /// @brief 是否已关闭。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// @brief 底层 fd（诊断）。
    int fd() const noexcept { return m_transport ? m_transport->fd() : -1; }

   private:
    Connection(std::unique_ptr<Transport> transport, const ClientOptions& options);

    void Start();
    void ReaderLoop();
    bool Dispatch(const frame::Frame& frame_value);
    bool HandleResponse(const frame::Frame& frame_value);
    bool HandleStreamData(const frame::Frame& frame_value);
    bool HandleStreamEnd(const frame::Frame& frame_value);
    bool HandlePong(const frame::Frame& frame_value);
    bool SendFrame(FrameType type, std::uint16_t flags, const std::vector<std::uint8_t>& payload);
    void FailAll();
    std::shared_ptr<StreamState> FindStream(std::uint64_t call_id);

    std::unique_ptr<Transport> m_transport;  ///< 传输。
    frame::FrameCodec m_codec;               ///< 帧编解码（读线程独占）。
    std::mutex m_write_mutex;                ///< 串行化写。
    std::atomic<bool> m_closed{false};       ///< 关闭标志。
    std::atomic<std::uint64_t> m_next_id{1}; ///< 下一个调用 id。
    std::thread m_reader;                    ///< 读线程。
    std::vector<std::uint8_t> m_read_buffer; ///< 读缓冲（读线程独占）。
    std::vector<frame::Frame> m_frames;      ///< 本轮帧（读线程独占）。

    std::mutex m_pending_mutex;                                  ///< 等待表锁。
    std::unordered_map<std::uint64_t, std::shared_ptr<CallState>> m_pending;  ///< 等待表。

    std::mutex m_streams_mutex;                                  ///< 流表锁。
    std::unordered_map<std::uint64_t, std::shared_ptr<StreamState>> m_streams;  ///< 流表。
};

std::shared_ptr<Connection> Connection::Create(std::unique_ptr<Transport> transport,
                                               const ClientOptions& options, Status* status) {
    auto connection = std::shared_ptr<Connection>(new Connection(std::move(transport), options));
    connection->Start();
    if (status != nullptr) {
        *status = Status::Ok();
    }
    return connection;
}

Connection::Connection(std::unique_ptr<Transport> transport, const ClientOptions& options)
    : m_transport(std::move(transport)), m_codec(MakeFrameSpecFromOptions(options)) {
    m_read_buffer.resize(64 * 1024);
}

Connection::~Connection() {
    Close();
    if (m_reader.joinable()) {
        if (m_reader.get_id() == std::this_thread::get_id()) {
            m_reader.detach();  // 最后一个 shared_ptr 在读线程释放：不能 join 自己。
        } else {
            m_reader.join();
        }
    }
}

void Connection::Start() {
    auto self = shared_from_this();
    m_reader = std::thread([self]() { self->ReaderLoop(); });
}

void Connection::Close() {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_transport->Close();
    FailAll();
}

void Connection::FailAll() {
    std::unordered_map<std::uint64_t, std::shared_ptr<CallState>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        pending.swap(m_pending);
    }
    for (auto& entry : pending) {
        const std::shared_ptr<CallState>& state = entry.second;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->done) {
                state->done = true;
                state->status = Status(StatusCode::kUnavailable, "connection closed");
            }
        }
        state->completed.store(true, std::memory_order_release);
        state->cv.notify_all();
    }
    std::unordered_map<std::uint64_t, std::shared_ptr<StreamState>> streams;
    {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        streams.swap(m_streams);
    }
    for (auto& entry : streams) {
        const std::shared_ptr<StreamState>& state = entry.second;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ended) {
                state->ended = true;
                state->end_status = Status(StatusCode::kUnavailable, "connection closed");
            }
        }
        state->cv.notify_all();
    }
}

Status Connection::RegisterPending(const std::shared_ptr<CallState>& state) {
    if (m_closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::kUnavailable, "connection closed");
    }
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    m_pending[state->id] = state;
    return Status::Ok();
}

void Connection::RemovePending(std::uint64_t call_id) {
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    m_pending.erase(call_id);
}

Status Connection::SendRequest(std::uint64_t call_id, std::string_view method,
                               const reflect::Buffer& args, bool streaming) {
    thread_local std::vector<std::uint8_t> payload;
    // P0：把调用线程当前的 TraceContext 注入请求帧；未设置时保持旧格式。
    const distributed::TraceContext& trace = distributed::CurrentTraceContext();
    const std::uint16_t trace_flag = trace.IsValid() ? kFlagTrace : 0;
    if (!EncodeRequest(payload, call_id, method, args, trace)) {
        return Status(StatusCode::kInvalidArgument, "invalid method name or arguments");
    }
    return SendFrame(FrameType::kRequest, (streaming ? kFlagStreaming : 0) | trace_flag,
                     payload)
               ? Status::Ok()
               : Status(StatusCode::kUnavailable, "failed to send request");
}

void Connection::SendCancel(std::uint64_t call_id) {
    thread_local std::vector<std::uint8_t> payload;
    (void)EncodeCancel(payload, call_id);
    (void)SendFrame(FrameType::kCancel, 0, payload);
}

Status Connection::SendPing(std::uint64_t call_id) {
    thread_local std::vector<std::uint8_t> payload;
    (void)EncodePing(payload, call_id, false);
    return SendFrame(FrameType::kPing, 0, payload)
               ? Status::Ok()
               : Status(StatusCode::kUnavailable, "failed to send ping");
}

bool Connection::SendFrame(FrameType type, std::uint16_t flags,
                           const std::vector<std::uint8_t>& payload) {
    if (m_closed.load(std::memory_order_acquire)) {
        return false;
    }
    frame::Frame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.flags = flags;
    frame.payload = payload;
    thread_local std::vector<std::uint8_t> wire;
    frame::Error error;
    if (!m_codec.Encode(frame, &wire, &error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_write_mutex);
    return m_transport->WriteAll(wire.data(), wire.size(), nullptr);
}

Status Connection::Wait(const std::shared_ptr<CallState>& state,
                        std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    // 先自旋一小段（100 µs）：本机 loopback 的 RTT 通常落在几十微秒，
    // 自旋能避开 futex 唤醒延迟；超过窗口再回退到条件变量。
    const auto spin_until = Clock::now() + std::chrono::microseconds(100);
    while (!state->completed.load(std::memory_order_acquire) &&
           Clock::now() < spin_until) {
        std::this_thread::yield();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->done) {
        if (timeout.count() < 0) {
            state->cv.wait(lock, [&state]() { return state->done; });
        } else {
            const auto remaining = deadline - Clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero() ||
                !state->cv.wait_for(lock, remaining, [&state]() { return state->done; })) {
                lock.unlock();
                RemovePending(state->id);
                SendCancel(state->id);
                return Status(StatusCode::kDeadlineExceeded, "rpc call timed out");
            }
        }
    }
    return state->status;
}

bool Connection::CancelCall(std::uint64_t call_id) {
    std::shared_ptr<CallState> state;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        const auto found = m_pending.find(call_id);
        if (found == m_pending.end()) {
            return false;
        }
        state = found->second;
        m_pending.erase(found);
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->done) {
            return false;
        }
        state->done = true;
        state->status = Status(StatusCode::kCancelled, "call cancelled by client");
    }
    state->completed.store(true, std::memory_order_release);
    state->cv.notify_all();
    SendCancel(call_id);
    return true;
}

Status Connection::RegisterStream(const std::shared_ptr<StreamState>& state) {
    if (m_closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::kUnavailable, "connection closed");
    }
    std::lock_guard<std::mutex> lock(m_streams_mutex);
    m_streams[state->id] = state;
    return Status::Ok();
}

void Connection::RemoveStream(std::uint64_t call_id) {
    std::lock_guard<std::mutex> lock(m_streams_mutex);
    m_streams.erase(call_id);
}

std::shared_ptr<StreamState> Connection::FindStream(std::uint64_t call_id) {
    std::lock_guard<std::mutex> lock(m_streams_mutex);
    const auto found = m_streams.find(call_id);
    if (found == m_streams.end()) {
        return nullptr;
    }
    return found->second;
}

bool Connection::NextStreamFrame(const std::shared_ptr<StreamState>& state, reflect::Buffer* out,
                                 Status* status, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state->mutex);
    bool ready = false;
    if (timeout.count() < 0) {
        state->cv.wait(lock, [&state]() { return state->ended || !state->frames.empty(); });
        ready = true;
    } else {
        ready = state->cv.wait_for(lock, timeout, [&state]() {
            return state->ended || !state->frames.empty();
        });
    }
    if (!ready) {
        if (status != nullptr) {
            *status = Status(StatusCode::kDeadlineExceeded, "stream receive timed out");
        }
        return false;
    }
    if (!state->frames.empty()) {
        std::vector<std::uint8_t> bytes = std::move(state->frames.front());
        state->frames.pop_front();
        state->buffered_bytes -= bytes.size();
        lock.unlock();
        out->Clear();
        if (!out->PutBytes(bytes.data(), bytes.size())) {
            if (status != nullptr) {
                *status = Status(StatusCode::kResourceExhausted, "stream message too large");
            }
            return false;
        }
        if (status != nullptr) {
            *status = Status::Ok();
        }
        return true;
    }
    if (status != nullptr) {
        *status = state->end_status;
    }
    return false;
}

Status Connection::FinishStream(const std::shared_ptr<StreamState>& state,
                                std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state->mutex);
    bool ended = false;
    if (timeout.count() < 0) {
        state->cv.wait(lock, [&state]() { return state->ended; });
        ended = true;
    } else {
        ended = state->cv.wait_for(lock, timeout, [&state]() { return state->ended; });
    }
    if (!ended) {
        return Status(StatusCode::kDeadlineExceeded, "stream finish timed out");
    }
    return state->end_status;
}

bool Connection::CancelStream(std::uint64_t call_id) {
    std::shared_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        const auto found = m_streams.find(call_id);
        if (found == m_streams.end()) {
            return false;
        }
        state = found->second;
        m_streams.erase(found);
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->ended) {
            return false;
        }
        state->ended = true;
        state->end_status = Status(StatusCode::kCancelled, "stream cancelled by client");
    }
    state->cv.notify_all();
    SendCancel(call_id);
    return true;
}

/**
 * @brief 客户端读线程主循环：读字节流、切帧、逐帧分派，退出时关闭连接。
 *
 * @return 无返回值；EOF、协议错误或 m_closed 都会结束循环。
 *
 * ## 定位
 * 客户端唯一的帧来源。切帧失败若继续读，后续帧边界会全部错位；退出时不 Close 会让等待者永不苏醒。
 *
 * ## 步骤
 * step1. 循环直到关闭标志；
 * step2. 从 transport 读一块字节，<=0 视为 EOF/错误退出；
 * step3. 交给 codec 切帧，畸形流直接断开；
 * step4. 逐帧 Dispatch，任一失败立即断开；
 * step5. 退出时 Close，唤醒所有等待中的调用。
 *
 * ## 并发/失败语义
 * - 调用约束：每连接一个读线程，由 Start 启动。
 * - 原子性：codec 内部保留跨读的半帧状态，只能单线程 Feed。
 * - 失败语义：不抛异常；Close 幂等。
 * - 不变量：循环退出后连接一定处于关闭状态，所有 pending 调用被唤醒。
 */
void Connection::ReaderLoop() {
    while (!m_closed.load(std::memory_order_acquire)) {
        Status status;
        const std::ptrdiff_t received =
            m_transport->Read(m_read_buffer.data(), m_read_buffer.size(), &status);
        if (received <= 0) {
            break;
        }
        m_frames.clear();
        frame::Error error;
        std::size_t consumed = 0;
        if (!m_codec.Feed(m_read_buffer.data(), static_cast<std::size_t>(received), &m_frames,
                          &consumed, &error)) {
            break;
        }
        bool healthy = true;
        for (const frame::Frame& frame_value : m_frames) {
            if (!Dispatch(frame_value)) {
                healthy = false;
                break;
            }
        }
        if (!healthy) {
            break;
        }
    }
    Close();
}

/**
 * @brief 按帧类型把服务端下行帧分派到对应处理器。
 *
 * @param frame_value [in] 已解析的完整帧；引用只在本次调用内有效。
 *
 * @return
 * - true：帧已处理（含可忽略的未知类型）——调用方**必须**继续读下一帧；
 * - false：收到非法方向帧或处理器失败——调用方**必须**断开，字节流已不可信。
 *
 * ## 定位
 * 客户端唯一的帧分派点。把 Request/Cancel 当成可忽略会让协议方向错误被静默掩盖；
 * 把未知类型当致命会让协议无法向前兼容。
 *
 * ## 步骤
 * step1. 按 type 分派 Response/StreamData/StreamEnd/Pong；
 * step2. 服务端方向的 Request/Cancel/Ping 一律返回 false；
 * step3. 未知类型返回 true（向前兼容，忽略）。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在 ReaderLoop 线程调用。
 * - 原子性：单帧处理；各处理器内部自行加锁。
 * - 失败语义：返回 false 表示连接级错误，不抛异常。
 * - 不变量：返回 true 后该帧的协议状态已完全消费。
 */
bool Connection::Dispatch(const frame::Frame& frame_value) {
    switch (static_cast<FrameType>(frame_value.type)) {
        case FrameType::kResponse:
            return HandleResponse(frame_value);
        case FrameType::kStreamData:
            return HandleStreamData(frame_value);
        case FrameType::kStreamEnd:
            return HandleStreamEnd(frame_value);
        case FrameType::kPong:
            return HandlePong(frame_value);
        case FrameType::kRequest:
        case FrameType::kCancel:
        case FrameType::kPing:
            return false;  // 服务端方向不该出现这些帧。
    }
    return true;  // 未知类型：向前兼容，忽略。
}

/**
 * @brief 解码 Response，按 call_id 唤醒等待中的调用；迟到响应直接丢弃。
 *
 * @param frame_value [in] Response 帧；payload 已由 codec 校验。
 *
 * @return true 表示已处理（含丢弃迟到响应）——调用方**必须**继续读；
 *         false 表示协议错误——调用方**必须**断开。
 *
 * ## 定位
 * 客户端等待链路的唯一唤醒点。漏掉 notify 会让调用方等到超时；把迟到响应当成错误会让取消路径误报。
 * 流式请求被服务端用普通 Response 回复时，必须转成 StreamEnd，否则流永远收不到结束。
 *
 * ## 步骤
 * step1. 解码 call_id/status/payload；
 * step2. 持 pending 锁取出并移除 CallState；
 * step3. 找不到调用时查流表：命中则把 Response 转成 StreamEnd 并唤醒；
 * step4. 都找不到说明已超时/取消，返回 true 丢弃；
 * step5. 命中则填状态、置 done、notify_all。
 *
 * ## 并发/失败语义
 * - 调用约束：ReaderLoop 线程调用；与 Wait/CancelCall 通过 CallState::mutex 同步。
 * - 原子性：从 pending 摘除与写状态分两把锁，但 done 标志保证只唤醒一次。
 * - 失败语义：不抛异常；重复响应被 done 检查吞掉。
 * - 不变量：返回 true 后该 call_id 不会再收到第二次唤醒。
 */
bool Connection::HandleResponse(const frame::Frame& frame_value) {
    ResponseMessage message;
    std::string error;
    if (!DecodeResponse(frame_value, &message, &error)) {
        return false;
    }
    std::shared_ptr<CallState> state;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        const auto found = m_pending.find(message.call_id);
        if (found != m_pending.end()) {
            state = found->second;
            m_pending.erase(found);
        }
    }
    if (!state) {
        // 服务端对「流式请求 + 非流式方法」可能回普通 Response：转成 StreamEnd。
        const std::shared_ptr<StreamState> stream_state = FindStream(message.call_id);
        if (stream_state) {
            {
                std::lock_guard<std::mutex> lock(stream_state->mutex);
                stream_state->ended = true;
                stream_state->end_status = std::move(message.status);
            }
            stream_state->cv.notify_all();
            RemoveStream(message.call_id);
        }
        return true;  // 已超时/取消的调用：迟到响应直接丢弃。
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->done) {
            return true;
        }
        state->status = std::move(message.status);
        state->payload = std::move(message.payload);
        state->done = true;
    }
    state->completed.store(true, std::memory_order_release);
    state->cv.notify_all();
    return true;
}

bool Connection::HandleStreamData(const frame::Frame& frame_value) {
    StreamDataMessage message;
    std::string error;
    if (!DecodeStreamData(frame_value, &message, &error)) {
        return false;
    }
    const std::shared_ptr<StreamState> state = FindStream(message.call_id);
    if (!state) {
        return true;  // 流已取消/结束：丢弃。
    }
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->ended) {
            return true;
        }
        if (state->frames.size() >= state->max_frames ||
            state->buffered_bytes + message.payload.size() > state->max_bytes) {
            state->ended = true;
            state->end_status =
                Status(StatusCode::kResourceExhausted, "stream receive buffer exceeded");
            overflow = true;
        } else {
            state->buffered_bytes += message.payload.size();
            state->frames.push_back(std::move(message.payload));
        }
    }
    state->cv.notify_all();
    if (overflow) {
        RemoveStream(message.call_id);
        SendCancel(message.call_id);
    }
    return true;
}

bool Connection::HandleStreamEnd(const frame::Frame& frame_value) {
    StreamEndMessage message;
    std::string error;
    if (!DecodeStreamEnd(frame_value, &message, &error)) {
        return false;
    }
    const std::shared_ptr<StreamState> state = FindStream(message.call_id);
    if (!state) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ended = true;
        state->end_status = std::move(message.status);
    }
    state->cv.notify_all();
    RemoveStream(message.call_id);
    return true;
}

bool Connection::HandlePong(const frame::Frame& frame_value) {
    std::uint64_t call_id = 0;
    std::string error;
    if (!DecodeCallId(frame_value, &call_id, &error)) {
        return false;
    }
    std::shared_ptr<CallState> state;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        const auto found = m_pending.find(call_id);
        if (found != m_pending.end()) {
            state = found->second;
            m_pending.erase(found);
        }
    }
    if (!state) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->status = Status::Ok();
        state->done = true;
    }
    state->completed.store(true, std::memory_order_release);
    state->cv.notify_all();
    return true;
}

// ---------------------------------------------------------------------------
// 自由函数（模板头文件通过它们访问非模板实现）
// ---------------------------------------------------------------------------

Status WaitForCall(const std::shared_ptr<Connection>& connection,
                   const std::shared_ptr<CallState>& state, std::chrono::milliseconds timeout) {
    if (!connection) {
        return Status(StatusCode::kUnavailable, "connection closed");
    }
    return connection->Wait(state, timeout);
}

bool CancelCall(const std::shared_ptr<Connection>& connection, std::uint64_t call_id) {
    return connection != nullptr && connection->CancelCall(call_id);
}

Status WaitForStreamFrame(const std::shared_ptr<Connection>& connection,
                          const std::shared_ptr<StreamState>& state, reflect::Buffer* out,
                          std::chrono::milliseconds timeout) {
    if (!connection) {
        return Status(StatusCode::kUnavailable, "connection closed");
    }
    Status status;
    (void)connection->NextStreamFrame(state, out, &status, timeout);
    return status;
}

Status WaitForStreamEnd(const std::shared_ptr<Connection>& connection,
                        const std::shared_ptr<StreamState>& state,
                        std::chrono::milliseconds timeout) {
    if (!connection) {
        return Status(StatusCode::kUnavailable, "connection closed");
    }
    return connection->FinishStream(state, timeout);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ClientStream
// ---------------------------------------------------------------------------

bool ClientStream::NextPayload(reflect::Buffer* out, Status* status,
                               std::chrono::milliseconds timeout) {
    if (!m_connection || !m_state || out == nullptr) {
        if (status != nullptr) {
            *status = Status(StatusCode::kFailedPrecondition, "invalid stream");
        }
        return false;
    }
    const auto resolved = timeout.count() < 0 ? m_timeout : timeout;
    return m_connection->NextStreamFrame(m_state, out, status, resolved);
}

Status ClientStream::Finish(std::chrono::milliseconds timeout) {
    if (!m_connection || !m_state) {
        return Status(StatusCode::kFailedPrecondition, "invalid stream");
    }
    const auto resolved = timeout.count() < 0 ? m_timeout : timeout;
    return m_connection->FinishStream(m_state, resolved);
}

bool ClientStream::Cancel() {
    if (!m_connection || !m_state) {
        return false;
    }
    return m_connection->CancelStream(m_state->id);
}

bool ClientStream::done() const {
    if (!m_state) {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->ended;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client(std::string_view address, ClientOptions options) : m_options(std::move(options)) {
    Endpoint endpoint;
    if (!Endpoint::Parse(address, &endpoint)) {
        m_connect_status = Status(StatusCode::kInvalidArgument, "invalid rpc endpoint");
        return;
    }
    Status status;
    auto transport = TcpTransport::Connect(endpoint, m_options.connect_timeout, &status);
    if (!transport) {
        m_connect_status = std::move(status);
        return;
    }
    m_connection = detail::Connection::Create(std::move(transport), m_options, &m_connect_status);
}

Client::Client(std::unique_ptr<Transport> transport, ClientOptions options)
    : m_options(std::move(options)) {
    if (!transport) {
        m_connect_status = Status(StatusCode::kInvalidArgument, "null transport");
        return;
    }
    m_connection = detail::Connection::Create(std::move(transport), m_options, &m_connect_status);
}

Client::~Client() = default;

bool Client::connected() const {
    return m_connection != nullptr && !m_connection->closed();
}

void Client::Close() {
    if (m_connection) {
        m_connection->Close();
    }
}

Status Client::BeginCall(std::string_view method, const reflect::Buffer& args, bool streaming,
                         std::shared_ptr<detail::CallState>* state) {
    if (state == nullptr) {
        return Status(StatusCode::kInvalidArgument, "null call state");
    }
    if (!m_connection) {
        return m_connect_status.ok() ? Status(StatusCode::kUnavailable, "not connected")
                                     : m_connect_status;
    }
    auto call = std::make_shared<detail::CallState>();
    call->id = m_connection->NextId();
    Status status = m_connection->RegisterPending(call);
    if (!status.ok()) {
        return status;
    }
    status = m_connection->SendRequest(call->id, method, args, streaming);
    if (!status.ok()) {
        m_connection->RemovePending(call->id);
        return status;
    }
    *state = std::move(call);
    return Status::Ok();
}

Status Client::WaitCall(const std::shared_ptr<detail::CallState>& state,
                        std::chrono::milliseconds timeout) {
    if (!m_connection) {
        return Status(StatusCode::kUnavailable, "not connected");
    }
    return m_connection->Wait(state, timeout);
}

ClientStream Client::OpenStreamRaw(std::string_view method, const reflect::Buffer& args) {
    if (!m_connection) {
        return ClientStream();
    }
    auto state = std::make_shared<detail::StreamState>();
    state->id = m_connection->NextId();
    state->max_frames = m_options.max_stream_frames;
    state->max_bytes = m_options.max_stream_bytes;
    Status status = m_connection->RegisterStream(state);
    if (!status.ok()) {
        return ClientStream();
    }
    status = m_connection->SendRequest(state->id, method, args, true);
    if (!status.ok()) {
        m_connection->RemoveStream(state->id);
        return ClientStream();
    }
    return ClientStream(m_connection, state, m_options.timeout);
}

Status Client::Ping() {
    if (!m_connection) {
        return m_connect_status.ok() ? Status(StatusCode::kUnavailable, "not connected")
                                     : m_connect_status;
    }
    auto call = std::make_shared<detail::CallState>();
    call->id = m_connection->NextId();
    Status status = m_connection->RegisterPending(call);
    if (!status.ok()) {
        return status;
    }
    status = m_connection->SendPing(call->id);
    if (!status.ok()) {
        m_connection->RemovePending(call->id);
        return status;
    }
    return m_connection->Wait(call, m_options.timeout);
}

}  // namespace go2cpp::rpc
