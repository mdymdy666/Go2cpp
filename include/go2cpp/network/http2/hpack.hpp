/**
 * @file hpack.hpp
 * @brief HTTP/2 头部压缩（HPACK，RFC 7541）的编解码实现。
 *
 * ## 为什么从 HPACK 开始
 *
 * HTTP/2 可以分成三块：**帧层**（RFC 9113 §4/§6）、**头部压缩**（RFC 7541）、
 * **连接/流状态机**。前两块是**纯逻辑**，与并发模型无关，可以直接照标准实现；
 * 只有第三块（以及 IO 等待）需要适配本项目的 Fiber 模型。
 *
 * 所以先做 HPACK：边界清晰、可完整验证、不涉及任何 IO 或线程。
 *
 * ## 已实现范围
 *
 * - 整数前缀编码（RFC 7541 §5.1）
 * - 字符串字面量（§5.2，支持 Huffman 编解码）
 * - 静态表 61 项（§2.3.1 / 附录 A）
 * - 动态表（§2.3.2，FIFO + 大小上限 + 逐出）
 * - 索引字段（§6.1）与字面量表示（§6.2 的三种：含增量索引、不含索引、绝不索引）
 * - 表大小更新（§6.3）
 *
 * ## 未实现（明确标注，不假装支持）
 *
 * **Huffman 编解码（§5.2）已在 `huffman.hpp` 中实现**：257 项码表来自 RFC 7541
 * 附录 B（经 Go 的 x/net/http2/hpack/tables.go 提取校验）。解码支持 Huffman，
 * 编码默认启用 Huffman。填充非法或出现 EOS 返回 `kHuffmanInvalid`。
 *
 * ## 本文件定义什么
 * - `HeaderField`：一个 header 名值对（HTTP/2 里名字必须小写）；
 * - `HpackStatus`：编解码结果码；
 * - `DecodeInteger()` / `EncodeInteger()`：RFC 7541 §5.1 的前缀整数；
 * - `StaticTableEntry()` / `FindStaticIndex()` / `FindStaticNameIndex()`：61 项静态表；
 * - `DynamicTable`：FIFO 动态表（含 O(1) 的「名字+值」查找）；
 * - `Decoder` / `Encoder`：整块 header block 的编解码。
 *
 * ## 阅读建议
 * 先看 `DynamicTable` 的索引口径（相对索引 1 是表首最新条目），再看
 * `Decoder::DecodeBlock()` 的四种表示（索引 / 含增量索引 / 不含索引 / 绝不索引），
 * 最后看 `Encoder::EncodeBlock()` 的择优顺序。
 *
 * ## 边界与陷阱
 * - 静态表索引 **1-based**，动态表相对索引 **1 表示最新**，拼接时容易差 61；
 * - 编码器会写动态表、解码器也会；两侧表必须同步，否则索引解释错位；
 * - `DecodeBlock()` 的 `kNeedMore` 表示「输入不完整」，调用方可以等更多字节后重试。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 一个头部字段。 */
/**
 * @brief 一个头部字段。
 *
 * ## 定位
 * `Connection` 与 `IncomingEvent` 之间的通用 header 表示，也是 HPACK 层的输入输出单位。
 */
struct HeaderField final {
    std::string name;
    std::string value;

    bool operator==(const HeaderField& other) const noexcept {
        return name == other.name && value == other.value;
    }
};

/** @brief HPACK 解码/编码的错误码。 */
enum class HpackStatus : std::uint8_t {
    kOk,
    /// 输入不完整（增量解析时需要更多字节）。
    kNeedMore,
    /// 索引越界。
    kIndexOutOfRange,
    /// 整数编码非法（溢出或前缀位非法）。
    kInvalidInteger,
    /// 字符串长度超出剩余输入。
    kTruncatedString,
    /// 遇到 Huffman 编码的字符串，但本实现尚未支持。
    /// @note 已经支持 Huffman；本值保留以兼容旧代码/测试。
    kHuffmanUnsupported,
    /// Huffman 字符串解码失败（填充非法或出现 EOS）。
    kHuffmanInvalid,
    /// 动态表大小更新值非法（超过 SETTINGS 上限）。
    kInvalidTableSize,
};

/** @brief 状态转字符串。 */
const char* ToString(HpackStatus status) noexcept;

/**
 * @brief 解码一个前缀整数（RFC 7541 §5.1）。
 * @param in 输入缓冲。
 * @param length 可用字节数。
 * @param prefix_bits 前缀位宽（1~8）；本实现常用 4/5/6/7。
 * @param out 成功时写入值。
 * @param consumed 成功时写入消耗字节数。
 * @return 状态。`kNeedMore` 表示后续还有续字节但输入已耗尽。
 * @note 解码时 `in[0]` 的高位（前缀之上的位）由调用方负责解释；
 *       本函数只使用低 `prefix_bits` 位与后续续字节。
 */
HpackStatus DecodeInteger(const std::uint8_t* in, std::size_t length,
                          unsigned prefix_bits, std::uint64_t* out,
                          std::size_t* consumed) noexcept;

/**
 * @brief 编码一个前缀整数，写入 `out`。
 * @param value 待编码值。
 * @param prefix_bits 前缀位宽。
 * @param first_byte_high_bits `out[0]` 的高位（前缀之上的位），例如 0x40 表示
 *        "增量索引 + 索引前缀"。
 * @param out 输出缓冲，至少 (1 + 10) 字节。
 * @param out_length `out` 容量。
 * @param written 成功时写入字节数。
 */
HpackStatus EncodeInteger(std::uint64_t value, unsigned prefix_bits,
                          std::uint8_t first_byte_high_bits, std::uint8_t* out,
                          std::size_t out_length, std::size_t* written) noexcept;

/** @brief 静态表条目数（61）。 */
std::size_t StaticTableSize() noexcept;

/**
 * @brief 取静态表条目（**1-based**，与 HPACK 索引一致）。
 * @return 索引在 [1, StaticTableSize()] 内返回条目，否则返回 nullptr。
 */
const HeaderField* StaticTableEntry(std::size_t index) noexcept;

/**
 * @brief 在静态表里查找完全匹配（name+value）的索引；找不到返回 0。
 */
std::size_t FindStaticIndex(const std::string& name,
                            const std::string& value) noexcept;

/**
 * @brief 在静态表里查找 name 匹配的索引；找不到返回 0。
 */
std::size_t FindStaticNameIndex(const std::string& name) noexcept;

/**
 * @brief HPACK 动态表：FIFO，按 `SETTINGS_HEADER_TABLE_SIZE` 限制总大小。
 * @details 条目大小按 RFC 7541 §4.1 计算：`name.size() + value.size() + 32`。
 *          插入导致超限时按 FIFO 逐出，直到装得下或表为空。**不修改调用方传入的
 *          name/value**（内部拷贝）。
 *
 * ## 定位
 * 被 `Decoder` 与 `Encoder` 各持有一份（收发方向的表互相独立）；
 * `Connection` 通过 SETTINGS_HEADER_TABLE_SIZE 间接控制它的大小。
 *
 * ## 不变量
 * - `m_size` 恒等于所有条目 `name+value+32` 之和，且 `<= m_max_size`；
 * - `m_entries[0]` 永远是最新条目（HPACK 相对索引 1）；
 * - `m_seqs` 与 `m_entries` 等长，序号单调递增，逐出只从尾部发生。
 */
class DynamicTable final {
public:
    explicit DynamicTable(std::size_t max_size = 4096) noexcept;

    /** @brief 设置上限；若当前大小超限会立即逐出。 */
    void SetMaxSize(std::size_t max_size) noexcept;

    std::size_t MaxSize() const noexcept { return m_max_size; }
    std::size_t Size() const noexcept { return m_size; }
    std::size_t Count() const noexcept { return m_entries.size(); }
    /** @brief 清空（用于连接重建）。 */
    void Clear() noexcept;

    /**
     * @brief 插入一个新条目到表首（HPACK 索引 62 对应最新条目）。
     * @return 插入后是否留下（条目本身大于上限时不插入，返回 false）。
     */
    bool Add(std::string name, std::string value);

    /**
     * @brief 按**动态表相对索引**取条目（1 表示最新插入的）。
     * @return 越界返回 nullptr。
     */
    const HeaderField* Get(std::size_t relative_index) const noexcept;

    /**
     * @brief 查找完全匹配 (name, value) 的相对索引；找不到返回 0。
     * @details **O(1)**：内部维护 `name\0value -> 单调序号` 的哈希索引，
     *          相对索引由 `序号` 与当前计数器直接推出，不需要扫描。
     *          这一点很关键 —— 最初用线性扫描实现，压测显示连接复用场景
     *          吞吐下降 36%（每个 header 字段都要扫最多 100 个条目）。
     */
    std::size_t FindRelative(const std::string& name,
                             const std::string& value) const noexcept;

private:
    static std::size_t EntrySize(const std::string& name, const std::string& value) noexcept;
    void EvictUntilFit(std::size_t incoming_size) noexcept;
    /// 用 string_view 计算组合哈希，**不分配内存**（这一点很关键，见 FindRelative 注释）。
    static std::size_t HashKey(std::string_view name, std::string_view value) noexcept;
    /// 按单调序号取条目；序号已被逐出时返回 nullptr。
    const HeaderField* EntryBySeq(std::uint64_t seq) const noexcept;

    std::size_t m_max_size;
    std::size_t m_size{0};
    /// 表首是最新条目（HPACK 相对索引 1）。
    std::vector<HeaderField> m_entries;
    /// 每个条目的单调序号；最新条目的序号是 m_next_seq - 1。
    /// 相对索引 = m_next_seq - seq，因此查找是 O(1)。
    std::vector<std::uint64_t> m_seqs;
    std::uint64_t m_next_seq{1};
    /// 组合哈希 -> 序号（允许多个条目共享哈希，冲突时逐个比对真实字符串）。
    std::unordered_multimap<std::size_t, std::uint64_t> m_index;
};

/**
 * @brief HPACK 解码器（服务端与客户端通用）。
 * @details 持有动态表与当前上限。解码过程中若遇到未知索引或 Huffman 字符串，
 *          返回错误并**不修改**动态表已成功应用的部分（HPACK 要求整块要么
 *          全部成功，要么连接出错；这里保守地先解析到临时表再提交）。
 *
 * ## 定位
 * 由 `Connection::m_decoder` 持有，只在 `ReadEvent()` 的 HEADERS 分支调用；
 * 测试里可单独用来验证 RFC 7541 附录 C 的样例。
 *
 * ## 不变量
 * - `m_settings_max` 是本端声明的接收上限，表大小更新超过它即 `kInvalidTableSize`；
 * - 解码失败时动态表可能已部分变更（不做事后回滚），因此失败后应断连而不是重试。
 */
class Decoder final {
public:
    explicit Decoder(std::size_t max_table_size = 4096) noexcept;

    /**
     * @brief 解码一整块 header block。
     * @param in 输入缓冲。
     * @param length 可用字节数。
     * @param out 成功时追加解码出的字段（不清空调用方已有的内容）。
     * @param error 失败时写入中文诊断。
     * @return 状态。`kNeedMore` 表示输入不完整，调用方可等更多数据后重试。
     */
    HpackStatus DecodeBlock(const std::uint8_t* in, std::size_t length,
                            std::vector<HeaderField>* out,
                            std::string* error = nullptr);

    /** @brief 设置动态表上限（来自 SETTINGS）。 */
    void SetMaxTableSize(std::size_t max_size) noexcept;

    /** @brief 当前动态表（只读）。 */
    const DynamicTable& Table() const noexcept { return m_table; }

private:
    DynamicTable m_table;
    /// 本端声明接收的最大表大小（SETTINGS_HEADER_TABLE_SIZE），
    /// 用于校验对端发来的表大小更新是否越界。
    std::size_t m_settings_max{4096};
};

/**
 * @brief HPACK 编码器。
 * @details 默认策略：能命中静态表就用索引；否则用「含增量索引的字面量」，
 *          并写入动态表。**不做 Huffman 编码**（见文件头说明）。
 *
 * ## 定位
 * 由 `Connection::m_encoder` 持有，`StartRequest()` 与 `SendResponseHeaders()` 调用；
 * 对端 SETTINGS 到达后由 `FinishSettingsHandshake()` 调 `SetTableSize()` 收缩。
 *
 * ## 不变量
 * - 命中静态表 -> 索引用 7 位前缀；否则「含增量索引字面量」用 6 位前缀；
 * - 值只在 Huffman 结果更短时才用 Huffman（避免长随机串膨胀）；
 * - 每次写「含增量索引」表示都会同步 `m_table.Add()`，两端表因此保持一致。
 */
class Encoder final {
public:
    explicit Encoder(std::size_t max_table_size = 4096) noexcept;

    /**
     * @brief 编码一组 header 字段。
     * @param fields 待编码字段（顺序保留）。
     * @param out 追加写入的字节。
     * @return 状态。
     */
    HpackStatus EncodeBlock(const std::vector<HeaderField>& fields,
                            std::vector<std::uint8_t>* out,
                            std::string* error = nullptr);

    /** @brief 追加一条动态表大小更新（§6.3）；通常在块首发送。 */
    HpackStatus AppendTableSizeUpdate(std::size_t new_size,
                                      std::vector<std::uint8_t>* out);

    /**
     * @brief 应用对端 `SETTINGS_HEADER_TABLE_SIZE`：只更新允许上限。
     * @details 对端声明的是"它愿意接收多大"，因此：
     *          - **收缩**：立即把动态表裁到新上限（绝不能让表超过对端上限）；
     *          - **扩容**：本函数**不**自动成长。扩容属于「改变编码器动态表大小」，
     *            按 RFC 7541 §4.2 必须先在下一个 header block 首部发送
     *            `001xxxxx` 表大小更新，否则对端解码器仍按旧上限解释索引，
     *            会因索引越界以 COMPRESSION_ERROR 断连（真实互通故障）。
     *            需要扩容时调用方应调用 `AppendTableSizeUpdate()`，由它同时
     *            写更新字节并推进本地表上限。
     */
    void SetTableSize(std::size_t max_size) noexcept {
        m_settings_max = max_size;
        if (m_table.MaxSize() > max_size) {
            m_table.SetMaxSize(max_size);
        }
    }

    /** @brief 当前动态表（只读，便于测试断言）。 */
    const DynamicTable& Table() const noexcept { return m_table; }

    /**
     * @brief 在动态表里查找完全匹配（name+value）的 HPACK 索引；找不到返回 0。
     * @details 返回的是可以直接写入「索引字段」的完整索引（62 起）。
     *          复杂度 O(表内条目数)，通常 < 100，远小于重发完整字符串的代价。
     */
    std::size_t FindDynamicIndex(const std::string& name,
                                 const std::string& value) const noexcept;

private:
    DynamicTable m_table;
    std::size_t m_settings_max{4096};
};

}  // namespace go2cpp::net::http2
