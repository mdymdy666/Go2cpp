/**
 * @file snapshot_codec.hpp
 * @brief 持久化格式的公共编解码原语：CRC32 + 定长大端读写 + 长度前缀字符串。
 *
 * ## 模块定位
 *
 * `metadata_store.cpp`（WAL 记录）与 `coordinator_snapshot.cpp`（协调者快照）
 * 都需要「自描述、可校验、截断可检出」的二进制格式。本头文件把这两个文件
 * 共用的字节序、长度前缀与 CRC32 收拢到一处，**不对外暴露**（位于
 * `src/distributed/`，只被本模块的实现文件包含）。
 *
 * ## 格式约定
 *
 * - 全部整数使用**大端**（网络序）：跨架构读取时不需要额外字节序判断；
 * - 字符串 = `u32 长度` + 原始字节，长度上限由调用方传入（防御损坏文件
 *   触发超大分配）；
 * - CRC32 使用 IEEE 多项式（与 zlib/PNG 一致），只做**完整性**校验，不做
 *   安全认证。
 *
 * ## 失败语义
 *
 * `SnapshotReader` 的所有读取函数在越界/长度非法时返回 `false` 并保持
 * 输出参数原值；调用方必须检查返回值并放弃整份快照（**不能**部分采用）。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::distributed::detail {

/// @brief IEEE CRC32（表驱动；表用 C++11 静态初始化，线程安全）。
inline std::uint32_t Crc32(const void *data, std::size_t size) noexcept {
    static const auto table = [] {
        static std::uint32_t storage[256];
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
            }
            storage[index] = value;
        }
        return &storage[0];
    }();
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

/// @brief 追加式写入器：定长大端编码 + 长度前缀字符串。
class SnapshotWriter {
  public:
    void U8(std::uint8_t value) { buffer.push_back(static_cast<char>(value)); }

    void U16(std::uint16_t value) {
        buffer.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        buffer.push_back(static_cast<char>(value & 0xFFU));
    }

    void U32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            buffer.push_back(
                static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }

    void U64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer.push_back(
                static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }

    /// @brief 写入 `u32 长度 + 字节`；调用方保证长度不超过 `u32` 上限。
    void String(std::string_view value) {
        U32(static_cast<std::uint32_t>(value.size()));
        buffer.append(value.data(), value.size());
    }

    /// @brief 在末尾追加 CRC32（覆盖此前全部内容）。
    void AppendCrc() { U32(Crc32(buffer.data(), buffer.size())); }

    std::string buffer;
};

/// @brief 读取器：任意一步失败都返回 false，输出参数保持原值。
class SnapshotReader {
  public:
    explicit SnapshotReader(std::string_view data) : m_data(data) {}

    bool U8(std::uint8_t *out) {
        if (!Take(1)) {
            return false;
        }
        *out = static_cast<std::uint8_t>(m_data[m_pos - 1]);
        return true;
    }

    bool U16(std::uint16_t *out) {
        if (!Take(2)) {
            return false;
        }
        *out = static_cast<std::uint16_t>(
            (static_cast<std::uint8_t>(m_data[m_pos - 2]) << 8U) |
            static_cast<std::uint8_t>(m_data[m_pos - 1]));
        return true;
    }

    bool U32(std::uint32_t *out) {
        std::uint32_t value = 0;
        if (!Take(4)) {
            return false;
        }
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | static_cast<std::uint8_t>(m_data[m_pos - 4 + index]);
        }
        *out = value;
        return true;
    }

    bool U64(std::uint64_t *out) {
        std::uint64_t value = 0;
        if (!Take(8)) {
            return false;
        }
        for (std::size_t index = 0; index < 8; ++index) {
            value = (value << 8U) | static_cast<std::uint8_t>(m_data[m_pos - 8 + index]);
        }
        *out = value;
        return true;
    }

    /// @brief 读 `u32 长度 + 字节`；长度超过 `max_size` 视为损坏。
    bool String(std::string *out, std::size_t max_size = std::size_t{1} << 20) {
        std::uint32_t size = 0;
        if (!U32(&size) || size > max_size || !Take(size)) {
            return false;
        }
        out->assign(m_data.data() + m_pos - size, size);
        return true;
    }

    /// @brief 跳过 `size` 字节（校验尾部 CRC 时用）。
    bool Skip(std::size_t size) { return Take(size); }

    /// @brief 从 `offset` 开始的 `size` 字节 CRC；调用方保证范围合法。
    std::uint32_t CrcAt(std::size_t offset, std::size_t size) const {
        return Crc32(m_data.data() + offset, size);
    }

    std::size_t position() const noexcept { return m_pos; }
    std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    std::string_view data() const noexcept { return m_data; }

  private:
    bool Take(std::size_t size) {
        if (size > m_data.size() - m_pos) {
            return false;
        }
        m_pos += size;
        return true;
    }

    std::string_view m_data;
    std::size_t m_pos{0};
};

} // namespace go2cpp::distributed::detail
