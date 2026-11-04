/**
 * @file server.cpp
 * @brief RPC 服务端实现：accept 线程 + 每连接读线程与工作线程池 + 流式发送器。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/server.hpp"

#include "go2cpp/logging/log.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
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

using frame::Frame;
using frame::FrameCodec;
using frame::FrameSpec;

/// @brief 由服务端选项构造帧格式。
FrameSpec MakeFrameSpecFromOptions(const ServerOptions& options) {
    int max_payload = static_cast<int>(options.max_payload_bytes);
    if (options.max_payload_bytes > static_cast<std::size_t>(1024) * 1024 * 1024) {
        max_payload = 1024 * 1024 * 1024;
    }
    FrameSpec spec = frame::MakeFrameSpec(frame::MakeMagic("G2RP"), kProtocolVersion, max_payload);
    if (!options.checksum) {
        spec.checksum = nullptr;
    }
    return spec;
}

}  // namespace

namespace detail {

/// @brief 一次待处理的请求（读线程 → 工作线程）。
struct PendingRequest {
    std::uint64_t call_id = 0;                    ///< 调用 id。
    std::string method;                           ///< 完整方法名。
    reflect::Buffer args;                         ///< 参数区。
    bool streaming = false;                       ///< 是否流式。
    std::shared_ptr<std::atomic<bool>> cancelled; ///< 取消标志（读线程置位）。
};

/**
 * @brief 流式发送器接口（`ServerStream` 的类型擦除后端）。
 */
class StreamSender {
   public:
    virtual ~StreamSender() = default;

    /// @brief 发送一条消息字节。
    virtual bool SendSerialized(const void* data, std::size_t size) = 0;

    /// @brief 结束流（幂等）。
    virtual Status Finish(Status status) = 0;

    /// @brief 对端是否取消。
    virtual bool cancelled() const = 0;

    /// @brief 流 id。
    virtual std::uint64_t id() const = 0;
};

class ServerImpl;

/**
 * @brief 一条服务端连接：读线程 + 工作线程池 + 写锁。
 *
 * 读线程只负责解码帧、登记取消位、把请求放进有界队列；handler 全部在
 * `m_worker_count` 个工作线程上执行。这样 `Cancel`/`Ping` 帧在慢 handler
 * 执行期间仍能被及时处理，取消对非流式调用同样生效。
 */
class ServerConnection : public std::enable_shared_from_this<ServerConnection> {
   public:
    ServerConnection(ServerImpl* server, std::unique_ptr<Transport> transport, FrameSpec spec,
                     std::size_t worker_threads, std::size_t max_pending);
    ~ServerConnection();

    /// @brief 启动读/工作线程（必须由 shared_ptr 调用）。
    void Start();

    /// @brief 关闭连接并唤醒线程（幂等）。
    void Close();

    /// @brief 等待两个线程退出。
    void Join();

    /// @brief 两个线程是否都已退出（可回收）。
    bool Finished() const noexcept;

    /// @brief 发送响应帧。
    bool SendResponse(std::uint64_t call_id, const Status& status,
                      const reflect::Buffer& result);

    /// @brief 发送流数据帧。
    bool SendStreamData(std::uint64_t call_id, std::uint64_t sequence, const void* data,
                        std::size_t size);

    /// @brief 发送流结束帧。
    bool SendStreamEnd(std::uint64_t call_id, const Status& status);

   private:
    void ReaderLoop();
    void WorkerLoop();
    bool HandleFrame(const Frame& frame);
    bool HandleRequest(const Frame& frame);
    bool HandleCancel(const Frame& frame);
    bool HandlePing(const Frame& frame);
    void Process(const std::shared_ptr<PendingRequest>& request);
    bool SendPayload(FrameType type, std::uint16_t flags, const std::vector<std::uint8_t>& payload);
    void EraseCancel(std::uint64_t call_id);

    ServerImpl* m_server;                    ///< 所属服务端。
    std::unique_ptr<Transport> m_transport;  ///< 传输。
    FrameCodec m_codec;                      ///< 帧编解码（Encode/Feed 无共享状态，多线程可用）。
    std::mutex m_write_mutex;                ///< 串行化写（读线程与多个 worker 共用）。
    std::atomic<bool> m_closed{false};       ///< 关闭标志。
    std::atomic<bool> m_reader_done{false};  ///< 读线程已退出。
    std::atomic<std::size_t> m_workers_done{0};  ///< 已退出的工作线程数。
    std::thread m_reader;                    ///< 读线程。
    std::vector<std::thread> m_workers;      ///< 工作线程池。
    std::size_t m_worker_count = 1;          ///< 工作线程数（构造时固定）。
    std::size_t m_max_pending = 1;           ///< 待执行队列上限。

    std::mutex m_queue_mutex;                         ///< 请求队列锁。
    std::condition_variable m_queue_cv;               ///< 请求到达/关闭通知。
    std::deque<std::shared_ptr<PendingRequest>> m_queue;  ///< 待执行请求。

    std::mutex m_cancels_mutex;  ///< 取消表锁。
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> m_cancels;

    std::vector<std::uint8_t> m_read_buffer;  ///< 读缓冲（读线程独占）。
    std::vector<Frame> m_frames;              ///< 本轮解出的帧（读线程独占）。
};

/**
 * @brief 服务端实现（PIMPL 后端）。
 */
class ServerImpl {
   public:
    explicit ServerImpl(ServerOptions options) : m_options(std::move(options)) {}
    ~ServerImpl() { Stop(); }

    ServerImpl(const ServerImpl&) = delete;
    ServerImpl& operator=(const ServerImpl&) = delete;

    /// @brief 注册表（可写，`Listen` 前）。
    ServiceRegistry& Registry() noexcept { return m_registry; }

    /// @brief 注册表（只读）。
    const ServiceRegistry& Registry() const noexcept { return m_registry; }

    /// @brief 开始监听。
    bool Listen(const Endpoint& endpoint);

    /// @brief 接管一条已建立传输。
    bool AddConnection(std::unique_ptr<Transport> transport);

    /// @brief 停止并 join 全部线程。
    void Stop();

    /// @brief 是否监听中。
    bool Running() const noexcept { return m_running.load(std::memory_order_acquire); }

    /// @brief 实际端口。
    std::uint16_t Port() const noexcept { return m_port; }

    /// @brief 监听地址文本。
    std::string Address() const { return m_address; }

    /// @brief 服务端选项。
    const ServerOptions& Options() const noexcept { return m_options; }

   private:
    /// @brief accept 循环（后台线程）。
    void AcceptLoop();

    /// @brief 回收已结束的连接（join 线程）。
    void Reap();

    ServerOptions m_options;                 ///< 选项。
    ServiceRegistry m_registry;              ///< 注册表。
    std::unique_ptr<TcpListener> m_listener; ///< 监听器。
    std::thread m_accept_thread;             ///< accept 线程。
    std::atomic<bool> m_running{false};      ///< 运行标志。
    std::mutex m_connections_mutex;          ///< 连接表锁。
    std::vector<std::shared_ptr<ServerConnection>> m_connections;  ///< 在线连接。
    std::atomic<std::size_t> m_connection_count{0};          ///< 在线连接数。
    std::uint16_t m_port{0};                 ///< 实际端口。
    std::string m_address;                   ///< 监听地址文本。
    std::mutex m_lifecycle_mutex;            ///< Listen/Stop 串行化。
};

// ---------------------------------------------------------------------------
// StreamSenderImpl（必须早于 ServerConnection::Process 定义）
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief `ServerStream` 的实际发送器：把消息编码成 StreamData 帧。
 */
class StreamSenderImpl final : public StreamSender {
   public:
    StreamSenderImpl(std::shared_ptr<ServerConnection> connection, std::uint64_t id,
                     std::shared_ptr<std::atomic<bool>> cancelled)
        : m_connection(std::move(connection)), m_id(id), m_cancelled(std::move(cancelled)) {}

    bool SendSerialized(const void* data, std::size_t size) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finished) {
            return false;
        }
        if (!m_connection->SendStreamData(m_id, m_sequence++, data, size)) {
            m_finished = true;
            return false;
        }
        return true;
    }

    Status Finish(Status status) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_finished) {
            return Status::Ok();
        }
        m_finished = true;
        if (!m_connection->SendStreamEnd(m_id, status)) {
            return Status(StatusCode::kUnavailable, "failed to send stream end");
        }
        return Status::Ok();
    }

    bool cancelled() const override {
        return m_cancelled != nullptr && m_cancelled->load(std::memory_order_relaxed);
    }

    std::uint64_t id() const override { return m_id; }

   private:
    std::shared_ptr<ServerConnection> m_connection;       ///< 连接。
    std::uint64_t m_id;                             ///< 流 id。
    std::shared_ptr<std::atomic<bool>> m_cancelled; ///< 取消标志。
    std::mutex m_mutex;                             ///< 保护下面字段。
    std::uint64_t m_sequence = 0;                   ///< 下一帧序号。
    bool m_finished = false;                        ///< 是否已结束。
};

}  // namespace

// ---------------------------------------------------------------------------
// ServerConnection
// ---------------------------------------------------------------------------

ServerConnection::ServerConnection(ServerImpl* server, std::unique_ptr<Transport> transport,
                                 FrameSpec spec, std::size_t worker_threads,
                                 std::size_t max_pending)
    : m_server(server),
      m_transport(std::move(transport)),
      m_codec(spec),
      m_worker_count(worker_threads == 0 ? 1 : worker_threads),
      m_max_pending(max_pending == 0 ? 1 : max_pending) {
    m_read_buffer.resize(64 * 1024);
}

ServerConnection::~ServerConnection() {
    Close();
}

void ServerConnection::Start() {
    auto self = shared_from_this();
    m_reader = std::thread([self]() { self->ReaderLoop(); });
    m_workers.reserve(m_worker_count);
    for (std::size_t index = 0; index < m_worker_count; ++index) {
        m_workers.emplace_back([self]() { self->WorkerLoop(); });
    }
}

void ServerConnection::Close() {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_transport->Close();
    m_queue_cv.notify_all();
}

void ServerConnection::Join() {
    if (m_reader.joinable()) {
        m_reader.join();
    }
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool ServerConnection::Finished() const noexcept {
    return m_reader_done.load(std::memory_order_acquire) &&
           m_workers_done.load(std::memory_order_acquire) >= m_worker_count;
}

bool ServerConnection::SendPayload(FrameType type, std::uint16_t flags,
                             const std::vector<std::uint8_t>& payload) {
    if (m_closed.load(std::memory_order_acquire)) {
        return false;
    }
    Frame frame;
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

bool ServerConnection::SendResponse(std::uint64_t call_id, const Status& status,
                              const reflect::Buffer& result) {
    thread_local std::vector<std::uint8_t> payload;
    if (!EncodeResponse(payload, call_id, status, result)) {
        return false;
    }
    return SendPayload(FrameType::kResponse, 0, payload);
}

bool ServerConnection::SendStreamData(std::uint64_t call_id, std::uint64_t sequence, const void* data,
                                std::size_t size) {
    thread_local std::vector<std::uint8_t> payload;
    (void)EncodeStreamData(payload, call_id, sequence, data, size);
    return SendPayload(FrameType::kStreamData, 0, payload);
}

bool ServerConnection::SendStreamEnd(std::uint64_t call_id, const Status& status) {
    thread_local std::vector<std::uint8_t> payload;
    if (!EncodeStreamEnd(payload, call_id, status)) {
        return false;
    }
    return SendPayload(FrameType::kStreamEnd, 0, payload);
}

void ServerConnection::ReaderLoop() {
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
            // 畸形字节流会静默断开连接；服务端要能区分「对端发脏数据」与「自身逻辑错」。
            GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("rpc"))
                << "server: frame decode failed, dropping connection: " << error.message;
            break;  // 字节流畸形：断开。
        }
        bool healthy = true;
        for (const Frame& frame_value : m_frames) {
            if (!HandleFrame(frame_value)) {
                healthy = false;
                break;
            }
        }
        if (!healthy) {
            break;
        }
    }
    m_reader_done.store(true, std::memory_order_release);
    Close();
}

bool ServerConnection::HandleFrame(const Frame& frame_value) {
    switch (static_cast<FrameType>(frame_value.type)) {
        case FrameType::kRequest:
            return HandleRequest(frame_value);
        case FrameType::kCancel:
            return HandleCancel(frame_value);
        case FrameType::kPing:
            return HandlePing(frame_value);
        case FrameType::kPong:
            return true;
        case FrameType::kResponse:
        case FrameType::kStreamData:
        case FrameType::kStreamEnd:
            return false;  // 客户端方向不该出现这些帧。
    }
    return true;  // 未知类型：向前兼容，忽略。
}

/**
 * @brief 解码一个 Request 帧，登记取消位，放入有界请求队列。
 *
 * @param frame_value [in] 已从字节流切出的完整帧，payload 由 codec 校验；引用只在本次调用内有效。
 *
 * @return
 * - true：请求已受理或已回 `kResourceExhausted`——调用方**必须**继续读下一帧；
 * - false：解码失败或连接已关闭——调用方**必须**断开连接，否则字节流状态已不可信。
 *
 * ## 定位
 * 服务端唯一请求入口。读线程**绝不能**在这里执行 handler：一旦执行，Cancel/Ping
 * 帧就会被拖到 handler 结束之后才读到，取消对非流式调用完全失效。队列有界是为了
 * 让「worker 卡住 + 客户端猛发」不会把内存吃光；满时立刻回错误而不是阻塞读线程。
 *
 * ## 步骤
 * step1. 解码 call_id/method/args/streaming，失败即断开；
 * step2. 构造 PendingRequest，分配共享取消位；
 * step3. 先登记到 m_cancels，保证随后到达的 Cancel 能命中（含已入队未执行的请求）；
 * step4. 持队列锁检查关闭与队列余量，有空位则入队；
 * step5. 队列满：清掉取消位并回 `kResourceExhausted`（流式用 StreamEnd，否则客户端挂死）；
 * step6. 入队成功后唤醒一个 worker，立刻返回读循环。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在 ReaderLoop 线程调用；m_cancels/m_queue 各自有互斥保护。
 * - 原子性：取消位登记先于入队，Cancel 与执行之间有明确先后；队列余量判断与
 *   入队在同一临界区，不会超卖容量。
 * - 失败语义：返回 false 表示连接级故障，调用方断开；不抛异常。
 * - 不变量：返回 true 后该 call_id 要么在 m_cancels 中等待执行，要么已收到错误响应。
 */
bool ServerConnection::HandleRequest(const Frame& frame_value) {
    RequestMessage message;
    std::string error;
    if (!DecodeRequest(frame_value, &message, &error)) {
        return false;
    }
    auto request = std::make_shared<PendingRequest>();
    request->call_id = message.call_id;
    request->method = std::move(message.method);
    request->args = std::move(message.args);
    request->streaming = message.streaming;
    request->cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(m_cancels_mutex);
        m_cancels[request->call_id] = request->cancelled;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
            return false;
        }
        if (m_queue.size() < m_max_pending) {
            m_queue.push_back(request);
            accepted = true;
        }
    }
    if (!accepted) {
        // step5: 队列满不是连接级故障：回错误帧让客户端立刻失败，读线程继续工作。
        EraseCancel(request->call_id);
        const Status overloaded(StatusCode::kResourceExhausted,
                                "server request queue is full");
        if (request->streaming) {
            (void)SendStreamEnd(request->call_id, overloaded);
        } else {
            reflect::Buffer empty;
            (void)SendResponse(request->call_id, overloaded, empty);
        }
        return !m_closed.load(std::memory_order_acquire);
    }
    m_queue_cv.notify_one();
    return true;
}

bool ServerConnection::HandleCancel(const Frame& frame_value) {
    std::uint64_t call_id = 0;
    std::string error;
    if (!DecodeCallId(frame_value, &call_id, &error)) {
        return false;
    }
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_cancels_mutex);
        const auto found = m_cancels.find(call_id);
        if (found != m_cancels.end()) {
            cancelled = found->second;
        }
    }
    if (cancelled) {
        cancelled->store(true, std::memory_order_relaxed);
    }
    return true;
}

bool ServerConnection::HandlePing(const Frame& frame_value) {
    std::uint64_t call_id = 0;
    std::string error;
    if (!DecodeCallId(frame_value, &call_id, &error)) {
        return false;
    }
    thread_local std::vector<std::uint8_t> payload;
    (void)EncodePing(payload, call_id, true);
    return SendPayload(FrameType::kPong, 0, payload);
}

/**
 * @brief 工作线程主循环：阻塞等待队列，取出请求并执行。
 *
 * @return 无返回值；m_closed 置位后退出并递增 m_workers_done。
 *
 * ## 定位
 * handler 的唯一执行位置（每连接 N 个这样的线程）。漏掉唤醒会让请求永远躺在队列里；
 * 漏掉关闭检查会让线程无法退出，`Stop()` 随之挂死。
 *
 * ## 步骤
 * step1. 持队列锁等待「有请求或已关闭」；
 * step2. 已关闭则退出循环；
 * step3. 取队首并弹栈；
 * step4. 释放锁后执行 Process，避免长 handler 阻塞入队与其它 worker。
 *
 * ## 并发/失败语义
 * - 调用约束：每连接 m_worker_count 个 worker 共享同一队列；与 ReaderLoop 通过
 *   m_queue + condvar 通信。
 * - 原子性：出队与关闭检查在同一临界区，不会丢唤醒；多个 worker 各自取走不同请求。
 * - 失败语义：无异常出口；Process 内部已吞掉错误。
 * - 不变量：每个线程退出时对 m_workers_done +1；全部退出后 Join 可以安全等待。
 */
void ServerConnection::WorkerLoop() {
    for (;;) {
        std::shared_ptr<PendingRequest> request;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this]() {
                return m_closed.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_closed.load(std::memory_order_acquire)) {
                break;
            }
            request = std::move(m_queue.front());
            m_queue.pop_front();
        }
        Process(request);
    }
    m_workers_done.fetch_add(1, std::memory_order_acq_rel);
}

void ServerConnection::EraseCancel(std::uint64_t call_id) {
    std::lock_guard<std::mutex> lock(m_cancels_mutex);
    m_cancels.erase(call_id);
}

/**
 * @brief 在注册表里查方法并执行，把结果编码成 Response 或 StreamEnd 发回客户端。
 *
 * @param request [in] 已解码的待处理请求，shared_ptr 保证执行期间存活——所有权借用，不延长生命周期。
 *
 * @return 无返回值；所有失败都通过错误响应表达，不向调用方抛异常。
 *
 * ## 定位
 * 唯一的方法执行点。方法未注册与 streaming 标志不匹配都必须回错误帧，否则客户端会永久等待；
 * 漏发 StreamEnd 会让流式调用挂死。
 *
 * ## 步骤
 * step1. 查注册表，未命中回 kNotFound（流式用 StreamEnd）；
 * step2. 校验 streaming 标志与注册绑定一致，不一致回 kInvalidArgument；
 * step3. 构造 CallContext（携带取消位）；
 * step4. 流式：建 StreamSender 并执行，最后 Finish 发送 StreamEnd；
 * step5. 非流式：执行后直接回 Response；
 * step6. 所有分支清理取消位登记。
 *
 * ## 并发/失败语义
 * - 调用约束：只能由工作线程调用；同一连接上的多个请求可并发执行。
 * - 原子性：单个请求的执行是串行的；跨请求并发由 handler 自行保证安全。
 * - 失败语义：handler 抛异常会由上层捕获转成 kInternal（见调用点）；本函数不抛。
 * - 不变量：返回前该 call_id 的取消位已从 m_cancels 移除。
 */
void ServerConnection::Process(const std::shared_ptr<PendingRequest>& request) {
    const BoundMethod* bound = m_server->Registry().Find(request->method);
    if (bound == nullptr) {
        if (request->streaming) {
            thread_local std::vector<std::uint8_t> payload;
            (void)EncodeStreamEnd(payload, request->call_id,
                                  Status(StatusCode::kNotFound, "method not registered"));
            (void)SendPayload(FrameType::kStreamEnd, 0, payload);
        } else {
            reflect::Buffer empty;
            (void)SendResponse(request->call_id,
                               Status(StatusCode::kNotFound, "method not registered"), empty);
        }
        EraseCancel(request->call_id);
        return;
    }
    if (bound->binding.streaming != request->streaming) {
        const Status mismatch(StatusCode::kInvalidArgument,
                              "streaming flag does not match registered method");
        if (request->streaming) {
            // 客户端按流打开：必须用 StreamEnd 结束，否则它会一直等消息。
            thread_local std::vector<std::uint8_t> payload;
            (void)EncodeStreamEnd(payload, request->call_id, mismatch);
            (void)SendPayload(FrameType::kStreamEnd, 0, payload);
        } else {
            reflect::Buffer empty;
            (void)SendResponse(request->call_id, mismatch, empty);
        }
        EraseCancel(request->call_id);
        return;
    }

    CallContext context(request->call_id, &request->method, request->cancelled.get());
    reflect::Buffer result;
    if (request->streaming) {
        auto sender = std::make_shared<StreamSenderImpl>(shared_from_this(), request->call_id,
                                                         request->cancelled);
        ServerStream stream(sender);
        Status status;
        // step4: handler 异常必须在这里转成 kInternal——穿透到线程函数会让整个
        //        服务进程 std::terminate，客户端只能看到连接突然断开。
        try {
            status = bound->binding.invoke(bound->service, context, request->args, result,
                                           &stream);
        } catch (const std::exception& error) {
            status = Status(StatusCode::kInternal,
                            std::string("handler threw: ") + error.what());
        } catch (...) {
            status = Status(StatusCode::kInternal, "handler threw unknown exception");
        }
        (void)sender->Finish(status);
    } else {
        Status status;
        try {
            status = bound->binding.invoke(bound->service, context, request->args, result,
                                           nullptr);
        } catch (const std::exception& error) {
            status = Status(StatusCode::kInternal,
                            std::string("handler threw: ") + error.what());
            result.Clear();
        } catch (...) {
            status = Status(StatusCode::kInternal, "handler threw unknown exception");
            result.Clear();
        }
        // 客户端已取消：响应已无人接收，省掉一次写；取消位在函数末尾统一清理。
        if (!request->cancelled->load(std::memory_order_acquire)) {
            (void)SendResponse(request->call_id, status, result);
        }
    }
    EraseCancel(request->call_id);
}

// ---------------------------------------------------------------------------
// ServerImpl
// ---------------------------------------------------------------------------

bool ServerImpl::Listen(const Endpoint& endpoint) {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    if (m_running.load(std::memory_order_acquire)) {
        return false;
    }
    Status status;
    auto listener = TcpListener::Bind(endpoint, m_options.backlog, &status);
    if (!listener) {
        return false;
    }
    m_registry.Freeze();
    m_listener = std::move(listener);
    m_port = m_listener->port();
    Endpoint actual = endpoint;
    actual.port = m_port;
    m_address = actual.ToString();
    m_running.store(true, std::memory_order_release);
    m_accept_thread = std::thread([this]() { AcceptLoop(); });
    return true;
}

bool ServerImpl::AddConnection(std::unique_ptr<Transport> transport) {
    if (!transport || !m_running.load(std::memory_order_acquire)) {
        return false;
    }
    if (m_connection_count.load(std::memory_order_acquire) >= m_options.max_connections) {
        transport->Close();
        return false;
    }
    auto connection = std::make_shared<ServerConnection>(
        this, std::move(transport), MakeFrameSpecFromOptions(m_options),
        m_options.worker_threads, m_options.max_pending_requests);
    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        if (!m_running.load(std::memory_order_acquire)) {
            connection->Close();
            return false;
        }
        m_connections.push_back(connection);
        m_connection_count.fetch_add(1, std::memory_order_acq_rel);
    }
    connection->Start();
    return true;
}

void ServerImpl::AcceptLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        Status status;
        auto transport = m_listener->Accept(m_options.accept_poll_ms, &status);
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (transport) {
            AddConnection(std::move(transport));
        }
        Reap();
    }
    Reap();
}

void ServerImpl::Reap() {
    std::vector<std::shared_ptr<ServerConnection>> finished;
    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if ((*it)->Finished()) {
                finished.push_back(*it);
                it = m_connections.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& connection : finished) {
        connection->Join();
        m_connection_count.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void ServerImpl::Stop() {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    m_running.store(false, std::memory_order_release);
    if (m_listener) {
        m_listener->Close();
    }
    if (m_accept_thread.joinable()) {
        m_accept_thread.join();
    }
    std::vector<std::shared_ptr<ServerConnection>> connections;
    {
        std::lock_guard<std::mutex> connections_lock(m_connections_mutex);
        connections.swap(m_connections);
    }
    for (auto& connection : connections) {
        connection->Close();
    }
    for (auto& connection : connections) {
        connection->Join();
    }
    m_connection_count.store(0, std::memory_order_release);
    m_listener.reset();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ServerStream
// ---------------------------------------------------------------------------

bool ServerStream::SendSerialized(const void* data, std::size_t size) {
    return m_sender != nullptr && m_sender->SendSerialized(data, size);
}

Status ServerStream::Finish(Status status) {
    if (m_sender == nullptr) {
        return Status(StatusCode::kFailedPrecondition, "invalid server stream");
    }
    return m_sender->Finish(std::move(status));
}

bool ServerStream::cancelled() const {
    return m_sender != nullptr && m_sender->cancelled();
}

std::uint64_t ServerStream::id() const {
    return m_sender != nullptr ? m_sender->id() : 0;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

Server::Server(ServerOptions options)
    : m_impl(std::make_unique<detail::ServerImpl>(std::move(options))) {}

Server::~Server() = default;

ServiceRegistry& Server::MutableRegistry() {
    return m_impl->Registry();
}

const ServiceRegistry& Server::registry() const {
    return m_impl->Registry();
}

bool Server::Listen(std::string_view address) {
    Endpoint endpoint;
    if (!Endpoint::Parse(address, &endpoint)) {
        return false;
    }
    return Listen(endpoint);
}

bool Server::Listen(const Endpoint& endpoint) {
    return m_impl->Listen(endpoint);
}

bool Server::AddConnection(std::unique_ptr<Transport> transport) {
    return m_impl->AddConnection(std::move(transport));
}

void Server::Stop() {
    m_impl->Stop();
}

bool Server::running() const {
    return m_impl->Running();
}

std::uint16_t Server::port() const {
    return m_impl->Port();
}

std::string Server::address() const {
    return m_impl->Address();
}

}  // namespace go2cpp::rpc
