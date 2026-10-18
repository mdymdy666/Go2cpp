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
 *
 * ## 模块定位
 *
 * 本文件（`include/go2cpp/hash/crc32.hpp`）是 `hash` 模块里唯一带「多项式
 * 选择」的算法：gzip/ZIP/PNG 用 IEEE，iSCSI/ext4 元数据用 Castagnoli，
 * 误码检测要求更高时用 Koopman。因此除了摘要器 `Crc32`，这里还公开可复用的
 * 查表类型 `Table` 与一次性函数。实现位于协议栈最底层，不依赖其它模块；
 * 当前仓库的直接调用方只有 `tests/hash/hash_test_support.hpp`（与 Python
 * `zlib.crc32` 对拍），生产代码尚未接入。
 *
 * ## 本文件定义什么
 *
 * - `IEEE`/`Castagnoli`/`Koopman`：三个反射形式的多项式常量；
 * - `Table`：基础 256 表 + 7 张 slicing-by-8 派生表；
 * - `MakeTable`：编译期生成查表（三个内建表因此是常量初始化，无运行期竞态）；
 * - `IEEETable`/`CastagnoliTable`/`KoopmanTable`：进程级静态表；
 * - `Crc32`：流式摘要器（`Hash` 子类），持有 `const Table*`。
 *
 * ## 阅读建议
 *
 * 先读 `Update` 的「进入取反 / 离开取反」语义（它决定分段计算能否拼接），
 * 再看 `Table` 的内存布局与 `Crc32` 的指针生命周期约束；slicing-by-8 的
 * 循环可以最后看 —— 它只是同一公式的一次 8 字节展开。
 *
 * ## 边界与陷阱
 *
 * - 空输入 `Checksum` 返回 0：`Update(0, ...)` 进入取反再离开取反，正好抵消；
 * - `Crc32` 只存 `Table*`，表先于摘要器销毁就是悬垂指针（内建表除外）；
 * - 分段计算必须用 `Update` 串接；两个 `Checksum` 结果不能相加/异或。
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
 *
 * ## 定位
 *
 * CRC 热路径上的只读数据结构。`Crc32` 只持有一个 `const Table*`，因此表的
 * 生命周期必须长于摘要器；三个内建表是静态存储，永远满足这一条件。自定义
 * 表通常由调用方持有（或声明成 `static const`），不要传临时对象。
 *
 * ## 不变量
 *
 * `entries` 一旦构造完成就不再变化（`MakeTable` 返回后即只读）；所有派生表
 * 都能由基础表逐级推出，因此不存在「只改一张表」的合法用法。
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
 *
 * ## 定位
 *
 * 给需要**流式** CRC 的调用方使用：每收到一段数据 `Write` 一次，最后用
 * `Sum32()`/`Sum()` 取结果；一次性场景直接用静态 `ChecksumIEEE`/
 * `Checksum`/`Update`，不必构造对象。作为 `Hash&` 擦除后也可参与「一次遍历
 * 多个摘要」。当前仓库调用方仅为 `tests/hash/`；`compress` 的 gzip 编码器
 * 是预期的生产调用方（帧尾 CRC32）。
 *
 * ## 生命周期与线程模型
 *
 * 构造时绑定 `Table`（存指针，不拥有）；调用方必须保证表比实例活得久。
 * 实例本身无锁：同一实例被多线程写需要外部同步，不同实例可并行。
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
    const Table* m_table;      ///< 指向构造时传入的查表（不拥有）；Write 解引用传给 Update，构造后不再改
    std::uint32_t m_crc{0};    ///< 当前 CRC（已含离开取反）；Write 累加、Reset 清零、Sum32/Sum 读取
};

}  // namespace go2cpp::hash
