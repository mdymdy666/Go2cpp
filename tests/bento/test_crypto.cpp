/**
 * @file test_crypto.cpp
 * @brief ChaCha20-Poly1305 / HChaCha20 / 重放窗口 / 加密帧的向量与安全测试。
 * @details 覆盖：
 *          - RFC 8439 §2.4.2 ChaCha20 流密码向量（前 16B + 解密还原）；
 *          - RFC 7539 §2.5.2 Poly1305 向量；
 *          - RFC 8439 §2.8.2 AEAD 向量（密文前缀 + tag + 解密还原）；
 *          - 篡改 / 截断 / 重放 / 错误密钥必须被拒绝；
 *          - 64 位滑动窗口边界与乱序接受；
 *          - 加解密吞吐 >= 50MB/s（CMake 在无构建类型时对本目标加 -O2）。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::vector<std::uint8_t> FromHex(const char* hex) {
    std::vector<std::uint8_t> bytes;
    int high = -1;
    for (const char* p = hex; *p != '\0'; ++p) {
        int value = -1;
        if (*p >= '0' && *p <= '9') {
            value = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            value = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            value = *p - 'A' + 10;
        } else {
            continue;
        }
        if (high < 0) {
            high = value;
        } else {
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | value));
            high = -1;
        }
    }
    return bytes;
}

std::vector<std::uint8_t> Bytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> FillSequence(std::size_t bytes, std::uint8_t start = 0) {
    std::vector<std::uint8_t> out(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(start + i);
    }
    return out;
}

bool StartsWith(const std::vector<std::uint8_t>& actual,
                const std::vector<std::uint8_t>& expected) {
    if (actual.size() < expected.size()) {
        return false;
    }
    return std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

void TestChaCha20Vector() {
    const std::vector<std::uint8_t> key = FillSequence(32);
    const std::vector<std::uint8_t> nonce = FromHex("000000000000004a00000000");
    const std::vector<std::uint8_t> plaintext = Bytes(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.");

    std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
    ChaCha20Xor(key.data(), 1, nonce.data(), plaintext.data(), ciphertext.data(),
                plaintext.size());

    // RFC 8439 §2.4.2 密文前缀。
    const std::vector<std::uint8_t> expected_prefix =
        FromHex("6e2e359a2568f98041ba0728dd0d6981");
    BENTO_CHECK(StartsWith(ciphertext, expected_prefix));

    std::vector<std::uint8_t> decrypted(plaintext.size(), 0);
    ChaCha20Xor(key.data(), 1, nonce.data(), ciphertext.data(), decrypted.data(),
                ciphertext.size());
    BENTO_CHECK(decrypted == plaintext);
    BENTO_CHECK_EQ(decrypted.size(), plaintext.size());
}

void TestPoly1305Vector() {
    const std::vector<std::uint8_t> key = FromHex(
        "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    const std::vector<std::uint8_t> message = Bytes("Cryptographic Forum Research Group");
    std::uint8_t tag[16] = {};
    Poly1305(key.data(), message.data(), message.size(), tag);
    const std::vector<std::uint8_t> expected =
        FromHex("a8061dc1305136c6c22b8baf0c0127a9");
    BENTO_CHECK(std::memcmp(tag, expected.data(), 16) == 0);

    // 篡改消息后 tag 必须变化。
    std::vector<std::uint8_t> tampered = message;
    tampered[0] = static_cast<std::uint8_t>(tampered[0] ^ 0x01);
    std::uint8_t other[16] = {};
    Poly1305(key.data(), tampered.data(), tampered.size(), other);
    BENTO_CHECK(std::memcmp(tag, other, 16) != 0);
}

void TestAeadVector() {
    const std::vector<std::uint8_t> key = FromHex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const std::vector<std::uint8_t> nonce = FromHex("070000004041424344454647");
    const std::vector<std::uint8_t> aad = FromHex("50515253c0c1c2c3c4c5c6c7");
    const std::vector<std::uint8_t> plaintext = Bytes(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.");

    std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
    std::uint8_t tag[16] = {};
    AeadSeal(key.data(), nonce.data(), aad.data(), aad.size(), plaintext.data(),
             plaintext.size(), ciphertext.data(), tag);

    const std::vector<std::uint8_t> expected_prefix =
        FromHex("d31a8d34648e60db7b86afbc53ef7ec2");
    const std::vector<std::uint8_t> expected_tag =
        FromHex("1ae10b594f09e26a7e902ecbd0600691");
    BENTO_CHECK(StartsWith(ciphertext, expected_prefix));
    BENTO_CHECK(std::memcmp(tag, expected_tag.data(), 16) == 0);

    std::vector<std::uint8_t> decrypted(plaintext.size(), 0);
    BENTO_CHECK(AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(), ciphertext.data(),
                         ciphertext.size(), tag, decrypted.data()));
    BENTO_CHECK(decrypted == plaintext);

    // 篡改 AAD / tag / 密文都必须失败。
    std::vector<std::uint8_t> bad_aad = aad;
    bad_aad[0] ^= 0x80;
    BENTO_CHECK(!AeadOpen(key.data(), nonce.data(), bad_aad.data(), bad_aad.size(),
                          ciphertext.data(), ciphertext.size(), tag, decrypted.data()));
    std::uint8_t bad_tag[16];
    std::memcpy(bad_tag, tag, 16);
    bad_tag[15] ^= 0x01;
    BENTO_CHECK(!AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(),
                          ciphertext.data(), ciphertext.size(), bad_tag, decrypted.data()));
    std::vector<std::uint8_t> bad_ciphertext = ciphertext;
    bad_ciphertext[3] ^= 0x10;
    BENTO_CHECK(!AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(),
                          bad_ciphertext.data(), bad_ciphertext.size(), tag,
                          decrypted.data()));
}

void TestKdfSeparation() {
    const std::vector<std::uint8_t> psk = FillSequence(32, 0xA0);
    const std::vector<std::uint8_t> client_nonce = FillSequence(16, 0x10);
    const std::vector<std::uint8_t> server_nonce = FillSequence(16, 0x30);

    std::uint8_t c2s[32] = {};
    std::uint8_t c2s_again[32] = {};
    std::uint8_t s2c[32] = {};
    std::uint8_t other[32] = {};
    DeriveSessionKey(psk.data(), client_nonce.data(), server_nonce.data(),
                     SecureDirection::kClientToServer, c2s);
    DeriveSessionKey(psk.data(), client_nonce.data(), server_nonce.data(),
                     SecureDirection::kClientToServer, c2s_again);
    DeriveSessionKey(psk.data(), client_nonce.data(), server_nonce.data(),
                     SecureDirection::kServerToClient, s2c);
    DeriveSessionKey(psk.data(), server_nonce.data(), client_nonce.data(),
                     SecureDirection::kClientToServer, other);

    BENTO_CHECK(std::memcmp(c2s, c2s_again, 32) == 0);
    BENTO_CHECK(std::memcmp(c2s, s2c, 32) != 0);
    BENTO_CHECK(std::memcmp(c2s, other, 32) != 0);

    std::uint8_t hs_c2s[32] = {};
    std::uint8_t hs_s2c[32] = {};
    DeriveHandshakeKey(psk.data(), SecureDirection::kClientToServer, hs_c2s);
    DeriveHandshakeKey(psk.data(), SecureDirection::kServerToClient, hs_s2c);
    BENTO_CHECK(std::memcmp(hs_c2s, hs_s2c, 32) != 0);
    BENTO_CHECK(std::memcmp(hs_c2s, c2s, 32) != 0);
}

void TestReplayWindow() {
    ReplayWindow window;
    BENTO_CHECK(!window.WouldAccept(0));
    BENTO_CHECK(window.WouldAccept(1));
    BENTO_CHECK(window.Accept(1));
    BENTO_CHECK(!window.Accept(1));

    // 乱序：3 先到，2 后到；重复的 3 仍拒绝。
    BENTO_CHECK(window.Accept(3));
    BENTO_CHECK(window.WouldAccept(2));
    BENTO_CHECK(window.Accept(2));
    BENTO_CHECK(!window.Accept(2));
    BENTO_CHECK(!window.Accept(3));

    // 窗口边界：highest=100 时 37 在窗口内（age 63），36 已滑出（age 64）。
    ReplayWindow edge;
    BENTO_CHECK(edge.Accept(100));
    BENTO_CHECK(edge.WouldAccept(37));
    BENTO_CHECK(!edge.WouldAccept(36));
    BENTO_CHECK(edge.Accept(37));
    BENTO_CHECK(edge.WouldAccept(164));
    BENTO_CHECK(edge.Accept(164));
    BENTO_CHECK(!edge.Accept(37));  // 被窗口推进淘汰
    BENTO_CHECK_EQ(edge.highest(), 164U);
}

FrameHeader SampleHeader() {
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 3;
    header.flags = kFlagAck;
    header.seq = 7;
    header.dep_seq = 9;
    return header;
}

void TestSealFrameTamperTruncateReplay() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0x20);
    const std::vector<std::uint8_t> payload = Bytes("hello bento");
    const std::vector<std::uint8_t> inner =
        BuildFrame(SampleHeader(), nullptr, payload.data(), payload.size());
    BENTO_CHECK(!inner.empty());

    std::vector<std::uint8_t> sealed;
    BENTO_CHECK(SealFrame(key.data(), SecureDirection::kClientToServer, 1, inner.data(),
                          inner.size(), &sealed));
    BENTO_CHECK_EQ(sealed.size(), inner.size() + kSecureOverheadBytes);

    // 外层头必须明文可解析，且带加密标志与 wire seq。
    FrameHeader outer;
    BENTO_CHECK(DecodeHeader(sealed.data(), sealed.size(), &outer));
    BENTO_CHECK((outer.flags & kFlagEncrypted) != 0);
    BENTO_CHECK_EQ(outer.seq, 1U);
    BENTO_CHECK_EQ(static_cast<std::uint8_t>(outer.type),
                   static_cast<std::uint8_t>(FrameType::kStream));

    ReplayWindow window;
    std::vector<std::uint8_t> opened;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed.data(),
                          sealed.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kOk);
    BENTO_CHECK(opened == inner);

    // 重放：同一包第二次必须被窗口拒绝。
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed.data(),
                           sealed.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kReplay);

    // 篡改 tag：新窗口下必须 kAuthFailed。
    ReplayWindow fresh;
    std::vector<std::uint8_t> tampered = sealed;
    tampered.back() ^= 0x01;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh,
                           tampered.data(), tampered.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed);

    // 截断：不足最小加密帧长度必须 kMalformed。
    ReplayWindow fresh2;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh2, sealed.data(),
                           kFrameHeaderBytes, &opened, &status));
    BENTO_CHECK(status == OpenStatus::kMalformed);

    // 只截掉 1B：长度仍够，但 tag 位置错位 -> 认证失败。
    ReplayWindow fresh2b;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh2b, sealed.data(),
                           sealed.size() - 1, &opened, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed);

    // 错误方向密钥：必须 kAuthFailed（nonce 域不同）。
    ReplayWindow fresh3;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kServerToClient, &fresh3, sealed.data(),
                           sealed.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed);

    // 未加密帧：kNotEncrypted。
    ReplayWindow fresh4;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh4, inner.data(),
                           inner.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kNotEncrypted);
}

void TestHandshakeFrameAndBodies() {
    const std::vector<std::uint8_t> psk = FillSequence(32, 0x40);
    HelloBody hello;
    hello.session = 0x12345678U;
    std::memcpy(hello.nonce, FillSequence(16, 0x60).data(), 16);
    hello.timestamp_ms = 123456789ULL;

    std::uint8_t raw[kHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, raw, sizeof(raw)));
    HelloBody decoded;
    BENTO_CHECK(DecodeHelloBody(raw, sizeof(raw), &decoded));
    BENTO_CHECK_EQ(decoded.session, hello.session);
    BENTO_CHECK(std::memcmp(decoded.nonce, hello.nonce, 16) == 0);
    BENTO_CHECK_EQ(decoded.timestamp_ms, hello.timestamp_ms);

    raw[0] ^= 0x01;  // magic 破坏
    BENTO_CHECK(!DecodeHelloBody(raw, sizeof(raw), &decoded));
    raw[0] ^= 0x01;

    std::vector<std::uint8_t> sealed;
    const std::uint32_t handshake_conn_id = 0xA1B2C3D4U;
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                   handshake_conn_id, 1, raw, sizeof(raw), &sealed));
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer, sealed.data(),
                                   sealed.size(), &inner_type, &session, &conn_id, &body,
                                   &status));
    BENTO_CHECK(status == OpenStatus::kOk);
    BENTO_CHECK_EQ(static_cast<std::uint8_t>(FrameType::kHello), inner_type);
    BENTO_CHECK_EQ(session, hello.session);
    BENTO_CHECK_EQ(conn_id, handshake_conn_id);
    BENTO_CHECK(body.size() == sizeof(raw));

    // 握手帧的截断/错误 PSK 也必须被拒绝。
    std::vector<std::uint8_t> bad_psk = psk;
    bad_psk[0] ^= 0xFF;
    BENTO_CHECK(!OpenHandshakeFrame(bad_psk.data(), SecureDirection::kClientToServer,
                                    sealed.data(), sealed.size(), &inner_type, &session, &conn_id,
                                    &body, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed || status == OpenStatus::kMalformed);
    BENTO_CHECK(!OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer, sealed.data(),
                                    sealed.size() - 4, &inner_type, &session, &conn_id, &body,
                                    &status));
}

void TestAeadThroughput() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0x11);
    const std::vector<std::uint8_t> nonce = FillSequence(12, 0x22);
    const std::vector<std::uint8_t> plaintext(1024 * 1024, 0x5A);
    std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
    std::vector<std::uint8_t> decrypted(plaintext.size(), 0);
    std::uint8_t tag[16] = {};
    constexpr int kIterations = 16;  // 16MB，耗时足够稳定又不会拖慢 ctest。

    const auto seal_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        AeadSeal(key.data(), nonce.data(), nullptr, 0, plaintext.data(), plaintext.size(),
                 ciphertext.data(), tag);
    }
    const auto seal_end = std::chrono::steady_clock::now();
    const double seal_seconds =
        std::chrono::duration<double>(seal_end - seal_start).count();
    const double seal_mbps =
        static_cast<double>(plaintext.size()) * kIterations / seal_seconds / 1000000.0;

    const auto open_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        BENTO_CHECK(AeadOpen(key.data(), nonce.data(), nullptr, 0, ciphertext.data(),
                             ciphertext.size(), tag, decrypted.data()));
    }
    const auto open_end = std::chrono::steady_clock::now();
    const double open_seconds =
        std::chrono::duration<double>(open_end - open_start).count();
    const double open_mbps =
        static_cast<double>(plaintext.size()) * kIterations / open_seconds / 1000000.0;

    std::printf("[INFO] aead throughput: seal=%.1fMB/s open=%.1fMB/s\n", seal_mbps, open_mbps);
    BENTO_CHECK(seal_mbps >= 50.0);
    BENTO_CHECK(open_mbps >= 50.0);
    BENTO_CHECK(decrypted == plaintext);
}

}  // namespace

int main() {
    TestChaCha20Vector();
    TestPoly1305Vector();
    TestAeadVector();
    TestKdfSeparation();
    TestReplayWindow();
    TestSealFrameTamperTruncateReplay();
    TestHandshakeFrameAndBodies();
    TestAeadThroughput();
    return bento_test::Finish("test_crypto");
}
