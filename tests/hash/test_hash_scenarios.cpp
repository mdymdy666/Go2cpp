/**
 * @file test_hash_scenarios.cpp
 * @brief 6 个真实场景：文件校验、分块流式、增量更新、传输校验、双哈希、统一接口。
 *
 * @details 每个场景都说明「模拟的真实用法 / 输入 / 期望 / 实际断言」。
 *          文件场景使用 `std::filesystem::temp_directory_path()` 下的临时文件，
 *          RAII 析构即删除，不残留。
 *
 *   1. 文件校验：把 44,000 字节的已知内容写入磁盘，再按 8 KiB 读回算 SHA-256 /
 *      MD5 / CRC32 / Adler32，与 Python 预先算好的摘要比对；
 *   2. 分块流式哈希：3 MiB 数据按 1 MiB、64 KiB、64 B、跨块边界、随机块长切分，
 *      结果必须与一次性哈希一致（五种算法全覆盖）；
 *   3. 增量更新：边写边多次取摘要，`Sum` 不改变状态，最终结果与一次性相同；
 *   4. 传输校验：1 MiB 包按 8 KiB 分片做 CRC32 链式校验；逐位翻转必须被三种
 *      多项式全部检出；翻转恢复后校验和回到原值；
 *   5. 大文件双哈希：16 MiB 文件一次读取、MD5 与 SHA-256 交替喂入，验证互不干扰；
 *   6. 统一接口：`std::vector<std::unique_ptr<Hash>>` 一次遍历算五种摘要，
 *      验证多态调用、Reset 复用与空输入。
 */
#include "hash_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::hash;
using go2cpp_hash_test::Clock;

/// 临时文件：构造时只算路径，析构时删除（不依赖测试是否成功）。
class TempFile {
 public:
    explicit TempFile(const char* tag)
        : m_path(std::filesystem::temp_directory_path() /
                 (std::string("go2cpp_hash_") + tag + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(Clock::now().time_since_epoch().count()) +
                  ".bin")) {}
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& Path() const { return m_path; }

    bool Write(ByteSpan data) const {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        return static_cast<bool>(out);
    }

 private:
    std::filesystem::path m_path;
};

/// 按固定块长读取文件，把每块交给 `consume`；读失败返回 false。
template <typename Consumer>
bool ReadFileChunks(const std::filesystem::path& path, std::size_t chunk_size,
                    Consumer&& consume) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<std::uint8_t> buffer(chunk_size);
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            consume(ByteSpan(buffer.data(), static_cast<std::size_t>(got)));
        }
    }
    return true;
}

/// 与场景 1 的 Python 计算结果一致（见 tests/hash/scripts/gen_oracle.py 的说明）。
constexpr const char* kFileContentLine =
    "The quick brown fox jumps over the lazy dog\n";

std::string MakeFileContent() {
    std::string content;
    content.reserve(std::string_view(kFileContentLine).size() * 1000);
    for (int i = 0; i < 1000; ++i) {
        content.append(kFileContentLine);
    }
    return content;
}

template <typename H>
std::vector<std::uint8_t> ChunkedDigest(ByteSpan data,
                                        const std::vector<std::size_t>& pattern) {
    H hash;
    std::size_t offset = 0;
    for (std::size_t chunk : pattern) {
        if (offset >= data.size()) {
            break;
        }
        const std::size_t take = std::min(chunk, data.size() - offset);
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
std::vector<std::uint8_t> OneShotDigest(ByteSpan data) {
    H hash;
    hash.Write(data);
    std::vector<std::uint8_t> digest(hash.Size());
    hash.Sum(ByteSpan(digest.data(), digest.size()));
    return digest;
}

// ===========================================================================
// 场景 1：文件校验
// ===========================================================================
void ScenarioFileChecksum() {
    go2cpp_hash_test::Announce(
        "scenario 1: file checksum (write 44,000 B, read back in 8 KiB chunks)");
    const std::string content = MakeFileContent();
    TempFile file("file_checksum");
    HASH_REQUIRE(file.Write(AsBytes(content)));
    HASH_REQUIRE(std::filesystem::file_size(file.Path()) == content.size());

    Sha256 sha256;
    Md5 md5;
    Crc32 crc;
    Adler32 adler;
    const bool read_ok = ReadFileChunks(
        file.Path(), 8192, [&](ByteSpan chunk) {
            sha256.Write(chunk);
            md5.Write(chunk);
            crc.Write(chunk);
            adler.Write(chunk);
        });
    HASH_REQUIRE(read_ok);

    const std::vector<std::uint8_t> sha_digest = sha256.Sum();
    const std::vector<std::uint8_t> md5_digest = md5.Sum();
    HASH_CHECK_HEX("file sha256", AsBytes(sha_digest),
                   "7facbb2ee69c40c546c483f9a12b87b36c9968a4d6db3dc8a3699b6790526e01");
    HASH_CHECK_HEX("file md5", AsBytes(md5_digest),
                   "32e15cb032070b2f7c0cc248837aec6b");
    HASH_CHECK_U32("file crc32", crc.Sum32(), "2d7f808a");
    HASH_CHECK_U32("file adler32", adler.Sum32(), "dd98125b");

    // 8 MiB 文件的流式校验：结果必须与内存中一次性哈希相同。
    const std::vector<std::uint8_t> big =
        go2cpp_hash_test::Generate(8u * 1024u * 1024u, 0x0123456789abcdefULL);
    TempFile big_file("file_checksum_big");
    HASH_REQUIRE(big_file.Write(AsBytes(big)));
    Sha256 big_sha;
    HASH_REQUIRE(ReadFileChunks(big_file.Path(), 1u << 20,
                                [&](ByteSpan chunk) { big_sha.Write(chunk); }));
    const std::vector<std::uint8_t> file_digest = big_sha.Sum();
    const std::vector<std::uint8_t> memory_digest =
        OneShotDigest<Sha256>(AsBytes(big));
    HASH_CHECK(file_digest == memory_digest);
}

// ===========================================================================
// 场景 2：分块流式哈希
// ===========================================================================
void ScenarioChunkedStreaming() {
    go2cpp_hash_test::Announce(
        "scenario 2: chunked streaming (3 MiB, 5 chunk patterns)");
    const std::vector<std::uint8_t> data =
        go2cpp_hash_test::Generate(3u * 1024u * 1024u, 0xfedcba9876543210ULL);
    const ByteSpan span = AsBytes(data);

    std::vector<std::vector<std::size_t>> patterns;
    patterns.push_back({1024u * 1024u});
    patterns.push_back({64u * 1024u});
    patterns.push_back({64u});
    patterns.push_back({1, 63, 64, 65, 127, 128, 129, 4096});
    {
        std::mt19937_64 rng(0x5deece66dULL);
        std::uniform_int_distribution<std::size_t> dist(1, 300000);
        std::vector<std::size_t> random_chunks;
        std::size_t remaining = data.size();
        while (remaining != 0) {
            const std::size_t chunk = std::min(dist(rng), remaining);
            random_chunks.push_back(chunk);
            remaining -= chunk;
        }
        patterns.push_back(std::move(random_chunks));
    }

    const std::vector<std::uint8_t> md5_whole = OneShotDigest<Md5>(span);
    const std::vector<std::uint8_t> sha1_whole = OneShotDigest<Sha1>(span);
    const std::vector<std::uint8_t> sha256_whole = OneShotDigest<Sha256>(span);
    const std::vector<std::uint8_t> crc_whole = OneShotDigest<Crc32>(span);
    const std::vector<std::uint8_t> adler_whole = OneShotDigest<Adler32>(span);

    for (const std::vector<std::size_t>& pattern : patterns) {
        HASH_CHECK(ChunkedDigest<Md5>(span, pattern) == md5_whole);
        HASH_CHECK(ChunkedDigest<Sha1>(span, pattern) == sha1_whole);
        HASH_CHECK(ChunkedDigest<Sha256>(span, pattern) == sha256_whole);
        HASH_CHECK(ChunkedDigest<Crc32>(span, pattern) == crc_whole);
        HASH_CHECK(ChunkedDigest<Adler32>(span, pattern) == adler_whole);
    }
}

// ===========================================================================
// 场景 3：增量更新（Write 多次 vs 一次性）
// ===========================================================================
template <typename H>
void CheckIncrementalDigest(ByteSpan data) {
    const std::vector<std::uint8_t> expected = OneShotDigest<H>(data);

    H hash;
    std::size_t offset = 0;
    std::size_t step = 0;
    while (offset < data.size()) {
        const std::size_t take = std::min<std::size_t>(2048 + step % 997,
                                                       data.size() - offset);
        hash.Write(ByteSpan(data.data() + offset, take));
        offset += take;
        ++step;
        if (step % 100 == 0) {
            // 中途取两次摘要：值必须相同，且不能影响后续写入。
            std::vector<std::uint8_t> mid1(hash.Size());
            std::vector<std::uint8_t> mid2(hash.Size());
            hash.Sum(ByteSpan(mid1.data(), mid1.size()));
            hash.Sum(ByteSpan(mid2.data(), mid2.size()));
            HASH_CHECK(mid1 == mid2);
        }
    }

    std::vector<std::uint8_t> actual(hash.Size());
    hash.Sum(ByteSpan(actual.data(), actual.size()));
    HASH_CHECK(actual == expected);
}

void ScenarioIncrementalUpdates() {
    go2cpp_hash_test::Announce(
        "scenario 3: incremental updates (many Writes vs one Write)");
    const std::vector<std::uint8_t> data =
        go2cpp_hash_test::Generate(2u * 1024u * 1024u, 0x1122334455667788ULL);
    const ByteSpan span = AsBytes(data);

    CheckIncrementalDigest<Md5>(span);
    CheckIncrementalDigest<Sha1>(span);
    CheckIncrementalDigest<Sha256>(span);
    CheckIncrementalDigest<Crc32>(span);
    CheckIncrementalDigest<Adler32>(span);
}

// ===========================================================================
// 场景 4：传输校验（CRC32 检测单字节/单比特翻转）
// ===========================================================================
void ScenarioTransportVerification() {
    go2cpp_hash_test::Announce(
        "scenario 4: transport verification (1 MiB, packetized CRC32)");
    std::vector<std::uint8_t> data =
        go2cpp_hash_test::Generate(1024u * 1024u, 0x0badc0ffee0ddf00ULL);
    const ByteSpan span(data.data(), data.size());

    const Table* const tables[] = {&IEEETable, &CastagnoliTable, &KoopmanTable};
    std::uint32_t baseline[3];
    for (int i = 0; i < 3; ++i) {
        baseline[i] = Crc32::Checksum(span, *tables[i]);
    }

    // 首字节、中间字节、末字节的 8 个比特位都必须被检出。
    const std::size_t positions[] = {0, data.size() / 2, data.size() - 1};
    for (std::size_t position : positions) {
        for (int bit = 0; bit < 8; ++bit) {
            data[position] ^= static_cast<std::uint8_t>(1u << bit);
            for (int i = 0; i < 3; ++i) {
                HASH_CHECK(Crc32::Checksum(span, *tables[i]) != baseline[i]);
            }
            data[position] ^= static_cast<std::uint8_t>(1u << bit);
            for (int i = 0; i < 3; ++i) {
                HASH_CHECK(Crc32::Checksum(span, *tables[i]) == baseline[i]);
            }
        }
    }

    // 分片链式校验：128 个 8 KiB 包的 Update 链接 == 整包校验和。
    constexpr std::size_t kPacket = 8192;
    for (int i = 0; i < 3; ++i) {
        std::uint32_t running = 0;
        for (std::size_t offset = 0; offset < data.size(); offset += kPacket) {
            const std::size_t take = std::min(kPacket, data.size() - offset);
            running = Crc32::Update(running, *tables[i],
                                    ByteSpan(data.data() + offset, take));
        }
        HASH_CHECK(running == baseline[i]);
    }

    // 只损坏一个包：只有该包 CRC 变化，其余包不受影响。
    std::vector<std::uint32_t> packet_crcs;
    for (std::size_t offset = 0; offset < data.size(); offset += kPacket) {
        const std::size_t take = std::min(kPacket, data.size() - offset);
        packet_crcs.push_back(
            Crc32::Checksum(ByteSpan(data.data() + offset, take), IEEETable));
    }
    data[3 * kPacket + 17] ^= 0x40;
    int changed = 0;
    for (std::size_t index = 0; index < packet_crcs.size(); ++index) {
        const std::size_t offset = index * kPacket;
        const std::size_t take = std::min(kPacket, data.size() - offset);
        const std::uint32_t current =
            Crc32::Checksum(ByteSpan(data.data() + offset, take), IEEETable);
        if (current != packet_crcs[index]) {
            ++changed;
            HASH_CHECK(index == 3);
        }
    }
    HASH_CHECK(changed == 1);
}

// ===========================================================================
// 场景 5：大文件双哈希（MD5 + SHA-256 同时算）
// ===========================================================================
void ScenarioDualHashLargeFile() {
    go2cpp_hash_test::Announce(
        "scenario 5: dual hash on 16 MiB file (MD5 + SHA-256 interleaved)");
    const std::vector<std::uint8_t> data = go2cpp_hash_test::Generate(
        16u * 1024u * 1024u, 0xabcdef0123456789ULL);
    TempFile file("dual_hash");
    HASH_REQUIRE(file.Write(AsBytes(data)));

    const std::vector<std::uint8_t> expected_md5 =
        OneShotDigest<Md5>(AsBytes(data));
    const std::vector<std::uint8_t> expected_sha256 =
        OneShotDigest<Sha256>(AsBytes(data));

    Md5 md5;
    Sha256 sha256;
    std::vector<std::uint8_t> md5_midpoint(0);
    std::size_t chunk_index = 0;
    const bool read_ok = ReadFileChunks(
        file.Path(), 1u << 20, [&](ByteSpan chunk) {
            if (chunk_index % 2 == 0) {
                md5.Write(chunk);
                sha256.Write(chunk);
            } else {
                sha256.Write(chunk);
                md5.Write(chunk);
            }
            if (chunk_index == 7) {
                md5_midpoint = md5.Sum();
            }
            ++chunk_index;
        });
    HASH_REQUIRE(read_ok);
    HASH_CHECK(chunk_index == 16);

    const std::vector<std::uint8_t> md5_digest = md5.Sum();
    const std::vector<std::uint8_t> sha_digest = sha256.Sum();
    HASH_CHECK(md5_digest == expected_md5);
    HASH_CHECK(sha_digest == expected_sha256);

    // 中途取到的 MD5 摘要不应被之后的 SHA-256 写入影响（反之亦然）。
    HASH_CHECK(md5_midpoint.size() == Md5::kSize);
    HASH_CHECK(md5_midpoint != md5_digest);

    // 再验证一次「互不干扰」：给 SHA-256 多写一段垃圾，MD5 摘要保持不变。
    sha256.Reset();
    sha256.Write(AsBytes(std::string_view("unrelated data")));
    const std::vector<std::uint8_t> md5_after = md5.Sum();
    HASH_CHECK(md5_after == md5_digest);
}

// ===========================================================================
// 场景 6：统一接口（一次遍历、多算法）
// ===========================================================================
void ScenarioUnifiedInterface() {
    go2cpp_hash_test::Announce(
        "scenario 6: unified Hash interface (all five in one pass)");
    const std::vector<std::uint8_t> data =
        go2cpp_hash_test::Generate(4u * 1024u * 1024u, 0xfeedfacecafebeefULL);
    const ByteSpan span = AsBytes(data);

    const std::vector<std::vector<std::uint8_t>> expected = {
        OneShotDigest<Md5>(span),
        OneShotDigest<Sha1>(span),
        OneShotDigest<Sha256>(span),
        OneShotDigest<Crc32>(span),
        OneShotDigest<Adler32>(span),
    };

    std::vector<std::unique_ptr<Hash>> hashes;
    hashes.emplace_back(new Md5());
    hashes.emplace_back(new Sha1());
    hashes.emplace_back(new Sha256());
    hashes.emplace_back(new Crc32());
    hashes.emplace_back(new Adler32());

    for (std::size_t offset = 0; offset < data.size(); offset += 7777) {
        const std::size_t take = std::min<std::size_t>(7777, data.size() - offset);
        const ByteSpan chunk = span.subspan(offset, take);
        for (const std::unique_ptr<Hash>& hash : hashes) {
            hash->Write(chunk);
        }
    }
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        HASH_CHECK(hashes[index]->Sum() == expected[index]);
        HASH_CHECK(hashes[index]->Size() == expected[index].size());
    }

    // Reset 后复用：算 1 MiB 的另一段数据，再与各自的一次性结果对比。
    const std::vector<std::uint8_t> other =
        go2cpp_hash_test::Generate(1024u * 1024u, 0x0011223344556677ULL);
    const ByteSpan other_span = AsBytes(other);
    const std::vector<std::vector<std::uint8_t>> expected_other = {
        OneShotDigest<Md5>(other_span),
        OneShotDigest<Sha1>(other_span),
        OneShotDigest<Sha256>(other_span),
        OneShotDigest<Crc32>(other_span),
        OneShotDigest<Adler32>(other_span),
    };
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        hashes[index]->Reset();
        hashes[index]->Write(other_span);
        HASH_CHECK(hashes[index]->Sum() == expected_other[index]);
    }

    // 空输入：所有算法都必须给出规范的空摘要。
    for (const std::unique_ptr<Hash>& hash : hashes) {
        hash->Reset();
    }
    HASH_CHECK_HEX("interface md5(empty)", AsBytes(hashes[0]->Sum()),
                   "d41d8cd98f00b204e9800998ecf8427e");
    HASH_CHECK_HEX("interface sha1(empty)", AsBytes(hashes[1]->Sum()),
                   "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    HASH_CHECK_HEX(
        "interface sha256(empty)", AsBytes(hashes[2]->Sum()),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    HASH_CHECK_HEX("interface crc32(empty)", AsBytes(hashes[3]->Sum()),
                   "00000000");
    HASH_CHECK_HEX("interface adler32(empty)", AsBytes(hashes[4]->Sum()),
                   "00000001");
}

}  // namespace

void RunScenarioTests() {
    go2cpp_hash_test::Announce("=== real-world scenarios ===");
    ScenarioFileChecksum();
    ScenarioChunkedStreaming();
    ScenarioIncrementalUpdates();
    ScenarioTransportVerification();
    ScenarioDualHashLargeFile();
    ScenarioUnifiedInterface();
}
