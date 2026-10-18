/**
 * @file hash.hpp
 * @brief Go `hash` 包的 C++ 对应实现：统一 `Hash` 接口 + 非拥有字节视图。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对照 Go 标准库；实现独立完成，未复制上游代码）
 *
 * @details 复用优先的结论：
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | Go 1.23 `hash`、`hash/crc32`、`hash/adler32`、`crypto/md5`、`crypto/sha1`、`crypto/sha256`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **作为语义基准**：`Write` 累积 / `Sum` 不改变状态 / `Reset` 复位 / `Size`、`BlockSize` 的取值逐项对照；CRC32 的多项式、`Update` 的「先取反再累加再取反」流程也照搬 |
 *   | OpenSSL（`libcrypto` 的 `MD5_*` / `SHA*_*` / `CRC32`） | Apache-2.0 | **不采用**：项目明确要求零外部依赖；且 OpenSSL 3.x 把 MD5/SHA-1 放进 legacy provider，接口与构建复杂度都远超本模块需要 |
 *   | zlib（`crc32` / `adler32`） | Zlib | **不采用**：只为两个校验和引入一个 C 依赖不划算，二者算法都只有几十行；zlib 仅作为**测试 oracle**（Python `zlib.crc32` / `zlib.adler32` 即其绑定） |
 *   | [Botan](https://botan.randombit.net/)、[tinycrypt](https://github.com/intel/tinycrypt) | BSD-2 / BSD-3 | **不采用**：前者体积大且 API 形态不同；后者只覆盖 AES/SHA-256 子集，无法一次满足 CRC32/Adler32/MD5/SHA-1/SHA-256 五个摘要 |
 *
 *   **为什么自己实现而不是移植 Go 源码**：Go 的 `crypto/md5`、`crypto/sha1`、
 *   `crypto/sha256` 在 amd64 上走汇编（AVX2/SHA-NI），纯 Go 版本也用了
 *   `encoding/binary` 与 Go 的 slice 语义；逐行翻译的收益低于按 RFC 1321 /
 *   FIPS 180-4 独立实现。本项目实现用 C++17 的 `std::uint32_t` 位运算，
 *   并用 **Python `zlib` / `hashlib` 与 RFC/FIPS 标准向量双向对拍**（见
 *   `tests/hash/`），保证的不是「抄得像」，而是「算得对」。
 *
 * ## 统一接口：对齐 Go `hash.Hash`
 *
 * Go 的接口是：
 * ```go
 * type Hash interface {
 *     io.Writer                 // Write(p []byte) (n int, err error)
 *     Sum(b []byte) []byte      // 把当前摘要**追加**到 b，不改变状态
 *     Reset()
 *     Size() int
 *     BlockSize() int
 * }
 * ```
 * C++ 侧保持四个语义点，只改表达方式：
 *   - `Write(ByteSpan)`：与 Go 一样「写多少算多少」，永不失败（Go 的 error 恒为 nil）；
 *   - `Sum(ByteSpan out)`：把摘要写进 `out[0, Size())`，**不改变自身状态**；
 *     Go 的 `Sum(b)` 是追加到 `b` 后面，本接口把「追加」交给调用方，避免每次
 *     摘要都分配内存。**`out.size()` 必须不小于 `Size()`**（否则是调用方违约）；
 *   - `Reset()`：回到刚构造的状态，可重复使用；
 *   - `BlockSize()`：与 Go 一致，是**内部块大小**（MD5/SHA-1/SHA-256 为 64，
 *     CRC32 为 1，Adler32 为 4），不是推荐缓冲大小。
 *
 * 额外的便利成员（不属于 Go 接口，但语义直接可推导）：
 *   - `std::vector<std::uint8_t> Hash::Sum() const`：等价于 Go 的 `Sum(nil)`；
 *   - `void Hash::Write(std::string_view)` / `Write(const void*, size_t)`：
 *     省去调用方手写 `AsBytes`。
 *
 * ## `ByteSpan` 的生命周期契约
 *
 * 与 `bufio::ByteSpan` 一样是**非拥有视图**，只保证到下一次修改底层缓冲为止有效。
 * 写接口（`Write`）与只读校验函数**绝不会**通过视图改写输入；需要传入只读数据时
 * 用 `AsBytes(const void*)` 系列重载，它们内部做了 `const_cast`，契约由本模块保证。
 * 传给 `Sum(out)` 的视图则必须是可写内存。
 *
 * ## 安全提示
 *
 * MD5 与 SHA-1 **已不适合任何安全用途**（两者都有实际构造出的碰撞），只可用于
 * 校验和、协议兼容、历史数据迁移。新设计请用 SHA-256 或更强的算法。
 * 该警告在 `md5.hpp`、`sha1.hpp` 的文件头各重复一次，避免只引子头时漏看。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::hash {

/**
 * @brief 指向外部字节缓冲的非拥有视图（语义接近 `std::span<std::uint8_t>`）。
 *
 * @details 拷贝视图不拷贝数据；`const` 对象仍允许改写底层字节（浅 const，
 *          与 `std::span` 一致）。空视图的 `data() == nullptr`。
 *
 *          为什么不用 `std::span`：项目统一 `cxx_std_17`，标准库还没有；
 *          `std::string_view` 只读且绑定 `char`，无法承载 `Sum` 的输出参数。
 */
class ByteSpan {
 public:
    using element_type = std::uint8_t;
    using value_type = std::uint8_t;
    using iterator = std::uint8_t*;

    /// 空视图（`data() == nullptr`，`size() == 0`）。
    constexpr ByteSpan() noexcept = default;

    /// 由裸指针 + 长度构造；调用方保证 `[data, data + size)` 可访问。
    constexpr ByteSpan(std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    /// 起始字节；空视图为 `nullptr`。
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

    /// 从 `offset` 开始取 `count` 字节；越界截断到剩余长度（不抛异常）。
    constexpr ByteSpan subspan(
        std::size_t offset,
        std::size_t count = static_cast<std::size_t>(-1)) const noexcept {
        if (offset >= m_size) {
            return ByteSpan(m_data + m_size, 0);
        }
        const std::size_t remaining = m_size - offset;
        return ByteSpan(m_data + offset, count < remaining ? count : remaining);
    }

 private:
    std::uint8_t* m_data{nullptr};
    std::size_t m_size{0};
};

// ============================================================================
// AsBytes：构造 ByteSpan 的工厂
//
// 两组重载的契约不同，命名故意保持同一个 `AsBytes`，但用参数 const 性区分：
//   - 非 const 参数：可写视图，可用于 `Sum(out)`；
//   - const 参数 / std::string_view：只读视图，只能传给 `Write` 与 `Checksum*`。
// ============================================================================

/// 可写视图：`uint8_t*` 缓冲。
inline ByteSpan AsBytes(std::uint8_t* data, std::size_t size) noexcept {
    return ByteSpan(data, size);
}

/// 可写视图：`char*` 缓冲。
inline ByteSpan AsBytes(char* data, std::size_t size) noexcept {
    return ByteSpan(reinterpret_cast<std::uint8_t*>(data), size);
}

/**
 * @brief 只读视图：`const uint8_t*` 缓冲。
 * @warning 返回的视图类型本身可写；本模块的 API 不会写它。**不要**把该视图
 *          传给任何会写数据的第三方函数。
 */
inline ByteSpan AsBytes(const std::uint8_t* data, std::size_t size) noexcept {
    return ByteSpan(const_cast<std::uint8_t*>(data), size);
}

/// 只读视图：`std::string_view`（含字符串字面量、`const std::string`）。
inline ByteSpan AsBytes(std::string_view text) noexcept {
    return ByteSpan(
        const_cast<std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(text.data())),
        text.size());
}

/// 可写视图：`std::string`（C++17 起 `data()` 可写）。
inline ByteSpan AsBytes(std::string& text) noexcept {
    return AsBytes(text.data(), text.size());
}

/// 可写视图：`std::vector<std::uint8_t>`。
inline ByteSpan AsBytes(std::vector<std::uint8_t>& buffer) noexcept {
    return ByteSpan(buffer.data(), buffer.size());
}

/// 只读视图：`const std::vector<std::uint8_t>`。
inline ByteSpan AsBytes(const std::vector<std::uint8_t>& buffer) noexcept {
    return AsBytes(buffer.data(), buffer.size());
}

/// 可写视图：`std::array<std::uint8_t, N>`（`Sum` 的常用输出容器）。
template <std::size_t N>
inline ByteSpan AsBytes(std::array<std::uint8_t, N>& buffer) noexcept {
    return ByteSpan(buffer.data(), N);
}

/// 只读视图：`const std::array<std::uint8_t, N>`。
template <std::size_t N>
inline ByteSpan AsBytes(const std::array<std::uint8_t, N>& buffer) noexcept {
    return AsBytes(buffer.data(), N);
}

/**
 * @brief Go `hash.Hash` 的 C++ 对应接口。
 *
 * @details 纯虚接口，五个摘要实现均继承它；用于「一次遍历、多个哈希」以及
 *          需要擦除具体算法类型的场景（例如配置驱动的校验器）。
 *          **接口本身不提供线程安全**：与 Go 一样，同一实例被多线程写需要外部同步。
 */
class Hash {
 public:
    virtual ~Hash() = default;

    Hash() = default;
    Hash(const Hash&) = default;
    Hash& operator=(const Hash&) = default;

    /// 追加数据。永不失败（Go 的 `Write` 返回的 error 恒为 nil）。
    virtual void Write(ByteSpan p) = 0;

    /**
     * @brief 把当前摘要写入 `out[0, Size())`，**不改变自身状态**。
     * @pre `out.size() >= Size()`。
     */
    virtual void Sum(ByteSpan out) const = 0;

    /// 回到刚构造时的状态；可重复使用。
    virtual void Reset() = 0;

    /// 摘要字节数。
    virtual std::size_t Size() const = 0;

    /// 内部块大小（Go `hash.Hash.BlockSize`；不是推荐缓冲大小）。
    virtual std::size_t BlockSize() const = 0;

    /// 便利重载：等价于 `Write(AsBytes(text))`（只读输入）。
    void Write(std::string_view text) { Write(AsBytes(text)); }

    /// 便利重载：只读输入 `[data, data + size)`。
    void Write(const void* data, std::size_t size) {
        Write(AsBytes(static_cast<const std::uint8_t*>(data), size));
    }

    /// 便利重载：等价于 Go 的 `Sum(nil)`——返回新分配的摘要。
    std::vector<std::uint8_t> Sum() const {
        std::vector<std::uint8_t> out(Size());
        Sum(ByteSpan(out.data(), out.size()));
        return out;
    }
};

}  // namespace go2cpp::hash
