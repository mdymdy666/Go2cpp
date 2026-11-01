#pragma once

/**
 * @file client.hpp
 * @brief RPC 客户端：同步调用、异步调用、流式、超时与取消。
 *
 * @details
 * 一条连接由一个读线程 + 一把写锁 + 一张 `call_id -> 等待者` 表组成：
 *
 * - **同步 `Call`**：分配 `call_id` → 写请求帧 → 在条件变量上等待响应；
 * - **多路复用**：多个线程可以同时在同一连接上调用，读线程按 `call_id`
 *   把响应投递给对应等待者；
 * - **超时**：等待超过 deadline 后从等待表摘除、发送 `Cancel` 帧并返回
 *   `kDeadlineExceeded`；迟到的响应会被读线程丢弃（不会串到下一次调用）；
 * - **取消**：`PendingCall::Cancel()` 本地立即唤醒并返回 `kCancelled`，
 *   同时发 `Cancel` 帧让服务端尽早停止；
 * - **流**：`OpenStream` 打开服务端流，`ClientStream::Recv<T>` 逐条取消息。
 *
 * ## Go 风格用法
 *
 * ```cpp
 * rpc::Client client("127.0.0.1:8080");
 * User user = client.Call<User>("User.Get", UserId{42});         // 失败抛 RpcError
 *
 * Status status = client.TryCall("User.Get", &user, UserId{42}); // 无异常
 * client.CallStatus("User.Remove", UserId{42});                  // 无返回值
 *
 * auto pending = client.AsyncCall<Reply>("Slow.Echo", request);   // 异步 + 取消
 * pending.Cancel();
 *
 * auto stream = client.OpenStream("Chat.Subscribe", Topic{"news"});
 * ChatMessage message;
 * while (stream.Recv(&message).ok()) { ... }
 * ```
 *
 * ## 内存与线程
 *
 * - 参数打包用 `thread_local` Buffer，热路径不分配（首次调用后容量复用）；
 * - 每条连接一个读线程；`Call` 本身不创建线程；
 * - `Client` 可被多个线程同时使用（写锁 + 等待表内部加锁）；
 * - 一个 `PendingCall` 只能 `Wait` 一次（结果缓冲被消费）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/rpc/protocol.hpp"
#include "go2cpp/rpc/status.hpp"
#include "go2cpp/rpc/transport.hpp"

namespace go2cpp::rpc {

namespace detail {

class Connection;
struct CallState;
struct StreamState;

/// @brief 表示「用客户端默认超时」的哨兵值。
inline constexpr std::chrono::milliseconds kUseDefaultTimeout{-1};

/// @brief 等待一次调用完成（超时返回 kDeadlineExceeded）。
Status WaitForCall(const std::shared_ptr<Connection>& connection,
                   const std::shared_ptr<CallState>& state,
                   std::chrono::milliseconds timeout);

/// @brief 取消一次调用（本地唤醒 + 发送 Cancel 帧）。
bool CancelCall(const std::shared_ptr<Connection>& connection, std::uint64_t call_id);

/// @brief 等待流的下一条消息；超时返回 kDeadlineExceeded，流结束返回对应状态。
Status WaitForStreamFrame(const std::shared_ptr<Connection>& connection,
                          const std::shared_ptr<StreamState>& state, reflect::Buffer* out,
                          std::chrono::milliseconds timeout);

/// @brief 等待流结束（不带数据）；返回流的最终状态。
Status WaitForStreamEnd(const std::shared_ptr<Connection>& connection,
                        const std::shared_ptr<StreamState>& state,
                        std::chrono::milliseconds timeout);

/// @brief 把参数按「varint 长度 + 序列化字节」逐个追加。
template <typename T>
void PackArgument(reflect::Buffer& out, const T& value) {
    thread_local reflect::Buffer scratch;
    scratch.Clear();
    if (!reflect::Serialize(value, scratch)) {
        out.Fail();
        return;
    }
    if (!out.PutVarint(static_cast<std::uint64_t>(scratch.size())) ||
        !out.PutBytes(scratch.data().data(), scratch.size())) {
        out.Fail();
    }
}

/// @brief 参数包展开。
template <typename... Args>
void PackArguments(reflect::Buffer& out, Args&&... args) {
    (PackArgument(out, args), ...);
}

}  // namespace detail

/**
 * @brief 客户端选项。
 */
struct ClientOptions {
    /// 建立连接的超时。
    std::chrono::milliseconds connect_timeout{1000};
    /// 单次调用默认超时（`Call`/`TryCall` 未显式指定时使用）。
    std::chrono::milliseconds timeout{5000};
    /// 单帧载荷上限（必须与服务端一致）。
    std::size_t max_payload_bytes = 4u * 1024u * 1024u;
    /// 是否启用帧校验和（必须与服务端一致）。
    bool checksum = true;
    /// 流缓冲上限（帧数）；超过后流以 `kResourceExhausted` 结束。
    std::size_t max_stream_frames = 1024;
    /// 流缓冲上限（字节）。
    std::size_t max_stream_bytes = 16u * 1024u * 1024u;
    /// 是否对 socket 设置 TCP_NODELAY（默认开启，见 transport.hpp 说明）。
    bool tcp_no_delay = true;
};

namespace detail {

/// @brief 一次调用的共享等待状态（读线程与调用线程之间）。
struct CallState {
    std::uint64_t id = 0;                  ///< 调用 id。
    std::mutex mutex;                      ///< 保护下面字段。
    std::condition_variable cv;            ///< 响应到达/超时/连接关闭时唤醒。
    bool done = false;                     ///< 是否已完成（响应、超时或取消）。
    std::atomic<bool> completed{false};    ///< 无锁完成标志（等待路径先自旋）。
    Status status;                         ///< 完成状态。
    reflect::Buffer payload;               ///< 结果字节。
};

/// @brief 一条客户端流的共享状态。
struct StreamState {
    std::uint64_t id = 0;                         ///< 流 id。
    std::mutex mutex;                             ///< 保护下面字段。
    std::condition_variable cv;                   ///< 消息到达/结束时唤醒。
    std::deque<std::vector<std::uint8_t>> frames; ///< 待消费消息（FIFO）。
    std::size_t buffered_bytes = 0;               ///< 当前缓冲字节数。
    bool ended = false;                           ///< 是否已收到 StreamEnd/连接关闭。
    Status end_status;                            ///< 流最终状态。
    std::size_t max_frames = 1024;                ///< 帧数上限。
    std::size_t max_bytes = 16u * 1024u * 1024u;  ///< 字节上限。
};

}  // namespace detail

/**
 * @brief 客户端流：服务端逐条推送，客户端逐条接收。
 *
 * @note 非线程安全：同一时刻只应有一个线程 `Recv`；不同流可以并发。
 */
class ClientStream {
   public:
    /// @brief 无效流（默认构造）。
    ClientStream() = default;

    /// @brief 流 id。
    std::uint64_t id() const noexcept { return m_state ? m_state->id : 0; }

    /// @brief 是否有效。
    bool valid() const noexcept { return static_cast<bool>(m_state); }

    /// @brief 是否已结束（收到 StreamEnd / 取消 / 连接关闭）。
    bool done() const;

    /**
     * @brief 取流的下一条消息。
     * @tparam T 消息类型（必须可反序列化）。
     * @param out 消息输出（非空）。
     * @param status 结束/错误状态：有消息返回时写 `kOk`；返回 false 时写流的
     *               最终状态（正常结束也是 `kOk`）。
     * @param timeout 等待超时；`-1` 用打开流时的默认值。
     * @return true = 拿到一条消息；false = 流结束或出错（看 `*status`）。
     */
    template <typename T>
    bool Next(T* out, Status* status,
              std::chrono::milliseconds timeout = detail::kUseDefaultTimeout) {
        if (out == nullptr) {
            if (status != nullptr) {
                *status = Status(StatusCode::kInvalidArgument, "null message pointer");
            }
            return false;
        }
        reflect::Buffer payload;
        Status end_status;
        if (!NextPayload(&payload, &end_status, timeout)) {
            if (status != nullptr) {
                *status = std::move(end_status);
            }
            return false;
        }
        if (!reflect::Deserialize(payload, *out) || payload.Remaining() != 0) {
            if (status != nullptr) {
                *status = Status(StatusCode::kInternal, "failed to decode stream message");
            }
            return false;
        }
        if (status != nullptr) {
            *status = Status::Ok();
        }
        return true;
    }

    /**
     * @brief 等待流结束（把剩余消息读完后返回最终状态）。
     * @param timeout 超时；`-1` 用默认值。
     * @return 流最终状态；超时返回 `kDeadlineExceeded`。
     */
    Status Finish(std::chrono::milliseconds timeout = detail::kUseDefaultTimeout);

    /// @brief 取消流（本地唤醒 + 发送 Cancel 帧）。
    bool Cancel();

   private:
    friend class Client;

    ClientStream(std::shared_ptr<detail::Connection> connection,
                 std::shared_ptr<detail::StreamState> state,
                 std::chrono::milliseconds timeout)
        : m_connection(std::move(connection)), m_state(std::move(state)), m_timeout(timeout) {}

    /// 取下一条消息的原始字节；返回 false 时写流的最终状态。
    bool NextPayload(reflect::Buffer* out, Status* status,
                     std::chrono::milliseconds timeout);

    std::shared_ptr<detail::Connection> m_connection;  ///< 所属连接。
    std::shared_ptr<detail::StreamState> m_state;      ///< 流状态。
    std::chrono::milliseconds m_timeout{5000};         ///< 默认超时。
};

/**
 * @brief 异步调用句柄：`Cancel` 可在另一个线程调用。
 * @tparam Result 结果类型（必须可默认构造/反序列化）。
 */
template <typename Result>
class PendingCall {
   public:
    /// @brief 无效句柄。
    PendingCall() = default;

    /// @brief 是否有效（`AsyncCall` 成功返回）。
    bool valid() const noexcept { return static_cast<bool>(m_state); }

    /// @brief 调用 id。
    std::uint64_t id() const noexcept { return m_state ? m_state->id : 0; }

    /// @brief 是否已完成。
    bool done() const {
        if (!m_state) {
            return true;
        }
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->done;
    }

    /**
     * @brief 取消调用：本地立即唤醒，`Wait` 返回 `kCancelled`。
     * @return 句柄有效且成功发出取消（连接已断时返回 false）。
     */
    bool Cancel() {
        if (!m_state) {
            return false;
        }
        return detail::CancelCall(m_connection, m_state->id);
    }

    /**
     * @brief 等待结果。
     * @param out 结果输出（非空）。
     * @param timeout 等待超时；`-1` 用创建时的默认值。
     * @return 成功 `kOk`；失败/超时/取消返回对应状态。
     */
    Status Wait(Result* out,
                std::chrono::milliseconds timeout = detail::kUseDefaultTimeout) {
        if (out == nullptr) {
            return Status(StatusCode::kInvalidArgument, "null result pointer");
        }
        if (!m_start_status.ok()) {
            return m_start_status;
        }
        if (!m_state) {
            return Status(StatusCode::kInternal, "invalid pending call");
        }
        Status status = detail::WaitForCall(m_connection, m_state, Resolve(timeout));
        if (!status.ok()) {
            return status;
        }
        if (!reflect::Deserialize(m_state->payload, *out)) {
            return Status(StatusCode::kInternal, "failed to decode response");
        }
        if (m_state->payload.Remaining() != 0) {
            return Status(StatusCode::kInternal, "trailing bytes in response");
        }
        return Status::Ok();
    }

    /// @brief 等待并返回结果（失败抛 `RpcError`）。
    Result Wait(std::chrono::milliseconds timeout = detail::kUseDefaultTimeout) {
        Result value{};
        Status status = Wait(&value, timeout);
        if (!status.ok()) {
            throw RpcError(status);
        }
        return value;
    }

   private:
    friend class Client;

    PendingCall(std::shared_ptr<detail::Connection> connection,
                std::shared_ptr<detail::CallState> state, Status start_status,
                std::chrono::milliseconds timeout)
        : m_connection(std::move(connection)),
          m_state(std::move(state)),
          m_start_status(std::move(start_status)),
          m_timeout(timeout) {}

    std::chrono::milliseconds Resolve(std::chrono::milliseconds timeout) const {
        return timeout.count() < 0 ? m_timeout : timeout;
    }

    std::shared_ptr<detail::Connection> m_connection;  ///< 所属连接。
    std::shared_ptr<detail::CallState> m_state;        ///< 调用状态。
    Status m_start_status;                             ///< 发起阶段的状态。
    std::chrono::milliseconds m_timeout{5000};         ///< 默认超时。
};

/**
 * @brief RPC 客户端（一条连接，内部多路复用）。
 */
class Client {
   public:
    /**
     * @brief 连接远端地址（失败不抛异常，后续调用返回 `kUnavailable`）。
     * @param address `"host:port"`。
     * @param options 客户端选项。
     */
    explicit Client(std::string_view address, ClientOptions options = {});

    /**
     * @brief 接管一条已建立的传输（WebSocket / 自定义传输）。
     * @param transport 传输对象（所有权转移）。
     * @param options 客户端选项。
     */
    explicit Client(std::unique_ptr<Transport> transport, ClientOptions options = {});

    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// @brief 是否已连接。
    bool connected() const;

    /// @brief 最近一次连接/IO 错误（诊断用）。
    const Status& last_error() const noexcept { return m_connect_status; }

    /// @brief 当前默认超时。
    std::chrono::milliseconds timeout() const noexcept { return m_options.timeout; }

    /// @brief 修改默认超时（线程安全：仅影响之后发起的调用）。
    void set_timeout(std::chrono::milliseconds timeout) noexcept { m_options.timeout = timeout; }

    /// @brief 关闭连接（唤醒所有等待者）。
    void Close();

    /**
     * @brief 同步调用，返回结果；失败抛 `RpcError`。
     * @tparam Result 结果类型；`void` 表示无返回值。
     * @param method 完整方法名（`"User.Get"`）。
     * @param args 参数（类型必须与服务端签名一致）。
     * @return 结果。
     */
    template <typename Result = void, typename... Args>
    Result Call(std::string_view method, Args&&... args) {
        if constexpr (std::is_void_v<Result>) {
            Status status = CallStatus(method, std::forward<Args>(args)...);
            if (!status.ok()) {
                throw RpcError(std::move(status));
            }
        } else {
            Result result{};
            Status status = TryCall(method, &result, std::forward<Args>(args)...);
            if (!status.ok()) {
                throw RpcError(std::move(status));
            }
            return result;
        }
    }

    /**
     * @brief 同步调用（无异常），结果写入 `out`。
     * @tparam Result 结果类型。
     * @param method 完整方法名。
     * @param out 结果输出（非空）。
     * @param args 参数。
     * @return 状态。
     */
    template <typename Result, typename... Args>
    Status TryCall(std::string_view method, Result* out, Args&&... args) {
        if (out == nullptr) {
            return Status(StatusCode::kInvalidArgument, "null result pointer");
        }
        reflect::Buffer packed;
        detail::PackArguments(packed, std::forward<Args>(args)...);
        if (!packed.ok()) {
            return Status(StatusCode::kInvalidArgument, "failed to serialize arguments");
        }
        std::shared_ptr<detail::CallState> state;
        Status status = BeginCall(method, packed, false, &state);
        if (!status.ok()) {
            return status;
        }
        status = WaitCall(state, m_options.timeout);
        if (!status.ok()) {
            return status;
        }
        if (!reflect::Deserialize(state->payload, *out)) {
            return Status(StatusCode::kInternal, "failed to decode response");
        }
        if (state->payload.Remaining() != 0) {
            return Status(StatusCode::kInternal, "trailing bytes in response");
        }
        return Status::Ok();
    }

    /**
     * @brief 同步调用（无返回值、无异常）。
     * @param method 完整方法名。
     * @param args 参数。
     * @return 状态。
     */
    template <typename... Args>
    Status CallStatus(std::string_view method, Args&&... args) {
        reflect::Buffer packed;
        detail::PackArguments(packed, std::forward<Args>(args)...);
        if (!packed.ok()) {
            return Status(StatusCode::kInvalidArgument, "failed to serialize arguments");
        }
        std::shared_ptr<detail::CallState> state;
        Status status = BeginCall(method, packed, false, &state);
        if (!status.ok()) {
            return status;
        }
        return WaitCall(state, m_options.timeout);
    }

    /**
     * @brief 发起异步调用（不等待）。
     * @tparam Result 结果类型。
     * @param method 完整方法名。
     * @param args 参数。
     * @return 句柄；发起失败时 `valid() == false`，`Wait` 返回发起错误。
     */
    template <typename Result, typename... Args>
    PendingCall<Result> AsyncCall(std::string_view method, Args&&... args) {
        reflect::Buffer packed;
        detail::PackArguments(packed, std::forward<Args>(args)...);
        std::shared_ptr<detail::CallState> state;
        Status status;
        if (!packed.ok()) {
            status = Status(StatusCode::kInvalidArgument, "failed to serialize arguments");
        } else {
            status = BeginCall(method, packed, false, &state);
        }
        return PendingCall<Result>(m_connection, std::move(state), std::move(status),
                                   m_options.timeout);
    }

    /**
     * @brief 打开服务端流。
     * @param method 完整方法名（服务端必须是流式方法）。
     * @param args 参数。
     * @return 流对象；打开失败时 `valid() == false`，后续 `Recv` 返回失败状态。
     */
    template <typename... Args>
    ClientStream OpenStream(std::string_view method, Args&&... args) {
        reflect::Buffer packed;
        detail::PackArguments(packed, std::forward<Args>(args)...);
        return OpenStreamRaw(method, packed);
    }

    /**
     * @brief 心跳：发 Ping 并等待 Pong。
     * @return 成功 `kOk`；超时/断开返回对应状态。
     */
    Status Ping();

   private:
    /// 发起一次调用（打包好的参数），返回等待状态。
    Status BeginCall(std::string_view method, const reflect::Buffer& args, bool streaming,
                     std::shared_ptr<detail::CallState>* state);

    /// 等待一次调用完成。
    Status WaitCall(const std::shared_ptr<detail::CallState>& state,
                    std::chrono::milliseconds timeout);

    /// 打开流（打包好的参数）。
    ClientStream OpenStreamRaw(std::string_view method, const reflect::Buffer& args);

    ClientOptions m_options;                          ///< 客户端选项。
    std::shared_ptr<detail::Connection> m_connection; ///< 连接（可能为空）。
    Status m_connect_status;                          ///< 建立连接时的状态。
};

}  // namespace go2cpp::rpc
