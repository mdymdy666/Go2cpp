/**
 * @file crc32.hpp
 * @brief CRC-32 校验和的 C++ 实现，接口对齐 Go `hash/crc32`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对照 Go `hash/crc32`，实现独立完成）
 *
 * @details 支持 Go 导出的三种多项式（均为**反射**表示，即最低位先算）：
 *   | 常量 | 值 | 用途 |
 *   |---|---|---|
 *   | `IEEE` | `0xedb88320` | 以太网 / ZIP / gzip / PNG（Go `ChecksumIEEE` 用它） |
 *   | `Castagnoli` | `0x82f63b78` | iSCSI / SCTP / Btrfs / ext4 元数据（CRC-32C） |
 *   | `Koopman` | `0xeb31d82e` | CRC-32K（Koopman 多项式，误码检测能力更强） |
 *
 *   CRC 的流式公式（与 Go 完全一致）：
 *   ```
 *   crc = ^crc                      // 进入时取反
 *   for each byte: crc = table[(crc ^ b) & 0xff] ^ (crc >> 8)
 *   return ^crc                     // 离开时再取反
 *   ```
 *   因此 `Update(Checksum(a), tab, b) == Checksum(a ++ b)`——这是传输校验
 *   分段计算的基础，测试里逐项验证。
 *
 * ## 查表宽度：为什么是 slicing-by-8 而不是 256 表
 *
 * 先用 256 表（Go 的 `Table` 形态）实测：Release 下约 0.5~0.8 GB/s，
 * 达不到本模块「>1 GB/s」的目标。因此 `Table` 内部额外存了 7 张派生表
 * （标准 slicing-by-8），每次处理 8 字节，仍属**纯查表**实现（无 ISA 扩展、
 * 无平台分支）。`Table::operator[]` 仍暴露**基础 256 表**，与 Go
 * `MakeTable` 的可见语义一致；派生表是从它逐级推出的内部加速结构。
 *
 * ## 与 Go `hash/crc32` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `crc32.MakeTable(poly)` | `MakeTable(poly)` |
 * | `crc32.IEEETable` / `CastagnoliTable` / `KoopmanTable` | `IEEETable` / `CastagnoliTable` / `KoopmanTable` |
 * | `crc32.ChecksumIEEE(data)` | `Crc32::ChecksumIEEE(data)` |
 * | `crc32.Checksum(data, tab)` | `Crc32::Checksum(data, tab)` |
 * | `crc32.Update(crc, tab, data)` | `Crc32::Update(crc, tab, data)` |
 * | `crc32.New(tab) hash.Hash32` | `Crc32 h(tab);` 或 `Crc32::New(tab)` |
 *
 * @note CRC-32 是**检错**码，不是密码学校验：它能高效发现传输中的随机错误，
 *      但攻击者可以构造任意 CRC 值的数据。防篡改请用 SHA-256（或 HMAC）。
 * @note `Crc32` 内部保存指向 `Table` 的指针，`table` 的生命周期必须覆盖
 *       `Crc32` 实例；三个内建表是静态存储，永不失效。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/// IEEE 多项式（反射），对应 Go `crc32.IEEE`。
inline constexpr std::uint32_t IEEE = 0xedb88320u;
/// Castagnoli 多项式（反射，CRC-32C），对应 Go `crc32.Castagnoli`。
inline constexpr std::uint32_t Castagnoli = 0x82f63b78u;
/// Koopman 多项式（反射，CRC-32K），对应 Go `crc32.Koopman`。
inline constexpr std::uint32_t Koopman = 0xeb31d82eu;

/**
 * @brief CRC 查表（基础 256 表 + 7 张 slicing-by-8 派生表）。
 *
 * @details 内存布局：`entries[0..255]` 是基础表（与 Go `MakeTable` 完全一致），
 *          `entries[k * 256 + i]`（k=1..7）是第 k 张派生表。公开 `operator[]`
 *          只访问基础表，避免把内部布局暴露给调用方。
 */
struct Table {
    /// 基础表项数（与 Go 一致）。
    static constexpr std::size_t kBaseEntries = 256;
    /// slicing-by-8 的表数量。
    static constexpr std::size_t kSlices = 8;

    /// 依次排列的 8 张表（基础表在前）。
    std::array<std::uint32_t, kBaseEntries * kSlices> entries{};

    /// 读取基础 256 表（与 Go `Table` 的下标语义一致）。
    constexpr std::uint32_t operator[](std::size_t index) const noexcept {
        return entries[index];
    }
};

/**
 * @brief 由多项式生成查表，对应 Go `crc32.MakeTable`。
 * @param poly 反射形式的多项式（如 `IEEE`）。
 * @return 可直接传给 `Crc32` / `Checksum` / `Update` 的表。
 *
 * @details 编译期可求值（`constexpr`），三个内建表因此在静态初始化阶段完成，
 *          没有运行期竞态，也不需要 Go 的 `sync.Once`。
 */
constexpr Table MakeTable(std::uint32_t poly) noexcept {
    Table table{};
    for (std::size_t i = 0; i < Table::kBaseEntries; ++i) {
        std::uint32_t crc = static_cast<std::uint32_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ poly : (crc >> 1);
        }
        table.entries[i] = crc;
    }
    for (std::size_t slice = 1; slice < Table::kSlices; ++slice) {
        for (std::size_t i = 0; i < Table::kBaseEntries; ++i) {
            const std::uint32_t previous =
                table.entries[(slice - 1) * Table::kBaseEntries + i];
            table.entries[slice * Table::kBaseEntries + i] =
                (previous >> 8) ^ table.entries[previous & 0xffu];
        }
    }
    return table;
}

/// IEEE 表，对应 Go `crc32.IEEETable`。
extern const Table IEEETable;
/// Castagnoli 表，对应 Go `crc32.CastagnoliTable`。
extern const Table CastagnoliTable;
/// Koopman 表，对应 Go `crc32.KoopmanTable`。
extern const Table KoopmanTable;

/**
 * @brief CRC-32 摘要器，对应 Go `crc32.New(table)` 返回的 `hash.Hash32`。
 *
 * @details `Write` 可任意拆分；`Sum32()` 是当前 CRC；`Sum` 写入 **大端** 4 字节
 *          （与 Go `crc32.digest.Sum` 一致：`byte(s>>24), ..., byte(s)`）。
 */
class Crc32 final : public Hash {
 public:
    /// 摘要长度（字节）。
    static constexpr std::size_t kSize = 4;
    /// 内部块大小（字节），对应 Go `crc32.digest.BlockSize()`（返回 1）。
    static constexpr std::size_t kBlockSize = 1;

    /**
     * @brief 用指定查表构造。
     * @param table 查表；默认 IEEE。其生命周期必须覆盖本实例。
     */
    explicit Crc32(const Table& table = IEEETable) noexcept : m_table(&table) {}

    void Write(ByteSpan p) override {
        m_crc = Update(m_crc, *m_table, p);
    }

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override { m_crc = 0; }

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /// 当前 CRC 值，对应 Go 的 `hash.Hash32.Sum32`。
    std::uint32_t Sum32() const noexcept { return m_crc; }

    /// 一次性计算 IEEE 多项式 CRC-32（对应 Go `crc32.ChecksumIEEE`）。
    static std::uint32_t ChecksumIEEE(ByteSpan data) noexcept;

    /// 一次性计算指定表的 CRC-32（对应 Go `crc32.Checksum`）。
    static std::uint32_t Checksum(ByteSpan data, const Table& table) noexcept;

    /**
     * @brief 把 `data` 追加到由 `crc` 表示的数据之后（对应 Go `crc32.Update`）。
     * @param crc 已算出的 CRC（不是内部取反后的原始状态）。
     * @param table 查表。
     * @param data 追加的数据。
     * @return `crc(data_so_far ++ data)`。
     */
    static std::uint32_t Update(std::uint32_t crc, const Table& table,
                                ByteSpan data) noexcept;

    /// 等价于 `Crc32(table)`；只为对齐 Go 的 `crc32.New` 而保留。
    static Crc32 New(const Table& table = IEEETable) noexcept {
        return Crc32(table);
    }

 private:
    const Table* m_table;
    std::uint32_t m_crc{0};
};

}  // namespace go2cpp::hash
