/**
 * @file sha1.hpp
 * @brief SHA-1 消息摘要（FIPS 180-4）的 C++ 实现，接口对齐 Go `crypto/sha1`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @warning **SHA-1 已不适合安全用途。** 2017 年 SHAttered 攻击给出了两个
 *          不同 PDF 的实际碰撞，此后碰撞成本持续下降（2020 年已低于 10 万美元）。
 *          NIST 自 2011 年起就禁止在数字签名中使用 SHA-1。它只能用于校验和、
 *          内容寻址、兼容旧协议（Git 对象名等）等**非安全**场景。新设计请用
 *          `Sha256` 或更强的算法；需要抗碰撞时**不要**用本类。
 *
 * @details 实现依据 FIPS 180-4（<https://doi.org/10.6028/NIST.FIPS.180-4>）：
 *   - 状态 5 个 `uint32`，初值 `0x67452301`、`0xefcdab89`、`0x98badcfe`、
 *     `0x10325476`、`0xc3d2e1f0`；
 *   - 80 轮，消息扩展 `W[i] = rotl(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1)`；
 *   - 输入与长度字段均为 **big-endian**（与 MD5 相反）；
 *   - 填充规则与 MD5 相同：`0x80` + 零，直到 ≡ 56 (mod 64)，再补 64 位比特长度。
 *
 * ## 与 Go `crypto/sha1` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `sha1.New() hash.Hash` | `Sha1 h;` |
 * | `sha1.Sum(data) [20]byte` | `Sha1::Digest(data)` |
 * | `h.Sum(nil)` | `h.Sum()` |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(20) / `kBlockSize`(64) |
 *
 * @note `Sum(ByteSpan)` 不改变状态，实现方式与 `Md5` 相同（拷贝状态后收尾）。
 * @note 未实现 Go 的 `New()` 工厂与二进制序列化接口（`MarshalBinary` 等）。
 *
 * ## 模块定位
 *
 * 本文件（`include/go2cpp/hash/sha1.hpp`）定义 SHA-1 摘要器。SHA-1 现在只用于
 * 兼容既有格式（Git 对象名、旧 TLS/HTTP 摘要、历史校验和），不能作为新的
 * 安全边界。位于 `hash` 模块最底层，不依赖其它模块；当前仓库引用方只有
 * `tests/hash/`（FIPS 向量 + 与 Python `hashlib.sha1` 对拍），生产代码尚无
 * 调用方。
 *
 * ## 本文件定义什么
 *
 * - `Sha1`：流式摘要器（`Hash` 子类），5 个 32 位链接变量 + 64 字节缓冲；
 * - `kSize`/`kBlockSize`：摘要 20 字节、块 64 字节，对应 Go `sha1.Size`/`BlockSize`；
 * - `Sha1::Digest`：一次性入口（对应 Go `sha1.Sum`）。
 *
 * ## 阅读建议
 *
 * 先确认 SHA 系列是 **big-endian**（与 MD5 相反），再按 `Write` → `Transform`
 * （80 轮，前 16 轮直接读、后 64 轮做消息扩展）→ `Finish` 的顺序读
 * `src/hash/sha1.cpp`。状态转化见 `docs/state-machines/Sha256.md`。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief SHA-1 摘要器（FIPS 180-4）。
 * @warning 不安全，见文件头。仅用于校验/兼容。
 *
 * ## 定位
 *
 * 供需要 SHA-1 兼容性的调用方使用（典型是 Git 对象名、旧协议摘要），也可
 * 作为 `Hash&` 擦除；当前仓库调用方仅为 `tests/hash/`。**不要**用它做新的
 * 签名或抗碰撞校验。
 *
 * ## 状态机
 *
 * 与 `Md5`/`Sha256` 同构：对象常驻「吸收中」，`Sum()` 在副本上「收尾」，
 * 原对象状态不变。状态转化表见 `docs/state-machines/Sha256.md`。
 *
 * ## 不变量
 *
 * - `0 <= m_buffer_len < 64`，`m_buffer[0, m_buffer_len)` 是未满块的尾部输入；
 * - `m_length` 是历史 `Write` 字节数之和，与填充无关；
 * - `Sum()` 在副本上收尾，不改变 `*this`，可重复调用。
 */
class Sha1 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `sha1.Size`。
    static constexpr std::size_t kSize = 20;
    /// 内部块大小（字节），对应 Go `sha1.BlockSize`。
    static constexpr std::size_t kBlockSize = 64;

    /// 构造即为空输入状态（等价于 Go `sha1.New()`）。
    Sha1() noexcept { Reset(); }

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 SHA-1（对应 Go `sha1.Sum`）。
     * @param data 输入字节视图（只读）。
     * @return 20 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);

 private:
    void Transform(const std::uint8_t* block) noexcept;
    void Finish(std::uint8_t* out) noexcept;

    std::uint32_t m_state[5]{};  ///< A/B/C/D/E 五个链接变量；Reset 置初值、Transform 更新、Finish 输出
    std::uint64_t m_length{0};  ///< 已写入的总字节数；Write 累加、Reset 清零、Finish 换算成比特数。
    std::uint8_t m_buffer[kBlockSize]{};  ///< 未满 64 字节的尾部输入；Write 追加、Transform 消费、Reset 只清长度
    std::size_t m_buffer_len{0};  ///< m_buffer 中有效字节数（0..63）；Write 增加、Transform 后归零、Reset 清零
};

}  // namespace go2cpp::hash
