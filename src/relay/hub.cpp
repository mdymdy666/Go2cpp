/**
 * @file hub.cpp
 * @brief Hub 连接注册表的实现：单锁保护的 key → 连接列表。
 *
 * ## 为什么用一把锁而不是分片锁
 *
 * Hub 的临界区只有容器操作（查找、追加、删除、整体搬出），没有任何 I/O 或
 * 用户代码；单锁已经能到数百万次/秒的量级（见 tests/relay/bench_relay.cpp
 * 的实测）。分片会引入「同一 key 的两条连接落到不同分片」的一致性负担，
 * 收益却只在极端热点下才出现——真到那个量级应当换中心化路由/无锁结构，
 * 而不是在注册表里加一层分片。
 *
 * ## 为什么 CloseKey 在锁外调用 Close()
 *
 * `Close()` 是用户代码：它可能（JinShan 就是这样）在关闭时回调
 * `Hub::Unregister` 做自我摘除。若持锁调用就会死锁。因此先把列表整体
 * 搬出、删 key、解锁，再逐条关闭：这样 `Lookup` 在 `CloseKey` 返回前就已经
 * 看不到该 key，符合「CloseKey 后 Lookup 为空」的验收要求。
 */
#include "go2cpp/relay/hub.hpp"

#include <algorithm>
#include <utility>

namespace go2cpp::relay {

bool Hub::Register(std::string_view key, std::shared_ptr<Hub::Stream> conn) {
    if (!conn) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    ConnList &conns = m_conns[std::string(key)];
    // 按对象地址判重：同一个 Stream 重复登记会让 CloseKey 对同一对象调用
    // 两次 Close（虽然要求幂等），也会让 Lookup 出现重复项，语义上更难用。
    for (const auto &existing : conns) {
        if (existing.get() == conn.get()) {
            return false;
        }
    }
    conns.push_back(std::move(conn));
    return true;
}

std::vector<std::shared_ptr<Hub::Stream>> Hub::Lookup(std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_conns.find(std::string(key));
    if (it == m_conns.end()) {
        return {};
    }
    // 复制 shared_ptr：调用方拿到快照后，即使并发 Unregister/CloseKey 把表清空，
    // 手里的连接对象仍然有效，不会出现悬空指针。
    return it->second;
}

void Hub::Unregister(std::string_view key, const std::shared_ptr<Hub::Stream> &conn) {
    if (!conn) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_conns.find(std::string(key));
    if (it == m_conns.end()) {
        return;
    }
    ConnList &conns = it->second;
    conns.erase(std::remove_if(conns.begin(), conns.end(),
                               [&conn](const std::shared_ptr<Stream> &candidate) {
                                   return candidate.get() == conn.get();
                               }),
                conns.end());
    // 空 key 不留在表里：Keys()/Lookup() 的语义是「key 存在即有连接」，
    // 保留空壳会让 CloseKey 对已经不存在的房间返回 true。
    if (conns.empty()) {
        m_conns.erase(it);
    }
}

bool Hub::CloseKey(std::string_view key) {
    ConnList conns;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_conns.find(std::string(key));
        if (it == m_conns.end()) {
            return false;
        }
        conns = std::move(it->second);
        m_conns.erase(it);
    }
    // 锁外关闭：Close() 里允许回调 Hub（自我注销/登记新连接）。
    for (const auto &conn : conns) {
        if (conn) {
            // 忽略 Close() 返回的错误：Hub 不是 I/O 层，无法对单连接错误做
            // 有意义的重试；需要统计的调用方在自己的 Close() 里记账。
            conn->Close();
        }
    }
    return true;
}

std::vector<std::string> Hub::Keys() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_conns.size());
    for (const auto &entry : m_conns) {
        keys.push_back(entry.first);
    }
    // 排序不是功能需求，而是为了让日志、测试断言和「取第一个房间」这类
    // 调试代码有稳定输出；unordered_map 的遍历顺序与插入历史有关，不稳定。
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace go2cpp::relay
