#pragma once

/**
 * @file transport.hpp
 * @brief 通用字节传输抽象 `ITransport`：连接、带超时的读写、半关闭与幂等关闭。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 本文件对应 JinShan 模块图中的 MOD-07（`ITransport`）：把「一条已连接
 *          或可连接的可靠字节流」抽象成四个动作 `Connect` / `Read` / `Write` /
 *          `Close`，并为上层（帧编解码、会话、转发）提供统一的超时与错误语义。
 *          MOD-08 的 TCP 实现与 MOD-09 的内存实现都只依赖本接口，不反过来被
 *          `ITransport` 引用。
 *
 * ## 为什么是「字节流」而不是「消息」
 *
 * 传输层只保证**有序字节**，不保留消息边界。TCP 本身如此，内存管道也刻意保持
 * 同样语义，这样同一份上层代码在两种实现上的行为一致。需要消息边界时，由
 * `frame.hpp` 的通用帧负责分帧；传输层不做任何解析。
 *
 * ## 超时语义（deadline，而不是「每次等待」）
 *
 * 每个 `timeout` 都是**整次调用**的总预算，而不是内部每次 `poll` 都重置。
 * `Read`/`Write` 内部循环（EAGAIN/EINTR）时必须扣除已经花掉的时间。`timeout`
 * 为 0 表示非阻塞尝试一次；负值按 0 处理。这样上层可以给一次完整操作一个
 * 上界，不会因为内核反复返回 EAGAIN 而无限延长。
 *
 * ## 返回值语义
 *
 * | code | 含义 | `bytes` |
 * |---|---|---|
 * | `kOk` | 成功 | `Read`/`Write` 实际搬运的字节数（可能小于请求） |
 * | `kTimeout` | 在 deadline 前未完成 | 可能已搬运一部分（仅 `Write`） |
 * | `kClosed` | 对端关闭 / 本端已关闭 | 关闭前已搬运的字节数 |
 * | `kError` | 其它系统错误 | 已搬运的字节数；`system_error` 为 errno |
 *
 * `Read` 返回 `kOk` 时至少搬运 1 字节；读到 EOF 返回 `kClosed`（`bytes == 0`）。
 * `Write` 允许**部分写**：`kOk` 不保证写完全部输入，调用方必须循环，或使用
 * 本文件提供的 `WriteFull`。这是流式传输的通用约定，也是实现背压的基础。
 *
 * ## 半关闭
 *
 * `ShutdownWrite()` 只关闭**本端写方向**（TCP 的 FIN）：对端 `Read` 会读到
 * `kClosed`，但本端仍可继续 `Read`，对端也仍可继续 `Write`。请求-响应协议
 * 必须依赖这一点：客户端写完请求后只半关闭，否则会截断服务端的响应。
 * `Close()` 才释放整个连接。
 *
 * ## 关闭的幂等性
 *
 * `Close()` 可以重复调用，始终返回 `kOk`；`ShutdownWrite()` 同理。关闭后
 * 再调用 `Read`/`Write` 返回 `kClosed`，不会访问已释放的 fd，也不会崩溃。
 *
 * ## 线程模型
 *
 * 接口本身不做全局串行化。`MemoryTransport` 的一对实例共享带锁的队列，可
 * 跨线程读写；`TcpTransport` 的 `Read` 与 `Write` 分别只操作 socket 的读/写
 * 方向，通常可并发，但 `Close` 与其它操作并发需要调用方协调（先停线程再
 * `Close`）。本层不隐藏锁，避免每个连接都付出不必要的同步成本。
 *
 * ## 错误映射
 *
 * `system_error` 保留原始 errno，便于日志与排障；`code` 只做**可移植分类**。
 * `ECONNRESET`/`EPIPE`/`ENOTCONN`/`ECONNABORTED`/`ESHUTDOWN` 归为 `kClosed`，
 * `ETIMEDOUT` 归为 `kTimeout`，其余归为 `kError`。上层不应依赖具体 errno 做
 * 控制流，除非确实需要区分（例如 `ECONNREFUSED` 与解析失败）。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace go2cpp::bento::net {

/**
 * @brief 传输端点（主机 + 端口）。
 *
 * @details `host` 可以是 IPv4/IPv6 字面量或主机名；具体解析策略由实现决定。
 *          `MemoryTransport` 忽略端点内容（它已经配对），但保留参数以维持
 *          接口一致。端口 0 在 `Listen` 中表示由内核分配；在 `Connect` 中
 *          视为非法参数。
 */
struct Endpoint {
    std::string host;        ///< 主机名或 IP 字面量。
    std::uint16_t port = 0;  ///< 端口（主机字节序）。
};

/**
 * @brief 传输操作的粗粒度结果码。
 *
 * @details 只保留四种分类：调用方通常只需要知道「成功 / 超时 / 已关闭 / 其它
 *          错误」。具体原因在 `Result::system_error` 中。
 */
enum class Code : std::uint8_t {
    kOk = 0,       ///< 成功。
    kTimeout = 1,  ///< deadline 到期仍未完成。
    kClosed = 2,   ///< 连接已关闭（EOF、RST、本端已 Close 等）。
    kError = 3,    ///< 其它错误（参数非法、系统调用失败等）。
};

/// 返回 `Code` 的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* CodeName(Code code) noexcept;

/**
 * @brief 一次传输操作的结果。
 *
 * @details 这是**返回值**而不是异常：传输层在正常业务路径上不应抛异常。
 *          `bytes` 对 `Read`/`Write` 有意义；`Connect`/`Close` 始终为 0。
 */
struct Result {
    Code code = Code::kOk;    ///< 结果分类。
    int system_error = 0;     ///< 原始 errno；0 表示没有系统错误。
    std::size_t bytes = 0;    ///< 已搬运字节数（仅 Read/Write 有意义）。

    /// 是否成功（`code == kOk`）。注意：成功不代表写完了全部输入。
    bool ok() const noexcept {
        return code == Code::kOk;
    }

    /// 允许 `if (result)` 写法。
    explicit operator bool() const noexcept {
        return ok();
    }
};

/**
 * @brief 可靠字节流的通用接口（JinShan MOD-07）。
 *
 * @details 设计上刻意保持最小：四个动作 + 半关闭。不在接口里放 socket、
 *          文件描述符、缓冲区或线程池，具体实现可以完全替换（TCP、内存、
 *          未来的 WebSocket/TLS），上层只依赖本接口。
 *
 * ## 生命周期
 *
 * 派生类析构必须等价于 `Close()`（RAII）。`ITransport` 不可拷贝、不可移动：
 * 连接是独占资源，拷贝语义只会带来双重关闭。
 *
 * ## 调用顺序
 *
 * 典型客户端：`Connect` → 若干 `Read`/`Write` → `ShutdownWrite`（可选）→
 * `Close`。服务端由具体实现的 `Listen`/`Accept` 产出已连接实例，随后同样
 * 使用 `Read`/`Write`/`Close`。`Connect` 在已连接的实例上返回 `kError`
 * （errno `EALREADY`），不会隐式重建连接。
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    ITransport(const ITransport&) = delete;
    ITransport& operator=(const ITransport&) = delete;
    ITransport(ITransport&&) = delete;
    ITransport& operator=(ITransport&&) = delete;

    /**
     * @brief 连接到端点。
     * @param endpoint 目标地址；`host` 为空或端口 0 视为非法参数。
     * @param timeout 连接总超时（deadline 语义）。
     * @return `kOk` 表示已建立连接；`kTimeout` 表示超时；`kError` 表示解析
     *         失败或连接被拒绝；`kClosed` 表示实现已关闭。
     *
     * @note DNS 解析发生在 deadline 之外（`getaddrinfo` 没有超时参数）。
     *       需要严格上界的调用方应传入 IP 字面量。
     */
    virtual Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) = 0;

    /**
     * @brief 读取至多 `capacity` 字节。
     * @param buffer 可写缓冲，长度至少 `capacity`。
     * @param capacity 本次最多读取的字节数。
     * @param timeout 本次调用的总超时。
     * @return `kOk` 且 `bytes > 0`；EOF 返回 `kClosed`；超时返回 `kTimeout`。
     *
     * @note 一次成功读取**不保证**读满 `capacity`。希望读满时用 `ReadFull`。
     * @note `capacity == 0` 是合法的空操作，返回 `kOk` 且 `bytes == 0`。
     */
    virtual Result Read(std::uint8_t* buffer, std::size_t capacity,
                        std::chrono::milliseconds timeout) = 0;

    /**
     * @brief 写入至多 `size` 字节。
     * @param data 只读数据，长度至少 `size`。
     * @param size 本次希望写入的字节数。
     * @param timeout 本次调用的总超时。
     * @return `kOk` 时 `bytes` 可能小于 `size`（部分写）；`kTimeout` 时
     *         `bytes` 是超时前已写入的部分；`kClosed` 表示对端已关闭或本端
     *         已半关闭/关闭。
     *
     * @note `size == 0` 是合法的空操作，返回 `kOk` 且 `bytes == 0`。
     */
    virtual Result Write(const std::uint8_t* data, std::size_t size,
                         std::chrono::milliseconds timeout) = 0;

    /**
     * @brief 半关闭写方向（发送 FIN），不影响读方向。
     * @return 首次成功返回 `kOk`；重复调用返回 `kOk`；已 `Close` 返回
     *         `kClosed`；对端已完全关闭时也按 `kOk` 处理（写方向已经不可用）。
     *
     * @details 调用后本实例的 `Write` 返回 `kClosed`（errno `EPIPE`），
     *          `Read` 仍然可用。这是请求-响应协议安全关闭的必要原语。
     */
    virtual Result ShutdownWrite() = 0;

    /**
     * @brief 关闭连接并释放资源（幂等）。
     * @return 始终 `kOk`；重复调用不产生副作用。
     *
     * @details 析构函数必须调用它。关闭后 `Read`/`Write` 返回 `kClosed`，
     *          `Connect` 返回 `kClosed`（实现可以重新 `Connect`，但本接口
     *          不要求）。
     */
    virtual Result Close() = 0;

    /**
     * @brief 循环 `Read` 直到读满 `size` 字节、EOF、超时或出错。
     * @param timeout 整个循环共享的总预算（不是每次 `Read` 重置）。
     * @return 成功时 `kOk` 且 `bytes == size`；提前结束时返回最后一次的
     *         `code`，`bytes` 为累计读取量。
     *
     * @note 这是非虚便利函数，不改变派生类的契约。
     */
    Result ReadFull(std::uint8_t* buffer, std::size_t size, std::chrono::milliseconds timeout);

    /**
     * @brief 循环 `Write` 直到写完 `size` 字节、超时或出错。
     * @param timeout 整个循环共享的总预算。
     * @return 成功时 `kOk` 且 `bytes == size`；部分写入后失败时返回对应
     *         `code`，`bytes` 为累计写入量。
     */
    Result WriteFull(const std::uint8_t* data, std::size_t size,
                     std::chrono::milliseconds timeout);

protected:
    ITransport() = default;
};

}  // namespace go2cpp::bento::net
