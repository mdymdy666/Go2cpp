#pragma once

/**
 * @file byte_array.hpp
 * @brief 面向协议编解码的可增长字节缓冲。
 * @details 默认小端用于本地序列化；网络协议应显式调用 set_little_endian(false)。
 *          ByteArray 是单所有者对象，读写游标不能被多个线程同时修改。
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
    void write_varuint64(std::uint64_t value);
    std::uint32_t read_varuint32();
    std::uint64_t read_varuint64();
    void write_varint32(std::int32_t value);
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
    void ensure_read(std::size_t length) const;
    std::size_t m_base_size;
    std::vector<std::uint8_t> m_data;
    std::size_t m_position{0};
    ByteOrder m_order{ByteOrder::Little};
};

}  // namespace go2cpp::net

namespace go2cpp { using net::ByteArray; using net::ByteOrder; }
