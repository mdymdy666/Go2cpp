#pragma once

/**
 * @file byte_array.hpp
 * @brief 面向协议编解码的可增长字节缓冲。
 * @details 默认小端用于本地序列化；网络协议应显式调用 set_little_endian(false)。
 *          ByteArray 是单所有者对象，读写游标不能被多个线程同时修改。
 *
 * ## 模块定位
 * 协议编解码用的可增长字节缓冲：定长整数、变长整数（protobuf 风格）、长度前缀
 * 字符串与 iovec 批量读写。被网络层与协议示例使用；不依赖 Socket 或调度器。
 *
 * ## 本文件定义什么
 * - `ByteOrder`：字节序（默认小端，为本地序列化优化）；
 * - `ByteArray`：读/写游标 + 可增长缓冲 + 定长/变长/字符串编解码 + iovec 视图。
 *
 * ## 阅读建议
 * 先看 `m_position`（读游标）与 `m_data`（全部字节）的关系，再看 `write_*`/`read_*`
 * 成对函数，最后看 varint 的 7 位组编码与 zigzag。
 *
 * ## 边界与陷阱
 * - 默认**小端**；网络协议必须显式 `set_little_endian(false)`，否则线上字节序错；
 * - 单所有者对象：读写游标不能被多个线程同时修改（非线程安全）；
 * - `read_*` 越界由 `ensure_read()` 抛 `std::length_error`，不会返回部分数据；
 * - varint 是 **protobuf 语义**（7 位组 + zigzag），不要与 LEB128 无符号变体混用。
 */

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/uio.h>

namespace go2cpp::net {

/** @brief 字节序。 */
enum class ByteOrder { Little, Big };

/** @brief 可增长的协议字节缓冲，提供定长、变长和长度前缀字符串操作。 */
/**
 * @brief 可增长的协议字节缓冲，提供定长、变长和长度前缀字符串操作。
 *
 * ## 定位
 * 被协议代码/示例当作双向缓冲：写入用 `write_*`，读取用 `read_*`；
 * `get_read_buffers()` / `get_write_buffers()` 供 socket 的 `writev`/`readv` 零拷贝使用。
 *
 * ## 不变量
 * - `m_position <= m_data.size()`：读游标不越过已写内容；
 * - 写总在末尾追加，读只推进 `m_position`；
 * - `readable_size() == m_data.size() - m_position`。
 *
 * ## 状态机
 * 没有显式状态机：`m_position` 是唯一游标，`clear()` 复位、`set_position()` 可回退
 * （调用方负责不越界）。
 */
class ByteArray final {
public:
    explicit ByteArray(std::size_t base_size = 4096);
    std::size_t position() const noexcept { return m_position; }
    void set_position(std::size_t position);
    std::size_t size() const noexcept { return m_data.size(); }
    std::size_t capacity() const noexcept { return m_data.capacity(); }
    std::size_t readable_size() const noexcept;
    void clear() noexcept;
    ByteOrder order() const noexcept { return m_order; }
    void set_little_endian(bool little) noexcept;
    bool little_endian() const noexcept { return m_order == ByteOrder::Little; }

    void write(const void* data, std::size_t length);
    std::size_t read(void* data, std::size_t length);
    std::string to_string() const;
    std::string to_hex_string() const;

    void write_fuint8(std::uint8_t value); std::uint8_t read_fuint8();
    void write_fint8(std::int8_t value); std::int8_t read_fint8();
    void write_fuint16(std::uint16_t value); std::uint16_t read_fuint16();
    void write_fint16(std::int16_t value); std::int16_t read_fint16();
    void write_fuint32(std::uint32_t value); std::uint32_t read_fuint32();
    void write_fint32(std::int32_t value); std::int32_t read_fint32();
    void write_fuint64(std::uint64_t value); std::uint64_t read_fuint64();
    void write_fint64(std::int64_t value); std::int64_t read_fint64();
    void write_float(float value); float read_float();
    void write_double(double value); double read_double();

    void write_uint32(std::uint32_t value); std::uint32_t read_uint32();
    void write_uint64(std::uint64_t value); std::uint64_t read_uint64();
    void write_int32(std::int32_t value); std::int32_t read_int32();
    void write_int64(std::int64_t value); std::int64_t read_int64();

    void write_string_f16(const std::string& value);
    void write_string_f32(const std::string& value);
    void write_string_vint(const std::string& value);
    void write_string_without_length(const std::string& value);
    std::string read_string_f16();
    std::string read_string_f32();
    std::string read_string_vint();
    std::string read_string_without_length(std::size_t length);
    void write_varuint32(std::uint32_t value);
    /**
     * @brief 写 protobuf 风格的变长无符号整数（7 位一组）。
     *
     * ## 说明
     * 每字节低 7 位是数据、最高位是「还有后续」；64 位值最多 10 字节。
     * 对应 `read_varuint64()` 必须按同样规则循环解码。
     */
    void write_varuint64(std::uint64_t value);
    std::uint32_t read_varuint32();
    std::uint64_t read_varuint64();
    void write_varint32(std::int32_t value);
    /**
     * @brief 写 zigzag 变长有符号整数。
     *
     * ## 说明
     * 先 zigzag 映射（`(n << 1) ^ (n >> 63)`）把负数变成小正整数，再走 varuint；
     * 这样绝对值小的负数也只占 1 字节。
     */
    void write_varint64(std::int64_t value);
    std::int32_t read_varint32();
    std::int64_t read_varint64();

    /** @brief 返回从 position 开始的只读 iovec，最多 max_count 段。 */
    std::size_t get_read_buffers(std::vector<iovec>& buffers,
                                 std::size_t max_count = 16) const;
    /** @brief 返回可直接接收数据的写 iovec。 */
    std::size_t get_write_buffers(std::vector<iovec>& buffers,
                                  std::size_t length,
                                  std::size_t max_count = 16);

private:
    template <class T> void write_fixed(T value);
    template <class T> T read_fixed();
    void ensure_write(std::size_t length);
    /**
     * @brief 校验剩余可读字节足够（不足抛 std::length_error）。
     *
     * ## 说明
     * 所有 `read_*` 都先过这里：**先校验再读**，避免把半截数据交付给调用方；
     * 抛异常而不是返回 false，是为了让调用方在任何读点都不必写错误分支。
     */
    void ensure_read(std::size_t length) const;
    /// 初始容量提示；ensure_write() 扩容时参考（不参与逻辑判定）。
    std::size_t m_base_size;
    /// 全部字节（含已读部分）；write/ensure_write 追加与扩容，clear 清空。
    std::vector<std::uint8_t> m_data;
    /// 读游标；read/read_* 推进，set_position 设置，clear 清零。
    std::size_t m_position{0};
    /// 字节序；默认小端，set_little_endian 修改，read/write_fixed 使用。
    ByteOrder m_order{ByteOrder::Little};
};

}  // namespace go2cpp::net

namespace go2cpp { using net::ByteArray; using net::ByteOrder; }
