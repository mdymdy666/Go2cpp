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
 *   | Go 1.23 `hash`、`hash/crc32`、`hash/adler32`、`crypto/md5`、`crypto/sha1`、`crypto/sha256`、`crypto/sha512`（本机 `third_party/go1.23.0.src.tar.gz`） | BSD-3-Clause | **作为语义基准**：`Write` 累积 / `Sum` 不改变状态 / `Reset` 复位 / `Size`、`BlockSize` 的取值逐项对照；CRC32 的多项式、`Update` 的「先取反再累加再取反」流程也照搬 |
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
 * 校验和、协议兼容、历史数据迁移。新设计请用 SHA-256 / SHA-512 或更强的算法。
 * 该警告在 `md5.hpp`、`sha1.hpp` 的文件头各重复一次，避免只引子头时漏看。
 *
 * ## 已知未支持项及原因（C 类，有意不做）
 *
 * | 未支持项 | 原因 |
 * |---|---|
 * | SHA-224 | 与 SHA-256 共用压缩函数，只是不同初始向量 + 截断 28 字节；使用面远小于 SHA-256/512。真要加只需约 30 行，但会增加一个几乎没有调用方的 API 面。 |
 * | SHA-384 / SHA-512/224 / SHA-512/256 | 与 SHA-512 共用压缩函数，差别是初始向量与截断长度。当前只提供最常用的 SHA-512；需要时按 FIPS 180-4 的 IV 表扩展（每个约 10 行）。 |
 * | HMAC（`crypto/hmac`） | 需要把任意 `Hash` 包装成 HMAC（内外两层 pad、密钥长度规范化）。属于 `crypto` 而非 `hash` 的职责；本项目没有密钥/认证场景，留待安全模块统一设计，避免在基础模块里放出「看起来安全」的半成品。 |
 * | CRC32 的 SSE4.2 / PCLMUL 硬件加速 | 纯可移植实现已达 **3~5 GB/s**（slicing-by-8），远超本项目 IO 路径需求；运行时 CPU 分派会引入编译期特性宏与平台分支，收益不匹配。 |
 * | `encoding.BinaryMarshaler` / `BinaryUnmarshaler` 序列化 | Go 用它做哈希状态缓存（`MarshalBinary`）。C++ 侧可直接拷贝对象（`Sha256`/`Sha512` 都是值类型），不需要额外序列化协议。 |
 * | 各算法的 `New()` 工厂（返回 `hash.Hash` 接口） | C++ 侧构造函数/默认构造已完成同一职责（`Sha256 h;`），再提供 `New()` 只会多一层无意义的间接；需要接口多态时用 `std::unique_ptr<Hash>` 或 `Hash&`。 |
 *
 * ## 模块定位
 *
 * 本文件（`include/go2cpp/hash/hash.hpp`）定义 `hash` 模块的**公共契约层**。
 * `hash` 位于项目最底层：不依赖调度器、IO 或任何其它业务模块，只依赖 C++
 * 标准库；上层按需 include 具体算法头即可，不会反向被依赖。当前仓库里引用
 * 本目录的**唯一**入口是 `tests/hash/hash_test_support.hpp`（标准向量、随机
 * 对拍与压测）；`compress`、`http2`、`websocket` 等模块目前各自实现或尚未
 * 接入校验和，因此本模块没有生产调用方，也没有 IO/线程依赖。
 *
 * ## 本文件定义什么
 *
 * - `ByteSpan`：非拥有字节视图，是本模块所有输入/输出参数的统一载体；
 * - `AsBytes` 重载族：把裸指针、`std::string`、`std::string_view`、
 *   `std::vector`、`std::array` 适配成 `ByteSpan`，并用参数的 `const` 性
 *   区分「可写（给 `Sum` 输出）」与「只读（给 `Write`/`Checksum` 输入）」；
 * - `Hash`：Go `hash.Hash` 的五个方法（`Write`/`Sum`/`Reset`/`Size`/
 *   `BlockSize`）加上三个 C++ 便利重载，是六个具体摘要的共同基类。
 *
 * 具体算法分别在 `adler32.hpp`、`crc32.hpp`、`md5.hpp`、`sha1.hpp`、
 * `sha256.hpp`、`sha512.hpp`；本文件不含任何算法实现。
 *
 * ## 阅读建议
 *
 * 先读 `Hash` 对 `Write`/`Sum` 的语义约定 —— 尤其是 **`Sum` 不改变自身
 * 状态**、**`out.size() >= Size()` 是调用方前置条件** 这两条，它们决定了
 * 「取完摘要还能不能继续写」；再看 `ByteSpan` 的所有权契约；最后才按需跳到
 * 具体算法。只要一次性摘要时，直接用各算法的静态 `Digest`/`Checksum` 即可，
 * 不必经过 `Hash` 接口。
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
 *
 * ## 定位
 *
 * 本模块所有公共 API 的参数类型：`Hash::Write`/`Hash::Sum`、各算法的
 * `Write`/`Digest`/`Checksum`/`Update` 都收发 `ByteSpan`。它被 `Hash` 接口
 * 与六个具体摘要共同使用，本身不含算法。因为不拥有数据，视图只能在底层
 * 缓冲有效期内使用（契约见文件头）；`Sum(out)` 传入的视图必须指向可写内存。
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
    std::uint8_t* m_data{nullptr};  ///< 起始字节（非拥有）；构造函数写入，data()/operator[]/subspan 读取
    std::size_t m_size{0};          ///< 字节数；构造函数写入，size()/empty()/subspan 读取
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
 *
 * ## 定位
 *
 * `go2cpp::hash` 的唯一多态入口：`Adler32`、`Crc32`、`Md5`、`Sha1`、
 * `Sha256`、`Sha512` 六个具体摘要都继承它。只有「运行时选择算法」或
 * 「一次遍历喂多个摘要」的场景才需要持有 `Hash&` / `Hash*`；否则直接用具体
 * 类型，避免虚调用并让编译器内联。
 *
 * ## 生命周期与线程模型
 *
 * 接口本身无状态，线程安全性由具体实现决定。当前六个实现都**不加内部锁**：
 * 同一实例被多线程同时 `Write`/`Sum` 需要调用方加锁；不同实例互不影响。
 * 通过 `Hash&` 持有实现时，实现的析构函数必须是虚的（本接口已声明）。
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
