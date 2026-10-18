/**
 * @file sha256.hpp
 * @brief SHA-256 消息摘要（FIPS 180-4）的 C++ 实现，接口对齐 Go `crypto/sha256`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @details 实现依据 FIPS 180-4（<https://doi.org/10.6028/NIST.FIPS.180-4>）：
 *   - 状态 8 个 `uint32`，取前 8 个素数平方根小数部分
 *     （`0x6a09e667` ... `0x5be0cd19`）；
 *   - 64 轮，轮常量 K 取前 64 个素数立方根小数部分（标准表）；
 *   - 消息扩展使用 σ0/σ1，压缩函数使用 Σ0/Σ1 与 Ch/Maj；
 *   - 输入与长度字段均为 **big-endian**；填充规则与 SHA-1 相同。
 *
 * ## 与 Go `crypto/sha256` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `sha256.New() hash.Hash` | `Sha256 h;` |
 * | `sha256.Sum256(data) [32]byte` | `Sha256::Digest(data)` |
 * | `h.Sum(nil)` | `h.Sum()` |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(32) / `kBlockSize`(64) |
 *
 * @note 本类只实现 SHA-256（不含 SHA-224）；`Size()==BlockSize()` 的组合
 *       与 Go 的 `crypto/sha256` 一致（64 字节块）。
 * @note `Sum(ByteSpan)` 不改变状态，实现方式与 `Md5`/`Sha1` 相同。
 * @note 未实现 Go 的 `New()` 工厂、`New224()` 与二进制序列化接口。
 *
 * ## 模块定位
 *
 * 本文件（`include/go2cpp/hash/sha256.hpp`）定义 SHA-256 摘要器，是本模块
 * 推荐的默认安全摘要：新设计需要抗碰撞时优先用它（或 `Sha512`）。位于
 * `hash` 模块最底层，不依赖其它模块；当前仓库引用方只有 `tests/hash/`
 * （FIPS 向量 + 与 Python `hashlib.sha256` 对拍、压测），生产代码尚无调用方
 * —— 不排除将来作为内容寻址/ETag/协议握手的摘要实现接入。
 *
 * ## 本文件定义什么
 *
 * - `Sha256`：流式摘要器（`Hash` 子类），8 个 32 位链接变量 + 64 字节缓冲；
 * - `kSize`/`kBlockSize`：摘要 32 字节、块 64 字节，对应 Go 的 `Size`/`BlockSize`；
 * - `Sha256::Digest`：一次性入口（对应 Go `sha256.Sum256`）。
 *
 * ## 阅读建议
 *
 * 先看 `Write` 的三段式缓冲（补块 → 整块 → 存余），再读 `Transform` 的
 * 消息扩展与 64 轮压缩，最后看 `Finish` 的填充与比特长度。状态转化见
 * `docs/state-machines/Sha256.md`（同时覆盖 `Md5`/`Sha1`）。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief SHA-256 摘要器（FIPS 180-4）。
 *
 * @details 性能预期：Release（`-O3`）下 200~500 MB/s 属正常范围（标量实现，
 *          未用 SHA-NI 指令）；与 Go 的汇编实现相比会有差距，但满足「纯 C++、
 *          零依赖」的约束。压测数据见 `tests/hash/test_hash_stress.cpp`。
 *
 * ## 定位
 *
 * 本模块的默认安全摘要，供需要抗碰撞的调用方直接使用（一次性用 `Digest`，
 * 流式用 `Write`+`Sum`），也可作为 `Hash&` 擦除；当前仓库调用方仅为
 * `tests/hash/`。
 *
 * ## 状态机
 *
 * 与 `Md5`/`Sha1` 同构：对象常驻「吸收中」，`Sum()` 在副本上「收尾」，
 * 原对象状态不变。状态转化表见 `docs/state-machines/Sha256.md`。
 *
 * ## 不变量
 *
 * - `0 <= m_buffer_len < 64`，`m_buffer[0, m_buffer_len)` 是未满块的尾部输入；
 * - `m_length` 是历史 `Write` 字节数之和，与填充无关；
 * - `Sum()` 在副本上 `Finish`，不改变 `*this`，可重复调用。
 */
class Sha256 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `sha256.Size`。
    static constexpr std::size_t kSize = 32;
    /// 内部块大小（字节），对应 Go `sha256.BlockSize`。
    static constexpr std::size_t kBlockSize = 64;

    /// 构造即为空输入状态（等价于 Go `sha256.New()`）。
    Sha256() noexcept { Reset(); }

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 SHA-256（对应 Go `sha256.Sum256`）。
     * @param data 输入字节视图（只读）。
     * @return 32 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);

 private:
    void Transform(const std::uint8_t* block) noexcept;
    void Finish(std::uint8_t* out) noexcept;

    std::uint32_t m_state[8]{};  ///< A..H 八个链接变量；Reset 置初值、Transform 更新、Finish 输出
    std::uint64_t m_length{0};  ///< 已写入的总字节数；Write 累加、Reset 清零、Finish 换算成比特数。
    std::uint8_t m_buffer[kBlockSize]{};  ///< 未满 64 字节的尾部输入；Write 追加、Transform 消费、Reset 只清长度
    std::size_t m_buffer_len{0};  ///< m_buffer 中有效字节数（0..63）；Write 增加、Transform 后归零、Reset 清零
};

}  // namespace go2cpp::hash
