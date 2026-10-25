/**
 * @file plugin_loader.hpp
 * @brief dlopen 插件加载器：把 mcp-extened 的 .so 插件适配到我们的 Registry，
 *        并把宿主通知出口注入插件的 C ABI 回调。
 *
 * @details
 * 插件主动通知的链路：
 *
 * ```
 * 插件（任意线程）
 *   └─ PluginAPI::notifications->SendToClient(name, json)
 *        │  C 函数指针，没有 userdata 参数
 *        ▼
 *   PluginNotificationTrampoline（宿主静态函数）
 *        │  按插件名查全局投递表（plugin_loader.cpp 内部）
 *        ▼
 *   NotificationHub::Deliver（解析 + 校验 + 计数）
 *        │  取 sink 快照（不持锁调用）
 *        ▼
 *   Server::SendNotification → 传输（stdio 直接写 / HTTP-SSE 广播所有会话）
 * ```
 *
 * 为什么按插件名查表：C ABI 的回调签名只有 `(pluginName, notification)`，
 * 无法携带 `this` 指针。宿主在加载时以插件自报的名字注册投递目标，插件回调
 * 时传的也是同一个名字，因此能对上。**插件名必须与 `GetName()` 一致**；
 * 若两个插件同名，后加载者覆盖前者（与注册表 upsert 语义一致）。
 *
 * 线程模型：插件可以在工作线程调用；`NotificationHub` 内部用互斥量保护 sink，
 * `Server::SendNotification` 内部再串行化对传输的写。本类自身的
 * `LoadDirectory`/析构**不支持并发调用**（加载/卸载都发生在服务启动/停止阶段）。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mcp/plugin/plugin_api.h"
#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::plugin {

/**
 * @brief 插件主动通知的宿主侧出口。
 *
 * @details 职责只有三件事：把插件给的 JSON 变成 `(method, params)`、校验它
 * 不能冒充请求或响应、把结果交给 sink。协议编码（补 `jsonrpc`、去 id）由
 * `Server::SendNotification` 统一完成，这里不重复。
 *
 * 语义约定（测试与文档以此为准）：
 * - 插件给完整通知帧 `{"jsonrpc":"2.0","method":"notifications/message","params":{...}}`
 *   时取 method/params 转发；`jsonrpc` 与 `id` 一律丢弃，**绝不允许插件伪造响应**。
 * - method 必须以 `notifications/` 开头，否则计入 `rejected()` 并丢弃。
 * - 插件只给 params（例如 `{"level":"info","data":"..."}`）时，按
 *   `notifications/message` 转发。
 * - 无 sink（未绑定宿主 / 已 `Detach`）时计入 `dropped()` 并丢弃，
 *   这是「卸载后不回调」以及「服务未就绪」的统一语义：**丢弃，不缓存**。
 * - payload 上限 1 MiB；超限、非法 JSON、非对象、params 非对象都计入
 *   `rejected()`，避免拖垮传输或污染协议流。
 */
class NotificationHub {
public:
    /// 出口：插件名、通知方法、参数。调用方不应在 sink 内再调用本类的写方法。
    using Sink = std::function<void(const std::string& plugin, const std::string& method,
                                    const json& params)>;

    NotificationHub() = default;
    explicit NotificationHub(Sink sink);

    NotificationHub(const NotificationHub&) = delete;
    NotificationHub& operator=(const NotificationHub&) = delete;

    /// 替换出口（线程安全）。传空 Function 等价于 Detach。
    void SetSink(Sink sink);

    /// 断开出口：之后所有投递只累加 dropped 计数，不再调用 sink。
    void Detach();

    /**
     * @brief 处理一次插件投递（C ABI 回调的落地实现）。
     * @note 绝不抛异常：异常穿回插件会破坏插件的调用栈。
     */
    void Deliver(const std::string& plugin, std::string_view payload) noexcept;

    /// 成功交给 sink 的通知数。
    std::size_t forwarded() const noexcept { return m_forwarded.load(std::memory_order_relaxed); }
    /// 合法但没有出口（未绑定 / 已 Detach）而丢弃的通知数。
    std::size_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    /// 非法或越权而拒绝的通知数。
    std::size_t rejected() const noexcept { return m_rejected.load(std::memory_order_relaxed); }

private:
    mutable std::mutex m_mutex;
    Sink m_sink;
    std::atomic<std::size_t> m_forwarded{0};
    std::atomic<std::size_t> m_dropped{0};
    std::atomic<std::size_t> m_rejected{0};
};

/**
 * @brief 加载目录下的 .so 插件并注册到注册表。
 *
 * 生命周期：插件句柄由本类持有，析构时先断开通知出口（`Detach`）、再调用
 * `Shutdown()`、注销投递表项、`DestroyPlugin()`、`dlclose()`。注册表里的
 * `std::function` 捕获了 `PluginAPI*`，所以**卸载必须发生在注册表被销毁或
 * 再次使用之前**——应用层的顺序是 `Server` 先构造、加载器后构造，加载器
 * 先析构，`Server` 析构时插件已全部卸载。
 */
class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    /// 绑定插件通知的宿主出口（通常转调 `Server::SendNotification`）。
    /// 必须在 `LoadDirectory` 之前调用，之后加载的插件才能投递。
    void SetNotificationSink(NotificationHub::Sink sink);

    /// 通知集线器；测试可在加载器析构后继续持有它，观察卸载期间的计数。
    std::shared_ptr<NotificationHub> notification_hub() const { return m_hub; }

    /**
     * @brief 加载目录下全部 .so。
     * @param directory 插件目录。
     * @param registry 目标注册表。
     * @param error 失败原因（至少一个成功则视为整体成功）。
     * @return 是否有插件被成功加载。
     */
    bool LoadDirectory(const std::string& directory, Registry& registry, std::string& error);

    /// 已加载插件数。
    std::size_t count() const noexcept { return m_handles.size(); }

private:
    bool LoadFile(const std::string& path, Registry& registry, std::string& error);
    void UnloadAll();

    struct Handle {
        void* library = nullptr;
        PluginAPI* api = nullptr;
        std::string path;
        std::string name;                    ///< 插件自报名字，也是投递表的键
        NotificationSystem notifications{};  ///< 宿主注入给插件的回调结构体
    };
    /// 用 unique_ptr 持有：注入的 `&Handle::notifications` 地址必须稳定，
    /// 不能随 vector 扩容而失效。
    std::vector<std::unique_ptr<Handle>> m_handles;
    std::shared_ptr<NotificationHub> m_hub;
};

}  // namespace go2cpp::mcp::plugin
