#pragma once

/**
 * @file fec.hpp
 * @brief 通用前向纠错：XOR 与 GF(256) Reed-Solomon 编解码器。
 * @details 为什么需要 FEC（弱网实测结论）：
 *
 *          Bento 的可靠通道只有「丢包 → 等 RTO/快速重传 → 补发」这一条恢复路径。
 *          在 100ms RTT 链路上，一次丢包至少要多花 1 个 RTT；5% 丢包时几乎每个
 *          窗口都要等一次，端到端延迟的 p99 被重传主导。FEC 用「多传 1/k 的
 *          冗余」把恢复延迟从「1 个 RTT」降到「组内最后一个包到达的时刻」，
 *          对实时流（操控、视频）价值最大。
 *
 *          两类编码器共用同一接口：
 *          - XOR（m=1）：单校验分片，只能恢复 1 个丢失分片，开销 1/k，编码/解码
 *            是纯 XOR，代价可忽略；适合 5% 丢包。
 *          - Reed-Solomon（Cauchy 矩阵，m>=1）：可恢复任意 m 个丢失分片，
 *            代价是 O(k·m·len) 的 GF(256) 乘加与 O(k³) 的矩阵求逆；适合突发丢包。
 *
 *          **MDS 保证**：Cauchy 矩阵 C[j][i] = 1 / (x_j ⊕ y_i)（x、y 互不相交）
 *          的任意方子阵可逆，因此「数据行（单位阵）+ 校验行（Cauchy）」构成的
 *          生成矩阵任意 k 行可逆——这是解码只需 k 个任意分片的原因。
 *
 *          边界：k+m 必须 <= 255（GF(256) 的指数表长度）；组内分片长度不同时
 *          统一零填充到 max_len，解码后按长度表裁剪。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::bento {

/// @brief FEC 编码类型。
enum class FecKind : std::uint8_t {
    kXor = 0,         ///< 单奇偶校验（m 必须为 1）。
    kReedSolomon = 1, ///< GF(256) Cauchy Reed-Solomon。
};

/// @brief FEC 类型名（日志/报告用）。
const char* FecKindName(FecKind kind) noexcept;

/// @brief 一个分片：index 为分片序号，data 为分片数据。
struct FecShard {
    std::uint16_t index = 0;
    std::vector<std::uint8_t> data;
};

/// @brief FEC 配置（通道层使用）。
struct FecConfig {
    /// 是否启用。默认关闭：FEC 是带宽换延迟的取舍，应由使用方显式开启。
    bool enabled = false;
    FecKind kind = FecKind::kXor;
    /// 每组数据分片数 k（2..64）。
    std::uint16_t data_shards = 8;
    /// 每组校验分片数 m（1..16）；XOR 固定为 1。
    std::uint16_t parity_shards = 1;
    /// 参与 FEC 的最大单帧载荷；超过该长度的帧不参与分组（见 Channel 的说明）。
    std::size_t max_payload_bytes = 1200;
    /// FEC 组的超时清理时间（毫秒）。
    std::uint32_t group_timeout_ms = 2000;
};

/// @brief 编码器：收集 k 个源分片，产出 m 个校验分片。
class FecEncoder final {
   public:
    FecEncoder(FecKind kind, std::uint16_t data_shards, std::uint16_t parity_shards);

    /// @brief 加入一个源分片。index 必须 < data_shards，同一 index 只能加一次。
    /// @return 长度超限、index 非法或重复时返回 false。
    bool AddSource(std::uint16_t index, const std::uint8_t* data, std::size_t bytes);

    /// @brief 生成校验分片；要求所有源分片都已加入。
    /// @param parity 输出 m 个分片（index = 0..m-1）。
    /// @param max_len 输出组内最大原始长度（填充后的分片长度）。
    /// @return 源分片不齐或参数非法时返回 false。
    bool Encode(std::vector<FecShard>* parity, std::size_t* max_len) const;

    /// @brief 清空状态，准备下一组。
    void Reset();

    /// @brief 是否已收齐所有源分片。
    bool complete() const noexcept {
        return m_source_count == m_data_shards;
    }
    std::uint16_t data_shards() const noexcept {
        return m_data_shards;
    }
    std::uint16_t parity_shards() const noexcept {
        return m_parity_shards;
    }
    FecKind kind() const noexcept {
        return m_kind;
    }
    /// @brief 组内最大分片长度（未收齐时为已收分片的最大值）。
    std::size_t max_len() const noexcept {
        return m_max_len;
    }

    /// @brief 校验分片的字节数 = 组内最大长度。
    std::size_t parity_bytes() const noexcept {
        return m_max_len;
    }

    /// @brief 在给定帧容量下，k/m 组合能支持的最大单帧载荷。
    /// @details FEC 分片本身也要放进一个 Bento 帧，因此
    ///          payload <= frame_capacity - fec_header_bytes - 2×k。
    static std::size_t MaxPayloadFor(std::size_t frame_capacity, std::size_t fec_header_bytes,
                                     std::uint16_t data_shards) noexcept;

   private:
    FecKind m_kind;
    std::uint16_t m_data_shards;
    std::uint16_t m_parity_shards;
    std::size_t m_max_len = 0;
    std::uint16_t m_source_count = 0;
    std::vector<std::vector<std::uint8_t>> m_sources;  ///< 下标 = 源分片 index。
    std::vector<bool> m_present;
};

/// @brief 解码器：加入任意 >= k 个分片（数据 + 校验），恢复缺失的数据分片。
class FecDecoder final {
   public:
    FecDecoder(FecKind kind, std::uint16_t data_shards, std::uint16_t parity_shards);

    /// @brief 设定填充长度（组内最大原始长度），必须在 Add* 之前调用。
    bool SetMaxLen(std::size_t max_len);

    /// @brief 加入一个源分片（未丢失的数据帧）。
    bool AddSource(std::uint16_t index, const std::uint8_t* data, std::size_t bytes);

    /// @brief 加入一个校验分片。
    bool AddParity(std::uint16_t index, const std::uint8_t* data, std::size_t bytes);

    /// @brief 尝试恢复缺失的源分片。
    /// @param recovered 输出恢复出的分片（按 index 升序），只在返回 true 时有意义。
    /// @param error 失败原因（可为 nullptr）。
    /// @return 仍缺分片、分片数不足或矩阵不可逆时返回 false。
    bool Recover(std::vector<FecShard>* recovered, std::string* error) const;

    /// @brief 缺失的源分片数量。
    std::uint16_t missing_count() const noexcept;
    /// @brief 已加入的分片数量（数据 + 校验）。
    std::uint16_t shard_count() const noexcept {
        return m_shard_count;
    }
    std::uint16_t data_shards() const noexcept {
        return m_data_shards;
    }
    std::uint16_t parity_shards() const noexcept {
        return m_parity_shards;
    }
    FecKind kind() const noexcept {
        return m_kind;
    }
    std::size_t max_len() const noexcept {
        return m_max_len;
    }
    bool has_source(std::uint16_t index) const noexcept;
    bool has_parity(std::uint16_t index) const noexcept;

    /// @brief 清空状态。
    void Reset();

   private:
    bool RecoverXor(std::vector<FecShard>* recovered, std::string* error) const;

    FecKind m_kind;
    std::uint16_t m_data_shards;
    std::uint16_t m_parity_shards;
    std::size_t m_max_len = 0;
    std::uint16_t m_shard_count = 0;
    std::vector<std::vector<std::uint8_t>> m_sources;
    std::vector<std::vector<std::uint8_t>> m_parities;
    std::vector<bool> m_source_present;
    std::vector<bool> m_parity_present;
};

/// @brief GF(256) 乘法（暴露给单测与上层统计用）。
std::uint8_t Gf256Multiply(std::uint8_t a, std::uint8_t b) noexcept;
/// @brief GF(256) 除法（b 为 0 时返回 0）。
std::uint8_t Gf256Divide(std::uint8_t a, std::uint8_t b) noexcept;
/// @brief GF(256) 幂。
std::uint8_t Gf256Pow(std::uint8_t base, std::uint8_t exponent) noexcept;

}  // namespace go2cpp::bento
