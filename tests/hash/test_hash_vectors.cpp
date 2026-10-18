/**
 * @file test_hash_vectors.cpp
 * @brief 标准测试向量与接口语义：RFC 1321 / FIPS 180-4 / CRC-32 / Adler-32。
 *
 * @details 覆盖：
 *   - MD5：RFC 1321 附录 A.5 的 7 条测试 + "quick brown fox"；
 *   - SHA-1：FIPS 180-4 的 `abc`、空串、448-bit 消息、896-bit 消息、1,000,000 个 'a'；
 *   - SHA-256：同上五类；
 *   - CRC-32：IEEE/Castagnoli/Koopman 三种多项式的标准检查值；
 *   - Adler-32：空串、Wikipedia、123456789；
 *   - 统一接口：Size/BlockSize、`Sum` 不改变状态、`Reset`、按任意块长 Write、
 *     `Sum` 的大端字节序、CRC 的 `Update` 组合律。
 */
#include "hash_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace go2cpp::hash;

namespace {

struct Vector {
    const char* input;
    const char* digest;
};

void CheckMd5Vectors() {
    go2cpp_hash_test::Announce("MD5 RFC 1321 vectors");
    const Vector vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz",
         "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"123456789012345678901234567890123456789012345678901234567890123456"
         "78901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
        {"The quick brown fox jumps over the lazy dog",
         "9e107d9d372bb6826bd81d3542a419d6"},
    };
    for (const Vector& vector : vectors) {
        const auto digest =
            Md5::Digest(AsBytes(std::string_view(vector.input)));
        HASH_CHECK_HEX("md5 vector", AsBytes(digest), vector.digest);
    }
}

void CheckSha1Vectors() {
    go2cpp_hash_test::Announce("SHA-1 FIPS 180-4 vectors");
    const Vector vectors[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "a49b2446a02c645bf419f995b67091253a04a259"},
    };
    for (const Vector& vector : vectors) {
        const auto digest =
            Sha1::Digest(AsBytes(std::string_view(vector.input)));
        HASH_CHECK_HEX("sha1 vector", AsBytes(digest), vector.digest);
    }

    // FIPS 180-4 的长消息：1,000,000 个 'a'，验证跨块与 64 位长度累加。
    const std::string million(1000000, 'a');
    const auto digest = Sha1::Digest(AsBytes(million));
    HASH_CHECK_HEX("sha1(1M a)", AsBytes(digest),
                   "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

void CheckSha256Vectors() {
    go2cpp_hash_test::Announce("SHA-256 FIPS 180-4 vectors");
    const Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    for (const Vector& vector : vectors) {
        const auto digest =
            Sha256::Digest(AsBytes(std::string_view(vector.input)));
        HASH_CHECK_HEX("sha256 vector", AsBytes(digest), vector.digest);
    }

    const std::string million(1000000, 'a');
    const auto digest = Sha256::Digest(AsBytes(million));
    HASH_CHECK_HEX(
        "sha256(1M a)", AsBytes(digest),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void CheckCrc32Vectors() {
    go2cpp_hash_test::Announce("CRC-32 vectors (IEEE / Castagnoli / Koopman)");
    const std::string_view empty;
    const std::string_view digits = "123456789";

    HASH_CHECK(Crc32::ChecksumIEEE(AsBytes(empty)) == 0u);
    HASH_CHECK(Crc32::ChecksumIEEE(AsBytes(digits)) == 0xcbf43926u);
    HASH_CHECK(Crc32::Checksum(AsBytes(digits), CastagnoliTable) ==
               0xe3069283u);
    HASH_CHECK(Crc32::Checksum(AsBytes(digits), KoopmanTable) == 0x2d3dd0aeu);
    HASH_CHECK(Crc32::ChecksumIEEE(AsBytes(std::string_view(
                   "The quick brown fox jumps over the lazy dog"))) ==
               0x414fa339u);

    // Go 语义：MakeTable 生成的基础 256 表与内建表逐项相同。
    HASH_CHECK(MakeTable(IEEE).entries == IEEETable.entries);
    HASH_CHECK(MakeTable(Castagnoli).entries == CastagnoliTable.entries);
    HASH_CHECK(MakeTable(Koopman).entries == KoopmanTable.entries);
    HASH_CHECK(IEEETable[1] == 0x77073096u);
    HASH_CHECK(CastagnoliTable[1] == 0xf26b8303u);
    HASH_CHECK(KoopmanTable[1] == 0x9695c4cau);

    // 分段计算（Go `crc32.Update`）：Update(Checksum(a), b) == Checksum(a ++ b)。
    const std::string_view head = "123456";
    const std::string_view tail = "789";
    const Table* const tables[] = {&IEEETable, &CastagnoliTable, &KoopmanTable};
    for (const Table* table : tables) {
        const std::uint32_t chained =
            Crc32::Update(Crc32::Checksum(AsBytes(head), *table), *table,
                          AsBytes(tail));
        HASH_CHECK(chained == Crc32::Checksum(AsBytes(digits), *table));
    }
}

void CheckAdler32Vectors() {
    go2cpp_hash_test::Announce("Adler-32 vectors");
    HASH_CHECK(Adler32::Checksum(AsBytes(std::string_view(""))) == 1u);
    HASH_CHECK(Adler32::Checksum(AsBytes(std::string_view("Wikipedia"))) ==
               0x11e60398u);
    HASH_CHECK(Adler32::Checksum(AsBytes(std::string_view("123456789"))) ==
               0x091e01deu);
    // 空输入经类计算也必须是 1（不是 0）。
    Adler32 adler;
    HASH_CHECK(adler.Sum32() == 1u);
}

void CheckSizeAndBlockSize() {
    go2cpp_hash_test::Announce("Hash interface: Size() / BlockSize()");
    const Md5 md5;
    const Sha1 sha1;
    const Sha256 sha256;
    const Crc32 crc;
    const Adler32 adler;

    HASH_CHECK(md5.Size() == 16 && md5.BlockSize() == 64);
    HASH_CHECK(sha1.Size() == 20 && sha1.BlockSize() == 64);
    HASH_CHECK(sha256.Size() == 32 && sha256.BlockSize() == 64);
    HASH_CHECK(crc.Size() == 4 && crc.BlockSize() == 1);
    HASH_CHECK(adler.Size() == 4 && adler.BlockSize() == 4);

    HASH_CHECK(Md5::kSize == 16 && Md5::kBlockSize == 64);
    HASH_CHECK(Sha1::kSize == 20 && Sha1::kBlockSize == 64);
    HASH_CHECK(Sha256::kSize == 32 && Sha256::kBlockSize == 64);
    HASH_CHECK(Crc32::kSize == 4 && Crc32::kBlockSize == 1);
    HASH_CHECK(Adler32::kSize == 4 && Adler32::kBlockSize == 4);
}

/// Go `Sum` 语义：取摘要不改变状态，之后还能继续 Write。
template <typename H>
void CheckSumIsPure() {
    const std::string_view first = "The quick brown fox ";
    const std::string_view second = "jumps over the lazy dog";

    H hash;
    hash.Write(AsBytes(first));
    std::vector<std::uint8_t> digest1(hash.Size());
    std::vector<std::uint8_t> digest2(hash.Size());
    hash.Sum(ByteSpan(digest1.data(), digest1.size()));
    hash.Sum(ByteSpan(digest2.data(), digest2.size()));
    HASH_CHECK(digest1 == digest2);

    hash.Write(AsBytes(second));
    std::vector<std::uint8_t> continued(hash.Size());
    hash.Sum(ByteSpan(continued.data(), continued.size()));

    H fresh;
    fresh.Write(AsBytes(first));
    fresh.Write(AsBytes(second));
    std::vector<std::uint8_t> expected(fresh.Size());
    fresh.Sum(ByteSpan(expected.data(), expected.size()));
    HASH_CHECK(continued == expected);
}

/// Reset 之后重新计算，结果与新实例相同。
template <typename H>
void CheckReset() {
    H hash;
    hash.Write(AsBytes(std::string_view("garbage data that must be discarded")));
    hash.Reset();
    hash.Write(AsBytes(std::string_view("abc")));

    H fresh;
    fresh.Write(AsBytes(std::string_view("abc")));

    std::vector<std::uint8_t> actual(hash.Size());
    std::vector<std::uint8_t> expected(fresh.Size());
    hash.Sum(ByteSpan(actual.data(), actual.size()));
    fresh.Sum(ByteSpan(expected.data(), expected.size()));
    HASH_CHECK(actual == expected);
}

/// 按任意块长 Write 与一次性 Write 结果相同（覆盖 64/128 字节块边界）。
template <typename H>
void CheckChunkedWrites() {
    std::string data(4096 + 37, 'x');
    std::vector<std::uint8_t> one_shot(H::kSize);
    {
        H hash;
        hash.Write(AsBytes(data));
        hash.Sum(ByteSpan(one_shot.data(), one_shot.size()));
    }

    const std::size_t chunk_sizes[] = {1, 2, 3, 7, 8, 63, 64, 65, 127, 128, 129};
    for (std::size_t chunk : chunk_sizes) {
        H hash;
        for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
            const std::size_t take =
                offset + chunk <= data.size() ? chunk : data.size() - offset;
            hash.Write(AsBytes(data.data() + offset, take));
        }
        std::vector<std::uint8_t> digest(hash.Size());
        hash.Sum(ByteSpan(digest.data(), digest.size()));
        HASH_CHECK(digest == one_shot);
    }
}

void CheckBigEndianSum() {
    go2cpp_hash_test::Announce("Hash Sum byte order (CRC32 / Adler32 big-endian)");
    {
        Crc32 crc;
        crc.Write(AsBytes(std::string_view("123456789")));
        HASH_CHECK(crc.Sum32() == 0xcbf43926u);
        std::array<std::uint8_t, Crc32::kSize> digest{};
        crc.Sum(AsBytes(digest));
        HASH_CHECK_HEX("crc32 Sum big-endian", AsBytes(digest), "cbf43926");
    }
    {
        Adler32 adler;
        adler.Write(AsBytes(std::string_view("Wikipedia")));
        HASH_CHECK(adler.Sum32() == 0x11e60398u);
        std::array<std::uint8_t, Adler32::kSize> digest{};
        adler.Sum(AsBytes(digest));
        HASH_CHECK_HEX("adler32 Sum big-endian", AsBytes(digest), "11e60398");
    }
}

void CheckBaseInterface() {
    go2cpp_hash_test::Announce("Hash interface: polymorphic Write/Sum/Reset");
    Md5 md5;
    Hash& hash = md5;
    hash.Write(std::string_view("abc"));
    const std::vector<std::uint8_t> digest = hash.Sum();  // Go `Sum(nil)`
    HASH_CHECK_HEX("Hash::Sum()", AsBytes(digest),
                   "900150983cd24fb0d6963f7d28e17f72");

    hash.Reset();
    hash.Write("abc", 3);  // const void* + size 便利重载
    std::vector<std::uint8_t> second(md5.Size());
    hash.Sum(ByteSpan(second.data(), second.size()));
    HASH_CHECK(second == digest);

    // 空 span 在任意位置都必须是 no-op。
    hash.Write(ByteSpan{});
    std::vector<std::uint8_t> third(md5.Size());
    hash.Sum(ByteSpan(third.data(), third.size()));
    HASH_CHECK(third == digest);
}

void CheckFactoryFunctions() {
    go2cpp_hash_test::Announce("Hash factories: New / MakeTable");
    const Crc32 ieee = Crc32::New();
    HASH_CHECK(ieee.Size() == 4);
    const Crc32 castagnoli = Crc32::New(CastagnoliTable);
    Crc32 castagnoli_copy = castagnoli;
    castagnoli_copy.Write(AsBytes(std::string_view("123456789")));
    HASH_CHECK(castagnoli_copy.Sum32() == 0xe3069283u);

    const Adler32 adler = Adler32::New();
    HASH_CHECK(adler.Size() == 4);

    const Table custom = MakeTable(0xedb88320u);
    HASH_CHECK(Crc32::Checksum(AsBytes(std::string_view("123456789")), custom) ==
               0xcbf43926u);
}

}  // namespace

void RunVectorTests() {
    go2cpp_hash_test::Announce("=== vectors and interface semantics ===");
    CheckMd5Vectors();
    CheckSha1Vectors();
    CheckSha256Vectors();
    CheckCrc32Vectors();
    CheckAdler32Vectors();
    CheckSizeAndBlockSize();
    CheckSumIsPure<Md5>();
    CheckSumIsPure<Sha1>();
    CheckSumIsPure<Sha256>();
    CheckSumIsPure<Crc32>();
    CheckSumIsPure<Adler32>();
    CheckReset<Md5>();
    CheckReset<Sha1>();
    CheckReset<Sha256>();
    CheckReset<Crc32>();
    CheckReset<Adler32>();
    CheckChunkedWrites<Md5>();
    CheckChunkedWrites<Sha1>();
    CheckChunkedWrites<Sha256>();
    CheckChunkedWrites<Crc32>();
    CheckChunkedWrites<Adler32>();
    CheckBigEndianSum();
    CheckBaseInterface();
    CheckFactoryFunctions();
}
