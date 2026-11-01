/**
 * @file crc32.hpp
 * @brief Go `hash/crc32` 包的 C++ 对应物（薄封装，复用既有硬件加速实现）。
 *
 * @author DeepSeek Harness
 * @date 2026-11-01
 * @copyright BSD-3-Clause（语义对照 Go `hash/crc32`，实现见 `go2cpp/hash/crc32.hpp`）
 *
 * @details
 * 本文件**不重新实现 CRC**：`include/go2cpp/hash/crc32.hpp` 已经提供了
 * 与 Go 逐项对齐的 `MakeTable` / `ChecksumIEEE` / `Checksum` / `Update` /
 * `Crc32`（`hash.Hash32` 对应物），并带 x86-64 的 PCLMULQDQ（IEEE）与
 * SSE4.2 `crc32q`（Castagnoli）硬件路径。这里只做一件事：按 Go 的
 * **包名 + 符号名**再暴露一层，让从 Go 迁移过来的代码可以机械替换：
 *
 * ```go
 * import "hash/crc32"
 *
 * sum := crc32.ChecksumIEEE(data)
 * tab := crc32.MakeTable(crc32.Castagnoli)
 * h := crc32.New(tab)
 * h.Write(data)
 * c := h.Sum32()
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * #include "go2cpp/go/crc32.hpp"
 *
 * std::uint32_t sum = go::crc32::ChecksumIEEE(data);          // 同 Go
 * const auto tab = go::crc32::MakeTable(go::crc32::Castagnoli);
 * go::crc32::Hash h = go::crc32::New(tab);                    // = crc32.New
 * h.Write(go2cpp::hash::AsBytes(data));
 * std::uint32_t c = h.Sum32();
 * ```
 *
 * ## 与 Go 的对应关系
 *
 * | Go | 本文件 | 备注 |
 * |---|---|---|
 * | `crc32.IEEE` / `Castagnoli` / `Koopman` | 同名常量 | 反射多项式，值与 Go 相同 |
 * | `crc32.IEEETable` 等（`*Table`） | `IEEETable` 等（`const Table&`） | C++ 用具名引用而非指针，禁止取空 |
 * | `crc32.MakeTable(poly)` | `MakeTable(poly)` | `constexpr`，可编译期生成 |
 * | `crc32.ChecksumIEEE(p)` | `ChecksumIEEE(p)` | `p` 为 `ByteSpan` 或任意可被 `AsBytes` 适配的类型 |
 * | `crc32.Checksum(p, tab)` | `Checksum(p, tab)` | 同上 |
 * | `crc32.Update(crc, tab, p)` | `Update(crc, tab, p)` | 分段累加语义一致 |
 * | `crc32.New(tab) hash.Hash32` | `New(tab)` → `Hash` | `Hash` 是 `go2cpp::hash::Crc32` 的别名 |
 * | `h.Write(p) (int, error)` | `h.Write(ByteSpan)` → `void` | C++ 的 `Write` 不会失败，Go 的 `err` 恒为 nil |
 * | `h.Sum(b []byte) []byte`（追加） | `h.Sum(ByteSpan out)`（写入） | 与 `go2cpp/hash` 的统一约定一致 |
 * | `h.Sum32()` | `h.Sum32()` | 相同 |
 *
 * ## 语义边界（与 Go 完全一致的部分）
 *
 * - **初始化/取反**：`Checksum` 一族进入时 `^crc`、离开时 `^crc`，空输入返回 0；
 * - **分段可拼接**：`Update(Checksum(a), tab, b) == Checksum(a ++ b)`；
 * - **三种多项式**：IEEE（以太网/ZIP/PNG，`ChecksumIEEE` 用它）、Castagnoli
 *   （iSCSI/ext4，CRC-32C）、Koopman（CRC-32K）。
 *
 * ## 已知差异
 *
 * - Go 的 `Table` 对外只有 256 项基础表；本项目 `Table` 额外内嵌 7 张
 *   slicing-by-8 派生表（对外仍以 `operator[]` 暴露基础表，语义不变）。
 * - Go 的 `New` 返回接口 `hash.Hash32`；C++ 返回值类型 `Hash`（值语义，可栈上
 *   构造），需要多态时用 `go2cpp::hash::Hash&` 擦除。
 * - `Write` 的签名按项目统一约定为 `void Write(ByteSpan)`，不返回 `(n, error)`。
 *
 * @note CRC-32 是**检错**码，不提供抗篡改能力；需要防篡改请用 SHA-256。
 * @note `Hash` 内部只持有 `const Table*`，自定义表必须比摘要器活得久
 *       （三个内建表是静态存储，永不失效）。
 */

#pragma once

#include "go2cpp/hash/crc32.hpp"

#include <cstddef>
#include <cstdint>

namespace go2cpp::go {
namespace crc32 {

/// CRC 查表（`go2cpp::hash::Table` 的别名），对应 Go 的 `*crc32.Table`。
using Table = ::go2cpp::hash::Table;
/// 流式摘要器，对应 Go `crc32.New` 返回的 `hash.Hash32`。
using Hash = ::go2cpp::hash::Crc32;
/// 非拥有字节视图，与 `go2cpp/hash` 全家保持一致。
using ByteSpan = ::go2cpp::hash::ByteSpan;

/// IEEE 多项式（反射），对应 Go `crc32.IEEE`（以太网 / ZIP / PNG）。
inline constexpr std::uint32_t IEEE = ::go2cpp::hash::IEEE;
/// Castagnoli 多项式（反射，CRC-32C），对应 Go `crc32.Castagnoli`。
inline constexpr std::uint32_t Castagnoli = ::go2cpp::hash::Castagnoli;
/// Koopman 多项式（反射，CRC-32K），对应 Go `crc32.Koopman`。
inline constexpr std::uint32_t Koopman = ::go2cpp::hash::Koopman;

/// IEEE 表，对应 Go `crc32.IEEETable`。用引用暴露，杜绝空表指针。
inline const Table& IEEETable = ::go2cpp::hash::IEEETable;
/// Castagnoli 表，对应 Go `crc32.CastagnoliTable`。
inline const Table& CastagnoliTable = ::go2cpp::hash::CastagnoliTable;
/// Koopman 表，对应 Go `crc32.KoopmanTable`。
inline const Table& KoopmanTable = ::go2cpp::hash::KoopmanTable;

/// 由多项式生成查表，对应 Go `crc32.MakeTable`。
using ::go2cpp::hash::MakeTable;

/**
 * @brief 一次性计算 IEEE 多项式 CRC-32，对应 Go `crc32.ChecksumIEEE`。
 * @param data 非拥有字节视图。
 * @return CRC-32（空输入为 0）。
 */
inline std::uint32_t ChecksumIEEE(ByteSpan data) noexcept {
    return Hash::ChecksumIEEE(data);
}

/**
 * @brief 一次性计算指定多项式的 CRC-32，对应 Go `crc32.Checksum`。
 * @param data 非拥有字节视图。
 * @param table 查表（通常是 `IEEETable` / `CastagnoliTable` / `KoopmanTable`）。
 */
inline std::uint32_t Checksum(ByteSpan data, const Table& table) noexcept {
    return Hash::Checksum(data, table);
}

/**
 * @brief 把 `data` 追加到已有 CRC 之后，对应 Go `crc32.Update`。
 * @param crc 已算出的 CRC（不是内部原始状态）。
 * @param table 查表。
 * @param data 追加数据。
 * @return `Checksum(prefix ++ data)`。
 */
inline std::uint32_t Update(std::uint32_t crc, const Table& table,
                            ByteSpan data) noexcept {
    return Hash::Update(crc, table, data);
}

/// 构造流式摘要器，对应 Go `crc32.New(tab)`；默认 IEEE。
inline Hash New(const Table& table = IEEETable) noexcept {
    return Hash(table);
}

// ============================================================================
// 便利重载：让 std::string / std::vector / std::array 直接可用
//
// 与 Go 的 []byte 一样，调用方不必手工构造 ByteSpan。这些模板只做
// `AsBytes` 转发，不引入额外运行期开销；传 ByteSpan 时精确匹配上面的
// 非模板重载，不会产生歧义。
// ============================================================================

/// `ChecksumIEEE` 的任意字节容器重载（`std::string` / `vector` / `array` / 字面量）。
template <typename Bytes>
inline std::uint32_t ChecksumIEEE(const Bytes& data) noexcept {
    return ChecksumIEEE(::go2cpp::hash::AsBytes(data));
}

/// `Checksum` 的任意字节容器重载。
template <typename Bytes>
inline std::uint32_t Checksum(const Bytes& data, const Table& table) noexcept {
    return Checksum(::go2cpp::hash::AsBytes(data), table);
}

/// `Update` 的任意字节容器重载。
template <typename Bytes>
inline std::uint32_t Update(std::uint32_t crc, const Table& table,
                            const Bytes& data) noexcept {
    return Update(crc, table, ::go2cpp::hash::AsBytes(data));
}

}  // namespace crc32
}  // namespace go2cpp::go
