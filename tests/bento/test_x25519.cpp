/**
 * @file test_x25519.cpp
 * @brief X25519（RFC 7748）测试：官方向量、随机密钥一致性、clamp 与低阶点拒绝。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 覆盖：
 *   - RFC 7748 §6.1 Alice/Bob 公私钥与共享密钥；
 *   - RFC 7748 §5.2 两组标量乘法向量；
 *   - RFC 7748 §5.2 迭代 1 次与 1000 次向量；
 *   - 随机密钥对的双向共享密钥一致性；
 *   - 私钥 clamp（低位与最高位不参与运算）；
 *   - 对端 u 坐标最高位忽略；
 *   - 全零、1、p-1、p、p+1 及两个大低阶点的拒绝；
 *   - 空指针参数、确定性与输入不变性。
 *
 * 断言只在主线程执行；`test_util.hpp` 的计数器不是原子变量。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "go2cpp/bento/net/x25519.hpp"
#include "test_util.hpp"

namespace {

using Key = std::array<std::uint8_t, go2cpp::bento::kX25519KeyBytes>;

void PrintCase(const char* name) {
    std::printf("[CASE] %s\n", name);
}

/// 解析 64 个十六进制字符（测试常量均为小写、长度固定）。
Key FromHex(const char* text) {
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        unsigned int value = 0;
        std::sscanf(text + 2 * i, "%2x", &value);
        key[i] = static_cast<std::uint8_t>(value);
    }
    return key;
}

bool IsZero(const Key& key) {
    std::uint8_t accumulator = 0;
    for (std::uint8_t byte : key) {
        accumulator |= byte;
    }
    return accumulator == 0;
}

/// 按 RFC 7748 §5.2 的迭代定义执行：k = X25519(k, u); u = 旧 k。
Key Iterate(Key scalar, Key point, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        Key next{};
        BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
            scalar.data(), point.data(), next.data()));
        point = scalar;
        scalar = next;
    }
    return scalar;
}

// RFC 7748 §6.1 的 Alice / Bob 密钥。
const Key kAlicePrivate =
    FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
const Key kAlicePublic =
    FromHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
const Key kBobPrivate =
    FromHex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
const Key kBobPublic =
    FromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
const Key kRfcShared =
    FromHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

// RFC 7748 §5.2 两组标量乘法向量。
const Key kScalarVector1 =
    FromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
const Key kPointVector1 =
    FromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
const Key kOutputVector1 =
    FromHex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

const Key kScalarVector2 =
    FromHex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
const Key kPointVector2 =
    FromHex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
const Key kOutputVector2 =
    FromHex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

// RFC 7748 §5.2 迭代向量的期望值。
const Key kIterated1 =
    FromHex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
const Key kIterated1000 =
    FromHex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");

void TestRfcAlicePublic() {
    PrintCase("x25519: RFC 7748 Alice public key");
    Key public_key{};
    BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(kAlicePrivate.data(),
                                                         public_key.data()));
    BENTO_CHECK(public_key == kAlicePublic);
}

void TestRfcBobPublic() {
    PrintCase("x25519: RFC 7748 Bob public key");
    Key public_key{};
    BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(kBobPrivate.data(),
                                                         public_key.data()));
    BENTO_CHECK(public_key == kBobPublic);
}

void TestRfcSharedSecret() {
    PrintCase("x25519: RFC 7748 shared secret (both directions)");
    Key client_shared{};
    Key server_shared{};
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kAlicePrivate.data(), kBobPublic.data(), client_shared.data()));
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kBobPrivate.data(), kAlicePublic.data(), server_shared.data()));
    BENTO_CHECK(client_shared == kRfcShared);
    BENTO_CHECK(server_shared == kRfcShared);
    BENTO_CHECK(client_shared == server_shared);
}

void TestRfcScalarMultVector1() {
    PrintCase("x25519: RFC 7748 scalar multiplication vector 1");
    Key output{};
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kScalarVector1.data(), kPointVector1.data(), output.data()));
    BENTO_CHECK(output == kOutputVector1);
}

void TestRfcScalarMultVector2() {
    PrintCase("x25519: RFC 7748 scalar multiplication vector 2");
    Key output{};
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kScalarVector2.data(), kPointVector2.data(), output.data()));
    BENTO_CHECK(output == kOutputVector2);
}

void TestRfcIterated1() {
    PrintCase("x25519: RFC 7748 iterated vector (1 round)");
    const Key base = FromHex("0900000000000000000000000000000000000000000000000000000000000000");
    const Key output = Iterate(base, base, 1);
    BENTO_CHECK(output == kIterated1);
}

void TestRfcIterated1000() {
    PrintCase("x25519: RFC 7748 iterated vector (1000 rounds)");
    const Key base = FromHex("0900000000000000000000000000000000000000000000000000000000000000");
    const Key output = Iterate(base, base, 1000);
    BENTO_CHECK(output == kIterated1000);
}

void TestRandomKeyPairAgreement() {
    PrintCase("x25519: random key pairs agree on shared secret (16 rounds)");
    for (int round = 0; round < 16; ++round) {
        Key alice_private{};
        Key alice_public{};
        Key bob_private{};
        Key bob_public{};
        go2cpp::bento::X25519GeneratePrivateKey(alice_private.data());
        go2cpp::bento::X25519GeneratePrivateKey(bob_private.data());
        BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(alice_private.data(),
                                                             alice_public.data()));
        BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(bob_private.data(),
                                                             bob_public.data()));
        BENTO_CHECK(alice_public != bob_public);

        Key alice_shared{};
        Key bob_shared{};
        BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
            alice_private.data(), bob_public.data(), alice_shared.data()));
        BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
            bob_private.data(), alice_public.data(), bob_shared.data()));
        BENTO_CHECK(alice_shared == bob_shared);
        BENTO_CHECK(!IsZero(alice_shared));
    }
}

void TestPrivateKeyClamp() {
    PrintCase("x25519: private key clamp ignores low/high bits");
    Key mutated = kAlicePrivate;
    mutated[0] ^= 0x07;
    mutated[31] ^= 0x80;

    Key original_public{};
    Key mutated_public{};
    BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(kAlicePrivate.data(),
                                                         original_public.data()));
    BENTO_CHECK(go2cpp::bento::X25519PublicKeyFromPrivate(mutated.data(),
                                                         mutated_public.data()));
    BENTO_CHECK(original_public == mutated_public);
}

void TestPeerPublicKeyHighBitIgnored() {
    PrintCase("x25519: peer public key high bit is ignored");
    Key normal = kBobPublic;
    Key high_bit = kBobPublic;
    high_bit[31] |= 0x80;

    Key normal_shared{};
    Key high_bit_shared{};
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kAlicePrivate.data(), normal.data(), normal_shared.data()));
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        kAlicePrivate.data(), high_bit.data(), high_bit_shared.data()));
    BENTO_CHECK(normal_shared == high_bit_shared);
    BENTO_CHECK(normal_shared == kRfcShared);
}

void TestLowOrderZeroAndOne() {
    PrintCase("x25519: reject low-order points u=0 and u=1");
    const Key zero{};
    Key one{};
    one[0] = 1;
    for (const Key& point : {zero, one}) {
        Key shared{};
        shared.fill(0xAA);
        BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
            kAlicePrivate.data(), point.data(), shared.data()));
        BENTO_CHECK(IsZero(shared));
    }
}

void TestLowOrderLargePoints() {
    PrintCase("x25519: reject large low-order points");
    const Key first =
        FromHex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800");
    const Key second =
        FromHex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157");
    for (const Key& point : {first, second}) {
        Key shared{};
        shared.fill(0x5A);
        BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
            kAlicePrivate.data(), point.data(), shared.data()));
        BENTO_CHECK(IsZero(shared));
    }
}

void TestLowOrderModulusVariants() {
    PrintCase("x25519: reject low-order points p-1, p, p+1");
    Key minus_one{};
    minus_one.fill(0xFF);
    minus_one[0] = 0xEC;
    minus_one[31] = 0x7F;

    Key modulus{};
    modulus.fill(0xFF);
    modulus[0] = 0xED;
    modulus[31] = 0x7F;

    Key plus_one{};
    plus_one.fill(0xFF);
    plus_one[0] = 0xEE;
    plus_one[31] = 0x7F;

    for (const Key& point : {minus_one, modulus, plus_one}) {
        Key shared{};
        shared.fill(0x33);
        BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
            kAlicePrivate.data(), point.data(), shared.data()));
        BENTO_CHECK(IsZero(shared));
    }
}

void TestNullArguments() {
    PrintCase("x25519: null arguments are rejected without crashing");
    Key key{};
    BENTO_CHECK(!go2cpp::bento::X25519PublicKeyFromPrivate(nullptr, key.data()));
    BENTO_CHECK(!go2cpp::bento::X25519PublicKeyFromPrivate(kAlicePrivate.data(), nullptr));
    BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
        nullptr, kBobPublic.data(), key.data()));
    BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
        kAlicePrivate.data(), nullptr, key.data()));
    BENTO_CHECK(!go2cpp::bento::X25519ComputeSharedSecret(
        kAlicePrivate.data(), kBobPublic.data(), nullptr));
    go2cpp::bento::X25519GeneratePrivateKey(nullptr);
}

void TestDeterministicAndInputsUnchanged() {
    PrintCase("x25519: deterministic output and inputs unchanged");
    Key private_key = kAlicePrivate;
    Key peer_public = kBobPublic;
    Key first{};
    Key second{};
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        private_key.data(), peer_public.data(), first.data()));
    BENTO_CHECK(go2cpp::bento::X25519ComputeSharedSecret(
        private_key.data(), peer_public.data(), second.data()));
    BENTO_CHECK(first == second);
    BENTO_CHECK(private_key == kAlicePrivate);
    BENTO_CHECK(peer_public == kBobPublic);
}

void TestGeneratedPrivateKeyVaries() {
    PrintCase("x25519: generated private keys are not constant");
    Key first{};
    Key second{};
    go2cpp::bento::X25519GeneratePrivateKey(first.data());
    go2cpp::bento::X25519GeneratePrivateKey(second.data());
    BENTO_CHECK(first != second);
}

}  // namespace

int main() {
    TestRfcAlicePublic();
    TestRfcBobPublic();
    TestRfcSharedSecret();
    TestRfcScalarMultVector1();
    TestRfcScalarMultVector2();
    TestRfcIterated1();
    TestRfcIterated1000();
    TestRandomKeyPairAgreement();
    TestPrivateKeyClamp();
    TestPeerPublicKeyHighBitIgnored();
    TestLowOrderZeroAndOne();
    TestLowOrderLargePoints();
    TestLowOrderModulusVariants();
    TestNullArguments();
    TestDeterministicAndInputsUnchanged();
    TestGeneratedPrivateKeyVaries();
    return bento_test::Finish("x25519");
}
