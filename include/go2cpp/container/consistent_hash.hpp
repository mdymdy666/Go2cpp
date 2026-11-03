/**
 * @file consistent_hash.hpp
 * @brief 一致性哈希环，接口对齐 Go `groupcache/consistenthash`。
 *
 * @details 用于 groupcache 把 key 映射到后端 peer：每个真实节点按
 *          `replicas` 个虚拟节点铺到 32 位哈希环上，查找时取顺时针方向
 *          第一个 `hash >= hash(key)` 的虚拟节点，环形回绕。节点增减时
 *          只有相邻区间迁移，大部分 key 的归属保持不变。
 *
 * ## 环的构建
 *
 * 对每个真实节点 `name`，生成 `replicas` 个虚拟节点：
 * ```
 * virtual_hash(i) = hash(name + "#" + i)      // i = 0 .. replicas-1
 * ```
 * 环按哈希值升序保存在**连续数组**里，`Get` 用二分查找定位后继；相比
 * `std::map` 的逐节点指针追逐，二分只访问 O(log n) 个连续缓存行。
 *
 * ## 与 Go 官方实现的差异
 *
 * 上游 `groupcache/consistenthash` 的虚拟节点是 `strconv.Itoa(i) + key`
 * （序号在前），本实现按任务约定改为 `key + "#" + i`（序号在后，带分隔符）。
 * 两者分布性质相同，但**哈希值不同**，因此与 Go 版本不能逐 key 对拍；
 * 语义（环 + 顺时针查找 + 回绕 + 幂等 Add）保持一致。
 *
 * ## 幂等与碰撞
 *
 * - `Add` 对已存在的真实节点直接跳过，因此重复调用不会重复铺虚拟节点，
 *   `Size()` 与 `Get()` 都不变。
 * - 32 位哈希存在碰撞可能：碰撞的虚拟节点都会留在环上（与 Go 把重复
 *   哈希写进 `keys` 切片的行为一致），`Get` 取排序后的第一个，结果确定。
 *
 * ## 空环
 *
 * 未 `Add` 任何节点时 `Get` 返回空串（调用方可据此判断「没有可用 peer」），
 * 与 Go 的 `IsEmpty` 语义一致。
 *
 * @author DeepSeek Harness
 * @date 2026-11-03
 * @copyright BSD-3-Clause（语义对照 Go `groupcache/consistenthash`，实现独立完成）
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/hash/crc32.hpp"

namespace go2cpp {
namespace container {

/**
 * @brief 一致性哈希环。
 *
 * @details `Map` 构造后即可并发调用 `Get`；`Add` 会改写环，必须与 `Get`
 *          串行（groupcache 只在初始化/成员变更时 `Add`，变更后用新 `Map`
 *          整体替换）。本实现不额外加锁，避免给热路径 `Get` 增加原子操作。
 */
class Map {
 public:
    /// 哈希函数类型，对应 Go `consistenthash.Hash`。
    using Hash = std::function<std::uint32_t(std::string_view)>;

    /**
     * @brief 构造空环。
     * @param replicas 每个真实节点的虚拟节点数，默认 50（与 Go 一致）。
     * @param hash 哈希函数；为空时使用 CRC32（`hash/crc32.hpp` 的
     *             `Crc32::ChecksumIEEE`），对应 Go 默认的 `crc32.ChecksumIEEE`。
     */
    explicit Map(std::size_t replicas = 50, Hash hash = nullptr)
        : m_replicas(replicas), m_hash(std::move(hash)) {
        if (!m_hash) {
            m_hash = [](std::string_view data) {
                return hash::Crc32::ChecksumIEEE(hash::AsBytes(data));
            };
        }
    }

    /**
     * @brief 加入真实节点（每个节点铺 `replicas` 个虚拟节点）。
     * @param keys 节点名列表；已加入的名字会被跳过（幂等）。
     *
     * @note 每次调用后对环整体排序，复杂度 O((N+R) log(N+R))；
     *       只在成员变更时调用，不在 `Get` 热路径上。
     */
    void Add(std::initializer_list<std::string_view> keys) {
        bool changed = false;
        std::string virtual_key;
        for (std::string_view key : keys) {
            const auto inserted = m_nodes.insert(std::string(key));
            if (!inserted.second) {
                continue;  // 幂等：该真实节点已经铺过
            }
            const std::string& name = *inserted.first;
            virtual_key.reserve(name.size() + 2 + 20);
            for (std::size_t replica = 0; replica < m_replicas; ++replica) {
                virtual_key.assign(name);
                virtual_key.push_back('#');
                virtual_key.append(std::to_string(replica));
                m_ring.push_back(VirtualNode{m_hash(virtual_key), &name});
            }
            changed = true;
        }
        if (!changed) {
            return;
        }
        std::sort(m_ring.begin(), m_ring.end(),
                  [](const VirtualNode& lhs, const VirtualNode& rhs) {
                      if (lhs.hash != rhs.hash) {
                          return lhs.hash < rhs.hash;
                      }
                      return *lhs.node < *rhs.node;
                  });
    }

    /**
     * @brief 查找 `key` 顺时针方向的后继节点。
     * @param key 任意键。
     * @return 节点名；环为空时返回空串。
     */
    std::string Get(std::string_view key) const {
        if (m_ring.empty()) {
            return std::string();
        }
        const std::uint32_t target = m_hash(key);
        const auto it = std::lower_bound(
            m_ring.begin(), m_ring.end(), target,
            [](const VirtualNode& node, std::uint32_t value) {
                return node.hash < value;
            });
        const auto& node = it == m_ring.end() ? m_ring.front() : *it;
        return *node.node;
    }

    /// 环上虚拟节点数（`真实节点数 × replicas`，哈希碰撞时按实际项数计）。
    std::size_t Size() const noexcept { return m_ring.size(); }

 private:
    /// 环上的虚拟节点；`node` 指向 `m_nodes` 内的字符串（引用稳定）。
    struct VirtualNode {
        std::uint32_t hash;   ///< 虚拟节点哈希
        const std::string* node;  ///< 所属真实节点名
    };

    std::size_t m_replicas;  ///< 每个真实节点的虚拟节点数
    Hash m_hash;             ///< 哈希函数（默认 CRC32）
    /// 已加入的真实节点名；`unordered_set` 的 rehash 不使元素指针失效，
    /// 因此 `VirtualNode::node` 始终有效。
    std::unordered_set<std::string> m_nodes;
    std::vector<VirtualNode> m_ring;  ///< 按 hash 升序的虚拟节点环
};

}  // namespace container
}  // namespace go2cpp
