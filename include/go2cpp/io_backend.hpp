#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace go2cpp::io {
/** @brief 后端统一事件位；token=0 保留给内部唤醒事件。 */
enum class BackendEventFlag : std::uint32_t {
    kReadable = 0x01U,
    kWritable = 0x02U,
    kError = 0x04U,
    kHangup = 0x08U,
};

/** @brief Poller 返回的统一就绪事件；token 由 IOManager 生成并原样回传。 */
struct BackendEvent final { std::uint64_t token{0}; std::uint32_t events{0}; };

/**
 * @brief IOManager 与操作系统多路复用器之间的替换协议。
 * @details 不拥有 Fiber、Task 或 fd 生命周期，只提供 readiness 注册、等待、
 * 唤醒和关闭。实现可替换为 epoll、kqueue、IOCP 或 io_uring，但必须保持
 * token、wait 返回值和 shutdown 后不再唤醒的协议。
 */
class IOBackend {
public:
    virtual ~IOBackend() = default;
    /** @brief 初始化平台资源；error 可选输出 errno 风格错误码。 */
    virtual bool initialize(int* error = nullptr) noexcept = 0;
    /**
     * @brief 注册或修改 fd 的 readiness。
     * @param fd 文件描述符。 @param interest 统一读写位（读 0x01、写 0x02）。
     * @param token 一次性代际令牌。 @param modify 是否修改已有注册。
     * @return 成功 0，否则 errno 风格错误码。
     */
    virtual int add_or_modify(int fd, std::uint32_t interest,
                              std::uint64_t token, bool modify) noexcept = 0;
    /** @brief 移除 fd 注册；目标已不存在时也返回 0。 */
    virtual int remove(int fd) noexcept = 0;
    /**
     * @brief 等待就绪事件。
     * @param output 输出数组（容量必须大于 0）。 @param capacity 数组容量。
     * @param timeout_ms 超时毫秒，负值表示无限等待。
     * @return 就绪数量；超时 0；失败返回负 errno。
     */
    virtual int wait(BackendEvent* output, std::size_t capacity,
                     int timeout_ms) noexcept = 0;
    /** @brief 唤醒 poller 线程；重复唤醒可合并。 */
    virtual void wake() noexcept = 0;
    /** @brief 排空唤醒事件；只应由 poller 线程调用。 */
    virtual void drain_wake() noexcept = 0;
    /** @brief 释放平台资源；可重复调用。 */
    virtual void shutdown() noexcept = 0;
};

using IOBackendPtr = std::shared_ptr<IOBackend>;
using IOBackendFactory = std::function<IOBackendPtr()>;
/** @brief 注册命名后端；同名注册失败，注册表进程级线程安全。 */
bool RegisterBackend(std::string name, IOBackendFactory factory);
/** @brief 注销后端；只影响后续 CreateBackend。 */
bool UnregisterBackend(const std::string& name) noexcept;
/** @brief 创建后端实例；空名称使用默认后端。 */
IOBackendPtr CreateBackend(const std::string& name = {});
/** @brief 返回默认后端名称；Linux 默认为 epoll。 */
std::string DefaultBackendName();
}  // namespace go2cpp::io
