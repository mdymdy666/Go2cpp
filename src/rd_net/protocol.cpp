#include "go2cpp/rd_net/protocol.hpp"

/// @file protocol.cpp
/// @brief REQ-008：把 ProtocolContext 接到 REQ-007 的 EventLoop 上。
///
/// @details 桥接方式：本文件内的 DatagramBridge / StreamBridge 直接实现统一的
///          `rd::net::Protocol` / `rd::net::StreamProtocol` 的事件循环层回调，
///          同时作为 `rd::net::ProtocolContext` 被 `AttachProtocol` 返回给插件，
///          再把事件循环层回调转发成协议层回调（带 ProtocolContext&）。只有一份
///          类型定义（protocol.hpp），没有 `rd::net::plugin` 子命名空间，也不再
///          继承任何「占位类」。event_loop.hpp / socket.hpp 均未改接口语义。
///
///          发送路径（零拷贝 + 入队）：
///          1. SendTo 只分配 token 并把 {借用视图, 目标, token} 压入队列，**不拷贝
///             payload、不发 syscall**，立即返回；
///          2. 每次插件回调返回后（同一 tick 内）按 64 个/次的预算把队列真正
///             send 出去，每个完成回调一次 OnSendComplete；
///          3. 队列还有剩余就挂 0ms 定时器，下一个 tick 继续（不饿死定时器/其它
///             socket）。
///
///          生命周期：Attachment 析构 → EventLoop::Detach → 本文件的 OnDetached →
///          插件 OnDetached(reason)。若 socket 先出错被 loop 摘除，则插件先收到
///          OnDetached，随后 Attachment 析构时的 Detach 返回 false（无重复回调）。

#include <cstddef>
#include <deque>
#include <memory>
#include <utility>

#include "go2cpp/rd_net/event_loop.hpp"

namespace rd::net {
namespace {

/// @brief 单 socket 单 tick 的发送预算（与 REQ-007 的 kMaxPacketsPerTick 对齐）。
constexpr std::size_t kSendBudgetPerTick = 64;

/// @brief 待发送队列上限：满时 SendTo 返回 kWouldBlock，由上层自行退避。
constexpr std::size_t kMaxPendingSends = 65536;

struct PendingDatagram {
    ByteView view{};   ///< 借用：指向调用方内存，真正 send 时才读。
    Endpoint to{};     ///< 目标端点（小字符串走 SSO，不分配）。
    std::uint64_t token{0};
};

struct PendingStreamChunk {
    ByteView view{};
    std::uint64_t token{0};
};

/// @brief UDP 桥接器：统一 Protocol 的事件循环层 ←→ 协议层（ProtocolContext）。
class DatagramBridge final : public Protocol,
                             public ProtocolContext,
                             public std::enable_shared_from_this<DatagramBridge> {
public:
    DatagramBridge(EventLoop& loop, DatagramSocket& socket, Protocol& handler)
        : loop_(loop), socket_(socket), handler_(handler),
          local_(socket.local_endpoint()) {}

    ~DatagramBridge() override { pending_.clear(); }

    DatagramBridge(const DatagramBridge&) = delete;
    DatagramBridge& operator=(const DatagramBridge&) = delete;

    // ---- 事件循环层：转发到协议层 ----

    void OnAttached(EventLoop& loop) override {
        (void)loop;
        if (attached_) {
            return;
        }
        attached_ = true;
        handler_.OnAttached(*this);
        Pump();
    }

    void OnDatagram(ByteView data, const Endpoint& from) override {
        if (!attached_) {
            return;
        }
        handler_.OnDatagram(*this, data, from);
        Pump();
    }

    void OnDetached() override {
        if (!attached_) {
            return;
        }
        attached_ = false;
        pending_.clear();
        handler_.OnDetached(*this, DetachReason::kClosed);
    }

    // ---- ProtocolContext ----

    /**
     * @brief 协议层 SendTo 的入队实现：只分配 token 并把借用视图压进待发队列，不发 syscall。
     *
     * @param data [in] 载荷视图——借用，必须活到真正 send 的那一刻（调用方需保证内存不被提前释放）。
     * @param to [in] 目标端点——按值复制进队列（SSO，不分配）。
     *
     * @return
     * - SendResult{kOk, token}：已入队——调用方**不要**认为已发出，完成通知经 OnSendComplete 异步到达；
     * - SendResult{kWouldBlock, 0}：队列已满（kMaxPendingSends）——调用方**必须**退避后重试，不要丢弃数据；
     * - SendResult{kInvalid, 0}：桥接器未挂载——调用方**不要**继续发送，等 OnAttached 之后再试。
     *
     * ## 定位
     * 发送队列的唯一入队点；队列长度与借用语义共同决定内存安全边界。
     * 写错（例如这里拷贝或提前释放视图）会导致悬垂读取或数据错发到别的连接。
     *
     * ## 步骤
     * step1. 校验 attached_，未挂载直接 kInvalid；
     * step2. 队列达到上限时返回 kWouldBlock，让调用方背压而不是无界增长内存；
     * step3. 分配递增 token 并入队 {view, to, token}，立即返回 kOk（真正的发送交给 Pump）。
     *
     * ## 并发/失败语义
     * - 调用约束：只在协议回调内（事件循环线程）调用；pending_ 无锁。
     * - 原子性：一次入队是单步 push_back；失败时队列不变。
     * - 失败语义：不抛异常（deque 分配失败会 terminate，取决于分配器；队列有上限，正常不触发）。
     * - 不变量：返回 kOk 时 token 唯一且一定会有一次 OnSendComplete（或 OnDetached 清空队列）。
     */
    SendResult SendTo(ByteView data, const Endpoint& to) override {
        // step1: 未挂载直接 kInvalid，避免把数据压进一个永不 Pump 的队列。
        if (!attached_) {
            return SendResult{IoStatus::kInvalid, 0};
        }
        // step2: 队列达上限时返回 kWouldBlock，让调用方背压而不是无界增长内存。
        if (pending_.size() >= kMaxPendingSends) {
            return SendResult{IoStatus::kWouldBlock, 0};
        }
        // step3: 分配唯一 token 并入队借用视图，立即返回 kOk；真正发送由 Pump 在回调后完成。
        const std::uint64_t token = next_token_++;
        pending_.push_back(PendingDatagram{data, to, token});
        return SendResult{IoStatus::kOk, token};
    }

    std::uint64_t ScheduleAfter(std::chrono::milliseconds delay,
                                std::function<void()> callback) override {
        if (!attached_) {
            return 0;
        }
        auto slot = std::make_shared<std::uint64_t>(0);
        const std::weak_ptr<DatagramBridge> weak = weak_from_this();
        *slot = loop_.ScheduleAfter(
            delay, [weak, slot, callback = std::move(callback)]() mutable {
                const std::shared_ptr<DatagramBridge> self = weak.lock();
                if (!self) {
                    return;
                }
                self->handler_.OnTimer(*self, *slot);
                if (callback) {
                    callback();
                }
                self->Pump();
            });
        return *slot;
    }

    const Endpoint& LocalEndpoint() const override { return local_; }

private:
    /// @brief 把队列里的数据报真正发出去；回调返回后由外层调用。
    /**
     * @brief 每个 tick 把待发队列真正发出去（每 tick 有预算），剩余部分挂 0ms 定时器续发。
     *
     * @param 无（消费 pending_ 并复用 socket_/handler_）。
     *
     * ## 定位
     * 发送队列的唯一出队点，也是「回调后立即 flush」与「限流续发」的结合处。
     * 写错会让队列无限堆积（饿死其它 socket）或在 OnDetached 之后继续发送（悬垂引用）。
     *
     * @return 无返回值——发送结果通过 handler_.OnSendComplete 逐个回调；调用方**不要**等待返回值。
     *
     * ## 步骤
     * step1. 未挂载或已在 pump 中时直接返回，防止 OnSendComplete 回调里重入 Pump 造成递归；
     * step2. 置 pumping_ 标志，取得本 tick 预算 kSendBudgetPerTick；
     * step3. 逐条取出队首调用 socket_.SendTo，发送后立即回调 OnSendComplete（含本次 token 与四态）；
     * step4. 回调可能触发 Detach：每轮回调后重新检查 attached_，已摘除则直接返回（不续发）；
     * step5. 预算耗尽且仍有剩余时挂 0ms 定时器在下一 tick 续发，避免长时间占用同一 tick 饿死定时器。
     *
     * ## 并发/失败语义
     * - 调用约束：只在事件循环线程调用（OnAttached/OnDatagram/OnTimer 之后）。
     * - 原子性：单条发送与它的 OnSendComplete 之间不原子——回调内可能再次入队，但不会重入 Pump。
     * - 失败语义：不抛异常；发送失败仅通过 OnSendComplete 的 status 表达，不中断后续条目。
     * - 不变量：返回时 pumping_ 必为 false（早退路径除外，此时由外层持有）；pump 期间的 pending_ 只减不增（回调内的新增除外）。
     */
    void Pump() {
        // step1: 未挂载或已在 pump 中直接返回，防止 OnSendComplete 回调里重入导致递归。
        if (!attached_ || pumping_) {
            return;
        }
        // step2: 置重入标志并取本 tick 发送预算 kSendBudgetPerTick。
        pumping_ = true;
        std::size_t budget = kSendBudgetPerTick;
        while (budget > 0 && attached_ && !pending_.empty()) {
            --budget;
            // step3: 取出队首真正发送，并立即以本次 token 回调 OnSendComplete（成功/失败都回调）。
            const PendingDatagram item = pending_.front();
            pending_.pop_front();
            SendResult raw{};
            const IoStatus status = socket_.SendTo(item.view, item.to, &raw);
            if (!attached_) {
                return;
            }
            handler_.OnSendComplete(*this, item.token,
                                    SendResult{status, item.token});
        }
        pumping_ = false;
        if (attached_ && !pending_.empty()) {
            const std::weak_ptr<DatagramBridge> weak = weak_from_this();
            // step4: 预算耗尽仍有剩余时挂 0ms 定时器续发，避免长时间占用同一 tick 饿死定时器与其它 socket。
            loop_.ScheduleAfter(std::chrono::milliseconds(0), [weak]() {
                if (const std::shared_ptr<DatagramBridge> self = weak.lock()) {
                    self->Pump();
                }
            });
        }
    }

    EventLoop& loop_;
    DatagramSocket& socket_;
    Protocol& handler_;
    Endpoint local_;
    std::deque<PendingDatagram> pending_;
    std::uint64_t next_token_{1};
    bool attached_{false};
    bool pumping_{false};
};

/// @brief TCP 桥接器：统一 StreamProtocol 的事件循环层 ←→ 协议层。
class StreamBridge final : public StreamProtocol,
                           public ProtocolContext,
                           public std::enable_shared_from_this<StreamBridge> {
public:
    StreamBridge(EventLoop& loop, StreamSocket& socket, StreamProtocol& handler)
        : loop_(loop), socket_(socket), handler_(handler),
          local_(socket.local_endpoint()) {}

    ~StreamBridge() override { pending_.clear(); }

    StreamBridge(const StreamBridge&) = delete;
    StreamBridge& operator=(const StreamBridge&) = delete;

    void OnAttached(EventLoop& loop) override {
        (void)loop;
        if (attached_) {
            return;
        }
        attached_ = true;
        handler_.OnAttached(*this);
        Pump();
    }

    void OnData(ByteView data) override {
        if (!attached_) {
            return;
        }
        handler_.OnData(*this, data);
        Pump();
    }

    /// @brief REQ-007 在对端关闭/重置时先 OnClosed，再 Detach（Detach 会 OnDetached）。
    void OnClosed() override {
        if (!attached_) {
            return;
        }
        attached_ = false;
        pending_.clear();
        handler_.OnDetached(*this, DetachReason::kEof);
    }

    void OnDetached() override {
        if (!attached_) {
            return;
        }
        attached_ = false;
        pending_.clear();
        handler_.OnDetached(*this, DetachReason::kClosed);
    }

    SendResult SendTo(ByteView data, const Endpoint& /*to*/) override {
        if (!attached_) {
            return SendResult{IoStatus::kInvalid, 0};
        }
        if (pending_.size() >= kMaxPendingSends) {
            return SendResult{IoStatus::kWouldBlock, 0};
        }
        const std::uint64_t token = next_token_++;
        pending_.push_back(PendingStreamChunk{data, token});
        return SendResult{IoStatus::kOk, token};
    }

    std::uint64_t ScheduleAfter(std::chrono::milliseconds delay,
                                std::function<void()> callback) override {
        if (!attached_) {
            return 0;
        }
        auto slot = std::make_shared<std::uint64_t>(0);
        const std::weak_ptr<StreamBridge> weak = weak_from_this();
        *slot = loop_.ScheduleAfter(
            delay, [weak, slot, callback = std::move(callback)]() mutable {
                const std::shared_ptr<StreamBridge> self = weak.lock();
                if (!self) {
                    return;
                }
                if (callback) {
                    callback();
                }
                self->Pump();
            });
        return *slot;
    }

    const Endpoint& LocalEndpoint() const override { return local_; }

private:
    void Pump() {
        if (!attached_ || pumping_) {
            return;
        }
        pumping_ = true;
        std::size_t budget = kSendBudgetPerTick;
        while (budget > 0 && attached_ && !pending_.empty()) {
            --budget;
            const PendingStreamChunk item = pending_.front();
            pending_.pop_front();
            SendResult raw{};
            const IoStatus status = socket_.Send(item.view, &raw);
            if (!attached_) {
                return;
            }
            // StreamProtocol 是最小集（OnData/OnDetach），发送失败直接上报 detach。
            if (status == IoStatus::kClosed || status == IoStatus::kReset ||
                status == IoStatus::kIoError) {
                attached_ = false;
                pending_.clear();
                handler_.OnDetached(*this, status == IoStatus::kReset
                                                ? DetachReason::kReset
                                                : (status == IoStatus::kClosed
                                                       ? DetachReason::kEof
                                                       : DetachReason::kError));
                return;
            }
        }
        pumping_ = false;
        if (attached_ && !pending_.empty()) {
            const std::weak_ptr<StreamBridge> weak = weak_from_this();
            loop_.ScheduleAfter(std::chrono::milliseconds(0), [weak]() {
                if (const std::shared_ptr<StreamBridge> self = weak.lock()) {
                    self->Pump();
                }
            });
        }
    }

    EventLoop& loop_;
    StreamSocket& socket_;
    StreamProtocol& handler_;
    Endpoint local_;
    std::deque<PendingStreamChunk> pending_;
    std::uint64_t next_token_{1};
    bool attached_{false};
    bool pumping_{false};
};

class DatagramAttachment final : public ProtocolAttachment {
public:
    DatagramAttachment(EventLoop& loop, DatagramSocket& socket,
                       std::shared_ptr<DatagramBridge> bridge)
        : loop_(loop), socket_(socket), bridge_(std::move(bridge)) {}

    ~DatagramAttachment() override {
        // 若 loop 已先摘除该 socket，Detach 返回 false，不会二次回调。
        loop_.Detach(socket_);
        bridge_.reset();
    }

private:
    EventLoop& loop_;
    DatagramSocket& socket_;
    std::shared_ptr<DatagramBridge> bridge_;
};

class StreamAttachment final : public ProtocolAttachment {
public:
    StreamAttachment(EventLoop& loop, StreamSocket& socket,
                     std::shared_ptr<StreamBridge> bridge)
        : loop_(loop), socket_(socket), bridge_(std::move(bridge)) {}

    ~StreamAttachment() override {
        loop_.Detach(socket_);
        bridge_.reset();
    }

private:
    EventLoop& loop_;
    StreamSocket& socket_;
    std::shared_ptr<StreamBridge> bridge_;
};

}  // namespace

const char* DetachReasonName(DetachReason reason) noexcept {
    switch (reason) {
        case DetachReason::kEof:
            return "kEof";
        case DetachReason::kReset:
            return "kReset";
        case DetachReason::kTimeout:
            return "kTimeout";
        case DetachReason::kError:
            return "kError";
        case DetachReason::kClosed:
            return "kClosed";
    }
    return "Unknown";
}

std::unique_ptr<ProtocolAttachment> AttachProtocol(EventLoop& loop,
                                                   DatagramSocket& socket,
                                                   Protocol& handler) {
    auto bridge = std::make_shared<DatagramBridge>(loop, socket, handler);
    if (!loop.AttachDatagram(socket, bridge.get())) {
        return nullptr;
    }
    return std::make_unique<DatagramAttachment>(loop, socket, std::move(bridge));
}

std::unique_ptr<ProtocolAttachment> AttachProtocol(EventLoop& loop,
                                                   StreamSocket& socket,
                                                   StreamProtocol& handler) {
    auto bridge = std::make_shared<StreamBridge>(loop, socket, handler);
    if (!loop.AttachStream(socket, bridge.get())) {
        return nullptr;
    }
    return std::make_unique<StreamAttachment>(loop, socket, std::move(bridge));
}

}  // namespace rd::net
