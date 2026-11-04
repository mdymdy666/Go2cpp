#pragma once

/**
 * @file node_id.hpp
 * @brief 节点身份：128 位、进程内唯一的 `NodeId`（header-only）。
 *
 * ## 模块定位
 *
 * `NodeId` 是分布式设施 Layer 0 的**身份标识**，用于替代现有
 * `ServiceRegistration::owner` / `LeaseView::owner` 里随手拼的字符串
 * （`"host-pid"`、`"worker-3"` 混用）。它只回答「这是哪个节点」，**不做
 * 认证**：跨机部署仍必须套 TLS/VPN（见 docs/reports/distributed-analysis.md
 * §4.1.1 与 §6）。
 *
 * 编码是 128 位定长值：
 *
 * - 高 64 位：进程级随机种子派生的伪随机值，用来区分**不同进程/机器**；
 * - 低 64 位：`(epoch 毫秒 << 32) | 进程内原子计数器`，用来保证**同一进程
 *   内**任意两次 `Generate()` 不相等。
 *
 * 两个半区都不做位域压缩：`ToString()` 是固定 32 个小写十六进制字符，
 * 可直接塞进现有的 `std::string owner` 字段，也可作为二进制键在 RPC 里
 * 透传（16 字节）。
 *
 * ## 与文档建议的关系
 *
 * 分析文档建议 `"host-pid-bootid"` 文本；本实现选择 128 位定长值，理由：
 * 文本格式在协议里要额外编码长度，且换 hostname 后不可解析；128 位值
 * 同时满足「可读十六进制」与「定长二进制」两种用途。host/pid/boot_id 的
 * 语义由调用方在 `NodeInfo` 层补充（P1 再做），不塞进身份值本身。
 *
 * ## 线程安全
 *
 * `Generate()` 只做一次 `fetch_add(relaxed)` 与纯计算，无锁、无系统调用，
 * 任意线程可并发调用；`Parse()` / `ToString()` / 比较都是纯函数，不共享
 * 可变状态。`NodeId` 本身是 16 字节可拷贝值类型。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/distributed/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::distributed {

/**
 * @brief 128 位节点身份（16 字节值类型，可拷贝、可比较）。
 *
 * @details 默认构造得到**无效**身份（全 0）；`IsValid()` 为 false 的
 *          `NodeId` 只能用于「未设置」，不能参与身份比较语义之外的操作。
 *          相等/排序都是全序（先比 `high()`，再比 `low()`）。
 */
class NodeId final {
public:
    /// 十六进制文本长度：32 个字符。
    static constexpr std::size_t kTextLength = 32;

    /// @brief 构造全 0 的无效身份（`IsValid() == false`）。
    constexpr NodeId() noexcept = default;

    /**
     * @brief 由两个半区构造身份（反序列化/测试用）。
     * @param high [in] 高 64 位（进程级随机区）。
     * @param low [in] 低 64 位（时间 + 计数器区）。
     * @return 构造出的身份；两个参数都为 0 时结果为无效身份。
     */
    static constexpr NodeId FromParts(std::uint64_t high, std::uint64_t low) noexcept {
        NodeId id;
        id.m_high = high;
        id.m_low = low;
        return id;
    }

    /**
     * @brief 生成新的节点身份（进程内唯一，无锁、无系统调用）。
     *
     * ## 定位
     * 身份值的唯一产生点；身份重复会让协调者把两个进程当成同一个 owner，
     * 从而允许「旧进程续约新进程的租约」——这是必须避免的静默错误。
     *
     * ## 步骤
     * step1. 原子计数器取一个进程内严格递增的序号；
     * step2. 取当前 epoch 毫秒，与序号拼成低 64 位（同毫秒靠序号区分）；
     * step3. 用启动期随机种子与序号做 SplitMix64，得到高 64 位随机区。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；不持锁、不阻塞。
     * - 原子性：`fetch_add(relaxed)` 保证序号不重复；其余为纯计算。
     * - 失败语义：`noexcept`，永不失败（时钟回拨时靠随机区与序号区分）。
     * - 不变量：同一进程内连续两次调用返回不同值。
     *
     * @return 新的有效身份；调用方可直接 `ToString()` 作为 owner 字符串。
     */
    static NodeId Generate() noexcept;

    /**
     * @brief 解析 32 个十六进制字符的文本。
     *
     * ## 定位
     * 配置/日志/协议边界上把文本还原为身份的**唯一入口**；它必须严格，
     * 否则长度错误或含非法字符的文本会静默变成另一个身份。
     *
     * ## 步骤
     * step1. 校验长度必须恰好为 32（空串、超长、过短都拒绝）；
     * step2. 逐字符解析十六进制（大小写都接受），任一非法字符即失败；
     * step3. 组装为身份并返回；调用方用 `IsValid()` 判断是否解析成功。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程，纯函数。
     * - 失败语义：`noexcept`；失败返回默认构造的无效身份（全 0），
     *   不会抛异常，也不会部分写入调用方状态。
     * - 不变量：`Parse(id.ToString()) == id` 对任意有效 id 成立。
     *
     * @param text [in] 待解析文本（借用；调用期间有效即可，不保留）。
     * @return 解析成功的身份；文本非法时返回无效身份（`IsValid() == false`）。
     */
    static NodeId Parse(std::string_view text) noexcept;

    /**
     * @brief `Parse` 的出参版本，便于区分「解析失败」与「恰好为全 0」。
     * @param text [in] 待解析文本（借用；不保留）。
     * @param out [out] 成功写入解析结果；失败时**保持原值不变**，可为 nullptr。
     * @return 成功 true；`out == nullptr` 或文本非法返回 false。
     */
    static bool TryParse(std::string_view text, NodeId* out) noexcept;

    /**
     * @brief 解析并**区分「文本非法」与「合法但全 0」**。
     *
     * @param text [in] 待解析文本（借用；调用期间有效，不保留）。
     * @return `kOk` + 解析结果（可能 `IsValid() == false`，表示配置里的
     *         「未设置」哨兵）；`kInvalidArgument` 表示文本非法，调用方
     *         **必须**拒绝该配置而不是当成「未设置」。
     *
     * ## 定位
     * 配置/协议边界上需要「解析成功但值是全 0」与「解析失败」不同处理的
     * 唯一入口。`Parse()` 两者都返回全 0，只能靠 `IsValid()` 猜；本函数
     * 把失败变成显式状态码，避免把拼错的节点 id 静默当成「未设置」。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程，纯函数；
     * - 失败语义：不抛异常；失败不写入任何调用方状态。
     */
    static Result<NodeId> ParseChecked(std::string_view text) noexcept;

    /// @brief 高 64 位（进程级随机区）。
    constexpr std::uint64_t high() const noexcept { return m_high; }
    /// @brief 低 64 位（epoch 毫秒 + 原子计数器）。
    constexpr std::uint64_t low() const noexcept { return m_low; }
    /// @brief 两个半区是否都不为 0；全 0 表示「未设置」。
    constexpr bool IsValid() const noexcept { return m_high != 0 || m_low != 0; }

    /**
     * @brief 转成 32 个小写十六进制字符。
     * @return 固定长度文本；无效身份输出 32 个 `'0'`。
     */
    std::string ToString() const;

    /// @brief 比较两个半区；两个无效身份相等。
    friend constexpr bool operator==(const NodeId& lhs, const NodeId& rhs) noexcept {
        return lhs.m_high == rhs.m_high && lhs.m_low == rhs.m_low;
    }
    friend constexpr bool operator!=(const NodeId& lhs, const NodeId& rhs) noexcept {
        return !(lhs == rhs);
    }
    /// @brief 全序：先高半区、再低半区（供 `std::map` / `std::set` 使用）。
    friend constexpr bool operator<(const NodeId& lhs, const NodeId& rhs) noexcept {
        return lhs.m_high < rhs.m_high ||
               (lhs.m_high == rhs.m_high && lhs.m_low < rhs.m_low);
    }

private:
    std::uint64_t m_high{0};  ///< 进程级随机区。
    std::uint64_t m_low{0};   ///< 时间 + 计数器区。
};

} // namespace go2cpp::distributed
