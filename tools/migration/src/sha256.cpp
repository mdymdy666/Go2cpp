/**
 * @file sha256.cpp
 * @brief 自带 SHA-256 / HMAC-SHA256 实现（FIPS 180-4 / RFC 2104）。
 *
 * 为什么不引 OpenSSL：`tools/migration` 是独立构建的演示工具，需求只有
 * 「对快照做完整性认证」，一个 200 行的 SHA-256 足够，且避免给主工程增加
 * 链接依赖。实现按标准逐轮写出，不做平台相关优化。
 *
 * 正确性由 `CryptoSelfTest()` 用 NIST / RFC 4231 向量自检（`migrate selftest`）。
 */
#include "migration/signature.hpp"

#include <cstring>
#include <cstdio>

namespace migration {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline std::uint32_t RotateRight(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

struct Sha256State {
    std::uint32_t h[8];
    std::uint8_t block[64];
    std::uint64_t total{0};
    std::size_t used{0};
};

void Sha256Init(Sha256State* state) {
    state->h[0] = 0x6a09e667U;
    state->h[1] = 0xbb67ae85U;
    state->h[2] = 0x3c6ef372U;
    state->h[3] = 0xa54ff53aU;
    state->h[4] = 0x510e527fU;
    state->h[5] = 0x9b05688cU;
    state->h[6] = 0x1f83d9abU;
    state->h[7] = 0x5be0cd19U;
    state->total = 0;
    state->used = 0;
}

void Sha256Compress(Sha256State* state, const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^
                                 RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^
                                 RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state->h[0];
    std::uint32_t b = state->h[1];
    std::uint32_t c = state->h[2];
    std::uint32_t d = state->h[3];
    std::uint32_t e = state->h[4];
    std::uint32_t f = state->h[5];
    std::uint32_t g = state->h[6];
    std::uint32_t h = state->h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^
                                 RotateRight(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^
                                 RotateRight(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state->h[0] += a;
    state->h[1] += b;
    state->h[2] += c;
    state->h[3] += d;
    state->h[4] += e;
    state->h[5] += f;
    state->h[6] += g;
    state->h[7] += h;
}

void Sha256Update(Sha256State* state, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    state->total += size;
    while (size > 0) {
        const std::size_t take = size < (64 - state->used) ? size : (64 - state->used);
        std::memcpy(state->block + state->used, bytes, take);
        state->used += take;
        bytes += take;
        size -= take;
        if (state->used == 64) {
            Sha256Compress(state, state->block);
            state->used = 0;
        }
    }
}

void Sha256Final(Sha256State* state, std::uint8_t out[32]) {
    const std::uint64_t bits = state->total * 8;
    const std::uint8_t pad = 0x80;
    Sha256Update(state, &pad, 1);
    const std::uint8_t zero = 0;
    while (state->used != 56) Sha256Update(state, &zero, 1);
    std::uint8_t length[8];
    for (int i = 0; i < 8; ++i) {
        length[i] = static_cast<std::uint8_t>((bits >> (56 - 8 * i)) & 0xFFU);
    }
    // 直接写长度字段，避免再走一次 Update 的计数器。
    std::memcpy(state->block + 56, length, 8);
    Sha256Compress(state, state->block);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((state->h[i] >> 24) & 0xFFU);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state->h[i] >> 16) & 0xFFU);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state->h[i] >> 8) & 0xFFU);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state->h[i] & 0xFFU);
    }
}

bool ConstantTimeEqual(const std::uint8_t* a, const std::uint8_t* b,
                       std::size_t size) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < size; ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool HexToBytes(const std::string& text, std::vector<std::uint8_t>* out) {
    if (text.size() % 2 != 0) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]);
        const int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    *out = std::move(bytes);
    return true;
}

std::string BytesToHex(const std::uint8_t* data, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    text.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text[i * 2] = kDigits[data[i] >> 4];
        text[i * 2 + 1] = kDigits[data[i] & 0x0FU];
    }
    return text;
}

}  // namespace

void Sha256(const void* data, std::size_t size, std::uint8_t out[32]) {
    Sha256State state;
    Sha256Init(&state);
    Sha256Update(&state, data, size);
    Sha256Final(&state, out);
}

void HmacSha256(const std::uint8_t* key, std::size_t key_size,
                const void* data, std::size_t size, std::uint8_t out[32]) {
    std::uint8_t normalized[32];
    const std::uint8_t* use_key = key;
    std::size_t use_size = key_size;
    if (key_size > 64) {
        Sha256(key, key_size, normalized);
        use_key = normalized;
        use_size = sizeof(normalized);
    }
    std::uint8_t inner_pad[64];
    std::uint8_t outer_pad[64];
    std::memset(inner_pad, 0x36, sizeof(inner_pad));
    std::memset(outer_pad, 0x5c, sizeof(outer_pad));
    for (std::size_t i = 0; i < use_size; ++i) {
        inner_pad[i] ^= use_key[i];
        outer_pad[i] ^= use_key[i];
    }
    std::uint8_t inner_hash[32];
    Sha256State state;
    Sha256Init(&state);
    Sha256Update(&state, inner_pad, sizeof(inner_pad));
    Sha256Update(&state, data, size);
    Sha256Final(&state, inner_hash);

    Sha256Init(&state);
    Sha256Update(&state, outer_pad, sizeof(outer_pad));
    Sha256Update(&state, inner_hash, sizeof(inner_hash));
    Sha256Final(&state, out);
}

std::string HexEncode(const std::uint8_t* data, std::size_t size) {
    return BytesToHex(data, size);
}

bool HexDecode(const std::string& hex, std::vector<std::uint8_t>* out) {
    return HexToBytes(hex, out);
}

bool CryptoSelfTest(std::string* detail) {
    struct Vector {
        const char* name;
        const char* message;
        const char* expected;
    };
    // FIPS 180-4 示例 + RFC 4231 test case 1/2。
    const Vector sha_vectors[] = {
        {"sha256(\"\")",
         "",
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"sha256(\"abc\")",
         "abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"sha256(448-bit)",
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    std::string text;
    bool ok = true;
    for (const auto& vector : sha_vectors) {
        std::uint8_t digest[32];
        Sha256(vector.message, std::strlen(vector.message), digest);
        const std::string actual = HexEncode(digest, sizeof(digest));
        if (actual != vector.expected) {
            ok = false;
            text += std::string("FAIL ") + vector.name + ": " + actual + "\n";
        } else {
            text += std::string("PASS ") + vector.name + "\n";
        }
    }
    {
        // RFC 4231 test case 2：key = "Jefe", data = "what do ya want for nothing?"
        const char* key = "Jefe";
        const char* data = "what do ya want for nothing?";
        std::uint8_t mac[32];
        HmacSha256(reinterpret_cast<const std::uint8_t*>(key), std::strlen(key),
                   data, std::strlen(data), mac);
        const std::string actual = HexEncode(mac, sizeof(mac));
        const char* expected =
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
        if (actual != expected) {
            ok = false;
            text += std::string("FAIL hmac-sha256(rfc4231#2): ") + actual + "\n";
        } else {
            text += "PASS hmac-sha256(rfc4231#2)\n";
        }
    }
    {
        // 恒定时间比较的语义自检。
        std::uint8_t a[4] = {1, 2, 3, 4};
        std::uint8_t b[4] = {1, 2, 3, 4};
        std::uint8_t c[4] = {1, 2, 3, 5};
        if (!ConstantTimeEqual(a, b, sizeof(a)) ||
            ConstantTimeEqual(a, c, sizeof(a))) {
            ok = false;
            text += "FAIL constant-time-compare\n";
        } else {
            text += "PASS constant-time-compare\n";
        }
    }
    if (detail != nullptr) *detail = text;
    return ok;
}

}  // namespace migration
