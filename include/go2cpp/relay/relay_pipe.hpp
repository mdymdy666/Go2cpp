/**
 * @file relay_pipe.hpp
 * @brief 通用双向字节中继：有界队列 + 背压 + 写 deadline + 慢 peer 隔离。
 *
 * @details 本模块是 JinShanProject REQ-002 的 C++ 落地，对应 Go 侧：
 *
 * | Go（JinShan 需求） | 本模块 |
 * |---|---|
 * | `io.ReadWriteCloser` | `relay::Stream`（`streams::Reader` + `Writer` + `Closer`） |
 * | `PipeConfig{CopyBuf, QueueCap, WriteTimeout}` | 同名 `PipeConfig`（默认 32 KiB / 256 KiB / 30 s） |
 * | `PipeStats{AtoB, BtoA, Dropped, Timeouts}` | 同名 `PipeStats`（`a_to_b` 等） |
 * | `NewRelayPipe(a, b, cfg)` | `RelayPipe(Stream* a, Stream* b, PipeConfig cfg)` |
 * | `Run(ctx context.Context) error` | `bool Run(const ContextPtr& ctx, ErrorPtr* err)` |
 * | `Stats() PipeStats` | `PipeStats Stats() const` |
 *
 * 只搬字节：**不解析、不重排、不改写**。配对/路由/协议语义由调用方负责，
 * 本模块只管理一条已经配对好的双向连接。
 *
 * ## 线程模型
 *
 * 每条方向（A→B、B→A）各有 **读线程 + 写线程**，共 4 条工作线程；另有一条
 * 低频看门狗线程负责写 deadline。方向之间只共享统计原子量与取消标志，
 * 队列、错误与生命周期完全独立——一条方向卡住不会阻塞另一条方向的读写。
 *
 * ## 直通模式（`PipeConfig::direct` / `GO2CPP_RELAY_DIRECT`）
 *
 * 开启后每方向只有 **一条** 泵线程，不建队列、不建写线程：
 *
 * ```
 *   A --Read--> [pump] --Write--> B      A <--Write-- [pump] <--Read-- B
 * ```
 *
 * 消掉每 chunk 的入队/出队（2 次互斥 + 2 次条件变量唤醒）与一次线程接力；
 * 读缓冲直接作为写缓冲，少 1 次 chunk 分配与 1 次拷贝。两个方向仍是独立
 * 线程，因此「一端慢拖垮另一端」不会发生；背压来自写阻塞本身（读不快于写），
 * 慢 peer 仍按 `write_timeout` 判定并由看门狗只关该方向的目的端。
 *
 * ```
 *   A --Read--> [reader] --> bounded queue --> [writer] --Write--> B
 *   A <--Write-- [writer] <-- bounded queue <-- [reader] <--Read--  B
 * ```
 *
 * ## 队列与背压
 *
 * 队列按**字节数**计容量（`queue_cap`），只保存已读出的 chunk。队列满时读线程
 * **阻塞等待**，绝不继续读、绝不无界增长。慢 peer 的判据是「队列持续满，
 * 且一个队列容量的数据在 `write_timeout` 内都没被排空」——此时关闭目的端、
 * 该方向失败、`timeouts + 1`，但**不主动关闭另一条方向的源端**。
 *
 * 为什么不是「单次等待超时」：一个每 30 ms 只读 4 KiB 的 peer 会让每次等待
 * 都在 30 ms 内成功，单次永远不超时。为什么还要看「排空一个容量」：健康的
 * 低速链路（如 100 Mbps）也会让队列长期处于满状态，但它在预算内能持续排空，
 * 不应被误杀；只有排水速度低于「一个队列 / `write_timeout`」才判定为慢 peer。
 *
 * `copy_buf` 同时是单次 `Read` 的上限；当 `queue_cap < copy_buf` 时，构造器把
 * 有效容量提升到 `copy_buf`，保证一个 chunk 至少能入队，同时仍然有界。
 *
 * ## 写超时
 *
 * 每个 chunk 有一个绝对 deadline（开始写该 chunk 的时刻 + `write_timeout`）。
 * 每次写调用前后由看门狗检查；一旦超时，看门狗关闭**目的端 Stream**以打断
 * 可能阻塞的 `Write`，该方向失败并计入 `timeouts`。`Stream::Close` 必须满足
 * 两条契约（见下），否则阻塞中的 `Read`/`Write` 无法被唤醒。
 *
 * ## Stream 契约
 *
 * `Stream` 是纯抽象接口，不拥有 fd、不假设 socket：
 *
 * - `Read(buffer, size)`：阻塞直到至少读到 1 字节、遇到 EOF（`streams::IoEof()`）
 *   或出错；允许 `count > 0` 同时带错误（Go 契约）。
 * - `Write(data, size)`：允许短写（返回 `count < size` 且无错误），实现内部
 *   循环写完；短写不会丢数据。
 * - `Close()`：**幂等**，且必须能从其它线程调用；调用后阻塞中的 `Read`/`Write`
 *   必须尽快返回错误。`Close` 自身不得无限阻塞。
 *
 * 这三条是 RelayPipe 能在超时/取消时回收线程的前提。真实 socket 适配层应先用
 * `shutdown(fd, SHUT_RDWR)` 再 `close`，避免 Linux 上 `close` 不打断阻塞 `write`。
 *
 * ## 所有权与生命周期
 *
 * `RelayPipe` **不拥有** `a` / `b`，只借用指针；调用方必须保证二者在 `Run`
 * 返回前存活。正常结束时本模块不关闭任何一端（半关闭语义由调用方决定）；
 * 只有以下情况会关闭 Stream：
 *
 * 1. 某方向写超时/队列停滞 → 只关闭该方向的**目的端**（慢 peer）；
 * 2. 某方向失败且读线程正阻塞在 `Read` 中 → 关闭该方向的**源端**以收回线程；
 * 3. `ctx` 取消 → 强制关闭两端（否则无法打断阻塞 I/O）。
 *
 * `Run` 只允许调用一次（并发或重复调用返回错误），返回后所有工作线程已回收。
 *
 * ## 统计口径
 *
 * - `a_to_b` / `b_to_a`：已成功写入目的端的字节数（短写按实际 count 累加）；
 * - `dropped`：已从源端读出、但因超时/错误/取消而未写出的字节数（含队列里
 *   被丢弃的 chunk）；
 * - `timeouts`：deadline 事件数（写超时或队列停滞），每个方向最多计一次。
 *
 * ## 最小用法
 *
 * ```cpp
 * class SocketStream final : public go2cpp::relay::Stream {
 *  public:
 *   ReadResult Read(char* p, std::size_t n) override { ... }
 *   WriteResult Write(const char* p, std::size_t n) override { ... }
 *   ErrorPtr Close() override { ... }
 * };
 *
 * SocketStream a(...), b(...);
 * go2cpp::relay::RelayPipe pipe(&a, &b, go2cpp::relay::PipeConfig{});
 * go2cpp::ErrorPtr err;
 * if (!pipe.Run(go2cpp::Context::Background(), &err)) {
 *   // err 是第一条失败的描述；pipe.Stats() 给出计数
 * }
 * ```
 *
 * @note 与 Go 版签名的一处 C++ 适配：Go 的 `error` 返回值在本项目里是
 *       `ErrorPtr`（`shared_ptr<const Error>`），因此输出参数用 `ErrorPtr*`；
 *       上下文用 `ContextPtr` 而不是裸 `Context*`（`Context` 不可拷贝、由
 *       `ContextPtr` 管理生命周期）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/streams.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace go2cpp {
namespace relay {

/**
 * @brief 双向字节流接口：Read / Write / Close 三件套。
 *
 * @details 直接继承 `streams::Reader` / `Writer` / `Closer`，因此任何已经实现
 *          这三个接口的类型都可以零成本适配；内存管道、socket、TLS 流都只需
 *          实现这三个方法。三个方法都可能被本模块的不同线程并发调用
 *          （一个方向 Read、另一个方向 Write），实现必须自行保证线程安全，
 *          或由适配层加锁。
 */
class Stream : public streams::Reader, public streams::Writer, public streams::Closer {
public:
    ~Stream() override = default;
};

/**
 * @brief RelayPipe 的可调参数。
 *
 * @details 零值会在构造时归一化为默认值：`copy_buf == 0` → 32 KiB，
 *          `queue_cap == 0` → 256 KiB，`write_timeout <= 0` → 30 s。
 *          `queue_cap` 小于 `copy_buf` 时会被提升到 `copy_buf`。
 */
struct PipeConfig {
    /// 单次 Read 缓冲大小，也是队列里单个 chunk 的上限；默认 32 KiB。
    std::size_t copy_buf = 32U * 1024U;
    /// 队列容量（按字节计）；默认 256 KiB。满时读线程阻塞（背压）。
    std::size_t queue_cap = 256U * 1024U;
    /// 单个 chunk 的写 deadline，同时作为队列停滞预算；默认 30 s。
    std::chrono::milliseconds write_timeout{30000};
    /**
     * @brief 直通模式：每个方向只保留一条线程「读到就写」，不建队列、不建写线程。
     *
     * @details 默认 `false`（走「读线程 → 有界队列 → 写线程」）。置 `true` 时
     *          每方向只有一条泵线程：`Read` 到的字节直接 `Write` 到目的端，
     *          省掉每 chunk 一次入队/出队与一次线程接力（以及 chunk 的堆分配
     *          与拷贝）。背压由「写阻塞」天然提供：读线程读不快于目的端的
     *          写出速度；慢 peer 仍由 write deadline + 看门狗隔离——单次写
     *          超过 `write_timeout` 只关闭该方向的**目的端**，另一方向不受影响。
     *
     *          环境变量 `GO2CPP_RELAY_DIRECT=1|true|on|yes`（大小写不敏感）
     *          可在未显式置位本字段时打开直通；字段为 `true` 时优先。
     *          默认关，便于 A/B 对比。
     */
    bool direct = false;
};

/// 运行期累计统计；`Stats()` 返回的是某一时刻的快照。
struct PipeStats {
    /// A → B 成功写出的字节数。
    std::uint64_t a_to_b = 0;
    /// B → A 成功写出的字节数。
    std::uint64_t b_to_a = 0;
    /// 已读出但未写出的字节数（超时/错误/取消导致的丢弃）。
    std::uint64_t dropped = 0;
    /// deadline 事件数（写超时 + 队列停滞），每个方向最多计一次。
    std::uint64_t timeouts = 0;
};

/**
 * @brief 写超时/队列停滞的哨兵错误。
 *
 * @details 实现返回的错误是 `Wrap(ErrTimeout(), "relay: a->b write")` 之类的
 *          包装值，因此 `Is(err, ErrTimeout())` 可以跨包装层识别超时；
 *          直接比较文本是脆弱的，测试与调用方应使用 `Is`。
 *
 * @note 与项目里其它哨兵错误一样故意泄漏，避免静态析构顺序影响身份比较。
 */
inline ErrorPtr ErrTimeout() {
    static const ErrorPtr* const kError = new ErrorPtr(NewError("relay: timeout"));
    return *kError;
}

namespace detail {
/// 实现状态；定义在 `src/relay/relay_pipe.cpp`，对外保持不透明。
struct RelayPipeState;
}  // namespace detail

/**
 * @brief 双向字节中继。
 *
 * @details 构造后调用一次 `Run()`，期间两个方向并发搬运字节；`Run()` 返回时
 *          所有内部线程都已回收。`Stats()` 可在运行期任意时刻调用（原子读）。
 *
 * ## 不变量
 * - 队列容量始终受 `queue_cap` 约束；
 * - 两个方向各自失败、互不取消；只有 `ctx` 取消才强制关闭两端；
 * - 正常 EOF 不关闭任何 Stream；
 * - `Run()` 至多成功进入一次。
 */
class RelayPipe final {
public:
    /**
     * @brief 绑定两端并归一化配置。
     * @param a 端点 A（不拥有；必须存活到 Run 返回）。
     * @param b 端点 B（不拥有；必须存活到 Run 返回）。
     * @param cfg 参数；零值按 `PipeConfig` 注释归一化。
     */
    RelayPipe(Stream* a, Stream* b, PipeConfig cfg);

    /// 析构；若 `Run()` 未调用则不启动任何线程。调用方须保证 Run 已返回。
    ~RelayPipe();

    RelayPipe(const RelayPipe&) = delete;
    RelayPipe& operator=(const RelayPipe&) = delete;
    RelayPipe(RelayPipe&&) = delete;
    RelayPipe& operator=(RelayPipe&&) = delete;

    /**
     * @brief 双向转发，直到两端 EOF、任一方向失败或 `ctx` 取消。
     *
     * @param ctx 取消上下文；空指针等价于 `Context::Background()`。
     * @param err 输出第一条失败；成功时为 null。允许传 nullptr。
     * @return 两个方向都干净 EOF 时 true；否则 false。
     *
     * @note 阻塞调用，直到两个方向都结束（或取消后强制关闭两端并回收线程）。
     * @note 重复/并发调用返回 false，`*err` 为 "relay: Run already called"。
     */
    bool Run(const ContextPtr& ctx, ErrorPtr* err);

    /// 便捷重载：使用永不取消的 Background 上下文。
    bool Run(ErrorPtr* err) { return Run(ContextPtr(), err); }

    /// 返回统计快照（原子读，允许在 Run 运行期调用）。
    PipeStats Stats() const;

    /// 返回归一化后的配置（便于诊断/测试）。
    const PipeConfig& Config() const noexcept;

private:
    std::shared_ptr<detail::RelayPipeState> m_state;
};

}  // namespace relay
}  // namespace go2cpp
