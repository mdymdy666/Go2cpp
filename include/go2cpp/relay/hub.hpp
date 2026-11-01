#pragma once

/**
 * @file hub.hpp
 * @brief 连接注册表：key → 多条连接的并发安全映射（JinShan REQ-003）。
 *
 * ## 模块定位
 *
 * `Hub` 只回答一个问题：**「这个 key 现在挂着哪些连接」**。它不解析协议、
 * 不配对连接、不做配额、不决定路由——这些是调用方（例如 JinShan 的 relay
 * 房间状态机）的事。对应 Go 侧 `Hub{ Register / Lookup / Unregister /
 * CloseKey / Keys }`，连接就是 Go 的 `io.Closer`。
 *
 * 典型用法是「房间/频道」：一个 key 下注册该房间的控制连接与数据连接，
 * 广播或清理时按 key 取快照，房间销毁时 `CloseKey` 一次性全部关闭。
 *
 * ## 连接契约：Hub::Stream 就是 streams::Closer
 *
 * REQ-003 的 `conn io.Closer` 在本项目里已有对应物：`go2cpp::streams::Closer`
 * （`Close() -> ErrorPtr`）。因此 Hub 不另造一套流接口，而是
 *
 * ```cpp
 * using Stream = ::go2cpp::streams::Closer;   // Hub 的成员别名
 * ```
 *
 * 这样做的两个理由：
 * 1. **复用**：任何实现 `streams::Closer` 的类型都能直接注册（SocketStream、
 *    `relay::Stream`（RelayPipe 的双向流，继承自 Closer）等都满足）；
 * 2. **不抢名字**：`go2cpp::relay::Stream` 已被 RelayPipe 用作「Read/Write/
 *    Close 三件套」的双向流接口；Hub 只需要「可关闭」，把更宽的接口当契约会
 *    逼着控制连接去实现用不到的读写方法。
 *
 * `Close()` 的返回值在 `CloseKey` 里被忽略：Hub 不是 I/O 层，无法对单条连接
 * 的错误做出有意义的重试或回滚；需要错误统计的调用方应在自己的 `Close()`
 * 实现里记账。实现方仍需保证 `Close()` 幂等——同一条连接可能挂在多个 key 下。
 *
 * ## 生命周期与所有权
 *
 * - Hub 只保存 `shared_ptr<Stream>`，**连接对象归调用方与 Hub 共享**；
 *   连接何时真正销毁由引用计数决定，Hub 不额外持有裸指针；
 * - `Unregister` 只把连接从表里摘掉，**不关闭**它；关闭由调用方决定；
 * - `CloseKey` 先摘表、**再在锁外**逐个 `Close()`，因此 `Close()` 里回调
 *   Hub（例如自我注销）不会死锁；
 * - `Stream::Close()` 由实现方保证幂等：同一条连接可能同时挂在多个 key 下，
 *   多次 `CloseKey` 会重复调用它（Hub 无法替代实现方保证这一点）。
 *
 * ## 并发模型
 *
 * 单一 `std::mutex` 保护整张表。临界区只做容器操作：`Lookup`/`Keys` 在锁内
 * 复制快照，`CloseKey` 在锁内把连接列表搬出、锁外执行用户 `Close()`。
 * 因此任意线程都可以在 `Close()` 回调里再次调用 Hub 的任意方法。
 *
 * ## 不做什么
 *
 * - **不内置业务配额**（「一个 key 最多 4 条连接」由调用方或限流组件实现）；
 * - 不检测连接是否已关闭（`streams::Closer` 不暴露状态）；
 * - 析构时不关闭任何连接：析构期做 I/O 会延长对象生命周期并可能触发回调，
 *   调用方应在销毁 Hub 前显式 `CloseKey` 清理。
 *
 * 头文件自包含，只依赖项目公共的 streams/error 与标准库。
 */

#include "go2cpp/streams.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace go2cpp::relay {

/**
 * @brief key → 多条共享连接的注册表。
 *
 * ## 使用约束
 * - `Register`/`Lookup`/`Unregister`/`CloseKey`/`Keys` 全部可并发调用；
 * - 同一连接对象（按对象地址判等）在同一 key 下只登记一次，重复 `Register`
 *   返回 `false` 且不改变表；不同 key 下允许重复登记同一条连接；
 * - `key` 与 `conn` 为空时按失败/空操作处理，不抛异常。
 *
 * ## 典型时序
 * @code
 * hub.Register("room:42", control);       // 建立房间
 * auto conns = hub.Lookup("room:42");     // 广播/转发时取快照
 * hub.Unregister("room:42", control);     // 单条下线
 * hub.CloseKey("room:42");                // 房间销毁：全部关闭并摘表
 * @endcode
 */
class Hub final {
public:
    /**
     * @brief Hub 管理的连接契约：等价于 Go 的 `io.Closer`。
     * @details 直接复用 `go2cpp::streams::Closer`，不新造接口：RelayPipe 的
     *          `relay::Stream`（Reader+Writer+Closer）也能隐式转换为它。
     */
    using Stream = ::go2cpp::streams::Closer;
    using ConnList = std::vector<std::shared_ptr<Stream>>;

    Hub() = default;
    ~Hub() = default;
    Hub(const Hub &) = delete;
    Hub &operator=(const Hub &) = delete;
    Hub(Hub &&) = delete;
    Hub &operator=(Hub &&) = delete;

    /**
     * @brief 把连接登记到 key 下，key 不存在时自动创建。
     * @param key 任意非空字节串（内部按值复制，允许包含 `\0`）。
     * @param conn 连接；为空时返回 `false`。
     * @return 新登记返回 `true`；连接为空、或同一对象已在该 key 下时返回 `false`。
     * @note 已经关闭的连接仍可登记——Hub 不感知连接状态，由调用方保证不这么做。
     */
    bool Register(std::string_view key, std::shared_ptr<Stream> conn);

    /**
     * @brief 返回 key 下连接的快照（插入顺序）。
     * @param key 要查询的 key。
     * @return 空 vector 表示 key 不存在或已无连接；返回值是 `shared_ptr` 副本，
     *         调用方遍历期间连接不会被 Hub 的并发操作销毁。
     */
    std::vector<std::shared_ptr<Stream>> Lookup(std::string_view key) const;

    /**
     * @brief 按对象身份从 key 下移除一条连接（不关闭它）。
     * @param key 连接所属 key。
     * @param conn 要移除的连接；为空时为空操作。
     * @note **幂等**：连接不存在、key 不存在、重复调用都不会报错或崩溃；
     *       移除最后一条连接后 key 自动从表中删除。
     */
    void Unregister(std::string_view key, const std::shared_ptr<Stream> &conn);

    /**
     * @brief 关闭 key 下的全部连接并从表中删除该 key。
     * @param key 要关闭的 key。
     * @return key 原本存在（无论有几条连接）返回 `true`；不存在返回 `false`。
     * @details 实现顺序是「锁内摘表 → 锁外 Close」，因此：
     *          - `Close()` 内可以安全回调 Hub（自我注销、登记新连接）；
     *          - 返回时该 key 已不可见，`Lookup` 保证为空；
     *          - 每条连接调用一次 `Close()`（同一条连接挂在多个 key 下时，
     *            每个 key 各调用一次，依赖 `Close()` 的幂等性）；
     *          - `Close()` 返回的错误被忽略（Hub 不做 I/O 错误处理）。
     */
    bool CloseKey(std::string_view key);

    /**
     * @brief 返回当前全部 key 的快照，按字典序排序（输出稳定，便于断言/日志）。
     */
    std::vector<std::string> Keys() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ConnList> m_conns;
};

} // namespace go2cpp::relay
