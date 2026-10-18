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
    std::uint32_t m_a{1};
    std::uint32_t m_b{0};
};

}  // namespace go2cpp::hash
