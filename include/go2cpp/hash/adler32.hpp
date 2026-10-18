/**
 * @file adler32.hpp
 * @brief Adler-32 校验和的 C++ 实现，接口对齐 Go `hash/adler32`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法由 Mark Adler 设计，按 RFC 1950 定义独立实现）
 *
 * @details Adler-32 由 RFC 1950（ZLIB 格式）定义：
 *   - `a` 初始 1，`b` 初始 0；
 *   - 每个字节：`a = (a + byte) mod 65521`，`b = (b + a) mod 65521`；
 *   - 输出 `(b << 16) | a`。
 *
 *   **为什么分块取模**：`a`、`b` 是 32 位无符号数，逐字节取模会引入一次除法；
 *   把 5552 字节作为一块、块内只累加、块末才取模，可以省掉绝大多数除法。
 *   5552 这个数字不是随便取的：块内 `b` 的最大值约为
 *   `255 * n * (n + 1) / 2 + (n + 1) * (65520)`，当 `n = 5552` 时仍小于
 *   `2^32`（实测 4,294,690,200），`n = 5553` 就会溢出——这正是 zlib
 *   `NMAX = 5552` 的来源，本实现沿用该边界并在块内做 16 字节展开。
 *
 * ## 与 Go `hash/adler32` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `adler32.New() hash.Hash32` | `Adler32 h;` 或 `Adler32::New()` |
 * | `adler32.Checksum(data) uint32` | `Adler32::Checksum(data)` |
 * | `h.Sum32()` | `h.Sum32()` |
 * | `h.Sum(nil)` | `h.Sum()`（4 字节，**大端**，与 Go 一致） |
 *
 * @note Adler-32 不是密码学校验：它针对随机错误设计，**不能**抵抗恶意篡改
 *       （攻击者可以轻易构造同校验和的数据）。需要检测篡改时用 SHA-256。
 * @note `Sum(ByteSpan)` 不改变状态（对齐 Go 的 `Sum` 语义）。
 *
 * ## 模块定位
 *
 * 本文件（`include/go2cpp/hash/adler32.hpp`）是校验和层最轻量的成员，位于
 * 协议栈最底层：`compress` 的 zlib 流尾（RFC 1950 的 4 字节大端 Adler-32）
 * 与任何需要低成本检错的路径都可以直接 include。当前仓库里只有
 * `tests/hash/hash_test_support.hpp` 引用它（与 Python `zlib.adler32`
 * 对拍）；尚无生产调用方。本模块不依赖调度器/IO，可在任意线程使用。
 *
 * ## 本文件定义什么
 *
 * - `Adler32`：流式摘要器，实现 `hash::Hash`，状态只有 `m_a`/`m_b` 两个
 *   32 位字；
 * - `Adler32::Checksum`：一次性计算的静态入口（等价于 Go `adler32.Checksum`）；
 * - `kSize`/`kBlockSize`：向 `Hash` 接口暴露的摘要长度与内部块大小。
 *
 * ## 阅读建议
 *
 * 先看 `Sum32()` 的位拼接（`b << 16 | a`）确认输出布局，再读
 * `src/hash/adler32.cpp` 的 `Write`。分块取模的 5552 字节推导已在上方
 * 完整说明；只关心正确性时可跳过性能细节，记住「块内累加、块末取模」这一条
 * 不变式即可。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief Adler-32 摘要器（RFC 1950）。
 *
 * @details 流式实现：`Write` 可以任意拆分输入，结果与一次性输入相同；
 *          `Sum32()` 返回当前校验和，`Sum` 写入 **大端** 4 字节
 *          （与 Go `adler32.digest.Sum` 的字节序一致）。
 *
 * ## 定位
 *
 * 被 `Hash` 接口的擦除场景使用（`Adler32` 作为 `Hash&`）；但当前仓库的直接
 * 调用方只有 `tests/hash/` 的向量与对拍代码。将来 `compress` 的 zlib 编码器
 * 生成流尾校验和时，一次性场景用静态 `Checksum`，流式场景把本类作为成员并
 * 用 `Reset` 复用，避免每段数据都重新构造。
 *
 * ## 不变量
 *
 * - 除 `Write` 块内累加的中间态外，恒有 `0 <= m_a < 65521`、`0 <= m_b < 65521`；
 * - 空输入恒有 `m_a == 1 && m_b == 0`，故 `Sum32() == 1`（**不是 0**）；
 * - `Write` 只做加法与取模、不读写任何全局状态，可重入。
 */
class Adler32 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `adler32.Size`。
    static constexpr std::size_t kSize = 4;
    /// 内部块大小（字节），对应 Go `adler32.BlockSize`（Go 返回 4）。
    static constexpr std::size_t kBlockSize = 4;

    /// 构造即为空输入状态（a=1, b=0）。
    Adler32() noexcept = default;

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /// 当前校验和（`b << 16 | a`），对应 Go 的 `hash.Hash32.Sum32`。
    std::uint32_t Sum32() const noexcept {
        return (m_b << 16) | m_a;
    }

    /**
     * @brief 一次性计算 Adler-32（对应 Go `adler32.Checksum`）。
     * @param data 输入字节视图（只读）。
     * @return 校验和；空输入返回 1（不是 0）。
     */
    static std::uint32_t Checksum(ByteSpan data) noexcept;

    /// 等价于 `Adler32()`；只为对齐 Go 的 `adler32.New()` 而保留。
    static Adler32 New() noexcept { return Adler32(); }

 private:
    std::uint32_t m_a{1};  ///< 累加和 a（初值 1）；Write 累加、Reset 复位、Sum32 读取
    std::uint32_t m_b{0};  ///< a 的累计和 b（初值 0）；Write 累加、Reset 复位、Sum32 读取
};

}  // namespace go2cpp::hash
