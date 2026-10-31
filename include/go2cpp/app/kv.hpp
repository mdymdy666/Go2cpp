#pragma once

/**
 * @file kv.hpp
 * @brief 有序 KV 表（`k1=v1;k2=v2;...`）：文本/二进制双编码。
 *
 * @details `KvTable` 是应用层框架的**元数据载体**：HEADERS 帧、资源元数据、
 *          资源索引、KV 流都落在它上面。设计目标是把用户最常写的
 *
 *          ```
 *          k1=v1;k2=v2;k3=v3;k4=v4;k5=v5
 *          ```
 *
 *          直接映射成有序键值表，同时提供一种**二进制紧凑编码**供帧载荷使用。
 *
 * ## 语义约定
 *
 *          - **保持插入顺序**：`entries()` 按 `Set` 的先后返回；对已存在的键
 *            `Set` 只更新值，不改变位置；
 *          - **键唯一**：`Set` 覆盖旧值；文本解析里的重复键同样后写覆盖；
 *          - **值可为空**：`k=` 合法，值为空串；
 *          - **键不可为空**：空键在解析时报错（避免 `=v` 这种歧义输入）；
 *          - 文本模式**不裁剪空白**：`"a = b"` 的键是 `"a "`、值是 `" b"`；
 *            需要语义化的空白裁剪由调用方决定；
 *          - 文本模式允许结尾 `;`、忽略完全空白的项；空字符串解析为空表。
 *
 * ## 转义
 *
 *          文本模式用反斜杠转义分隔符与反斜杠本身：`\\`、`\;`、`\=`。
 *          其余字符（含 UTF-8）原样保留。序列化总是转义，因此
 *          `ParseText(t.ToText()) == t` 对任意键值成立（随机测试覆盖）。
 *
 * ## 二进制编码
 *
 *          ```
 *          varint count
 *          count * { varint key_len, key, varint value_len, value }
 *          ```
 *
 *          上限：条目数 1M、单键/单值 16 MiB、总解码输入 256 MiB，均可在
 *          调用参数里收紧。解码失败返回 false 并给出可读错误，**不抛出异常**。
 *
 * ## 与 HTTP/2 头部的差异
 *
 *          HTTP/2 头部有小写化、伪头部、禁止重复等多个协议约束；本表刻意
 *          **不做**这些约束：应用层资源元数据需要大小写不敏感的键并不合适
 *          （`FileName` 与 `filename` 是不同业务键）。需要 HTTP 语义时由上层
 *          自行规范化，框架不替用户做有损转换。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::app {

/// @brief 有序键值表。
class KvTable {
   public:
    /// @brief 一项键值对（顺序即插入顺序）。
    using Entry = std::pair<std::string, std::string>;

    /// @brief 空表。
    KvTable() = default;

    /// @brief 从初始化列表构造（重复键后者覆盖）。
    KvTable(std::initializer_list<Entry> entries);

    /// @brief 条目数。
    std::size_t size() const noexcept { return m_entries.size(); }

    /// @brief 是否为空。
    bool empty() const noexcept { return m_entries.empty(); }

    /// @brief 全部条目（只读，顺序稳定）。
    const std::vector<Entry>& entries() const noexcept { return m_entries; }

    /// @brief 查找键；不存在返回 nullptr。
    const std::string* Find(std::string_view key) const noexcept;

    /**
     * @brief 读取值。
     * @param key 键。
     * @param fallback 键不存在时的返回值。
     * @return 值或 `fallback`。
     */
    std::string Get(std::string_view key, std::string_view fallback = {}) const;

    /**
     * @brief 写入/覆盖。
     * @return 是否新增了键（false 表示覆盖了已存在的键）。
     */
    bool Set(std::string key, std::string value);

    /// @brief 删除键；返回是否删除。
    bool Erase(std::string_view key);

    /// @brief 清空。
    void Clear() noexcept {
        m_entries.clear();
        m_index.clear();
        m_index_used = 0;
    }

    /// @brief 值是否全为可打印 ASCII（不含控制字符/非 ASCII）。
    bool PrintableAscii() const noexcept;

    /// @brief 文本编码：`k1=v1;k2=v2`（自动转义）。
    std::string ToText() const;

    /**
     * @brief 解析文本编码。
     * @param text 输入文本。
     * @param out 输出表；成功时覆盖。
     * @param error 非空时写入可读错误。
     * @return 成功返回 true；非法转义/空键/超限返回 false。
     */
    static bool ParseText(std::string_view text, KvTable* out,
                          std::string* error = nullptr);

    /// @brief 二进制编码（帧载荷用）。
    std::vector<std::uint8_t> ToBinary() const;

    /**
     * @brief 二进制编码写入既有缓冲（复用容量，热路径零分配）。
     * @param out 输出缓冲；`nullptr` 时什么都不做。写入前会 clear（保留容量）。
     */
    void ToBinaryInto(std::vector<std::uint8_t>* out) const;

    /**
     * @brief 解析二进制编码。
     * @param data 输入。
     * @param size 字节数。
     * @param out 输出表；成功时覆盖。
     * @param error 非空时写入可读错误。
     * @return 成功返回 true。
     */
    static bool ParseBinary(const std::uint8_t* data, std::size_t size, KvTable* out,
                            std::string* error = nullptr);

    /// @brief 二进制编码后的字节数（不实际分配）。
    std::size_t BinarySize() const noexcept;

    /// @brief 值相等（键、值、顺序都相同）。
    bool operator==(const KvTable& other) const noexcept {
        return m_entries == other.m_entries;
    }
    bool operator!=(const KvTable& other) const noexcept { return !(*this == other); }

   private:
    /// @brief 条目数不超过该值时只用线性扫描，不建哈希索引。
    /// @details 小表（HEADERS 常见 3-10 条）的线性比较比维护哈希表更省：
    ///          免去一次索引分配与探测，且这些键通常在同一 cache line 内。
    static constexpr std::size_t kLinearScanLimit = 8;

    /// @brief 键不存在时的哨兵下标。
    static constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

    /**
     * @brief 哈希索引槽（开放寻址 + 线性探测）。
     * @details 槽里存的是 `m_entries` 的**下标**而不是 `string_view`：
     *          `m_entries` 扩容或 `erase` 时 `Entry` 会被移动，短键（SSO）
     *          的字符数据跟着对象走，任何指向键的视图都会悬垂。存下标 +
     *          每次比较时回查 `m_entries[下标].first`，既零拷贝又不依赖地址稳定。
     *          `hash` 先做一次整数比较，冲突时才比较字符串。
     */
    struct IndexSlot {
        std::uint64_t hash = 0;         ///< 键的 64 位哈希。
        std::uint32_t entry_plus_one = 0;  ///< `m_entries` 下标 + 1；0 表示空槽。
    };

    /// @brief 键的 64 位哈希（FNV-1a）；只在进程内分槽，不上线。
    static std::uint64_t HashKey(std::string_view key) noexcept;

    /// @brief 返回键在 `m_entries` 中的下标；不存在返回 kNoIndex。
    /// @details 索引为空（小表）时退化为线性扫描，语义与旧实现完全一致。
    std::size_t Locate(std::string_view key) const noexcept;

    /// @brief 按当前 `m_entries` 重建索引；条目数 <= kLinearScanLimit 时清空索引。
    void RebuildIndex();

    /// @brief 把一条已存在的条目插入索引；调用方保证索引已分配且键不重复。
    void IndexInsert(std::size_t entry_index) noexcept;

    std::vector<Entry> m_entries;  ///< 有序条目。
    /// 哈希索引；`empty()` 表示小表/未启用，此时走线性扫描。容量始终是 2 的幂，
    /// 负载因子 <= 0.5，保证线性探测一定能遇到空槽。
    std::vector<IndexSlot> m_index;
    std::size_t m_index_used = 0;  ///< 已占用槽数（用于判断是否需要扩容）。
};

/**
 * @brief 转义文本模式的分隔符/反斜杠。
 * @details 只转义 `\`、`;`、`=`，其余字符（含 UTF-8）原样输出。
 */
std::string EscapeKvText(std::string_view value);

/**
 * @brief 把任意字符串渲染成可打印形式（日志用；不可打印字节转 `\xNN`）。
 */
std::string PrintablePreview(std::string_view value, std::size_t max_bytes = 64);

}  // namespace go2cpp::app
