/**
 * @file byte_array.cpp
 * @brief 实现单所有者协议缓冲的游标、定长/变长整数和字符串编码。
 * @details 读操作统一做边界检查，变长整数拒绝溢出，长度前缀先校验再分配。
 */
#include "go2cpp/network/byte_array.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace go2cpp::net {

namespace {
template <class T> T reverse_bytes(T value) {
    T result{};
    auto* source = reinterpret_cast<const std::uint8_t*>(&value);
    auto* target = reinterpret_cast<std::uint8_t*>(&result);
    for (std::size_t i = 0; i < sizeof(T); ++i) target[i] = source[sizeof(T) - i - 1];
    return result;
}
}

ByteArray::ByteArray(std::size_t base_size) : m_base_size(std::max<std::size_t>(base_size, 1)) {
    m_data.reserve(m_base_size);
}
void ByteArray::set_position(std::size_t position) {
    if (position > m_data.size()) throw std::out_of_range("ByteArray position");
    m_position = position;
}
std::size_t ByteArray::readable_size() const noexcept {
    return m_position <= m_data.size() ? m_data.size() - m_position : 0;
}
void ByteArray::clear() noexcept { m_data.clear(); m_position = 0; }
void ByteArray::set_little_endian(bool little) noexcept {
    m_order = little ? ByteOrder::Little : ByteOrder::Big;
}
void ByteArray::ensure_write(std::size_t length) {
    if (length > std::numeric_limits<std::size_t>::max() - m_position)
        throw std::length_error("ByteArray size overflow");
    const std::size_t required = m_position + length;
    if (required > m_data.size()) m_data.resize(required);
}
void ByteArray::ensure_read(std::size_t length) const {
    if (length > readable_size()) throw std::out_of_range("ByteArray read beyond end");
}
void ByteArray::write(const void* data, std::size_t length) {
    if (length && !data) throw std::invalid_argument("ByteArray null data");
    ensure_write(length);
    if (length) std::memcpy(m_data.data() + m_position, data, length);
    m_position += length;
}
std::size_t ByteArray::read(void* data, std::size_t length) {
    if (length && !data) throw std::invalid_argument("ByteArray null output");
    ensure_read(length);
    if (length) std::memcpy(data, m_data.data() + m_position, length);
    m_position += length;
    return length;
}
std::string ByteArray::to_string() const {
    return std::string(reinterpret_cast<const char*>(m_data.data()), m_data.size());
}
std::string ByteArray::to_hex_string() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (auto value : m_data) stream << std::setw(2) << static_cast<unsigned>(value);
    return stream.str();
}

template <class T> void ByteArray::write_fixed(T value) {
    if (m_order == ByteOrder::Big) value = reverse_bytes(value);
    write(&value, sizeof(value));
}
template <class T> T ByteArray::read_fixed() {
    T value{};
    read(&value, sizeof(value));
    if (m_order == ByteOrder::Big) value = reverse_bytes(value);
    return value;
}
void ByteArray::write_fuint8(std::uint8_t value) { write_fixed(value); }
std::uint8_t ByteArray::read_fuint8() { return read_fixed<std::uint8_t>(); }
void ByteArray::write_fint8(std::int8_t value) { write_fixed(value); }
std::int8_t ByteArray::read_fint8() { return read_fixed<std::int8_t>(); }
void ByteArray::write_fuint16(std::uint16_t value) { write_fixed(value); }
std::uint16_t ByteArray::read_fuint16() { return read_fixed<std::uint16_t>(); }
void ByteArray::write_fint16(std::int16_t value) { write_fixed(value); }
std::int16_t ByteArray::read_fint16() { return read_fixed<std::int16_t>(); }
void ByteArray::write_fuint32(std::uint32_t value) { write_fixed(value); }
std::uint32_t ByteArray::read_fuint32() { return read_fixed<std::uint32_t>(); }
void ByteArray::write_fint32(std::int32_t value) { write_fixed(value); }
std::int32_t ByteArray::read_fint32() { return read_fixed<std::int32_t>(); }
void ByteArray::write_fuint64(std::uint64_t value) { write_fixed(value); }
std::uint64_t ByteArray::read_fuint64() { return read_fixed<std::uint64_t>(); }
void ByteArray::write_fint64(std::int64_t value) { write_fixed(value); }
std::int64_t ByteArray::read_fint64() { return read_fixed<std::int64_t>(); }
void ByteArray::write_float(float value) { static_assert(sizeof(float) == 4); write_fixed(value); }
float ByteArray::read_float() { return read_fixed<float>(); }
void ByteArray::write_double(double value) { static_assert(sizeof(double) == 8); write_fixed(value); }
double ByteArray::read_double() { return read_fixed<double>(); }
void ByteArray::write_uint32(std::uint32_t value) { write_varuint32(value); }
std::uint32_t ByteArray::read_uint32() { return read_varuint32(); }
void ByteArray::write_uint64(std::uint64_t value) { write_varuint64(value); }
std::uint64_t ByteArray::read_uint64() { return read_varuint64(); }
void ByteArray::write_int32(std::int32_t value) { write_varint32(value); }
std::int32_t ByteArray::read_int32() { return read_varint32(); }
void ByteArray::write_int64(std::int64_t value) { write_varint64(value); }
std::int64_t ByteArray::read_int64() { return read_varint64(); }

void ByteArray::write_varuint32(std::uint32_t value) {
    while (value >= 0x80) { write_fuint8(static_cast<std::uint8_t>(value | 0x80)); value >>= 7; }
    write_fuint8(static_cast<std::uint8_t>(value));
}
void ByteArray::write_varuint64(std::uint64_t value) {
    while (value >= 0x80) { write_fuint8(static_cast<std::uint8_t>(value | 0x80)); value >>= 7; }
    write_fuint8(static_cast<std::uint8_t>(value));
}
std::uint32_t ByteArray::read_varuint32() {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const auto byte = read_fuint8();
        if (i == 4 && (byte & 0x70U)) throw std::range_error("varuint32 overflow");
        result |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * i);
        if (!(byte & 0x80U)) return result;
    }
    throw std::range_error("varuint32 too long");
}
std::uint64_t ByteArray::read_varuint64() {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 10; ++i) {
        const auto byte = read_fuint8();
        if (i == 9 && (byte & 0x7eU)) throw std::range_error("varuint64 overflow");
        result |= static_cast<std::uint64_t>(byte & 0x7fU) << (7 * i);
        if (!(byte & 0x80U)) return result;
    }
    throw std::range_error("varuint64 too long");
}
void ByteArray::write_varint32(std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    write_varuint32((bits << 1) ^ static_cast<std::uint32_t>(value < 0 ? 0xffffffffU : 0U));
}
void ByteArray::write_varint64(std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    write_varuint64((bits << 1) ^ static_cast<std::uint64_t>(value < 0 ? 0xffffffffffffffffULL : 0ULL));
}
std::int32_t ByteArray::read_varint32() {
    const auto value = read_varuint32();
    return static_cast<std::int32_t>((value >> 1) ^ (-(value & 1)));
}
std::int64_t ByteArray::read_varint64() {
    const auto value = read_varuint64();
    return static_cast<std::int64_t>((value >> 1) ^ (-(value & 1)));
}

void ByteArray::write_string_f16(const std::string& value) {
    if (value.size() > 0xffffU) throw std::length_error("StringF16 too long");
    write_fuint16(static_cast<std::uint16_t>(value.size())); write(value.data(), value.size());
}
void ByteArray::write_string_f32(const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("StringF32 too long");
    write_fuint32(static_cast<std::uint32_t>(value.size())); write(value.data(), value.size());
}
void ByteArray::write_string_vint(const std::string& value) { write_varuint64(value.size()); write(value.data(), value.size()); }
void ByteArray::write_string_without_length(const std::string& value) { write(value.data(), value.size()); }
std::string ByteArray::read_string_f16() { const auto length = read_fuint16(); return read_string_without_length(length); }
std::string ByteArray::read_string_f32() { const auto length = read_fuint32(); return read_string_without_length(length); }
std::string ByteArray::read_string_vint() {
    const auto length = read_varuint64();
    if (length > 64ULL * 1024ULL * 1024ULL) throw std::length_error("string too long");
    return read_string_without_length(static_cast<std::size_t>(length));
}
std::string ByteArray::read_string_without_length(std::size_t length) {
    ensure_read(length);
    std::string value(reinterpret_cast<const char*>(m_data.data() + m_position), length);
    m_position += length;
    return value;
}

std::size_t ByteArray::get_read_buffers(std::vector<iovec>& buffers, std::size_t max_count) const {
    buffers.clear();
    if (!max_count || readable_size() == 0) return 0;
    buffers.push_back(iovec{const_cast<std::uint8_t*>(m_data.data() + m_position), readable_size()});
    return 1;
}
std::size_t ByteArray::get_write_buffers(std::vector<iovec>& buffers, std::size_t length,
                                         std::size_t max_count) {
    buffers.clear();
    if (!max_count) return 0;
    ensure_write(length);
    buffers.push_back(iovec{m_data.data() + m_position, length});
    m_position += length;
    return 1;
}

}  // namespace go2cpp::net
