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
 * - 字符串字面量（§5.2，**仅原样，不含 Huffman**）
 * - 静态表 61 项（§2.3.1 / 附录 A）
 * - 动态表（§2.3.2，FIFO + 大小上限 + 逐出）
 * - 索引字段（§6.1）与字面量表示（§6.2 的三种：含增量索引、不含索引、绝不索引）
 * - 表大小更新（§6.3）
 *
 * ## 未实现（明确标注，不假装支持）
 *
 * **Huffman 解码与编码（§5.2）**。真实对端几乎总是用 Huffman 编码 header，
 * 因此本实现目前**不能与真实 HTTP/2 服务器互通**；解码遇到 Huffman 位会返回
 * 明确错误 `kHuffmanUnsupported` 而不是静默出错。补上 Huffman 需要 257 项码表
 * （RFC 7541 附录 B），属下一步。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 一个头部字段。 */
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
    kHuffmanUnsupported,
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

private:
    static std::size_t EntrySize(const std::string& name, const std::string& value) noexcept;
    void EvictUntilFit(std::size_t incoming_size) noexcept;

    std::size_t m_max_size;
    std::size_t m_size{0};
    /// 表首是最新条目（HPACK 相对索引 1）。
    std::vector<HeaderField> m_entries;
};

/**
 * @brief HPACK 解码器（服务端与客户端通用）。
 * @details 持有动态表与当前上限。解码过程中若遇到未知索引或 Huffman 字符串，
 *          返回错误并**不修改**动态表已成功应用的部分（HPACK 要求整块要么
 *          全部成功，要么连接出错；这里保守地先解析到临时表再提交）。
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

    /** @brief 当前动态表（只读，便于测试断言）。 */
    const DynamicTable& Table() const noexcept { return m_table; }

private:
    DynamicTable m_table;
    std::size_t m_settings_max{4096};
};

}  // namespace go2cpp::net::http2
