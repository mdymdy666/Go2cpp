#pragma once

/**
 * @file buffer.hpp
 * @brief 编解码字节缓冲：varint、字符串、原始字节的读写与错误状态。
 *
 * @details 这个 Buffer 同时承担「写入器」和「读取器」两个角色：编码时只追加，
 *          解码时从 `m_read` 位置顺序消费。所有方法失败时都会把 `ok()` 置为
 *          false 并返回 false；调用方可以在任意一步之后停止检查，最后统一看
 *          `ok()`——这是为了让字段序列化代码保持线性、不写嵌套 if。
 *
 *          为什么不用 `std::vector` 直接拼：一是要把「长度前缀 + 载荷」的
 *          成对约束收敛到一处，二是读路径必须在分配前校验声明长度，防止
 *          攻击者用一个 4 GiB 的字符串长度触发 OOM。因此 `GetString` 会先检查
 *          `length <= Remaining()`，再分配。
 *
 *          整数编码：
 *          - 无符号整数用 LEB128 varint（小端 7 位组）；
 *          - 有符号整数先 zigzag 再 varint，使 -1/1 都只占 1 字节；
 *          - varint 最多 10 字节，第 10 字节只允许 0/1，超出的位组合视为
 *            溢出并判为坏数据（而不是静默截断）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::reflect {

/// @brief 单个 Buffer 的字节上限（64 MiB）。超过视为协议错误而非合法消息。
inline constexpr std::size_t kMaxBufferBytes = 64u * 1024u * 1024u;

/// @brief 单个字符串字段的上限（16 MiB）。防止长度字段直接触发大分配。
inline constexpr std::size_t kMaxStringBytes = 16u * 1024u * 1024u;

/// @brief 单个字节数组字段的上限（16 MiB）。
inline constexpr std::size_t kMaxBlobBytes = 16u * 1024u * 1024u;

/// @brief varint 的最大编码字节数（64 位）。
inline constexpr std::size_t kMaxVarintBytes = 10;

/**
 * @brief 可读写的字节缓冲。
 *
 * @note 非线程安全：一个 Buffer 只能由同一线程使用。
 */
class Buffer {
   public:
    Buffer() = default;

    /// @param limit 写入上限；传 0 表示使用 `kMaxBufferBytes`。
    explicit Buffer(std::size_t limit) : m_limit(limit == 0 ? kMaxBufferBytes : limit) {}

    // --- 容量/状态 ---------------------------------------------------------

    /// @brief 已写入字节数（不受读取位置影响）。
    std::size_t size() const noexcept {
        return m_data.size();
    }

    /// @brief 是否为空缓冲。
    bool empty() const noexcept {
        return m_data.empty();
    }

    /// @brief 尚未读取的字节数。
    std::size_t Remaining() const noexcept {
        return m_data.size() - m_read;
    }

    /// @brief 当前读取位置。
    std::size_t ReadPosition() const noexcept {
        return m_read;
    }

    /// @brief 读写过程中是否出现过错误。
    bool ok() const noexcept {
        return !m_failed;
    }

    /// @brief 显式标记失败（供上层把「语义错误」转成 Buffer 失败）。
    void Fail() noexcept {
        m_failed = true;
    }

    /// @brief 清空内容并复位读写状态（保留 limit）。
    void Clear() noexcept {
        m_data.clear();
        m_read = 0;
        m_failed = false;
    }

    /// @brief 预留容量；失败（超限）时标记失败。
    void Reserve(std::size_t capacity) {
        if (capacity > m_limit) {
            Fail();
            return;
        }
        try {
            m_data.reserve(capacity);
        } catch (const std::bad_alloc&) {
            Fail();
        }
    }

    /// @brief 原始字节视图（整段已写入数据）。
    const std::vector<std::uint8_t>& data() const noexcept {
        return m_data;
    }

    /// @brief 可写引用，仅供把已经校验过的整段数据交给下游。
    std::vector<std::uint8_t>& data() noexcept {
        return m_data;
    }

    /// @brief 从当前读取位置开始的指针；`Remaining() == 0` 时可能为 nullptr。
    const std::uint8_t* Cursor() const noexcept {
        return m_read < m_data.size() ? m_data.data() + m_read : nullptr;
    }

    // --- 写 ---------------------------------------------------------------

    /// @brief 写入 1 字节。
    bool PutU8(std::uint8_t value) {
        if (!CanWrite(1)) {
            return false;
        }
        m_data.push_back(value);
        return true;
    }

    /// @brief 写入裸字节；`size == 0` 时是无操作。
    bool PutBytes(const void* data, std::size_t size) {
        if (size == 0) {
            return true;
        }
        if (data == nullptr) {
            Fail();
            return false;
        }
        if (!CanWrite(size)) {
            return false;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        try {
            m_data.insert(m_data.end(), bytes, bytes + size);
        } catch (const std::bad_alloc&) {
            Fail();
            return false;
        }
        return true;
    }

    /// @brief 写入「varint 长度 + UTF-8 字节」。
    bool PutString(std::string_view text) {
        if (text.size() > kMaxStringBytes) {
            Fail();
            return false;
        }
        if (!PutVarint(static_cast<std::uint64_t>(text.size()))) {
            return false;
        }
        return PutBytes(text.data(), text.size());
    }

    /// @brief 写入无符号 LEB128 varint。
    bool PutVarint(std::uint64_t value) {
        std::uint8_t scratch[kMaxVarintBytes] = {};
        std::size_t count = 0;
        while (value >= 0x80u) {
            scratch[count++] = static_cast<std::uint8_t>(value) | 0x80u;
            value >>= 7;
        }
        scratch[count++] = static_cast<std::uint8_t>(value);
        return PutBytes(scratch, count);
    }

    /// @brief 写入 zigzag varint（有符号整数）。
    bool PutSignedVarint(std::int64_t value) {
        const std::uint64_t encoded =
            (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
        return PutVarint(encoded);
    }

    // --- 读 ---------------------------------------------------------------

    /// @brief 读取 1 字节。
    bool GetU8(std::uint8_t& value) {
        if (Remaining() < 1) {
            Fail();
            return false;
        }
        value = m_data[m_read++];
        return true;
    }

    /// @brief 读取裸字节；目标必须至少有 `size` 字节。
    bool GetBytes(void* out, std::size_t size) {
        if (size == 0) {
            return true;
        }
        if (out == nullptr) {
            Fail();
            return false;
        }
        if (Remaining() < size) {
            Fail();
            return false;
        }
        const auto* source = m_data.data() + m_read;
        std::memcpy(out, source, size);
        m_read += size;
        return true;
    }

    /// @brief 读取「varint 长度 + 字节」到字符串。
    bool GetString(std::string& out) {
        std::uint64_t length = 0;
        if (!GetVarint(length)) {
            return false;
        }
        if (length > kMaxStringBytes || length > Remaining()) {
            Fail();
            return false;
        }
        if (length == 0) {
            out.clear();
            return true;
        }
        try {
            out.assign(reinterpret_cast<const char*>(m_data.data() + m_read),
                       static_cast<std::size_t>(length));
        } catch (const std::bad_alloc&) {
            Fail();
            return false;
        }
        m_read += static_cast<std::size_t>(length);
        return true;
    }

    /// @brief 读取无符号 varint；溢出（第 10 字节高位）判为坏数据。
    bool GetVarint(std::uint64_t& out) {
        std::uint64_t result = 0;
        unsigned shift = 0;
        for (std::size_t index = 0; index < kMaxVarintBytes; ++index) {
            std::uint8_t byte = 0;
            if (!GetU8(byte)) {
                return false;
            }
            // 第 10 字节只剩 1 个有效位；再高的位无法放进 uint64。
            if (index == kMaxVarintBytes - 1 && byte > 1) {
                Fail();
                return false;
            }
            result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) {
                out = result;
                return true;
            }
            shift += 7;
        }
        Fail();
        return false;
    }

    /// @brief 读取 zigzag varint。
    bool GetSignedVarint(std::int64_t& out) {
        std::uint64_t encoded = 0;
        if (!GetVarint(encoded)) {
            return false;
        }
        out = static_cast<std::int64_t>(encoded >> 1) ^ -static_cast<std::int64_t>(encoded & 1u);
        return true;
    }

    /// @brief 跳过 `size` 字节（用于跳过未知字段的预留扩展）。
    bool Skip(std::size_t size) {
        if (Remaining() < size) {
            Fail();
            return false;
        }
        m_read += size;
        return true;
    }

   private:
    bool CanWrite(std::size_t count) noexcept {
        if (count > m_limit || m_data.size() > m_limit - count) {
            Fail();
            return false;
        }
        return true;
    }

    std::vector<std::uint8_t> m_data;       ///< 已写入字节。
    std::size_t m_read = 0;                 ///< 已消费前缀长度。
    std::size_t m_limit = kMaxBufferBytes;  ///< 写入上限。
    bool m_failed = false;                  ///< 是否出现过错误。
};

}  // namespace go2cpp::reflect
