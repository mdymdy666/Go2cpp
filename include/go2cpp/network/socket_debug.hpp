#pragma once

/**
 * @file socket_debug.hpp
 * @brief Socket 级调试账本：每 fd 计数 + 256 条事件环形缓冲。
 * @details 本文件属于「SocketDebug 最小实现」：只提供 per-fd 的计数、
 *          事件与快照，不新建 debug 视图/服务，也不改 debug 模块。
 *
 * ## 设计取舍（为什么这样写）
 * - **以 fd 为键**：最小实现直接复用 `Socket` 已有的 fd 身份，接入点不需要
 *   在 `Socket` 对象里加字段（`sizeof(Socket)` 不变）。已知局限：fd 关闭后
 *   号码会被内核复用，归档只保留「最近关闭的 256 个 fd」，一旦 fd 复用，
 *   新连接会覆盖归档中的同号旧条目。需要跨复用归属时请改造成稳定 socket_id
 *   （见 `docs/research/socket-debug-mode.md` §2 方案 D）。
 * - **编译期 OFF 零开销**：`GO2CPP_DEBUG_SOCKET` 默认 0，由 CMake 注入。
 *   OFF 时 `GO2CPP_SOCKET_DEBUG_RECORD(...)` 走 `if constexpr (false)` 被
 *   整块丢弃，`socket.cpp` 不生成任何额外代码，也不引用本文件的符号。
 * - **运行时 Disable 近零**：`Enabled()` 在头文件内联为一次 relaxed 原子读，
 *   调用点先判 `Enabled()` 再进入记录函数，Disable 状态下只剩这次读 + 分支。
 * - **事件环固定 256 条/fd**：与任务约定一致；写满后覆盖最旧，`Events(fd, n)`
 *   返回最近 n 条（按时间升序）。事件与计数都是 best-effort，不构成事务。
 *
 * ## 计数口径
 * - `send_calls` / `recv_calls`：`Socket::send/sendv/send_to` 与
 *   `recv/recvv/recv_from` 的**每次调用**计 1（partial 重试不重复计）；
 * - `bytes_sent/recv`：一次调用最终成功返回的字节数（partial 只计实际写入）；
 * - `errors`：调用最终失败（error != 0）的次数；`timeouts` 是其中
 *   `ETIMEDOUT` 的子集（两者不可相加）；
 * - `connect_time_ns`：`Socket::connect` 从进入到返回的累计纳秒数（成功与
 *   失败都计入），只累计、不取平均，避免除法与额外状态；
 * - `events`：本 fd 累计写入环形缓冲的事件条数（含已被覆盖的）。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// CMake 在 GO2CPP_DEBUG_SOCKET=ON 时注入 =1；单独编译本头文件时默认 OFF。
#ifndef GO2CPP_DEBUG_SOCKET
#define GO2CPP_DEBUG_SOCKET 0
#endif

namespace go2cpp::net {

/** @brief Socket 事件类型；数值只在进程内使用，不写盘、不跨进程。 */
enum class SocketEventKind : std::uint8_t {
    kConnect = 1,  ///< connect 成功。
    kAccept = 2,   ///< accept 成功（fd 为新建连接）。
    kRead = 3,     ///< recv 成功。
    kWrite = 4,    ///< send 成功。
    kClose = 5,    ///< close 完成，fd 已归档。
    kError = 6,    ///< 系统调用失败（error 非 0，且不是超时）。
    kTimeout = 7,  ///< 等待超时（error == ETIMEDOUT）。
};

/** @brief 一条 socket 事件；`seq` 在本 fd 内单调递增，用于排序与去重。 */
struct SocketEvent {
    std::uint64_t seq{0};         ///< 本 fd 内的事件序号，从 1 开始。
    std::uint64_t ts_ns{0};       ///< steady_clock 纳秒时间戳。
    SocketEventKind kind{SocketEventKind::kError};
    int fd{-1};                   ///< 事件归属的 fd（关闭后归档仍保留原值）。
    int error{0};                 ///< 失败时的 errno；成功为 0。
    std::uint64_t bytes{0};       ///< 本次成功收发的字节数（accept/close 为 0）。
    std::uint64_t elapsed_ns{0};  ///< 仅 connect 事件填写：连接耗时。
};

/** @brief 一个 fd 的累计账本，`Snapshot(fd)` 返回它的副本。 */
struct SocketStats {
    int fd{-1};                    ///< 归档/活条目对应的 fd；未知 fd 时为 -1。
    bool tracked{false};           ///< false 表示该 fd 从未被记录过。
    bool closed{false};            ///< true 表示已 close 并进入归档窗口。
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_recv{0};
    std::uint64_t send_calls{0};
    std::uint64_t recv_calls{0};
    std::uint64_t errors{0};       ///< 所有失败调用（含 timeouts）。
    std::uint64_t timeouts{0};     ///< 其中 ETIMEDOUT 的次数。
    std::uint64_t connects{0};     ///< connect 尝试次数（成功 + 失败）。
    std::uint64_t connect_ok{0};   ///< connect 成功次数。
    std::uint64_t accepts{0};      ///< accept 成功次数。
    std::uint64_t closes{0};       ///< close 归档次数。
    std::uint64_t connect_time_ns{0};  ///< connect 累计耗时。
    std::uint64_t events{0};       ///< 累计事件条数（含已覆盖）。
    std::uint64_t last_activity_ns{0};  ///< 最近一次记录的 steady_clock 时间。
    int last_errno{0};             ///< 最近一次失败的 errno；成功不覆盖。
};

#if GO2CPP_DEBUG_SOCKET

namespace detail {
/// 运行时开关；用 extern 原子量让 `Enabled()` 在头文件内联为一次 relaxed 读。
extern std::atomic<bool> g_socket_debug_enabled;
}  // namespace detail

/**
 * @brief Socket 调试账本（进程级单例，全部静态接口）。
 * @note 记录接口都带 noexcept：记录失败（例如注册表满）只计入 dropped，
 *       绝不向 socket 热路径抛异常。OOM 属于不可恢复错误，会 terminate。
 */
class SocketDebug final {
public:
    /// 每 fd 事件环容量（固定 256，改它需要重新编译）。
    static constexpr std::size_t kEventRingCapacity = 256;
    /// 同时跟踪的活 fd 上限；超出后新 fd 不记账，计入 dropped。
    static constexpr std::size_t kMaxTrackedSockets = 4096;
    /// 已关闭 fd 的归档窗口大小（按关闭先后，最近优先）。
    static constexpr std::size_t kMaxFinishedSockets = 256;

    /// @brief 打开运行时记录；可重复调用。
    /// @return 编译期已启用时恒为 true。
    static bool Enable() noexcept;
    /// @brief 关闭运行时记录；已有账本保留，Disable 期间新增调用不再记账。
    static void Disable() noexcept;
    /// @brief 热路径开关：一次 relaxed 原子读。
    static bool Enabled() noexcept {
        return detail::g_socket_debug_enabled.load(std::memory_order_relaxed);
    }
    /// @brief 编译期是否启用（`GO2CPP_DEBUG_SOCKET`）；OFF 构建恒 false。
    static constexpr bool Available() noexcept { return true; }

    /// @brief 取某 fd 的账本副本；未知 fd 返回默认值（`fd == -1`）。
    /// @note 已关闭但在归档窗口内的 fd 仍可查到，`closed == true`。
    static SocketStats Snapshot(int fd) noexcept;
    /// @brief 取某 fd 最近的事件（按 seq/时间升序）。
    /// @param max_events 0 表示最多 256 条；大于 256 时按 256 截断。
    static std::vector<SocketEvent> Events(int fd, std::size_t max_events = 0);
    /// @brief 把当前账本渲染成多行文本（dump 路径允许分配）。
    static void DumpTo(std::string& out);
    /// @brief `DumpTo` 的返回值版本，便于测试与日志直接使用。
    static std::string DumpToString();
    /// @brief 当前活条目数量（不含归档）。
    static std::size_t TrackedCount() noexcept;
    /// @brief 清空所有计数、事件与归档，但不改变 Enabled 状态（测试/复用入口）。
    static void Reset() noexcept;

    // ---- 内部记账接口：只允许 src/network/socket.cpp 调用 ----
    /// @brief accept 成功：登记新连接 fd 并记一条 kAccept。
    static void RecordAccept(int fd) noexcept;
    /// @brief connect 返回：`error == 0` 记成功，否则按 errno 记 kError/kTimeout。
    static void RecordConnect(int fd, int error, std::uint64_t elapsed_ns) noexcept;
    /// @brief recv 返回：`bytes` 为本次实际收到的字节数。
    static void RecordRead(int fd, std::uint64_t bytes, int error) noexcept;
    /// @brief send 返回：`bytes` 为本次实际发出的字节数。
    static void RecordWrite(int fd, std::uint64_t bytes, int error) noexcept;
    /// @brief close 完成：活条目转入归档窗口并记一条 kClose。
    static void RecordClose(int fd) noexcept;
};

/**
 * @brief 热路径记录宏：OFF 构建整块丢弃，ON+Disable 时只花一次 relaxed 读。
 * @note `statement` 必须是一条以分号结尾的语句；宏内部不额外求值参数。
 */
#define GO2CPP_SOCKET_DEBUG_RECORD(statement)                     \
    do {                                                          \
        if constexpr (::go2cpp::net::SocketDebug::Available()) {  \
            if (::go2cpp::net::SocketDebug::Enabled()) {          \
                statement;                                        \
            }                                                     \
        }                                                         \
    } while (false)

#else  // GO2CPP_DEBUG_SOCKET == 0

/**
 * @brief 编译期关闭时的空实现。
 * @details 保留同名类型与接口，使调用方无需在业务代码里到处 `#if`；
 *          `GO2CPP_SOCKET_DEBUG_RECORD` 会因为 `Available() == false` 被
 *          `if constexpr` 整块丢弃，因此这些 inline 空函数不会进入生成的代码。
 */
class SocketDebug final {
public:
    static constexpr std::size_t kEventRingCapacity = 256;
    static constexpr std::size_t kMaxTrackedSockets = 4096;
    static constexpr std::size_t kMaxFinishedSockets = 256;

    static bool Enable() noexcept { return false; }
    static void Disable() noexcept {}
    static bool Enabled() noexcept { return false; }
    static constexpr bool Available() noexcept { return false; }

    static SocketStats Snapshot(int) noexcept { return {}; }
    static std::vector<SocketEvent> Events(int, std::size_t = 0) { return {}; }
    static void DumpTo(std::string& out) {
        out.assign("socket_debug: unavailable (GO2CPP_DEBUG_SOCKET=OFF)\n");
    }
    static std::string DumpToString() {
        return "socket_debug: unavailable (GO2CPP_DEBUG_SOCKET=OFF)\n";
    }
    static std::size_t TrackedCount() noexcept { return 0; }
    static void Reset() noexcept {}

    static void RecordAccept(int) noexcept {}
    static void RecordConnect(int, int, std::uint64_t) noexcept {}
    static void RecordRead(int, std::uint64_t, int) noexcept {}
    static void RecordWrite(int, std::uint64_t, int) noexcept {}
    static void RecordClose(int) noexcept {}
};

#define GO2CPP_SOCKET_DEBUG_RECORD(statement) \
    do {                                      \
        if constexpr (false) {                \
            statement;                        \
        }                                     \
    } while (false)

#endif  // GO2CPP_DEBUG_SOCKET

}  // namespace go2cpp::net
