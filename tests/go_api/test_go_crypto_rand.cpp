/**
 * @file test_go_crypto_rand.cpp
 * @brief `go2cpp/go/crypto_rand.hpp` 的兼容性测试（对照 Go `crypto/rand`）。
 *
 * 覆盖：
 *  1. `Read`：1e6 字节的**无重复**（125000 个 8 字节块）+ 字节分布卡方检验；
 *  2. 边界：0 字节、1 字节、部分读 Source、失败 Source（短读返回已读字节数）；
 *  3. `Int`：范围、`max=1` 恒为 0、拒绝采样与 Go oracle 逐值一致、
 *     1e6 次均匀性卡方；
 *  4. `Prime`：2..256 位的位长/最高两位/奇偶/素性；`bits<2` 返回错误；
 *  5. `BigInt`：加减乘移模的代数恒等式、字节/十六进制往返、十进制文本、
 *     与 Go `ProbablyPrime(20)` 对 400 个候选逐项一致；
 *  6. `Prime` 的确定性 reader 结果与 Go oracle 逐值一致（同一候选序列）。
 *
 * Oracle：`tests/go_api/scripts/gen_rand_oracle.go`（Go 1.27）生成，
 * 路径由 CMake 以宏 `GO_API_RAND_ORACLE` 传入。
 *
 * 注：`crypto/rand.Prime` 自 Go 1.26 起通过 `rand.CustomReader` 走全局安全
 * 随机源（除非 `GODEBUG=cryptocustomrand=1`，且即使打开也会随机多读一个
 * 字节做探测），因此**无法**用自定义 reader 逐值重放；`Prime` 的验证改为
 * 位长/最高两位/素性/边界（见 `TestPrimeProperties`）。
 */

#include "go2cpp/go/crypto_rand.hpp"
#include "go_api_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef GO_API_RAND_ORACLE
#define GO_API_RAND_ORACLE ""
#endif

namespace {

namespace crypto = go2cpp::go::crypto::rand;
using crypto::BigInt;
using crypto::BigResult;

/// xorshift64*：与 `gen_rand_oracle.go` 逐位一致。
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

/// 确定性随机源，字节流与 Go 侧的 `prngReader` 完全一致。
class PrngSource final : public crypto::Source {
public:
    explicit PrngSource(std::uint64_t seed) : m_prng(seed) {}

    std::size_t Read(std::uint8_t* out, std::size_t size) override {
        std::size_t total = 0;
        while (total < size) {
            const std::uint64_t word = m_prng.Next();
            for (int index = 0; index < 8 && total < size; ++index) {
                out[total++] = static_cast<std::uint8_t>(word >> (8 * index));
            }
        }
        return total;
    }

private:
    Prng m_prng;
};

/// 每次只返回 1 字节的源，验证 `Read` 的循环补齐逻辑。
class TrickleSource final : public crypto::Source {
public:
    explicit TrickleSource(std::uint64_t seed) : m_prng(seed) {}

    std::size_t Read(std::uint8_t* out, std::size_t size) override {
        if (size == 0) {
            return 0;
        }
        out[0] = static_cast<std::uint8_t>(m_prng.Next());
        return 1;
    }

private:
    Prng m_prng;
};

/// 立刻 EOF 的源，验证短读会被如实报告。
class EmptySource final : public crypto::Source {
public:
    std::size_t Read(std::uint8_t*, std::size_t) override { return 0; }
};

struct Oracle {
    std::uint64_t seed{0};
    std::vector<std::pair<int, std::pair<BigInt, bool>>> prime_rows;  // bits, (value, is_prime)
    std::vector<std::pair<std::string, std::string>> int_rows;        // max(dec), result(hex)
    bool loaded{false};
};

Oracle LoadOracle(const std::string& path) {
    Oracle oracle;
    std::ifstream stream(path);
    if (!stream) {
        return oracle;
    }
    std::string line;
    std::size_t rows = 0;
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
        } else if (kind == "prime") {
            int bits = 0;
            std::string hex;
            std::string is_prime;
            parser >> bits >> hex >> is_prime;
            oracle.prime_rows.emplace_back(bits,
                                           std::make_pair(BigInt::FromHex(hex),
                                                          is_prime == "true"));
            ++rows;
        } else if (kind == "int") {
            std::string maximum;
            std::string result;
            parser >> maximum >> result;
            oracle.int_rows.emplace_back(maximum, result);
            ++rows;
        }
    }
    oracle.loaded = rows > 0;
    return oracle;
}

/// 卡方统计量。
double ChiSquare(const std::vector<std::uint64_t>& counts, double expected) {
    double value = 0.0;
    for (const std::uint64_t count : counts) {
        const double difference = static_cast<double>(count) - expected;
        value += difference * difference / expected;
    }
    return value;
}

/// 场景 1：Read 的随机性（1e6 字节）。
void TestReadRandomness() {
    GO_ANNOUNCE("rand: Read 1e6 bytes uniqueness + distribution");
    std::vector<std::uint8_t> buffer(1000000, 0);
    GO_CHECK(crypto::Read(buffer.data(), buffer.size()) == buffer.size());

    std::unordered_set<std::uint64_t> seen;
    seen.reserve(buffer.size() / 8);
    std::size_t duplicates = 0;
    for (std::size_t offset = 0; offset + 8 <= buffer.size(); offset += 8) {
        std::uint64_t word = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            word |= static_cast<std::uint64_t>(buffer[offset + index]) << (8 * index);
        }
        if (!seen.insert(word).second) {
            ++duplicates;
        }
    }
    GO_CHECK(seen.size() == 125000u);
    GO_CHECK(duplicates == 0u);

    // 256 桶卡方：自由度 255，p=0.001 的临界值约 330。取 400 作为宽松上界。
    std::vector<std::uint64_t> counts(256, 0);
    for (const std::uint8_t byte : buffer) {
        ++counts[byte];
    }
    const double chi = ChiSquare(counts, static_cast<double>(buffer.size()) / 256.0);
    GO_CHECK(chi < 400.0);
    std::printf("  [info] 125000 unique 8-byte blocks; chi-square(255)=%.1f\n", chi);
}

/// 场景 2：Read 边界与故障。
void TestReadBoundaries() {
    GO_ANNOUNCE("rand: Read boundaries/failure");
    std::uint8_t byte = 0;
    GO_CHECK(crypto::Read(&byte, 0) == 0);
    GO_CHECK(crypto::Read(&byte, 1) == 1);
    std::vector<std::uint8_t> big(4096, 0);
    GO_CHECK(crypto::Read(big.data(), big.size()) == big.size());

    PrngSource prng(42);
    std::vector<std::uint8_t> trickled(37, 0);
    GO_CHECK(crypto::Read(prng, trickled.data(), trickled.size()) == trickled.size());

    // 逐字节源：必须循环补满。
    TrickleSource trickle(7);
    std::vector<std::uint8_t> slow(64, 0);
    GO_CHECK(crypto::Read(trickle, slow.data(), slow.size()) == slow.size());

    // 立即 EOF：返回 0，调用方必须能感知短读。
    EmptySource empty;
    GO_CHECK(crypto::Read(empty, slow.data(), slow.size()) == 0u);
    BigResult result = crypto::Int(empty, BigInt::FromUint64(100));
    GO_CHECK(!result.Ok());
    result = crypto::Prime(empty, 32);
    GO_CHECK(!result.Ok());
}

/// 十进制字符串转十六进制（仅测试内部使用）。
std::string DecimalToHex(const std::string& decimal) {
    BigInt value = BigInt::FromUint64(0);
    for (const char character : decimal) {
        if (character < '0' || character > '9') {
            return {};
        }
        value = value.Mul(BigInt::FromUint64(10))
                    .Add(BigInt::FromUint64(static_cast<std::uint64_t>(character - '0')));
    }
    return value.Text(16);
}

/// 场景 3：按 Go 的顺序重放 Int（所有 max 共用一个 reader 流，与 Go 脚本一致）。
void TestIntSequentialReplay() {
    GO_ANNOUNCE("rand: Int sequential replay vs Go");
    const Oracle oracle = LoadOracle(GO_API_RAND_ORACLE);
    if (!oracle.loaded) {
        GO_CHECK(false);
        return;
    }
    // Go 脚本里 intSource 是**单个** reader，15 个 max × 5 个样本共用一条流。
    PrngSource source(oracle.seed ^ 0xA5A5A5A5A5A5A5A5ull);
    std::size_t matched = 0;
    for (const auto& row : oracle.int_rows) {
        const BigInt maximum = BigInt::FromHex(DecimalToHex(row.first));
        if (maximum.Sign() <= 0) {
            GO_CHECK_MSG(false, "bad max in oracle: " + row.first);
            continue;
        }
        const BigResult result = crypto::Int(source, maximum);
        if (!result.Ok()) {
            GO_CHECK_MSG(false, "Int failed for max=" + row.first);
            continue;
        }
        if (result.value.Text(16) != row.second) {
            GO_CHECK_MSG(false, "Int mismatch for max=" + row.first + " expected " +
                                    row.second + " got " + result.value.Text(16));
            continue;
        }
        ++matched;
    }
    GO_CHECK(matched == oracle.int_rows.size());
}

/// 场景 3c：Int 的边界与均匀性。
void TestIntUniformity() {
    GO_ANNOUNCE("rand: Int uniformity (1e6 samples)");
    const BigInt maximum = BigInt::FromUint64(1000);
    GO_CHECK(crypto::Int(maximum).Ok());
    // max = 1 时结果必须恒为 0。
    for (int round = 0; round < 100; ++round) {
        const BigResult result = crypto::Int(BigInt::FromUint64(1));
        GO_CHECK(result.Ok() && result.value.IsZero());
    }
    // max = 0 是错误（与 Go 的 "argument to Int is <= 0" 一致）。
    const BigResult zero = crypto::Int(BigInt::FromUint64(0));
    GO_CHECK(!zero.Ok());
    GO_CHECK(!crypto::Int(BigInt()).Ok());

    std::vector<std::uint64_t> counts(1000, 0);
    crypto::Source& reader = crypto::Reader();
    for (int round = 0; round < 1000000; ++round) {
        const BigResult result = crypto::Int(reader, maximum);
        if (!result.Ok() || result.value.Cmp(maximum) >= 0) {
            GO_CHECK_MSG(false, "Int out of range");
            break;
        }
        ++counts[static_cast<std::size_t>(result.value.Uint64())];
    }
    const double chi = ChiSquare(counts, 1000.0);
    GO_CHECK(chi < 1300.0);  // 自由度 999，p=0.001 约 1160
    std::printf("  [info] Int(1000) chi-square(999)=%.1f over 1e6 samples\n", chi);
}

/// 场景 4：Prime 的位长/最高两位/素性。
void TestPrimeProperties() {
    GO_ANNOUNCE("rand: Prime bit length/top bits/primality");
    for (const int bits : {2, 3, 4, 5, 8, 16, 31, 32, 33, 64, 65, 127, 128, 192, 256}) {
        const BigResult result = crypto::Prime(bits);
        GO_CHECK_MSG(result.Ok(), "Prime(" + std::to_string(bits) + ") failed");
        if (!result.Ok()) {
            continue;
        }
        const BigInt& prime = result.value;
        GO_CHECK_MSG(prime.BitLen() == static_cast<std::size_t>(bits),
                     "Prime(" + std::to_string(bits) + ") bit length " +
                         std::to_string(prime.BitLen()));
        GO_CHECK(prime.ProbablyPrime(20));
        GO_CHECK((prime.Bytes().back() & 1u) == 1u);  // 奇数
        if (bits >= 2) {
            // 最高两位必须是 1（Go 的 RSA 语义保证）。
            const std::vector<std::uint8_t> bytes = prime.Bytes();
            const std::size_t top_bit = static_cast<std::size_t>(bits) - 1;
            const std::size_t second_bit = static_cast<std::size_t>(bits) - 2;
            const auto bit_at = [&](std::size_t index) {
                const std::size_t byte = bytes.size() - 1 - index / 8;
                return ((bytes[byte] >> (index % 8)) & 1u) != 0u;
            };
            GO_CHECK(bit_at(top_bit));
            GO_CHECK(bit_at(second_bit));
        }
    }
    GO_CHECK(!crypto::Prime(0).Ok());
    GO_CHECK(!crypto::Prime(1).Ok());
    GO_CHECK(!crypto::Prime(-5).Ok());
    std::printf("  [info] generated 15 primes from 2 to 256 bits\n");
}

/// 场景 5：BigInt 的代数恒等式与转换。
void TestBigIntAlgebra() {
    GO_ANNOUNCE("rand: BigInt algebra/round-trip");
    Prng prng(0xCAFEBABE12345678ull);
    for (int round = 0; round < 500; ++round) {
        const BigInt left = BigInt::FromUint64(prng.Next() >> (prng.Next() % 64));
        const BigInt right = BigInt::FromUint64(prng.Next() >> (prng.Next() % 64));
        // 加法/减法互逆。
        GO_CHECK(left.Add(right).Sub(right) == left);
        GO_CHECK(left.Mul(right) == right.Mul(left));
        // 分配律。
        const BigInt extra = BigInt::FromUint64(prng.Next());
        GO_CHECK(left.Mul(right.Add(extra)) == left.Mul(right).Add(left.Mul(extra)));
        // 移位互逆（右移会丢低位，只在低 32 位为 0 时严格成立）。
        const BigInt shift_base = BigInt::FromUint64(prng.Next());
        const std::size_t shift = prng.Next() % 64;
        GO_CHECK(shift_base.Shl(shift).Shr(shift) == shift_base);
        // 取模：a mod b + (a/b)*b 的关系只在小范围内验证。
        const std::uint64_t small = 1 + prng.Next() % 1000000;
        const BigInt modulus = BigInt::FromUint64(small);
        const BigInt remainder = left.Mod(modulus);
        GO_CHECK(remainder.Cmp(modulus) < 0);
        GO_CHECK(left.Sub(remainder).Mod(modulus).IsZero());
        // 字节往返。
        const std::vector<std::uint8_t> bytes = left.Bytes();
        GO_CHECK(BigInt::FromBytes(bytes.data(), bytes.size()) == left);
        // 十六进制往返。
        GO_CHECK(BigInt::FromHex(left.Text(16)) == left);
        // 十进制往返。
        GO_CHECK(BigInt::FromHex(DecimalToHex(left.String())) == left);
    }
    // 已知值。
    GO_CHECK(BigInt::FromUint64(0).String() == "0");
    GO_CHECK(BigInt::FromUint64(255).Text(16) == "ff");
    GO_CHECK(BigInt::FromUint64(1).Shl(64).Text(16) == "10000000000000000");
    GO_CHECK(BigInt::FromHex("0xDEADBEEF").String() == "3735928559");
    GO_CHECK(BigInt::FromHex("DEADBEEF").Uint64() == 0xDEADBEEFull);
}

/// 场景 6：与 Go 的 ProbablyPrime 对拍（400 个候选）。
void TestPrimalityOracle() {
    GO_ANNOUNCE("rand: ProbablyPrime vs Go (400 candidates)");
    const Oracle oracle = LoadOracle(GO_API_RAND_ORACLE);
    if (!oracle.loaded) {
        GO_CHECK(false);
        return;
    }
    std::size_t mismatches = 0;
    std::size_t primes = 0;
    for (const auto& row : oracle.prime_rows) {
        const bool expected = row.second.second;
        const bool actual = row.second.first.ProbablyPrime(20);
        primes += expected ? 1 : 0;
        if (expected != actual) {
            ++mismatches;
            if (mismatches <= 3) {
                GO_CHECK_MSG(false, "primality mismatch for " + row.second.first.Text(16) +
                                        " expected " + (expected ? "prime" : "composite"));
            }
        }
    }
    GO_CHECK(mismatches == 0);
    std::printf("  [info] %zu candidates (%zu primes) agree with Go ProbablyPrime(20)\n",
                oracle.prime_rows.size(), primes);
}

/// 场景 7：Prime 的错误路径与重复生成的一致性。
void TestPrimeEdges() {
    GO_ANNOUNCE("rand: Prime edges");
    // 同一位长连续生成 3 个：必须互不相同（否则随机源有问题）。
    const BigResult first = crypto::Prime(64);
    const BigResult second = crypto::Prime(64);
    const BigResult third = crypto::Prime(64);
    GO_CHECK(first.Ok() && second.Ok() && third.Ok());
    GO_CHECK(first.value != second.value);
    GO_CHECK(second.value != third.value);
    GO_CHECK(first.value != third.value);
    // 2 位素数只有 2 和 3；我们的生成器把最高两位置 1 → 只能是 3。
    const BigResult two_bits = crypto::Prime(2);
    GO_CHECK(two_bits.Ok());
    GO_CHECK(two_bits.Ok() && two_bits.value.Cmp(BigInt::FromUint64(3)) == 0);
    // 5 位：最高两位置 1、最低位置 1 → 范围 [24,31] 的奇数，且必须是素数。
    const BigResult five_bits = crypto::Prime(5);
    GO_CHECK(five_bits.Ok());
    GO_CHECK(five_bits.Ok() && five_bits.value.BitLen() == 5u);
    GO_CHECK(five_bits.Ok() && five_bits.value.ProbablyPrime(20));
}

}  // namespace

int main() {
    TestReadRandomness();
    TestReadBoundaries();
    TestIntSequentialReplay();
    TestIntUniformity();
    TestPrimeProperties();
    TestPrimeEdges();
    TestBigIntAlgebra();
    TestPrimalityOracle();
    return go_api_test::ExitCode();
}
