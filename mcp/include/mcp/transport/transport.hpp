/**
 * @file transport.hpp
 * @brief 传输层抽象：把「一条完整的 JSON-RPC 文本」收进来、发出去。
 *
 * @details
 * 参考项目的 `ITransport` 同时暴露同步/异步两套读写，并把端口、版本塞进接口，
 * 结果三种实现里各自维护线程与状态机。这里的边界更窄：
 * - 传输只负责**消息边界**与**字节搬运**，不解析 JSON；
 * - 读写都是阻塞语义，由调用方决定放在普通线程还是 Fiber 里；
 * - 不提供 `GetPort()` 之类只有 HTTP 有意义的成员，参数通过构造函数注入。
 *
 * 实现必须保证：`WriteMessage` 可以被多线程调用（内部加锁），
 * `ReadMessage` 只允许单个读线程调用。
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace go2cpp::mcp {

/**
 * @brief 一条消息传输通道。
 *
 * 生命周期约定：`Start()` 成功后才可以 `ReadMessage` / `WriteMessage`；
 * `Stop()` 必须能让阻塞中的 `ReadMessage` 尽快返回 `std::nullopt`。
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    /// 启动传输。返回 false 表示资源准备失败，调用方应退出。
    virtual bool Start() = 0;

    /// 停止传输，唤醒阻塞的读。
    virtual void Stop() = 0;

    /**
     * @brief 阻塞读取下一条消息。
     * @return 消息文本（不含分隔符）；EOF 或已停止返回 `std::nullopt`。
     */
    virtual std::optional<std::string> ReadMessage() = 0;

    /**
     * @brief 发送一条消息（实现负责追加分隔符）。
     * @return 写成功返回 true。
     */
    virtual bool WriteMessage(std::string_view message) = 0;

    /// 传输名，用于日志与 initialize 响应。
    virtual std::string Name() const = 0;
};

}  // namespace go2cpp::mcp
