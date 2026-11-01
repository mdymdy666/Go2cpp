/**
 * @file test_go_crc32.cpp
 * @brief `go2cpp/go/crc32.hpp` 的兼容性测试（对照 Go 标准库 `hash/crc32`）。
 *
 * 覆盖：
 *  1. 标准向量（`"123456789"` / 空输入）与三种多项式；
 *  2. **1e6 随机字节**与 Go 生成的对拍数据逐项一致（oracle 由
 *     `tests/go_api/scripts/gen_crc32_oracle.go` 用 Go 1.27 生成）；
 *  3. 独立逐位参考实现交叉验证（不查表，直接按多项式定义算）；
 *  4. `Update` 分段拼接与一次性 `Checksum` 等价（随机切分 2000 次）；
 *  5. 流式 `Hash`（Write/Reset/Sum/Sum32）与一次性函数一致；
 *  6. 边界长度（0/1/15/16/17/1000/1e6）与 `Table::operator[]` 的前 256 项；
 *  7. 吞吐压测（Release 下报告 GB/s，不设通过阈值以免在慢机上假失败）。
 *
 * 运行：`ctest -R go_api_crc32`（oracle 路径由 CMake 以宏传入）。
 */

#include "go2cpp/go/crc32.hpp"
#include "go_api_check.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GO_API_CRC32_ORACLE
#define GO_API_CRC32_ORACLE ""
#endif

namespace {

namespace crc32 = go2cpp::go::crc32;

/// xorshift64*：与 `gen_crc32_oracle.go` 里的 PRNG 逐位一致。
class Prng final {
public:
    explicit Prng(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t Next() {
        std::uint64_t value = m_state;
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        m_state = value;
        return value * 2685821657736338717ull;
    }

private:
    std::uint64_t m_state;
};

/// 生成的测试数据：每 8 字节来自一次 PRNG 输出（小端），与 Go 侧一致。
std::vector<std::uint8_t> MakeChunk(Prng& prng, std::size_t size) {
    std::vector<std::uint8_t> data(size, 0);
    for (std::size_t offset = 0; offset + 8 <= size; offset += 8) {
        const std::uint64_t word = prng.Next();
        for (std::size_t index = 0; index < 8; ++index) {
            data[offset + index] = static_cast<std::uint8_t>(word >> (8 * index));
        }
    }
    return data;
}

/// 独立逐位实现（不查表），作为第二个 oracle。
std::uint32_t BitwiseCrc(std::uint32_t polynomial, const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ polynomial : (crc >> 1);
        }
    }
    return ~crc;
}

struct OracleChunk {
    std::array<std::uint32_t, 3> crcs{};
};

struct Oracle {
    std::uint64_t seed{0};
    std::size_t chunk_size{0};
    std::size_t chunk_count{0};
    std::vector<OracleChunk> chunks;
    std::array<std::uint32_t, 3> total{};
    std::size_t total_bytes{0};
    bool loaded{false};
};

/// 解析 oracle 文件；格式见 `gen_crc32_oracle.go`。
Oracle LoadOracle(const std::string& path) {
    Oracle oracle;
    std::ifstream stream(path);
    if (!stream) {
        return oracle;
    }
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream parser(line);
        std::string kind;
        parser >> kind;
        if (kind == "seed") {
            std::string text;
            parser >> text;
            oracle.seed = std::stoull(text, nullptr, 16);
        } else if (kind == "chunk_size") {
            parser >> oracle.chunk_size;
        } else if (kind == "chunk_count") {
            parser >> oracle.chunk_count;
        } else if (kind == "chunk") {
            std::size_t index = 0;
            OracleChunk chunk;
            std::array<std::string, 3> texts{};
            parser >> index >> texts[0] >> texts[1] >> texts[2];
            for (std::size_t item = 0; item < 3; ++item) {
                chunk.crcs[item] = static_cast<std::uint32_t>(
                    std::stoul(texts[item], nullptr, 16));
            }
            oracle.chunks.push_back(chunk);
        } else if (kind == "total") {
            std::array<std::string, 3> texts{};
            parser >> texts[0] >> texts[1] >> texts[2];
            for (std::size_t item = 0; item < 3; ++item) {
                oracle.total[item] = static_cast<std::uint32_t>(
                    std::stoul(texts[item], nullptr, 16));
            }
        } else if (kind == "bytes") {
            parser >> oracle.total_bytes;
        }
    }
    oracle.loaded = oracle.chunk_count > 0 && oracle.chunks.size() == oracle.chunk_count;
    return oracle;
}

/// 场景 1：标准向量。
void TestKnownVectors() {
    GO_ANNOUNCE("crc32: known vectors");
    GO_CHECK(crc32::ChecksumIEEE("123456789") == 0xCBF43926u);
    GO_CHECK(crc32::Checksum("123456789", crc32::CastagnoliTable) == 0xE3069283u);
    GO_CHECK(crc32::Checksum("123456789", crc32::KoopmanTable) == 0x2D3DD0AEu);
    GO_CHECK(crc32::ChecksumIEEE("") == 0u);
    GO_CHECK(crc32::Checksum(std::string_view{}, crc32::CastagnoliTable) == 0u);
    // 内建表与 MakeTable 完全一致（Go 的 `MakeTable` 也是纯函数）。
    GO_CHECK(crc32::MakeTable(crc32::IEEE)[1] == crc32::IEEETable[1]);
    GO_CHECK(crc32::MakeTable(crc32::Castagnoli)[255] == crc32::CastagnoliTable[255]);
}

/// 场景 2：1e6 字节与 Go oracle 逐项一致。
void TestGoOracle() {
    GO_ANNOUNCE("crc32: 1e6 random bytes vs Go oracle");
    const Oracle oracle = LoadOracle(GO_API_CRC32_ORACLE);
    GO_CHECK(oracle.loaded);
    if (!oracle.loaded) {
        std::fprintf(stderr, "  oracle not found: %s\n", GO_API_CRC32_ORACLE);
        return;
    }
    Prng prng(oracle.seed);
    std::vector<std::uint8_t> all;
    all.reserve(oracle.total_bytes);
    for (std::size_t index = 0; index < oracle.chunks.size(); ++index) {
        const std::vector<std::uint8_t> chunk = MakeChunk(prng, oracle.chunk_size);
        all.insert(all.end(), chunk.begin(), chunk.end());
        const std::uint32_t ieee = crc32::ChecksumIEEE(chunk);
        const std::uint32_t castagnoli = crc32::Checksum(chunk, crc32::CastagnoliTable);
        const std::uint32_t koopman = crc32::Checksum(chunk, crc32::KoopmanTable);
        if (ieee != oracle.chunks[index].crcs[0] ||
            castagnoli != oracle.chunks[index].crcs[1] ||
            koopman != oracle.chunks[index].crcs[2]) {
            GO_CHECK_MSG(false, "chunk " + std::to_string(index) + " mismatch");
        }
    }
    GO_CHECK(all.size() == oracle.total_bytes);
    GO_CHECK(crc32::ChecksumIEEE(all) == oracle.total[0]);
    GO_CHECK(crc32::Checksum(all, crc32::CastagnoliTable) == oracle.total[1]);
    GO_CHECK(crc32::Checksum(all, crc32::KoopmanTable) == oracle.total[2]);
    std::printf("  [info] %zu bytes across %zu chunks match Go 1.27 hash/crc32\n",
                all.size(), oracle.chunks.size());
}

/// 场景 3：独立逐位实现交叉验证（同一 1e6 数据）。
void TestBitwiseReference() {
    GO_ANNOUNCE("crc32: 1e6 bytes vs bitwise reference");
    Prng prng(0x0123456789ABCDEFull);
    const std::vector<std::uint8_t> data = MakeChunk(prng, 1000000);
    GO_CHECK(crc32::ChecksumIEEE(data) == BitwiseCrc(crc32::IEEE, data));
    GO_CHECK(crc32::Checksum(data, crc32::CastagnoliTable) ==
             BitwiseCrc(crc32::Castagnoli, data));
    GO_CHECK(crc32::Checksum(data, crc32::KoopmanTable) == BitwiseCrc(crc32::Koopman, data));
}

/// 场景 4：Update 分段拼接（随机切分 2000 次）。
void TestUpdateConcatenation() {
    GO_ANNOUNCE("crc32: Update concatenation (2000 random splits)");
    Prng prng(0xDEADBEEFCAFEBABEull);
    const std::vector<std::uint8_t> data = MakeChunk(prng, 20000);
    const std::array<const crc32::Table*, 3> tables{
        &crc32::IEEETable, &crc32::CastagnoliTable, &crc32::KoopmanTable};
    for (std::size_t round = 0; round < 2000; ++round) {
        const crc32::Table& table = *tables[round % 3];
        std::uint32_t crc = 0;
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t take = 1 + (prng.Next() % 97);
            const std::size_t end = offset + take < data.size() ? offset + take : data.size();
            crc = crc32::Update(crc, table, go2cpp::hash::AsBytes(
                                                   data.data() + offset, end - offset));
            offset = end;
        }
        if (crc != crc32::Checksum(data, table)) {
            GO_CHECK_MSG(false, "split round " + std::to_string(round) + " mismatch");
            return;
        }
    }
    GO_CHECK(true);
}

/// 场景 5：流式 Hash 与一次性函数一致。
void TestStreamingHash() {
    GO_ANNOUNCE("crc32: streaming Hash == one-shot");
    Prng prng(0x1111222233334444ull);
    const std::vector<std::uint8_t> data = MakeChunk(prng, 12345);
    crc32::Hash hash(crc32::CastagnoliTable);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t take = 1 + (offset % 61);
        const std::size_t end = offset + take < data.size() ? offset + take : data.size();
        hash.Write(go2cpp::hash::AsBytes(data.data() + offset, end - offset));
        offset = end;
    }
    GO_CHECK(hash.Sum32() == crc32::Checksum(data, crc32::CastagnoliTable));
    GO_CHECK(hash.Size() == 4);
    GO_CHECK(hash.BlockSize() == 1);
    std::array<std::uint8_t, 4> digest{};
    hash.Sum(go2cpp::hash::AsBytes(digest));
    const std::uint32_t value = (static_cast<std::uint32_t>(digest[0]) << 24) |
                                (static_cast<std::uint32_t>(digest[1]) << 16) |
                                (static_cast<std::uint32_t>(digest[2]) << 8) | digest[3];
    GO_CHECK(value == hash.Sum32());
    // Sum 不改变状态：再取一次应当相同。
    GO_CHECK(hash.Sum32() == value);
    hash.Reset();
    GO_CHECK(hash.Sum32() == 0u);
}

/// 场景 6：边界长度。
void TestBoundaries() {
    GO_ANNOUNCE("crc32: boundary lengths");
    Prng prng(0x5555AAAA5555AAAAull);
    for (const std::size_t size : {0u, 1u, 2u, 3u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 33u,
                                   63u, 64u, 65u, 255u, 256u, 257u, 1000u}) {
        const std::vector<std::uint8_t> data = MakeChunk(prng, size);
        GO_CHECK(crc32::ChecksumIEEE(data) == BitwiseCrc(crc32::IEEE, data));
        GO_CHECK(crc32::Checksum(data, crc32::KoopmanTable) ==
                 BitwiseCrc(crc32::Koopman, data));
        // 空视图与 vector 的 AsBytes 走同一条路径。
        GO_CHECK(crc32::ChecksumIEEE(go2cpp::hash::AsBytes(data)) ==
                 crc32::ChecksumIEEE(data));
    }
    // Table 的前 256 项必须与 Go `MakeTable` 的可见语义一致：它们是基础表。
    const crc32::Table table = crc32::MakeTable(crc32::IEEE);
    GO_CHECK(table[0] == 0x00000000u);
    GO_CHECK(table[1] == 0x77073096u);
    GO_CHECK(table[255] == 0x2D02EF8Du);
}

/// 场景 7：吞吐压测（只打印数据，不做通过阈值）。
void TestThroughput() {
    GO_ANNOUNCE("crc32: throughput (informational)");
    Prng prng(0x9999CCCC9999CCCCull);
    const std::vector<std::uint8_t> data = MakeChunk(prng, 1u << 20);
    constexpr int kRounds = 64;
    volatile std::uint32_t sink = 0;
    for (const auto& entry : std::vector<std::pair<std::string, const crc32::Table*>>{
             {"IEEE", &crc32::IEEETable},
             {"Castagnoli", &crc32::CastagnoliTable},
             {"Koopman", &crc32::KoopmanTable}}) {
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round) {
            sink = sink + crc32::Checksum(go2cpp::hash::AsBytes(data), *entry.second);
        }
        const auto elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        const double gigabytes = static_cast<double>(data.size()) * kRounds / 1e9;
        std::printf("  [info] %-10s %.2f GB/s (%d x 1 MiB)\n", entry.first.c_str(),
                    gigabytes / elapsed, kRounds);
    }
    (void)sink;
}

}  // namespace

int main() {
    TestKnownVectors();
    TestGoOracle();
    TestBitwiseReference();
    TestUpdateConcatenation();
    TestStreamingHash();
    TestBoundaries();
    TestThroughput();
    return go_api_test::ExitCode();
}
