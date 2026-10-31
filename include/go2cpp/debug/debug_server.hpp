#pragma once

/**
 * @file debug_server.hpp
 * @brief 只读调试服务：进程内 dump 注册表 + Unix domain socket 服务端。
 *
 * ## 定位
 *
 * 供外部 CLI（另一个独立进程）通过 `/tmp/go2cpp-<pid>.sock` 读取运行时快照。
 * 服务端**只读**：命令不会改变调度器/Channel/锁/IO 状态。
 *
 * ## 协议（GO2CPP-DEBUG/1，稳定）
 *
 * 客户端发送一行 UTF-8 命令（`\n` 结束，最长 4096 字节）：
 *
 * ```text
 * PING                      -> pong
 * LIST                      -> 组件清单（可用/不可用 + 描述）
 * STATUS                    -> pid/uptime/路径/组件数/构建开关
 * DUMP <component>|ALL      -> text（key=value，schema 见 debug_dump.hpp）
 * TEXT <component>|ALL      -> 同 DUMP
 * JSON <component>|ALL      -> JSON 对象
 * EVENTS [limit]            -> 事件环（text）
 * SUBSCRIBE <topic> [interval_ms] [max_events]
 *                           -> 增量推送（见下）
 * HELP                      -> 命令列表
 * ```
 *
 * ## SUBSCRIBE（增量推送，GO2CPP-DEBUG-PUSH/1）
 *
 * `topic` 目前只支持 `events`（别名 `event` / `events:all` / `*`）：
 *
 * ```text
 * C: SUBSCRIBE events 20 128
 * S: GO2CPP-DEBUG/1 OK <n>\n<subscription header>
 * S: GO2CPP-DEBUG/1 PUSH <n>\n<push payload>      (0..N 帧，事件产生后推送)
 * S: GO2CPP-DEBUG/1 PUSH <n>\n<push payload>
 * ```
 *
 * 语义与背压：
 * - 订阅从**当前最新事件**开始（`cursor_start` = 时间戳，辅以
 *   `cursor_start_thread_id` / `cursor_start_seq`），只推送之后产生的新事件；
 * - 服务端在独立线程里按 `interval_ms`（默认 20）轮询事件环，用
 *   `EventRing::ReadSince(EventCursor, max_events)` 取增量，**不阻塞**事件记录方；
 * - 推送帧最大 `max_events` 条（默认 256），保证单帧有界；
 * - 客户端读得慢时，发送使用 `MSG_DONTWAIT`：整批发送失败即**丢弃最旧**的
 *   这一批并推进游标，后续 push 里的 `dropped_total` / `dropped_backpressure`
 *   如实报告丢弃量（不假装无损）；
 * - 客户端关闭连接 / 服务端 `Stop()` 时订阅线程在 `interval_ms` 内退出。
 *
 * push payload（text）：
 *
 * ```text
 * schema=go2cpp.dump.events.push/1
 * version=1
 * first_seq=... last_seq=... event_count=...
 * dropped_total=... dropped_gap=... dropped_backpressure=...
 * seq_last=... cursor=...
 * # seq ts_ns tid code gid arg2 pid
 * ...
 * ```
 *
 * 响应：
 *
 * ```text
 * GO2CPP-DEBUG/1 OK <payload_bytes>\n<payload>
 * GO2CPP-DEBUG/1 ERR <code> <message>\n
 * ```
 *
 * `payload_bytes` 是 `<payload>` 的**精确字节数**（UTF-8），客户端先读头行
 * 再按字节数读 payload。错误码固定：`bad-command` / `unknown-component` /
 * `disabled` / `internal`。
 *
 * ## 编译开关
 *
 * `GO2CPP_DEBUG_DUMP=ON` 才真正监听 socket；OFF 构建 `Start()` 返回 false、
 * `last_error()=="debug-dump-disabled"`，但注册表与进程内 `Dump()` 仍可用
 * （组件 dump 本身不依赖宏，只有事件环记录依赖）。
 *
 * ## 生命周期
 *
 * 注册的组件 provider 只保存指针/weak_ptr，**不延长被观测对象生命周期**；
 * 调用方必须保证对象活到 `Unregister` 或进程结束。服务端单例在进程退出时
 * 不需要显式 Stop（但是测试/嵌入方应显式调用）。
 */

#include "go2cpp/debug/debug_dump.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp::debug {

/// 组件 provider：返回一份双格式 payload；不得抛出（异常会被隔离）。
using DebugProvider = std::function<DumpPayload()>;

/// 协议版本字符串；CLI 必须与此匹配。
inline constexpr const char* kDebugProtocol = "GO2CPP-DEBUG/1";

class DebugServer final {
public:
    /// 进程内唯一实例（堆上常驻，避免静态析构顺序问题）。
    static DebugServer& Instance();

    ~DebugServer();
    DebugServer(const DebugServer&) = delete;
    DebugServer& operator=(const DebugServer&) = delete;

    /**
     * @brief 开始监听 Unix domain socket。
     * @param path 空表示 `DefaultPath()`（`/tmp/go2cpp-<pid>.sock`）。
     * @return 已运行或本次启动成功返回 true；OFF 构建/绑定失败返回 false。
     */
    bool Start(const std::string& path = {});
    /// 停止监听并删除 socket 文件（可重复调用，线程安全）。
    void Stop();

    bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }
    std::string path() const;
    std::string last_error() const;

    /// 注册/覆盖组件（线程安全）。name 为空或 provider 为空返回 false。
    bool Register(const std::string& name, const std::string& description,
                  DebugProvider provider);
    /// 移除组件；返回是否存在。
    bool Unregister(const std::string& name);
    /// 当前已注册组件名（字典序）。
    std::vector<std::string> Components() const;

    /// 聚合 dump；component 为 "all"/"*" 时返回全部已注册组件。
    DumpPayload Dump(const std::string& component) const;

    /// 处理一条文本命令，返回**完整响应**（含 `GO2CPP-DEBUG/1 ...` 头）。
    /// @note `SUBSCRIBE` 需要长连接，进程内调用返回 `bad-command`；
    ///       请通过 socket 使用（见文件头的 SUBSCRIBE 章节）。
    std::string HandleCommand(const std::string& line);
    /**
     * @brief 处理一行 JSON 请求（CLI 占位协议兼容层）。
     * @details 请求 `{"id":1,"cmd":"status"}` / `{"cmd":"dump",...}`；
     *          响应一行 JSON `{"id":1,"ok":true,"data":{...}}\n`。
     *          与文本协议并存：socket 首字符为 `{` 时走本入口。
     */
    std::string HandleJsonCommand(const std::string& line);

    /// 默认 socket 路径 `/tmp/go2cpp-<pid>.sock`。
    static std::string DefaultPath();

private:
    DebugServer();
    void Run() noexcept;
    void HandleClient(int client_fd) noexcept;
    /// 在独立线程里为一条 SUBSCRIBE 连接推送增量；fd 由本函数负责关闭。
    void ServeSubscription(int fd, const std::string& argument) noexcept;
    void InstallBuiltins();

    mutable std::mutex m_mutex;
    std::map<std::string, std::pair<std::string, DebugProvider>> m_components;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_path_mutex;
    std::string m_path;
    mutable std::mutex m_error_mutex;
    std::string m_last_error;
    std::thread m_thread;
    int m_listen_fd{-1};
    /// 当前活跃的订阅连接数（STATUS 里可见；用于测试与运维观测）。
    std::atomic<std::uint64_t> m_subscribers{0};
    std::uint64_t m_started_ns{0};
    std::atomic<std::uint64_t> m_requests{0};
    /// 最近一条 accept 连接的 TCP_NODELAY 状态：-1 不适用/未接受，0 关闭，1 开启。
    std::atomic<int> m_last_accept_nodelay{-1};
};

// ---------------------------------------------------------------------------
// 便捷注册（只保存指针/weak_ptr，不拥有被观测对象）
// ---------------------------------------------------------------------------

bool RegisterScheduler(const std::string& name, Scheduler& scheduler);
bool RegisterIOManager(const std::string& name, io::IOManager& manager);
bool RegisterMutex(const std::string& name, sync::Mutex& mutex);
bool RegisterConditionVariable(const std::string& name,
                               sync::ConditionVariable& condition);
bool RegisterParkingCondition(const std::string& name,
                              core::ParkingCondition& condition);
bool RegisterFiber(const std::string& name, Fiber& fiber);
bool RegisterEventRing(const std::string& name = kComponentEvents);
/// 注册 TimerService 的只读 dump；被观测服务必须活到 Unregister。
bool RegisterTimer(const std::string& name, core::TimerService& service);

/**
 * @brief 注册一个 Channel<T> 的 dump provider。
 * @note 保存 weak_ptr：Channel 销毁后该组件返回 `unavailable reason=destroyed`，
 *       不会访问已释放内存。
 */
template <typename T>
bool RegisterChannel(const std::string& name,
                     std::shared_ptr<Channel<T>> channel) {
    if (!channel) {
        return false;
    }
    std::weak_ptr<Channel<T>> weak = channel;
    return DebugServer::Instance().Register(
        name, "Channel<T> buffer/waiters/select registrations",
        [weak]() -> DumpPayload {
            const auto locked = weak.lock();
            if (!locked) {
                return DumpPayload::Unavailable("destroyed");
            }
            return DumpChannel(locked->DebugSnapshot());
        });
}

}  // namespace go2cpp::debug
