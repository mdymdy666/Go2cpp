/**
 * @file mux.cpp
 * @brief `MuxSession` / `MuxStream` 的实现。
 *
 * @details 关键不变量：
 *
 * 1. **接收线程永不阻塞在单条流上**：数据投递到 per-stream 有界队列，
 *    队列满就关闭该流（`FailStream`），整条控制连接继续工作。
 * 2. **锁序固定为 stream → session**：`MuxStream` 的方法只在自己的
 *    `m_mutex` 之外调用 session 接口；session 分发时先拷贝 `shared_ptr`
 *    再释放 `m_streams_mutex`，因此不会形成交叉持有。
 * 3. **`Close()` 幂等**：RelayPipe 的看门狗与调用方都可能关闭同一条流。
 * 4. **`Shutdown()` 可重入**：接收线程自己就会调用它，因此它不 join 线程；
 *    join 只在 `Stop()` / 析构里做。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/mux.hpp"

#include "go2cpp/logging/log.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace go2cpp {
namespace tunnel {
namespace {

/// 热路径失败日志的冷段：`noinline` + 集中构造，避免日志流被内联进
/// 每 chunk 调用的 `MuxStream::Write`，把栈帧从 ~150 B 撑到 ~750 B。
/// @param stream_id 流 id；@param offset 本 chunk 在本次 Write 内的偏移；
/// @param chunk 本 chunk 字节数；@param detail 失败阶段；@param error 错误文本。
[[gnu::noinline]] void LogStreamWriteFailure(std::uint32_t stream_id,
                                             std::size_t offset,
                                             std::size_t chunk,
                                             const char* detail,
                                             const std::string& error) {
    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("tunnel.mux"))
        << detail << " stream=" << stream_id << " offset=" << offset
        << " chunk=" << chunk << " err=" << error;
}

/// 帧写失败的冷段日志（连接不可用 / 写超时）。
[[gnu::noinline]] void LogFrameWriteFailure(int type, std::size_t bytes,
                                            const std::string& error) {
    GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("tunnel.mux"))
        << "frame write failed type=" << type << " bytes=" << bytes
        << " err=" << error;
}

/// 把 `stream_id` 拼进 CLOSE 载荷。
std::vector<std::uint8_t> MakeClosePayload(std::uint32_t stream_id,
                                           std::uint8_t code) {
    Close close;
    close.stream_id = stream_id;
    close.code = code;
    std::vector<std::uint8_t> payload;
    (void)EncodeClose(close, &payload, nullptr);
    return payload;
}

/// 从 DATA 帧载荷前 4 字节读出 stream_id（小端）；畸形帧返回 0。
std::uint32_t PayloadStreamId(const frame::Frame& frame) {
    if (frame.payload.size() < 4U) {
        return 0;
    }
    return static_cast<std::uint32_t>(frame.payload[0]) |
           (static_cast<std::uint32_t>(frame.payload[1]) << 8U) |
           (static_cast<std::uint32_t>(frame.payload[2]) << 16U) |
           (static_cast<std::uint32_t>(frame.payload[3]) << 24U);
}

/// 小端写 32 位；批量帧头直接落栈缓冲，不经过 `FrameCodec::Encode`。
void StoreU32Le(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

/// @brief 精确读满 `bytes` 字节（不预读），到期返回 false。
/// @param conn [in] 连接；不可为 nullptr
/// @param out [out] 目标缓冲，至少 `bytes` 字节
/// @param bytes [in] 需要读满的字节数
/// @param deadline [in] 绝对截止时间（steady_clock）
/// @return 读满返回 true；超时 / 对端关闭 / 读错误返回 false
/// @details 只用于 `FrameReader::NextExact`：任何按 8 KiB 缓冲的预读都会
///          吞掉 ACK 之后的裸字节流，因此这里每次只请求剩余字节。
bool ReadExactly(TcpConn* conn, std::uint8_t* out, std::size_t bytes,
                 std::chrono::steady_clock::time_point deadline) {
    std::size_t done = 0;
    while (done < bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int slice =
            static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
        const IoResult result = conn->Read(out + done, bytes - done, slice);
        if (!result.ok()) {
            return false;
        }
        done += result.count;
    }
    return true;
}

}  // namespace

// ============================================================================
// FrameReader / WriteFrame
// ============================================================================

FrameReader::FrameReader(TcpConn::Ptr conn, bool with_checksum)
    : m_conn(std::move(conn)),
      m_codec(MakeTunnelFrameSpec(with_checksum)) {}

bool FrameReader::Next(int timeout_ms, frame::Frame* out, std::string* err) {
    if (out == nullptr) {
        return false;
    }
    if (!m_pending.empty()) {
        *out = std::move(m_pending.front());
        m_pending.pop_front();
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    std::vector<std::uint8_t> buffer(8192);
    std::vector<frame::Frame> frames;
    while (frames.empty()) {
        if (std::chrono::steady_clock::now() > deadline) {
            if (err != nullptr) {
                *err = "frame timeout";
            }
            return false;
        }
        const IoResult result = m_conn->Read(buffer.data(), buffer.size(), 500);
        if (result.eof) {
            if (err != nullptr) {
                *err = "connection closed";
            }
            return false;
        }
        if (result.error == ETIMEDOUT) {
            continue;
        }
        if (!result.ok()) {
            if (err != nullptr) {
                *err = "read fail";
            }
            return false;
        }
        std::size_t consumed = 0;
        frame::Error frame_error;
        if (!m_codec.Feed(buffer.data(), result.count, &frames, &consumed,
                          &frame_error)) {
            if (err != nullptr) {
                *err = frame_error.message;
            }
            return false;
        }
    }
    for (std::size_t index = 1; index < frames.size(); ++index) {
        m_pending.push_back(std::move(frames[index]));
    }
    *out = std::move(frames.front());
    return true;
}

bool FrameReader::NextExact(int timeout_ms, frame::Frame* out,
                            std::string* err) {
    if (out == nullptr) {
        return false;
    }
    if (!m_pending.empty()) {
        *out = std::move(m_pending.front());
        m_pending.pop_front();
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    std::uint8_t header[kFrameHeaderBytes] = {};
    if (!ReadExactly(m_conn.get(), header, sizeof(header), deadline)) {
        if (err != nullptr) {
            *err = "frame header timeout";
        }
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(header[8]) |
                               (static_cast<std::size_t>(header[9]) << 8U) |
                               (static_cast<std::size_t>(header[10]) << 16U) |
                               (static_cast<std::size_t>(header[11]) << 24U);
    if (length > kMaxPayload) {
        if (err != nullptr) {
            *err = "frame payload too large";
        }
        return false;
    }
    const std::size_t trailer = m_codec.Spec().HasChecksum() ? 4U : 0U;
    std::vector<std::uint8_t> wire(kFrameHeaderBytes + length + trailer);
    std::memcpy(wire.data(), header, kFrameHeaderBytes);
    if (!ReadExactly(m_conn.get(), wire.data() + kFrameHeaderBytes,
                     length + trailer, deadline)) {
        if (err != nullptr) {
            *err = "frame payload timeout";
        }
        return false;
    }
    frame::FrameCodec codec(MakeTunnelFrameSpec(m_codec.Spec().HasChecksum()));
    std::vector<frame::Frame> frames;
    std::size_t consumed = 0;
    frame::Error frame_error;
    if (!codec.Feed(wire.data(), wire.size(), &frames, &consumed,
                    &frame_error) ||
        frames.size() != 1U) {
        if (err != nullptr) {
            *err = frame_error.message.empty() ? "malformed frame"
                                               : frame_error.message;
        }
        return false;
    }
    *out = std::move(frames.front());
    return true;
}

bool WriteFrame(const TcpConn::Ptr& conn, bool with_checksum, FrameType type,
                const std::vector<std::uint8_t>& payload) {
    if (!conn) {
        return false;
    }
    frame::FrameCodec codec(MakeTunnelFrameSpec(with_checksum));
    frame::Frame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.flags = 0;
    frame.payload = payload;
    std::vector<std::uint8_t> wire;
    frame::Error frame_error;
    if (!codec.Encode(frame, &wire, &frame_error)) {
        return false;
    }
    return conn->WriteAll(wire.data(), wire.size(), 5000, nullptr);
}

// ============================================================================
// MuxStream
// ============================================================================

MuxStream::MuxStream(std::weak_ptr<MuxSession> session, std::uint32_t id,
                     std::size_t queue_cap)
    : m_session(std::move(session)), m_id(id), m_queue_cap(queue_cap) {}

streams::ReadResult MuxStream::Read(char* data, std::size_t size) {
    if (size == 0) {
        return {0, nullptr};
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] {
        return !m_queue.empty() || m_closed.load(std::memory_order_acquire) ||
               m_eof.load(std::memory_order_acquire);
    });
    // 本地主动关闭（RelayPipe 超时/取消）：立刻 EOF，让读线程退出。
    if (m_closed.load(std::memory_order_acquire)) {
        return {0, streams::IoEof()};
    }
    if (m_queue.empty()) {
        return {0, streams::IoEof()};
    }
    // 批量取：一次锁内把多个队列元素拼进调用方缓冲（最多 size 字节），
    // 减少「每 64 KiB 帧一次加锁 + 一次唤醒」。改前每次 Read 只取一个元素，
    // RelayPipe 的 256 KiB 读缓冲要拆 4 次 Read、4 次条件变量等待。
    std::size_t copied = 0;
    while (copied < size && !m_queue.empty()) {
        std::vector<std::uint8_t>& front = m_queue.front();
        const std::size_t take = std::min(size - copied, front.size());
        std::memcpy(data + copied, front.data(), take);
        copied += take;
        if (take == front.size()) {
            m_queued_bytes -= front.size();
            m_queue.pop_front();
        } else {
            front.erase(front.begin(),
                        front.begin() + static_cast<std::ptrdiff_t>(take));
            m_queued_bytes -= take;
        }
    }
    return {copied, nullptr};
}

streams::WriteResult MuxStream::Write(const char* data, std::size_t size) {
    if (size == 0) {
        return {0, nullptr};
    }
    if (m_closed.load(std::memory_order_acquire) ||
        m_eof.load(std::memory_order_acquire)) {
        return {0, NewError("tunnel: stream closed")};
    }
    const MuxSession::Ptr session = m_session.lock();
    if (!session) {
        return {0, NewError("tunnel: session gone")};
    }
    // 合并写：整段数据切成 DATA 帧后由一次 writev 提交；帧头落栈缓冲，
    // body 直接进 iovec（零拷贝）。改前这里是逐 chunk 的 Encode + send，
    // 每次 RelayPipe 的 32 KiB chunk 各付一次 syscall 与一次发送锁。
    if (!session->SendData(m_id, data, size)) {
        LogStreamWriteFailure(m_id, 0, size, "DATA send failed", std::string());
        return {0, NewError("tunnel: send failed")};
    }
    m_bytes_out.fetch_add(size, std::memory_order_relaxed);
    return {size, nullptr};
}

ErrorPtr MuxStream::Close() {
    {
        // 必须持 `m_mutex` 置位并唤醒：`Read` 在锁内检查谓词后进入等待，
        // 若在锁外 notify，会撞上「谓词已判 false、线程尚未挂上 futex」的
        // 窗口，唤醒直接丢失，读线程永久阻塞——RelayPipe 的 Join 随之卡死
        // （TSan 报 Direction 线程 finished-but-not-joined）。
        std::lock_guard<std::mutex> lock(m_mutex);
        bool expected = false;
        if (!m_closed.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return nullptr;
        }
        m_cv.notify_all();
    }
    // 锁外再做网络动作：`SendPayload` 可能阻塞到写超时，不能占着流锁。
    const MuxSession::Ptr session = m_session.lock();
    if (session) {
        (void)session->SendPayload(FrameType::kClose,
                                   MakeClosePayload(m_id, 0));
        session->RemoveStream(m_id);
    }
    return nullptr;
}

bool MuxStream::PushData(const std::uint8_t* data, std::size_t length,
                         bool notify) {
    if (length == 0) {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_closed.load(std::memory_order_acquire) ||
        m_eof.load(std::memory_order_acquire)) {
        return true;
    }
    if (m_queued_bytes + length > m_queue_cap) {
        return false;
    }
    m_queue.emplace_back(data, data + length);
    m_queued_bytes += length;
    lock.unlock();
    if (!notify) {
        return true;
    }
    m_cv.notify_one();
    m_bytes_in.fetch_add(length, std::memory_order_relaxed);
    return true;
}

void MuxStream::MarkPeerClosed(const char* /*reason*/) {
    // 同 `Close()`：持锁置位 + 唤醒，避免与 `Read` 的谓词检查交错时丢唤醒。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_eof.store(true, std::memory_order_release);
    m_cv.notify_all();
}

// ============================================================================
// MuxSession
// ============================================================================

MuxSession::MuxSession(TcpConn::Ptr conn, MuxConfig config)
    : m_conn(std::move(conn)), m_config(config) {
    m_encoder.reset(
        new frame::FrameCodec(MakeTunnelFrameSpec(m_config.with_checksum)));
}

MuxSession::~MuxSession() { Stop(); }

MuxSession::Ptr MuxSession::Create(TcpConn::Ptr conn, MuxConfig config) {
    if (!conn) {
        return nullptr;
    }
    return Ptr(new MuxSession(std::move(conn), config));
}

bool MuxSession::Start(OnOpen on_open, ErrorPtr* err) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        if (err != nullptr) {
            *err = NewError("tunnel: mux already started");
        }
        return false;
    }
    if (!m_conn) {
        if (err != nullptr) {
            *err = NewError("tunnel: mux null connection");
        }
        m_alive.store(false, std::memory_order_release);
        return false;
    }
    m_on_open = std::move(on_open);
    m_last_rx_ms.store(NowMs(), std::memory_order_relaxed);
    try {
        m_receive_thread = std::thread([this] { ReceiveLoop(); });
        m_heartbeat_thread = std::thread([this] { HeartbeatLoop(); });
    } catch (const std::exception& error) {
        if (err != nullptr) {
            *err = NewError(std::string("tunnel: start thread: ") + error.what());
        }
        Shutdown("start thread failed");
        Stop();
        return false;
    }
    return true;
}

bool MuxSession::WaitHelloAck(int timeout_ms, HelloAck* ack) {
    std::unique_lock<std::mutex> lock(m_ack_mutex);
    const bool ready = m_ack_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [this] {
            return m_ack_received || !alive();
        });
    if (!ready || !m_ack_received) {
        return false;
    }
    if (ack != nullptr) {
        *ack = m_hello_ack;
    }
    return m_hello_ack.status == Status::kOk;
}

MuxStream::Ptr MuxSession::OpenStream(const std::string& peer_ip,
                                      std::uint16_t peer_port) {
    if (!alive() || draining()) {
        return nullptr;  // GOAWAY 之后不再开新流（drain 语义）
    }
    std::uint32_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    }
    MuxStream::Ptr stream(new MuxStream(weak_from_this(), id,
                                        m_config.recv_queue_cap));
    {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        m_streams.emplace(id, stream);
    }
    m_streams_total.fetch_add(1, std::memory_order_relaxed);

    Open open;
    open.stream_id = id;
    open.peer_ip = peer_ip;
    open.peer_port = peer_port;
    std::vector<std::uint8_t> payload;
    ErrorPtr encode_error;
    if (!EncodeOpen(open, &payload, &encode_error) ||
        !SendPayload(FrameType::kOpen, payload)) {
        FailStream(id, "open send failed");
        return nullptr;
    }
    return stream;
}

std::uint32_t MuxSession::AllocateStreamId() {
    if (!alive() || draining()) {
        return 0;
    }
    std::uint32_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

bool MuxSession::SendOpenRaw(std::uint32_t stream_id,
                             const std::string& peer_ip,
                             std::uint16_t peer_port) {
    if (stream_id == 0 || !alive() || draining()) {
        return false;
    }
    Open open;
    open.stream_id = stream_id;
    open.peer_ip = peer_ip;
    open.peer_port = peer_port;
    std::vector<std::uint8_t> payload;
    ErrorPtr encode_error;
    if (!EncodeOpen(open, &payload, &encode_error)) {
        return false;
    }
    return SendPayload(FrameType::kOpenRaw, payload);
}

TcpConn::Ptr MuxSession::DetachConn() {
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    TcpConn::Ptr conn = m_conn;
    m_conn.reset();
    return conn;
}

bool MuxSession::SendPayload(FrameType type,
                             const std::vector<std::uint8_t>& payload) {
    if (!alive()) {
        return false;
    }
    frame::Frame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.flags = 0;
    frame.payload = payload;
    std::vector<std::uint8_t> wire;
    frame::Error frame_error;
    std::lock_guard<std::mutex> lock(m_send_mutex);
    // 取到发送锁后再次确认会话存活：等锁期间 `Stop()` 可能已经完成，这里
    // 立即失败返回，不进入最长 30 s 的 `WriteAll`（writer 因而可被 join）。
    if (!alive()) {
        return false;
    }
    // `m_conn` 由 `m_conn_mutex` 保护（`DetachConn` 会并发清空它）：持锁取一份
    // 快照，既消除数据竞争，也保证 WriteAll 期间连接不会被摘走。
    TcpConn::Ptr conn;
    {
        std::lock_guard<std::mutex> conn_lock(m_conn_mutex);
        conn = m_conn;
    }
    if (!conn) {
        return false;
    }
    if (!m_encoder || !m_encoder->Encode(frame, &wire, &frame_error)) {
        return false;
    }
    std::string write_error;
    if (!conn->WriteAll(wire.data(), wire.size(), 30000, &write_error)) {
        LogFrameWriteFailure(static_cast<int>(type), wire.size(), write_error);
        return false;
    }
    m_bytes_out.fetch_add(wire.size(), std::memory_order_relaxed);
    return true;
}

bool MuxSession::SendData(std::uint32_t stream_id, const char* data,
                           std::size_t size) {
    if (size == 0) {
        return true;
    }
    if (data == nullptr || !alive()) {
        return false;
    }
    // 启用帧尾 CRC 时无法用 iovec 直写（校验和覆盖帧头 + 载荷），
    // 回退到逐帧 Encode + SendPayload；默认配置走批量路径。
    if (m_config.with_checksum) {
        std::vector<std::uint8_t> payload;
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t chunk = std::min(size - offset, kMaxDataChunk);
            ErrorPtr encode_error;
            if (!EncodeData(stream_id,
                            reinterpret_cast<const std::uint8_t*>(data) + offset,
                            chunk, &payload, &encode_error) ||
                !SendPayload(FrameType::kData, payload)) {
                LogStreamWriteFailure(stream_id, offset, chunk,
                                      "DATA encode/send failed", std::string());
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    // 帧头（12 B）+ stream_id（4 B）拼在一条栈缓冲里，body 直接引用入参，
    // 因此整个批量发送只有一次 writev、零中间拷贝。
    std::uint8_t headers[kMaxSendBatchFrames][kFrameHeaderBytes + 4U];
    struct iovec vectors[kMaxSendBatchFrames * 2U];
    std::size_t offset = 0;
    std::lock_guard<std::mutex> lock(m_send_mutex);
    while (offset < size) {
        // 每批重新取连接快照：`DetachConn` 摘走连接后必须立刻停止写入。
        TcpConn::Ptr conn;
        {
            std::lock_guard<std::mutex> conn_lock(m_conn_mutex);
            conn = m_conn;
        }
        if (!alive() || !conn) {
            return false;
        }
        std::size_t frames = 0;
        std::size_t cursor = offset;
        std::size_t batch_bytes = 0;
        while (frames < kMaxSendBatchFrames && cursor < size) {
            const std::size_t chunk = std::min(size - cursor, kMaxDataChunk);
            std::uint8_t* const header = headers[frames];
            std::memcpy(header, kFrameMagic, 4U);
            header[4] = static_cast<std::uint8_t>(kProtocolVersion);
            header[5] = static_cast<std::uint8_t>(FrameType::kData);
            header[6] = 0;
            header[7] = 0;
            const std::uint32_t body_length =
                static_cast<std::uint32_t>(chunk) + 4U;
            StoreU32Le(header + 8U, body_length);
            StoreU32Le(header + kFrameHeaderBytes, stream_id);
            vectors[frames * 2U].iov_base = header;
            vectors[frames * 2U].iov_len = kFrameHeaderBytes + 4U;
            vectors[frames * 2U + 1U].iov_base =
                const_cast<std::uint8_t*>(bytes + cursor);
            vectors[frames * 2U + 1U].iov_len = chunk;
            cursor += chunk;
            batch_bytes += kFrameHeaderBytes + 4U + chunk;
            ++frames;
        }
        std::string write_error;
        if (!conn->WriteV(vectors, static_cast<int>(frames * 2U), 30000,
                            &write_error)) {
            LogFrameWriteFailure(static_cast<int>(FrameType::kData), batch_bytes,
                                 write_error);
            return false;
        }
        m_bytes_out.fetch_add(batch_bytes, std::memory_order_relaxed);
        offset = cursor;
    }
    return true;
}

bool MuxSession::SendGoAway(std::uint8_t reason,
                            std::uint32_t retry_after_ms) {
    if (!alive()) {
        return false;
    }
    m_draining.store(true, std::memory_order_release);
    GoAway goaway;
    goaway.reason = reason;
    goaway.retry_after_ms = retry_after_ms;
    std::vector<std::uint8_t> payload;
    if (!EncodeGoAway(goaway, &payload, nullptr)) {
        return false;
    }
    return SendPayload(FrameType::kGoAway, payload);
}

void MuxSession::Shutdown(const char* reason) {
    if (!m_alive.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)reason;
    TcpConn::Ptr conn;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        conn = m_conn;
    }
    if (conn) {
        conn->Shutdown();
    }
    MarkAllStreamsClosed(reason);
    m_ack_cv.notify_all();
    m_closed_cv.notify_all();
}

void MuxSession::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        if (m_stopped) {
            return;
        }
        m_stopped = true;
    }
    Shutdown("session stopped");
    const auto self = std::this_thread::get_id();
    if (m_receive_thread.joinable()) {
        if (m_receive_thread.get_id() == self) {
            m_receive_thread.detach();
        } else {
            m_receive_thread.join();
        }
    }
    if (m_heartbeat_thread.joinable()) {
        if (m_heartbeat_thread.get_id() == self) {
            m_heartbeat_thread.detach();
        } else {
            m_heartbeat_thread.join();
        }
    }
}

std::size_t MuxSession::streams_active() const {
    std::lock_guard<std::mutex> lock(m_streams_mutex);
    return m_streams.size();
}

HelloAck MuxSession::hello_ack() const {
    std::lock_guard<std::mutex> lock(m_ack_mutex);
    return m_hello_ack;
}

void MuxSession::RemoveStream(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(m_streams_mutex);
    m_streams.erase(id);
}

void MuxSession::FailStream(std::uint32_t id, const char* reason) {
    MuxStream::Ptr stream;
    {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        const auto found = m_streams.find(id);
        if (found == m_streams.end()) {
            return;
        }
        stream = found->second;
        m_streams.erase(found);
    }
    stream->MarkPeerClosed(reason);
    (void)SendPayload(FrameType::kClose, MakeClosePayload(id, 1));
}

void MuxSession::MarkAllStreamsClosed(const char* reason) {
    std::vector<MuxStream::Ptr> streams;
    {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        streams.reserve(m_streams.size());
        for (const auto& entry : m_streams) {
            streams.push_back(entry.second);
        }
        m_streams.clear();
    }
    for (const MuxStream::Ptr& stream : streams) {
        stream->MarkPeerClosed(reason);
    }
}

void MuxSession::Dispatch(const frame::Frame& frame, bool notify_data) {
    switch (static_cast<FrameType>(frame.type)) {
        case FrameType::kHelloAck: {
            HelloAck ack;
            if (!DecodeHelloAck(frame.payload.data(), frame.payload.size(), &ack,
                                nullptr)) {
                Shutdown("malformed HELLO_ACK");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_ack_mutex);
                m_hello_ack = ack;
                m_ack_received = true;
            }
            m_ack_cv.notify_all();
            if (ack.status != Status::kOk) {
                Shutdown("HELLO rejected");
            }
            return;
        }
        case FrameType::kOpen: {
            Open open;
            if (!DecodeOpen(frame.payload.data(), frame.payload.size(), &open,
                            nullptr)) {
                Shutdown("malformed OPEN");
                return;
            }
            if (!alive()) {
                return;
            }
            MuxStream::Ptr stream(new MuxStream(weak_from_this(), open.stream_id,
                                                m_config.recv_queue_cap));
            {
                std::lock_guard<std::mutex> lock(m_streams_mutex);
                m_streams.emplace(open.stream_id, stream);
            }
            m_streams_total.fetch_add(1, std::memory_order_relaxed);
            if (m_on_open) {
                m_on_open(stream);
            } else {
                FailStream(open.stream_id, "no open handler");
            }
            return;
        }
        case FrameType::kOpenRaw: {
            Open open;
            if (!DecodeOpen(frame.payload.data(), frame.payload.size(),
                            &open, nullptr) ||
                open.stream_id == 0) {
                Shutdown("malformed OPEN_RAW");
                return;
            }
            if (!alive()) {
                return;
            }
            if (!m_on_raw_open) {
                Shutdown("no raw open handler");
                return;
            }
            m_on_raw_open(open.stream_id, open.peer_ip);
            return;
        }
        case FrameType::kRawBind:
            // 只有服务端会解析 RAW_BIND（数据连接握手阶段同步读）；
            // 会话层收到它说明对端错发，静默丢弃，不断连接。
            return;
        case FrameType::kData: {
            DataFrame data;
            if (!DecodeData(frame.payload.data(), frame.payload.size(), &data,
                            nullptr)) {
                Shutdown("malformed DATA");
                return;
            }
            MuxStream::Ptr stream;
            {
                std::lock_guard<std::mutex> lock(m_streams_mutex);
                const auto found = m_streams.find(data.stream_id);
                if (found != m_streams.end()) {
                    stream = found->second;
                }
            }
            if (!stream) {
                // 流已被本端关闭：晚到的数据直接丢弃，不必断开连接。
                return;
            }
            if (!stream->PushData(data.body, data.length, notify_data)) {
                GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("tunnel.mux"))
                    << "recv queue overflow stream=" << data.stream_id
                    << " bytes=" << data.length;
                FailStream(data.stream_id, "recv queue overflow");
            }
            return;
        }
        case FrameType::kClose: {
            Close close;
            if (!DecodeClose(frame.payload.data(), frame.payload.size(), &close,
                             nullptr)) {
                Shutdown("malformed CLOSE");
                return;
            }
            MuxStream::Ptr stream;
            {
                std::lock_guard<std::mutex> lock(m_streams_mutex);
                const auto found = m_streams.find(close.stream_id);
                if (found != m_streams.end()) {
                    stream = found->second;
                    m_streams.erase(found);
                }
            }
            if (stream) {
                stream->MarkPeerClosed("peer close");
            }
            return;
        }
        case FrameType::kPing: {
            // 原样回 PONG：nonce 不解释，只用于对端配对。
            (void)SendPayload(FrameType::kPong, frame.payload);
            return;
        }
        case FrameType::kPong:
            return;
        case FrameType::kGoAway: {
            GoAway goaway;
            if (!DecodeGoAway(frame.payload.data(), frame.payload.size(),
                              &goaway, nullptr)) {
                Shutdown("malformed GOAWAY");
                return;
            }
            // 对端要走了：停止开新流，但让在途流继续跑完（drain）。
            m_draining.store(true, std::memory_order_release);
            return;
        }
        case FrameType::kHello:
        case FrameType::kChallenge:
        case FrameType::kAuth:
            Shutdown("unexpected handshake frame on established session");
            return;
    }
    Shutdown("unknown frame type");
}

void MuxSession::ReceiveLoop() {
    frame::FrameCodec codec(MakeTunnelFrameSpec(m_config.with_checksum));
    // 一次 recv 尽量多拿整帧：512 KiB = 2 x 256 KiB 帧，减少 recv 次数与
    // 「半帧留在 FrameCodec 里」的续接开销。
    std::vector<std::uint8_t> buffer(512U * 1024U);
    std::vector<frame::Frame> frames;
    while (alive()) {
        const IoResult result = m_conn->Read(
            buffer.data(), buffer.size(), std::max(1, m_config.read_slice_ms));
        if (result.eof) {
            break;
        }
        if (result.error == ETIMEDOUT) {
            continue;  // 空闲由心跳线程判定，这里只是分片等待
        }
        if (!result.ok()) {
            GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("tunnel.mux"))
                << "control read failed errno=" << result.error;
            break;
        }
        m_last_rx_ms.store(NowMs(), std::memory_order_relaxed);
        m_bytes_in.fetch_add(result.count, std::memory_order_relaxed);
        std::size_t consumed = 0;
        frame::Error frame_error;
        frames.clear();
        if (!codec.Feed(buffer.data(), result.count, &frames, &consumed,
                        &frame_error)) {
            GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("tunnel.mux"))
                << "frame decode failed bytes=" << result.count
                << " err=" << frame_error.message;
            break;  // 帧边界不可信，整条连接作废
        }
        // 同一条流的连续 DATA 帧只对最后一帧唤醒读线程：一次 recv 常含
        // 多帧（512 KiB 缓冲），改前每帧一次 futex 唤醒。
        for (std::size_t index = 0; index < frames.size(); ++index) {
            bool notify = true;
            if (static_cast<FrameType>(frames[index].type) == FrameType::kData &&
                index + 1 < frames.size() &&
                static_cast<FrameType>(frames[index + 1].type) ==
                    FrameType::kData) {
                notify = PayloadStreamId(frames[index]) !=
                         PayloadStreamId(frames[index + 1]);
            }
            Dispatch(frames[index], notify);
            if (!alive()) {
                break;
            }
        }
    }
    Shutdown("receive loop ended");
}

void MuxSession::HeartbeatLoop() {
    const int interval = std::max(1000, m_config.ping_interval_ms);
    while (alive()) {
        {
            std::unique_lock<std::mutex> lock(m_closed_mutex);
            m_closed_cv.wait_for(lock, std::chrono::milliseconds(interval),
                                 [this] { return !alive(); });
        }
        if (!alive()) {
            break;
        }
        const std::int64_t now = NowMs();
        const std::int64_t last =
            m_last_rx_ms.load(std::memory_order_relaxed);
        if (last != 0 && m_config.idle_timeout_ms > 0 &&
            now - last > m_config.idle_timeout_ms) {
            Shutdown("heartbeat timeout");
            break;
        }
        const std::uint64_t nonce = m_ping_nonce.fetch_add(1);
        (void)SendPayload(FrameType::kPing, EncodeNonce(nonce));
    }
}

std::int64_t MuxSession::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace tunnel
}  // namespace go2cpp
