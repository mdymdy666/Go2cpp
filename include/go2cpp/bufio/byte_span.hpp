/**
 * @file byte_span.hpp
 * @brief Go `[]byte` 的**非拥有**视图，供 `bufio` 模块内部与公开 API 使用。
 *
 * @details **为什么需要它**：Go 的 `bufio.Reader.Peek` 返回的是
 *   「指向内部缓冲区的切片」，这是零拷贝的关键；C++17 没有 `std::span`
 *   （项目统一用 `cxx_std_17`），`std::string_view` 又是只读且绑定 `char`。
 *   因此这里提供一个极小的可变字节视图：`(uint8_t* data, size_t size)`。
 *
 *   **生命周期契约**：`ByteSpan` 不拥有数据。`Peek` / `Scanner::Bytes`
 *   返回的 span 只在**下一次修改该缓冲区之前**有效（对 `Reader` 是下一次
 *   `Read`/`Peek`/`Discard`，对 `Scanner` 是下一次 `Scan`）。把它们存到
 *   这些调用之后再使用，就是悬垂引用。
 *
 *   **与 Go 的差异**：Go 的切片携带容量（cap），本类只有长度；`bufio`
 *   的所有缓冲都由类自己管理容量，调用方不需要 cap。
 *
 * ## 模块定位
 *
 * `bufio` 的公共底座，不依赖模块内其它头文件：`Reader::Peek`/`ReadSlice` 用它
 * 零拷贝交付缓冲片段，`Scanner::Bytes` 用它暴露 token，`Writer::Write`/
 * `WriteString`/`ReadFrom` 用它接收输入；`AsBytes` 系列则负责在模块内常见的
 * `char*`/`std::string`/`std::vector<std::uint8_t>` 三种表示之间转换。
 *
 * ## 本文件定义什么
 *
 * - `ByteSpan`：`(uint8_t* data, size_t size)` 的非拥有可变视图；
 * - `AsBytes(...)` 三个重载：从 `char*`、`std::string`、`vector<uint8_t>` 造可写视图；
 * - `AsStringView`：只读视图，不分配；`ToString`：唯一会分配拷贝的入口。
 *
 * ## 阅读建议
 *
 * 先看类注释里的生命周期契约，再回来看文件头的「为什么不用 `string_view`」。
 * 之后每看到一个 `ByteSpan`，都应立刻反问：它所指向的内存在下一次读操作后还活着吗？
 *
 * ## 边界与陷阱
 *
 * 拷贝 `ByteSpan` 不会延长任何数据的生命周期；`const ByteSpan` 仍可写底层字节
 * （浅 const，与 `std::span` 一致）；`subspan` 越界会静默截断而不是报错。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bufio {

/**
 * @brief 指向外部字节缓冲的非拥有视图。
 * @details 语义接近 `std::span<std::uint8_t>`：拷贝视图不拷贝数据；
 *          对 `operator[]` 的写操作会直接落到原缓冲区。`const` 对象仍允许
 *          写底层字节（浅 const，与 `std::span` 一致）。
 *
 * ## 定位
 * `bufio` 内部的数据搬运表示：`Reader::Peek`/`ReadSlice` 用它零拷贝交付缓冲片段，
 * `Scanner::Bytes` 用它暴露 token，`Writer::Write`/`WriteString`/`ReadFrom`
 * 用它接收字节。它不参与容量管理，也不拥有任何内存。
 *
 * ## 生命周期与线程模型
 * 视图本身可平凡拷贝；底层字节的生命周期由创建者（`Reader`/`Scanner` 内部的
 * `std::vector`）决定，最迟在下一次填充/压缩缓冲时失效。不提供线程安全保证：
 * 通常与所属对象在同一线程使用。
 *
 * ## 不变量
 * - `empty()` 等价于 `size() == 0`，此时 `data()` 可以为 `nullptr`；
 * - 非空视图的 `[data(), data() + size())` 必须可访问；
 * - `first(n)` 由调用方保证 `n <= size()`，`subspan` 则自行钳制越界。
 */
class ByteSpan {
public:
    using element_type = std::uint8_t;
    using value_type = std::uint8_t;
    using iterator = std::uint8_t*;

    /// 空视图（`data()==nullptr`，`size()==0`）。
    constexpr ByteSpan() noexcept = default;

    /// 由裸指针 + 长度构造；调用方保证 `[data, data + size)` 可访问。
    constexpr ByteSpan(std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    /// 指向的起始字节；空视图为 `nullptr`。
    constexpr std::uint8_t* data() const noexcept { return m_data; }
    /// 字节数。
    constexpr std::size_t size() const noexcept { return m_size; }
    /// 是否为空视图。
    constexpr bool empty() const noexcept { return m_size == 0; }

    /// 不检查越界的下标访问（与 `std::span` 一致）。
    constexpr std::uint8_t& operator[](std::size_t index) const noexcept {
        return m_data[index];
    }

    constexpr iterator begin() const noexcept { return m_data; }
    constexpr iterator end() const noexcept { return m_data + m_size; }

    /// 取前 `count` 字节；`count` 必须不大于 `size()`。
    constexpr ByteSpan first(std::size_t count) const noexcept {
        return ByteSpan(m_data, count);
    }

    /**
     * @brief 从 `offset` 开始取 `count` 字节。
     * @note 越界直接截断到剩余长度，**不**抛异常也不崩溃——缓冲切片的边界
     *       由上层按 `Buffered()` 计算，这里只做安全钳制。
     */
    constexpr ByteSpan subspan(std::size_t offset,
                               std::size_t count = static_cast<std::size_t>(-1)) const noexcept {
        if (offset >= m_size) {
            return ByteSpan(m_data + m_size, 0);
        }
        const std::size_t remaining = m_size - offset;
        return ByteSpan(m_data + offset, count < remaining ? count : remaining);
    }

private:
    std::uint8_t* m_data{nullptr}; ///< 起始字节；构造时设置，被 data()/operator[]/first()/subspan() 读取
    std::size_t m_size{0};         ///< 字节数；构造时设置，被 size()/end()/first()/subspan() 读取
};

/// 由 `char*` 缓冲构造可写视图（项目里大量接口仍用 `char*`）。
inline ByteSpan AsBytes(char* data, std::size_t size) noexcept {
    return ByteSpan(reinterpret_cast<std::uint8_t*>(data), size);
}

/// 由 `std::string` 构造可写视图（`std::string` 的 `data()` 在 C++17 起可写）。
inline ByteSpan AsBytes(std::string& text) noexcept {
    return AsBytes(text.data(), text.size());
}

/// 由 `std::vector<std::uint8_t>` 构造可写视图。
inline ByteSpan AsBytes(std::vector<std::uint8_t>& buffer) noexcept {
    return ByteSpan(buffer.data(), buffer.size());
}

/// 只读视图（`std::string_view` 要求 `const char*`）。
inline std::string_view AsStringView(ByteSpan span) noexcept {
    return std::string_view(reinterpret_cast<const char*>(span.data()), span.size());
}

/// 复制成 `std::string`（会分配；只在确实需要拥有数据时使用）。
inline std::string ToString(ByteSpan span) {
    if (span.empty()) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(span.data()), span.size());
}

}  // namespace go2cpp::bufio
