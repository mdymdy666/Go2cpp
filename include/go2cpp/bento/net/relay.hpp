#pragma once

/**
 * @file relay.hpp
 * @brief 通用字节中继（JinShan MOD-15）：在两条 `ITransport` 之间做带背压的
 *        双向字节拷贝，并处理慢客户端、半关闭、空闲/连接超时与统计。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 本文件是 `ITransport`（MOD-07）之上的「搬运层」：它不解析任何应用
 *          协议，只把 `src` 读到的字节原样写到 `dst`，同时把 `dst` 读到的字节
 *          原样写回 `src`。典型用法是中继/端口转发/网关：一条入站连接与一条
 *          出站连接都满足 `ITransport`，中继负责在两者之间搬运。
 *
 * ## 与 `TcpProxy` 的关系
 *
 * `TcpProxy` 是「监听 → accept → 拨号 → 双向转发」的一体化工具，直接操作
 * fd 并用 `poll` 驱动；`Relay` 是**单连接**、**只依赖 `ITransport`** 的可复用
 * 原语，不碰 socket、线程或事件循环。两者的背压、半关闭与超时语义刻意保持
 * 一致（见 `src/bento/net/tcp_proxy.cpp` 的 `PumpDirection` /
 * `ServiceConnection`），差别只在驱动方式：`TcpProxy` 用 `poll` 等待就绪，
 * `Relay` 用 `ITransport` 自带的 deadline 读写做切片轮转。
 *
 * ## 为什么是单线程阻塞循环
 *
 * `ITransport` 的契约是「每次调用自带 deadline 的阻塞读写」，没有暴露 fd，
 * 也没有 `poll` 钩子。因此中继只能在**一个线程内**交替调用两条连接的
 * `Read`/`Write`，靠较短的 slice 轮转保证两个方向都能推进。这适合一条连接
 * 一个中继线程的用法（与 `TcpProxy` 的连接级并发等价），不适合在单线程里
 * 驱动上万条连接——那种场景应由更底层的 `poll`/`epoll` 驱动实现负责。
 * 注意每条**空闲**连接每个 `poll_interval_ms` 仍会唤醒一次：1000 条空闲连接、
 * `poll_interval_ms=1` 在 12 核 WSL2 上实测约 5–6 核 CPU；调到 5ms 可降到约
 * 1.6–2.7 核（复测随负载波动），代价是转发延迟上界变大。连接数上千时应调大
 * 该值或改用事件驱动实现。
 *
 * ## 有界缓冲与背压（核心约束）
 *
 * 每个方向各有一个**固定容量**的 pending 缓冲（默认 64KB，两个方向合计
 * 128KB/relay，在 `Run` 开始时一次性分配，不随流量增长）：
 *
 *   - 读到但还没写出的字节留在 pending 里；
 *   - pending 未满时继续读，让链路尽量跑满；
 *   - pending 满时**停止从源读**，只向目标写——这就是端到端背压。
 *
 * 于是慢客户端最多让中继多占 `buffer_bytes` 字节，而不是把源端的数据无限
 * 吸进内存。测试里用 `peak_pending_*` 与「读入量 - 写出量」两条断言验证
 * 这个上限。
 *
 * ## 慢客户端
 *
 * 目标端长时间一个字节都写不出去时，继续等下去只会占着连接。`Relay` 记录
 * **连续无写进展**的时长：一旦 pending 里还有数据、写调用持续没有写出任何
 * 字节，且持续时间达到 `write_timeout_ms`，就断开并返回
 * `RelayCloseReason::kSlowConsumer`。注意判据是「无进展」而不是「某一次
 * `Write` 超时」：只要还有字节写出去，计时就重置，因此**慢但还能动**的客户端
 * 不会被误杀。`write_timeout_ms == 0` 关闭该保护。
 *
 * ## 半关闭传播（为什么不能读到 EOF 就 Close）
 *
 * TCP 是全双工：客户端写完请求后发 FIN，服务端往往还要回一大段响应。若中继
 * 读到一侧 EOF 就关闭整条连接，响应会被截断。正确顺序是：
 *
 *   1. 读到 EOF 只把该方向标记为结束；
 *   2. 等该方向的 pending **全部写出去**；
 *   3. 再对下游调用 `ShutdownWrite()`（发 FIN，只关写方向）；
 *   4. 反方向继续搬运，直到它也 EOF 并且 pending 排空。
 *
 * 两个方向都结束后 `Run` 才返回 `kCompleted`，并在返回前 `Close()` 两条
 * 传输（半关闭只是写方向，完整释放仍由 `Close` 负责）。
 *
 * ## 超时
 *
 * - **空闲超时**：双向都没有字节流动累计达到 `idle_timeout_ms` 就结束
 *   （`kIdleTimeout`）。半关闭本身不算活动，所以「一端 FIN 后对端一直不说话」
 *   也会被回收，不会永久占用线程。
 * - **连接超时**：只在 `ConnectAndRun` 里使用；`Run` 接收的两条传输必须已经
 *   连接好。`ConnectAndRun` 把 `dst.Connect` 的 deadline 设为
 *   `connect_timeout_ms`，超时返回 `kConnectTimeout`，其它失败返回
 *   `kConnectFailed`。
 *
 * ## 线程模型
 *
 * `Run`/`ConnectAndRun` 是**阻塞调用**，由调用线程独占 `src`/`dst` 直到返回；
 * 中继内部不创建线程、不加锁。`Relay` 自身的统计成员不是线程安全的，同一个
 * 实例不要跨线程并发调用。需要并发中继多条连接时，每条连接一个 `Relay`
 * 实例（统计按实例累计）。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::net {

/**
 * @brief 中继结束的原因。
 *
 * @details 每个原因都对应一个可观测的统计字段，便于上层告警而不是只看日志。
 *          `kCompleted` 是唯一的正常结束；其余都意味着中继主动断开。
 */
enum class RelayCloseReason : std::uint8_t {
    kCompleted = 0,     ///< 双向都 EOF 且 pending 已排空（正常结束）。
    kSlowConsumer = 1,  ///< 目标端连续 `write_timeout_ms` 写不出任何字节。
    kIdleTimeout = 2,   ///< 双向连续 `idle_timeout_ms` 没有字节流动。
    kConnectTimeout = 3,  ///< `ConnectAndRun` 拨号超时。
    kConnectFailed = 4,   ///< `ConnectAndRun` 拨号失败（拒绝/解析失败等）。
    kReadError = 5,       ///< 某个方向 `Read` 返回不可恢复错误。
    kWriteError = 6,      ///< `Write` 失败/对端不再接收（EPIPE/RST），或 `ShutdownWrite` 返回 kError。
};

/// 返回结束原因的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* RelayCloseReasonName(RelayCloseReason reason) noexcept;

/**
 * @brief 中继配置。
 *
 * @details 所有时间字段单位均为毫秒。`0` 的含义逐字段说明：对超时类字段，
 *          `0` 表示**关闭该超时**（不因它主动断开）；对切片类字段，`0` 表示
 *          **非阻塞调用一次**（配合 `poll_interval_ms` 休眠）。
 */
struct RelayOptions {
    /// 每个方向 pending 缓冲的容量上限（字节），默认 64KB。
    /// 这就是单方向的最大额外内存；缓冲满时停止从源读（背压）。
    std::size_t buffer_bytes = 64 * 1024;
    /// 单次 `Read` 最多请求的字节数；0 或超过 buffer_bytes 时按 buffer_bytes。
    std::size_t read_chunk_bytes = 16 * 1024;
    /// `Read` 调用的 deadline；0 = 非阻塞尝试一次（推荐，配合 poll_interval）。
    std::uint32_t read_slice_ms = 0;
    /// `Write` 调用的 deadline；单次写最多阻塞这么久，避免拖死另一个方向。
    std::uint32_t write_slice_ms = 5;
    /// 两个方向都没有任何进展时的休眠时长，避免空转占满 CPU；0 = 不休眠。
    std::uint32_t poll_interval_ms = 1;
    /// 连续写不出任何字节的容忍时长；达到后判定慢客户端并断开；0 = 不限制。
    std::uint32_t write_timeout_ms = 2000;
    /// 双向都没有字节流动的空闲超时；0 = 不限制。
    std::uint32_t idle_timeout_ms = 30000;
    /// `ConnectAndRun` 拨号的 deadline（透传给 `ITransport::Connect`）。
    std::uint32_t connect_timeout_ms = 3000;
};

/**
 * @brief 中继计数器（单次运行或跨运行累计）。
 *
 * @details `bytes_*` 是**成功写出**的字节数，`read_*` 是从源**读入**的字节数。
 *          正常完成时两者相等；慢客户端/写错误时 `read_*` 会大于 `bytes_*`，
 *          差值就是滞留在 pending 里、最终被丢弃的字节。`peak_*` 是运行期间
 *          观察到的高水位，用于验证有界缓冲没有被突破。
 */
struct RelayStats {
    std::uint64_t bytes_src_to_dst = 0;  ///< src → dst 方向成功写出的字节数。
    std::uint64_t bytes_dst_to_src = 0;  ///< dst → src 方向成功写出的字节数。
    std::uint64_t read_src_to_dst = 0;   ///< 从 src 读入的字节数（含未写出的）。
    std::uint64_t read_dst_to_src = 0;   ///< 从 dst 读入的字节数（含未写出的）。
    std::uint64_t peak_pending_src_to_dst = 0;  ///< src → dst pending 峰值。
    std::uint64_t peak_pending_dst_to_src = 0;  ///< dst → src pending 峰值。
    std::uint64_t peak_buffered_bytes = 0;  ///< 双向 pending 之和的峰值。
    std::uint64_t half_closes = 0;  ///< 成功传播的 FIN 次数（单向计一次；kClosed/kError 不计）。
    std::uint64_t slow_consumers = 0;       ///< 因慢客户端断开次数。
    std::uint64_t idle_timeouts = 0;        ///< 空闲超时次数。
    std::uint64_t connect_timeouts = 0;     ///< 拨号超时次数。
    std::uint64_t connect_failures = 0;     ///< 拨号失败次数。
    std::uint64_t read_errors = 0;          ///< 读错误次数。
    std::uint64_t write_errors = 0;         ///< 写错误次数。

    /// 累加另一份统计；峰值字段取最大值（不能相加），其余字段相加。
    RelayStats& operator+=(const RelayStats& other) noexcept;
};

/**
 * @brief 一次中继运行的结果。
 *
 * @details `stats` 是**本次运行**的计数（不是实例累计）；跨运行的累计由
 *          `Relay::Stats()` 提供。`elapsed_ms` 在 `ConnectAndRun` 里包含拨号
 *          耗时。
 */
struct RelayResult {
    RelayCloseReason reason = RelayCloseReason::kCompleted;  ///< 结束原因。
    RelayStats stats;              ///< 本次运行的计数快照。
    std::uint64_t elapsed_ms = 0;  ///< 从开始拨号/搬运到结束的耗时。
    int system_error = 0;          ///< 最后一次失败的 errno；0 表示没有系统错误。

    /// 是否正常结束（双向 EOF）。
    bool completed() const noexcept {
        return reason == RelayCloseReason::kCompleted;
    }
};

/**
 * @brief 通用字节中继（单连接，阻塞式）。
 *
 * @details 用法：
 * @code
 *   Relay relay;
 *   RelayOptions options;
 *   options.idle_timeout_ms = 30000;
 *   RelayResult result = relay.Run(client_side, upstream_side, options);
 *   // 或先拨号再搬运：
 *   RelayResult result = relay.ConnectAndRun(client_side, upstream, {"10.0.0.8", 3389});
 * @endcode
 *
 * @note `Run` 返回前会 `Close()` 两条传输；调用方不需要（也不应该）在返回后
 *       继续使用它们。若传输实现抛异常或缓冲分配失败（`std::bad_alloc`），
 *       `Run` 无法返回，此时**不会**执行 `Close`，调用方需自行释放两端。
 */
class Relay final {
public:
    Relay() = default;

    /**
     * @brief 在两条**已连接**的传输之间双向搬运，直到正常结束或触发断开条件。
     * @param src 入站传输（读到的字节写给 dst）。
     * @param dst 出站传输（读到的字节写回 src）。
     * @param options 本次运行的配置；默认值见 `RelayOptions`。
     * @return 结束原因与本次运行的统计。返回前两条传输都会被 `Close()`。
     */
    RelayResult Run(ITransport& src, ITransport& dst, const RelayOptions& options = RelayOptions{});

    /**
     * @brief 先拨号连接 `dst`，再执行 `Run`。
     * @param src 已连接的入站传输。
     * @param dst 尚未连接的出站传输。
     * @param destination 目标端点。
     * @param options 配置；`connect_timeout_ms` 用于拨号。
     * @return 拨号失败时返回 `kConnectTimeout`/`kConnectFailed` 并关闭两端；
     *         成功后返回 `Run` 的结果（`elapsed_ms` 含拨号耗时）。
     */
    RelayResult ConnectAndRun(ITransport& src, ITransport& dst, const Endpoint& destination,
                              const RelayOptions& options = RelayOptions{});

    /// 本实例历次 `Run`/`ConnectAndRun` 的累计统计（拷贝返回）。
    RelayStats Stats() const noexcept {
        return m_stats;
    }

private:
    /// 跨运行累计；峰值字段由 `operator+=` 取最大值。
    RelayStats m_stats;
};

}  // namespace go2cpp::bento::net
