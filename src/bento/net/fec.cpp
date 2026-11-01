/**
 * @file fec.cpp
 * @brief 通用 FEC 实现：GF(256) 运算、XOR 与 Cauchy Reed-Solomon 编解码。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/bento/net/fec.hpp"

#include <algorithm>
#include <cstring>

namespace go2cpp::bento {
namespace {

/// GF(256) 的不可约多项式（x^8 + x^4 + x^3 + x^2 + 1），与 RS(255,223) 同源。
constexpr std::uint16_t kGfPrimitivePoly = 0x11D;

struct GfTables {
    std::uint8_t exp[512];
    std::uint8_t log[256];

    GfTables() noexcept {
        std::uint16_t value = 1;
        for (std::uint16_t i = 0; i < 255; ++i) {
            exp[i] = static_cast<std::uint8_t>(value);
            log[value] = static_cast<std::uint8_t>(i);
            value = static_cast<std::uint16_t>(value << 1);
            if ((value & 0x100U) != 0) {
                value ^= kGfPrimitivePoly;
            }
        }
        for (std::size_t i = 255; i < 512; ++i) {
            exp[i] = exp[i - 255];
        }
        log[0] = 0;  // 0 的对数无定义；乘除法先判 0，不会用到该值。
    }
};

const GfTables& Tables() noexcept {
    static const GfTables tables;
    return tables;
}

/// Cauchy 生成矩阵系数：校验行 j、数据列 i。
/// 取 x_j = k + j、y_i = i，二者集合不相交，保证 (k+j) ^ i != 0。
std::uint8_t CauchyCoefficient(std::uint16_t data_shards, std::uint16_t parity_index,
                               std::uint16_t source_index) noexcept {
    const std::uint8_t denominator =
        static_cast<std::uint8_t>((data_shards + parity_index) ^ source_index);
    return Gf256Divide(1, denominator);
}

bool ValidShape(std::uint16_t data_shards, std::uint16_t parity_shards, FecKind kind) noexcept {
    if (data_shards < 2 || data_shards > 64) {
        return false;
    }
    if (parity_shards < 1 || parity_shards > 16) {
        return false;
    }
    if (kind == FecKind::kXor && parity_shards != 1) {
        return false;
    }
    return static_cast<std::uint32_t>(data_shards) + parity_shards <= 255U;
}

/// 原地求 GF(256) 下的 n×n 逆矩阵；不可逆返回 false。
bool InvertMatrix(std::vector<std::uint8_t>* matrix, std::size_t n) {
    if (matrix == nullptr || matrix->size() != n * n) {
        return false;
    }
    std::vector<std::uint8_t> augmented(n * 2 * n, 0);
    for (std::size_t row = 0; row < n; ++row) {
        for (std::size_t column = 0; column < n; ++column) {
            augmented[row * 2 * n + column] = (*matrix)[row * n + column];
        }
        augmented[row * 2 * n + n + row] = 1;
    }

    const std::size_t width = 2 * n;
    for (std::size_t column = 0; column < n; ++column) {
        std::size_t pivot = n;
        for (std::size_t row = column; row < n; ++row) {
            if (augmented[row * width + column] != 0) {
                pivot = row;
                break;
            }
        }
        if (pivot == n) {
            return false;  // 奇异矩阵
        }
        if (pivot != column) {
            for (std::size_t k = 0; k < width; ++k) {
                std::swap(augmented[pivot * width + k], augmented[column * width + k]);
            }
        }
        const std::uint8_t pivot_value = augmented[column * width + column];
        if (pivot_value != 1) {
            const std::uint8_t inverse = Gf256Divide(1, pivot_value);
            for (std::size_t k = 0; k < width; ++k) {
                augmented[column * width + k] = Gf256Multiply(augmented[column * width + k], inverse);
            }
        }
        for (std::size_t row = 0; row < n; ++row) {
            if (row == column) {
                continue;
            }
            const std::uint8_t factor = augmented[row * width + column];
            if (factor == 0) {
                continue;
            }
            for (std::size_t k = 0; k < width; ++k) {
                augmented[row * width + k] =
                    static_cast<std::uint8_t>(augmented[row * width + k] ^
                                              Gf256Multiply(factor, augmented[column * width + k]));
            }
        }
    }

    for (std::size_t row = 0; row < n; ++row) {
        for (std::size_t column = 0; column < n; ++column) {
            (*matrix)[row * n + column] = augmented[row * width + n + column];
        }
    }
    return true;
}

}  // namespace

std::uint8_t Gf256Multiply(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    const GfTables& tables = Tables();
    return tables.exp[static_cast<std::uint16_t>(tables.log[a]) + tables.log[b]];
}

std::uint8_t Gf256Divide(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    const GfTables& tables = Tables();
    const std::uint16_t exponent =
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(tables.log[a]) + 255U -
                                   tables.log[b]);
    return tables.exp[exponent % 255U];
}

std::uint8_t Gf256Pow(std::uint8_t base, std::uint8_t exponent) noexcept {
    if (exponent == 0) {
        return 1;
    }
    if (base == 0) {
        return 0;
    }
    const GfTables& tables = Tables();
    return tables.exp[(static_cast<std::uint16_t>(tables.log[base]) * exponent) % 255U];
}

const char* FecKindName(FecKind kind) noexcept {
    switch (kind) {
        case FecKind::kXor:
            return "xor";
        case FecKind::kReedSolomon:
            return "reed-solomon";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// FecEncoder
// ---------------------------------------------------------------------------

FecEncoder::FecEncoder(FecKind kind, std::uint16_t data_shards, std::uint16_t parity_shards)
    : m_kind(kind),
      m_data_shards(ValidShape(data_shards, parity_shards, kind) ? data_shards : 0),
      m_parity_shards(ValidShape(data_shards, parity_shards, kind) ? parity_shards : 0) {
    if (m_data_shards != 0) {
        m_sources.resize(m_data_shards);
        m_present.assign(m_data_shards, false);
    }
}

std::size_t FecEncoder::MaxPayloadFor(std::size_t frame_capacity, std::size_t fec_header_bytes,
                                      std::uint16_t data_shards) noexcept {
    const std::size_t overhead = fec_header_bytes + static_cast<std::size_t>(data_shards) * 2U;
    if (frame_capacity <= overhead) {
        return 0;
    }
    return frame_capacity - overhead;
}

bool FecEncoder::AddSource(std::uint16_t index, const std::uint8_t* data, std::size_t bytes) {
    if (m_data_shards == 0 || index >= m_data_shards || (bytes > 0 && data == nullptr)) {
        return false;
    }
    if (bytes > 0xFFFFU || m_present[index]) {
        return false;
    }
    m_sources[index].assign(data, data + bytes);
    m_present[index] = true;
    ++m_source_count;
    m_max_len = std::max(m_max_len, bytes);
    return true;
}

bool FecEncoder::Encode(std::vector<FecShard>* parity, std::size_t* max_len) const {
    if (parity == nullptr || m_data_shards == 0 || m_source_count != m_data_shards) {
        return false;
    }
    parity->clear();
    parity->resize(m_parity_shards);
    for (std::uint16_t j = 0; j < m_parity_shards; ++j) {
        FecShard& shard = (*parity)[j];
        shard.index = j;
        shard.data.assign(m_max_len, 0);
        if (m_kind == FecKind::kXor) {
            // XOR 是 Cauchy 在 m=1 时的特例；直接异或更快。
            for (std::uint16_t i = 0; i < m_data_shards; ++i) {
                const std::vector<std::uint8_t>& source = m_sources[i];
                for (std::size_t pos = 0; pos < source.size(); ++pos) {
                    shard.data[pos] = static_cast<std::uint8_t>(shard.data[pos] ^ source[pos]);
                }
            }
            continue;
        }
        for (std::uint16_t i = 0; i < m_data_shards; ++i) {
            const std::uint8_t coefficient = CauchyCoefficient(m_data_shards, j, i);
            if (coefficient == 0) {
                continue;
            }
            const std::vector<std::uint8_t>& source = m_sources[i];
            for (std::size_t pos = 0; pos < source.size(); ++pos) {
                shard.data[pos] =
                    static_cast<std::uint8_t>(shard.data[pos] ^ Gf256Multiply(coefficient, source[pos]));
            }
        }
    }
    if (max_len != nullptr) {
        *max_len = m_max_len;
    }
    return true;
}

void FecEncoder::Reset() {
    for (std::uint16_t i = 0; i < m_data_shards; ++i) {
        m_sources[i].clear();
        m_present[i] = false;
    }
    m_source_count = 0;
    m_max_len = 0;
}

// ---------------------------------------------------------------------------
// FecDecoder
// ---------------------------------------------------------------------------

FecDecoder::FecDecoder(FecKind kind, std::uint16_t data_shards, std::uint16_t parity_shards)
    : m_kind(kind),
      m_data_shards(ValidShape(data_shards, parity_shards, kind) ? data_shards : 0),
      m_parity_shards(ValidShape(data_shards, parity_shards, kind) ? parity_shards : 0) {
    if (m_data_shards != 0) {
        m_sources.resize(m_data_shards);
        m_parities.resize(m_parity_shards);
        m_source_present.assign(m_data_shards, false);
        m_parity_present.assign(m_parity_shards, false);
    }
}

bool FecDecoder::SetMaxLen(std::size_t max_len) {
    if (max_len == 0 || max_len > 0xFFFFU) {
        return false;
    }
    if (m_max_len != 0 && m_max_len != max_len) {
        return false;  // 同一组内不允许改变填充长度
    }
    m_max_len = max_len;
    return true;
}

bool FecDecoder::AddSource(std::uint16_t index, const std::uint8_t* data, std::size_t bytes) {
    if (m_data_shards == 0 || m_max_len == 0 || index >= m_data_shards ||
        (bytes > 0 && data == nullptr) || bytes > m_max_len || m_source_present[index]) {
        return false;
    }
    m_sources[index].assign(m_max_len, 0);
    if (bytes > 0) {
        std::memcpy(m_sources[index].data(), data, bytes);
    }
    m_source_present[index] = true;
    ++m_shard_count;
    return true;
}

bool FecDecoder::AddParity(std::uint16_t index, const std::uint8_t* data, std::size_t bytes) {
    if (m_data_shards == 0 || m_max_len == 0 || index >= m_parity_shards ||
        (bytes > 0 && data == nullptr) || bytes != m_max_len || m_parity_present[index]) {
        return false;
    }
    m_parities[index].assign(data, data + bytes);
    m_parity_present[index] = true;
    ++m_shard_count;
    return true;
}

std::uint16_t FecDecoder::missing_count() const noexcept {
    if (m_data_shards == 0) {
        return 0;
    }
    std::uint16_t missing = 0;
    for (std::uint16_t i = 0; i < m_data_shards; ++i) {
        if (!m_source_present[i]) {
            ++missing;
        }
    }
    return missing;
}

bool FecDecoder::has_source(std::uint16_t index) const noexcept {
    return index < m_source_present.size() && m_source_present[index];
}

bool FecDecoder::has_parity(std::uint16_t index) const noexcept {
    return index < m_parity_present.size() && m_parity_present[index];
}

void FecDecoder::Reset() {
    for (auto& source : m_sources) {
        source.clear();
    }
    for (auto& parity : m_parities) {
        parity.clear();
    }
    m_source_present.assign(m_data_shards, false);
    m_parity_present.assign(m_parity_shards, false);
    m_shard_count = 0;
    m_max_len = 0;
}

bool FecDecoder::Recover(std::vector<FecShard>* recovered, std::string* error) const {
    if (recovered == nullptr) {
        return false;
    }
    recovered->clear();
    if (m_data_shards == 0 || m_max_len == 0) {
        if (error != nullptr) {
            *error = "invalid decoder shape";
        }
        return false;
    }
    if (missing_count() == 0) {
        return true;  // 没丢，无需恢复
    }
    if (m_kind == FecKind::kXor) {
        return RecoverXor(recovered, error);
    }

    // 收集已知分片（数据 + 校验），需要 k 个才能解出 k 个未知数。
    struct KnownShard {
        bool is_parity = false;
        std::uint16_t index = 0;
    };
    std::vector<KnownShard> known;
    known.reserve(m_data_shards);
    for (std::uint16_t i = 0; i < m_data_shards && known.size() < m_data_shards; ++i) {
        if (m_source_present[i]) {
            known.push_back(KnownShard{false, i});
        }
    }
    for (std::uint16_t j = 0; j < m_parity_shards && known.size() < m_data_shards; ++j) {
        if (m_parity_present[j]) {
            known.push_back(KnownShard{true, j});
        }
    }
    if (known.size() < m_data_shards) {
        if (error != nullptr) {
            *error = "not enough shards";
        }
        return false;
    }

    const std::size_t n = m_data_shards;
    std::vector<std::uint8_t> matrix(n * n, 0);
    for (std::size_t row = 0; row < n; ++row) {
        const KnownShard& shard = known[row];
        if (!shard.is_parity) {
            matrix[row * n + shard.index] = 1;
            continue;
        }
        for (std::size_t column = 0; column < n; ++column) {
            matrix[row * n + column] =
                CauchyCoefficient(m_data_shards, shard.index, static_cast<std::uint16_t>(column));
        }
    }
    if (!InvertMatrix(&matrix, n)) {
        if (error != nullptr) {
            *error = "singular fec matrix";
        }
        return false;
    }

    // 对每个缺失的源分片：data[c] = Σ_row inv[c][row] × known[row]。
    for (std::uint16_t column = 0; column < m_data_shards; ++column) {
        if (m_source_present[column]) {
            continue;
        }
        FecShard shard;
        shard.index = column;
        shard.data.assign(m_max_len, 0);
        for (std::size_t row = 0; row < n; ++row) {
            const std::uint8_t coefficient = matrix[column * n + row];
            if (coefficient == 0) {
                continue;
            }
            const KnownShard& source = known[row];
            const std::vector<std::uint8_t>& data =
                source.is_parity ? m_parities[source.index] : m_sources[source.index];
            for (std::size_t pos = 0; pos < m_max_len; ++pos) {
                shard.data[pos] = static_cast<std::uint8_t>(
                    shard.data[pos] ^ Gf256Multiply(coefficient, data[pos]));
            }
        }
        recovered->push_back(std::move(shard));
    }
    return true;
}

bool FecDecoder::RecoverXor(std::vector<FecShard>* recovered, std::string* error) const {
    std::uint16_t missing_index = 0;
    std::uint16_t missing = 0;
    for (std::uint16_t i = 0; i < m_data_shards; ++i) {
        if (!m_source_present[i]) {
            missing_index = i;
            ++missing;
        }
    }
    if (missing != 1) {
        if (error != nullptr) {
            *error = missing == 0 ? "nothing to recover" : "xor cannot recover multiple losses";
        }
        return false;
    }
    if (!m_parity_present[0]) {
        if (error != nullptr) {
            *error = "missing xor parity";
        }
        return false;
    }
    FecShard shard;
    shard.index = missing_index;
    shard.data = m_parities[0];
    for (std::uint16_t i = 0; i < m_data_shards; ++i) {
        if (!m_source_present[i]) {
            continue;
        }
        const std::vector<std::uint8_t>& source = m_sources[i];
        for (std::size_t pos = 0; pos < m_max_len; ++pos) {
            shard.data[pos] = static_cast<std::uint8_t>(shard.data[pos] ^ source[pos]);
        }
    }
    recovered->push_back(std::move(shard));
    return true;
}

}  // namespace go2cpp::bento
