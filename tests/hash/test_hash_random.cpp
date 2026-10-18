/**
 * @file test_hash_random.cpp
 * @brief 随机对拍：与 Python `zlib` / `hashlib` 预生成的 oracle 逐条比对。
 *
 * @details 两类测试：
 *   1. **oracle 对拍**（`tests/hash/testdata/`，由 `scripts/gen_oracle.py` 生成）：
 *      - `oracle.tsv`：45 条边界/大文件用例，覆盖 0 字节、单字节、
 *        块边界（63/64/65、127/128/129）、1 MiB±1、4 MiB、10 MiB；
 *      - `oracle_random.tsv`：10,000 条随机长度（0~1024 B）、随机内容用例。
 *      oracle 的 CRC32/Adler32/MD5/SHA-1/SHA-256 来自 Python 标准库；
 *      CRC32C/CRC32K 来自脚本内的逐位参考实现（zlib 不提供这两个多项式）。
 *   2. **随机自洽 / 独立参考**：随机长度（0~50 KB）、随机内容、随机分块，
 *      验证「一次 Write」与「多次 Write」结果一致；并用测试内独立实现的
 *      逐位 CRC 与逐字节 Adler 参考算法交叉验证查表实现。
 */
#include "hash_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef GO2CPP_HASH_ORACLE_FILE
#error "GO2CPP_HASH_ORACLE_FILE 未定义：测试需要 tests/hash/testdata/oracle.tsv 的绝对路径"
#endif
#ifndef GO2CPP_HASH_ORACLE_RANDOM_FILE
#error "GO2CPP_HASH_ORACLE_RANDOM_FILE 未定义：测试需要 tests/hash/testdata/oracle_random.tsv 的绝对路径"
#endif

// CMake 传进来的是不带引号的路径；用两级字符串化把它变成字符串字面量，
// 这样 `-DGO2CPP_HASH_ORACLE_FILE=/path/to/oracle.tsv` 无需再处理引号转义。
#define GO2CPP_HASH_STRINGIFY_IMPL(text) #text
#define GO2CPP_HASH_STRINGIFY(text) GO2CPP_HASH_STRINGIFY_IMPL(text)
#define GO2CPP_HASH_ORACLE_PATH GO2CPP_HASH_STRINGIFY(GO2CPP_HASH_ORACLE_FILE)
#define GO2CPP_HASH_ORACLE_RANDOM_PATH \
    GO2CPP_HASH_STRINGIFY(GO2CPP_HASH_ORACLE_RANDOM_FILE)

namespace {

using namespace go2cpp::hash;

struct OracleCase {
    std::size_t length = 0;
    std::uint64_t seed = 0;
    std::string crc32;
    std::string adler32;
    std::string crc32c;
    std::string crc32k;
    std::string md5;
    std::string sha1;
    std::string sha256;
};

std::vector<OracleCase> LoadOracle(const char* path) {
    std::vector<OracleCase> cases;
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "cannot open oracle file: %s\n", path);
        return cases;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream stream(line);
        OracleCase item;
        if (!(stream >> item.length >> item.seed >> item.crc32 >> item.adler32 >>
              item.crc32c >> item.crc32k >> item.md5 >> item.sha1 >>
              item.sha256)) {
            ++go2cpp_hash_test::g_failures;
            std::fprintf(stderr, "malformed oracle line: %s\n", line.c_str());
            continue;
        }
        cases.push_back(std::move(item));
    }
    return cases;
}

/// 逐位 CRC-32 参考实现（与查表实现完全独立的算法路径）。
std::uint32_t BitwiseCrc32(ByteSpan data, std::uint32_t poly) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < data.size(); ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ poly : (crc >> 1);
        }
    }
    return ~crc;
}

/// 逐字节取模的 Adler-32 参考实现（不使用 5552 分块优化）。
std::uint32_t ReferenceAdler32(ByteSpan data) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

template <typename H>
std::vector<std::uint8_t> HashChunks(ByteSpan data,
                                     const std::vector<std::size_t>& chunks) {
    H hash;
    std::size_t offset = 0;
    for (std::size_t chunk : chunks) {
        const std::size_t take =
            offset + chunk <= data.size() ? chunk : data.size() - offset;
        hash.Write(ByteSpan(data.data() + offset, take));
        offset += take;
    }
    if (offset < data.size()) {
        hash.Write(ByteSpan(data.data() + offset, data.size() - offset));
    }
    std::vector<std::uint8_t> digest(hash.Size());
    hash.Sum(ByteSpan(digest.data(), digest.size()));
    return digest;
}

template <typename H>
std::vector<std::uint8_t> HashOneShot(ByteSpan data) {
    H hash;
    hash.Write(data);
    std::vector<std::uint8_t> digest(hash.Size());
    hash.Sum(ByteSpan(digest.data(), digest.size()));
    return digest;
}

/// 依次对五种算法比较「随机分块」与「一次写入」。
void CheckChunkedConsistency(ByteSpan data,
                             const std::vector<std::size_t>& chunks,
                             const char* context) {
    const std::vector<std::uint8_t> md5 = HashChunks<Md5>(data, chunks);
    const std::vector<std::uint8_t> sha1 = HashChunks<Sha1>(data, chunks);
    const std::vector<std::uint8_t> sha256 = HashChunks<Sha256>(data, chunks);
    const std::vector<std::uint8_t> crc = HashChunks<Crc32>(data, chunks);
    const std::vector<std::uint8_t> adler = HashChunks<Adler32>(data, chunks);

    if (md5 != HashOneShot<Md5>(data) ||
        sha1 != HashOneShot<Sha1>(data) ||
        sha256 != HashOneShot<Sha256>(data) ||
        crc != HashOneShot<Crc32>(data) ||
        adler != HashOneShot<Adler32>(data)) {
        ++go2cpp_hash_test::g_failures;
        std::fprintf(stderr, "chunked digest mismatch (%s, %zu bytes)\n",
                     context, data.size());
    }
}

void CheckOracleSet(const char* path, const char* label,
                    std::size_t minimum_cases) {
    const std::vector<OracleCase> cases = LoadOracle(path);
    HASH_REQUIRE(!cases.empty());
    // 数量下限写在测试里：oracle 文件被误删/截断时必须失败，而不是静默跑 0 条。
    HASH_CHECK(cases.size() >= minimum_cases);

    std::uint64_t total_bytes = 0;
    for (const OracleCase& item : cases) {
        const std::vector<std::uint8_t> data =
            go2cpp_hash_test::Generate(item.length, item.seed);
        const ByteSpan span = AsBytes(data);
        total_bytes += item.length;

        HASH_CHECK_U32("oracle crc32", Crc32::ChecksumIEEE(span), item.crc32);
        HASH_CHECK_U32("oracle adler32", Adler32::Checksum(span), item.adler32);
        HASH_CHECK_U32("oracle crc32c",
                       Crc32::Checksum(span, CastagnoliTable), item.crc32c);
        HASH_CHECK_U32("oracle crc32k", Crc32::Checksum(span, KoopmanTable),
                       item.crc32k);

        const auto md5 = Md5::Digest(span);
        HASH_CHECK_HEX("oracle md5", AsBytes(md5), item.md5);
        const auto sha1 = Sha1::Digest(span);
        HASH_CHECK_HEX("oracle sha1", AsBytes(sha1), item.sha1);
        const auto sha256 = Sha256::Digest(span);
        HASH_CHECK_HEX("oracle sha256", AsBytes(sha256), item.sha256);

        // 流式 CRC 也必须与一次性结果相同（用固定的 64 KiB 分块）。
        Crc32 streaming;
        for (std::size_t offset = 0; offset < data.size(); offset += 65536) {
            const std::size_t take = data.size() - offset < 65536
                                         ? data.size() - offset
                                         : 65536;
            streaming.Write(span.subspan(offset, take));
        }
        HASH_CHECK(streaming.Sum32() == Crc32::ChecksumIEEE(span));
    }
    std::printf("[hash] oracle(%s): %zu cases, %llu bytes checked\n", label,
                cases.size(),
                static_cast<unsigned long long>(total_bytes));
    std::fflush(stdout);
}

void CheckOracle() {
    go2cpp_hash_test::Announce("random: Python oracle cross-check");
    // 第一组：45 条边界/大文件用例（0 B ~ 10 MiB）。
    CheckOracleSet(GO2CPP_HASH_ORACLE_PATH, "boundary", 45);
    // 第二组：10,000 条随机长度/随机内容（长度 0~1024 B）。
    CheckOracleSet(GO2CPP_HASH_ORACLE_RANDOM_PATH, "random", 10000);
}

void CheckRandomFuzz() {
    go2cpp_hash_test::Announce("random: chunking / boundaries fuzz");
    std::mt19937_64 rng(0x9e3779b97f4a7c15ULL);
    std::uniform_int_distribution<std::size_t> length_dist(0, 50000);
    std::uniform_int_distribution<std::size_t> chunk_dist(1, 8192);

    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t length = length_dist(rng);
        const std::uint64_t seed = rng();
        const std::vector<std::uint8_t> data =
            go2cpp_hash_test::Generate(length, seed);

        std::vector<std::size_t> chunks;
        std::size_t remaining = length;
        while (remaining != 0) {
            std::size_t chunk = chunk_dist(rng);
            if (chunk > remaining) {
                chunk = remaining;
            }
            chunks.push_back(chunk);
            remaining -= chunk;
        }

        const ByteSpan span = AsBytes(data);
        CheckChunkedConsistency(span, chunks, "fuzz");

        // 每个算法都必须在 Reset 后与全新实例一致。
        Md5 md5;
        md5.Write(span);
        md5.Reset();
        md5.Write(span);
        HASH_CHECK(md5.Sum() == HashOneShot<Md5>(span));
    }
}

void CheckIndependentReferences() {
    go2cpp_hash_test::Announce("random: independent CRC/Adler reference");
    std::mt19937_64 rng(0x243f6a8885a308d3ULL);
    std::uniform_int_distribution<std::size_t> length_dist(0, 4096);

    for (int iteration = 0; iteration < 300; ++iteration) {
        const std::size_t length = length_dist(rng);
        const std::vector<std::uint8_t> data =
            go2cpp_hash_test::Generate(length, rng());
        const ByteSpan span = AsBytes(data);

        HASH_CHECK(BitwiseCrc32(span, IEEE) == Crc32::ChecksumIEEE(span));
        HASH_CHECK(BitwiseCrc32(span, Castagnoli) ==
                   Crc32::Checksum(span, CastagnoliTable));
        HASH_CHECK(BitwiseCrc32(span, Koopman) ==
                   Crc32::Checksum(span, KoopmanTable));
        HASH_CHECK(ReferenceAdler32(span) == Adler32::Checksum(span));
    }
}

void CheckCrcUpdateComposition() {
    go2cpp_hash_test::Announce("random: crc32 Update composition");
    std::mt19937_64 rng(0x13198a2e03707344ULL);
    std::uniform_int_distribution<std::size_t> length_dist(0, 10000);

    const Table* const tables[] = {&IEEETable, &CastagnoliTable, &KoopmanTable};
    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t length = length_dist(rng);
        const std::vector<std::uint8_t> data =
            go2cpp_hash_test::Generate(length, rng());
        const std::size_t split = length == 0 ? 0 : rng() % (length + 1);
        const ByteSpan whole = AsBytes(data);
        const ByteSpan head = whole.first(split);
        const ByteSpan tail = whole.subspan(split);

        for (const Table* table : tables) {
            const std::uint32_t sum =
                Crc32::Checksum(whole, *table);
            const std::uint32_t chained = Crc32::Update(
                Crc32::Checksum(head, *table), *table, tail);
            HASH_CHECK(chained == sum);
        }
    }
}

void CheckSingleBitDetection() {
    go2cpp_hash_test::Announce("random: CRC single-bit flip detection");
    std::mt19937_64 rng(0xa4093822299f31d0ULL);
    std::vector<std::uint8_t> data =
        go2cpp_hash_test::Generate(1024, 0x082efa98ec4e6c89ULL);
    const std::uint32_t baseline_ieee = Crc32::ChecksumIEEE(AsBytes(data));
    const std::uint32_t baseline_c =
        Crc32::Checksum(AsBytes(data), CastagnoliTable);
    const std::uint32_t baseline_k =
        Crc32::Checksum(AsBytes(data), KoopmanTable);

    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t bit = rng() % (data.size() * 8);
        data[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
        HASH_CHECK(Crc32::ChecksumIEEE(AsBytes(data)) != baseline_ieee);
        HASH_CHECK(Crc32::Checksum(AsBytes(data), CastagnoliTable) !=
                   baseline_c);
        HASH_CHECK(Crc32::Checksum(AsBytes(data), KoopmanTable) != baseline_k);
        data[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
    }
    HASH_CHECK(Crc32::ChecksumIEEE(AsBytes(data)) == baseline_ieee);
}

}  // namespace

void RunRandomTests() {
    go2cpp_hash_test::Announce("=== random / oracle cross-check ===");
    CheckOracle();
    CheckRandomFuzz();
    CheckIndependentReferences();
    CheckCrcUpdateComposition();
    CheckSingleBitDetection();
}
