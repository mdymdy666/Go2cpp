/**
 * @file test_crypto.cpp
 * @brief ChaCha20-Poly1305 / HChaCha20 / 重放窗口 / 加密帧的向量与安全测试。
 * @details 覆盖：
 *          - RFC 8439 §2.4.2 ChaCha20 流密码向量（前 16B + 解密还原）；
 *          - RFC 7539 §2.5.2 Poly1305 向量；
 *          - RFC 8439 §2.8.2 AEAD 向量（密文前缀 + tag + 解密还原）；
 *          - 篡改 / 截断 / 重放 / 错误密钥必须被拒绝；
 *          - 64 位滑动窗口边界、乱序接受、0-RTT InitializeBefore 语义与
 *            32 位 seq 回绕；
 *          - 12 种消息长度（0/1/15/16/17/63/64/65/32/256/1400/4096）的 AEAD
 *            往返、原地加解密、1000 轮随机密钥/nonce/明文/篡改；
 *          - 200 轮帧级随机篡改、参数边界矩阵、200 轮票据随机篡改、
 *            4 线程并发 Seal/Open；
 *          - 加解密吞吐 >= 50MB/s（CMake 在无构建类型时对本目标加 -O2）。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
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
    // HChaCha20 已知答案（draft-irtf-cfrg-xchacha-03 §2.2.1）：
    // key = 00..1f，input = 000000090000004a0000000031415927。
    // 该向量锁定常量、轮数与输出字序，防止 KDF 在跨语言互通时静默漂移。
    const std::vector<std::uint8_t> hc_key = FillSequence(32, 0x00);
    const std::vector<std::uint8_t> hc_input = FromHex("000000090000004a0000000031415927");
    std::uint8_t hc_out[32] = {};
    HChaCha20(hc_key.data(), hc_input.data(), hc_out);
    const std::vector<std::uint8_t> hc_expected =
        FromHex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc");
    BENTO_CHECK_EQ(hc_input.size(), 16U);
    BENTO_CHECK_EQ(hc_expected.size(), 32U);
    BENTO_CHECK(std::memcmp(hc_out, hc_expected.data(), 32) == 0);

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

std::vector<std::uint8_t> RandomBytes(std::mt19937& rng, std::size_t bytes) {
    std::vector<std::uint8_t> out(bytes, 0);
    for (std::uint8_t& byte : out) {
        byte = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    return out;
}

/// 重放窗口的 0-RTT 初始化语义：窗口内旧序号必须全部视为已消费。
void TestReplayWindowInitializeBefore() {
    ReplayWindow window;
    BENTO_CHECK(!window.initialized());
    window.InitializeBefore(100);
    BENTO_CHECK(window.initialized());
    BENTO_CHECK_EQ(window.highest(), 99U);
    for (std::uint32_t age = 0; age < ReplayWindow::kWindow; ++age) {
        BENTO_CHECK(!window.WouldAccept(99 - age));
    }
    BENTO_CHECK(window.WouldAccept(100));
    BENTO_CHECK(window.Accept(100));
    BENTO_CHECK(!window.WouldAccept(100));
    BENTO_CHECK(!window.WouldAccept(36));  // age=64，已滑出
    BENTO_CHECK_EQ(window.highest(), 100U);

    // next_seq <= 1 等价于 Reset：首包重新从 1 开始。
    ReplayWindow fresh;
    fresh.InitializeBefore(2);
    BENTO_CHECK(fresh.initialized());
    BENTO_CHECK(!fresh.WouldAccept(1));
    BENTO_CHECK(fresh.WouldAccept(2));
    fresh.InitializeBefore(1);
    BENTO_CHECK(!fresh.initialized());
    BENTO_CHECK(fresh.WouldAccept(1));

    // 32 位 seq 回绕：0xFFFFFFF0 -> 0xFFFFFFFF -> 1 必须按有符号差值处理。
    ReplayWindow wrap;
    BENTO_CHECK(wrap.Accept(0xFFFFFFF0U));
    BENTO_CHECK(wrap.Accept(0xFFFFFFFFU));
    BENTO_CHECK(wrap.Accept(1));
    BENTO_CHECK(!wrap.Accept(1));
    BENTO_CHECK(!wrap.WouldAccept(0xFFFFFFF0U));
    BENTO_CHECK_EQ(wrap.highest(), 1U);

    // 初始化后跳号接收：旧序号标记必须随窗口平移保留，不能因为一次
    // 大跨度推进就把窗口内的旧会话序号重新放行。
    ReplayWindow jump;
    jump.InitializeBefore(1000);
    BENTO_CHECK(jump.Accept(1002));
    BENTO_CHECK(jump.WouldAccept(1001));   // 新序号、尚未收到
    BENTO_CHECK(!jump.WouldAccept(999));   // 旧会话已消费
    BENTO_CHECK(!jump.WouldAccept(998));
    BENTO_CHECK(jump.Accept(1001));
    BENTO_CHECK(!jump.WouldAccept(1001));

    // 初始化点接近 32 位回绕：next_seq 本身可接受，回绕后的 1 也可接受，
    // 回绕前的旧序号仍然拒绝。
    ReplayWindow near_wrap;
    near_wrap.InitializeBefore(0xFFFFFFFEU);
    BENTO_CHECK(!near_wrap.WouldAccept(0xFFFFFFFDU));
    BENTO_CHECK(near_wrap.Accept(0xFFFFFFFEU));
    BENTO_CHECK(!near_wrap.WouldAccept(0xFFFFFFFEU));
    BENTO_CHECK(near_wrap.Accept(1));
    BENTO_CHECK(!near_wrap.WouldAccept(0));
    BENTO_CHECK(!near_wrap.WouldAccept(0xFFFFFFFDU));
}

/// AEAD 在 Poly1305 块边界上的往返与 tag 稳定性。
void TestAeadMessageSizes() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0x91);
    const std::vector<std::uint8_t> nonce = FillSequence(12, 0x17);
    for (const std::size_t bytes : {std::size_t{0}, std::size_t{1}, std::size_t{15},
                                    std::size_t{16}, std::size_t{17}, std::size_t{63},
                                    std::size_t{64}, std::size_t{65}, std::size_t{32},
                                    std::size_t{256}, std::size_t{1400},
                                    std::size_t{4096}}) {
        const std::vector<std::uint8_t> plaintext = FillSequence(bytes, 0x3B);
        std::vector<std::uint8_t> ciphertext(bytes, 0xEE);
        std::uint8_t tag[kAeadTagBytes] = {};
        AeadSeal(key.data(), nonce.data(), nullptr, 0, plaintext.data(), bytes,
                 ciphertext.data(), tag);
        std::vector<std::uint8_t> decrypted(bytes, 0xEE);
        BENTO_CHECK(AeadOpen(key.data(), nonce.data(), nullptr, 0, ciphertext.data(), bytes, tag,
                             decrypted.data()));
        BENTO_CHECK(decrypted == plaintext);
        // 同输入必须同 tag（确定性），且再加密不会改变结果。
        std::uint8_t tag_again[kAeadTagBytes] = {};
        AeadSeal(key.data(), nonce.data(), nullptr, 0, plaintext.data(), bytes,
                 ciphertext.data(), tag_again);
        BENTO_CHECK(std::memcmp(tag, tag_again, kAeadTagBytes) == 0);
        // tag 翻转 1 bit 必须失败。
        std::uint8_t bad_tag[kAeadTagBytes] = {};
        std::memcpy(bad_tag, tag, kAeadTagBytes);
        bad_tag[bytes % kAeadTagBytes] ^= 0x01;
        BENTO_CHECK(!AeadOpen(key.data(), nonce.data(), nullptr, 0, ciphertext.data(), bytes,
                              bad_tag, decrypted.data()));
    }
}

/// 原地加解密（in == out）必须与分离缓冲结果一致。
void TestAeadInPlace() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0x73);
    const std::vector<std::uint8_t> nonce = FillSequence(12, 0x29);
    const std::vector<std::uint8_t> original = FillSequence(300, 0x0F);
    std::vector<std::uint8_t> buffer = original;
    std::uint8_t tag[kAeadTagBytes] = {};
    AeadSeal(key.data(), nonce.data(), nullptr, 0, buffer.data(), buffer.size(), buffer.data(),
             tag);
    BENTO_CHECK(buffer != original);
    BENTO_CHECK(AeadOpen(key.data(), nonce.data(), nullptr, 0, buffer.data(), buffer.size(), tag,
                         buffer.data()));
    BENTO_CHECK(buffer == original);
}

/// 1000 轮随机密钥/nonce/明文/AAD + 4 种篡改；认证失败时不得写入明文。
void TestAeadRandomTamper() {
    std::mt19937 rng(0x51ED270BU);
    int accepted = 0;
    for (int round = 0; round < 1000; ++round) {
        const std::vector<std::uint8_t> key = RandomBytes(rng, kChaCha20KeyBytes);
        const std::vector<std::uint8_t> nonce = RandomBytes(rng, kChaCha20NonceBytes);
        const std::size_t aad_bytes = rng() % 33;
        const std::size_t plaintext_bytes = rng() % 300;
        const std::vector<std::uint8_t> aad = RandomBytes(rng, aad_bytes);
        const std::vector<std::uint8_t> plaintext = RandomBytes(rng, plaintext_bytes);
        std::vector<std::uint8_t> ciphertext(plaintext_bytes, 0);
        std::uint8_t tag[kAeadTagBytes] = {};
        AeadSeal(key.data(), nonce.data(), aad.data(), aad.size(), plaintext.data(),
                 plaintext.size(), ciphertext.data(), tag);
        std::vector<std::uint8_t> decrypted(plaintext_bytes, 0xEE);
        BENTO_CHECK(AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(), ciphertext.data(),
                             plaintext.size(), tag, decrypted.data()));
        BENTO_CHECK(decrypted == plaintext);

        std::vector<std::uint8_t> tampered = ciphertext;
        std::size_t tampered_bytes = tampered.size();
        std::uint8_t tampered_tag[kAeadTagBytes] = {};
        std::memcpy(tampered_tag, tag, kAeadTagBytes);
        const unsigned int mode = rng() % 4;
        if (mode == 0 && !tampered.empty()) {
            tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
        } else if (mode == 1 && !tampered.empty()) {
            for (int change = 0; change < 3; ++change) {
                tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(rng() & 0xFFU);
            }
        } else if (mode == 2) {
            tampered_bytes = tampered.size() > 4 ? tampered.size() - 1 - (rng() % 4) : 0;
        } else if (mode == 3) {
            tampered_tag[rng() % kAeadTagBytes] ^=
                static_cast<std::uint8_t>(1U << (rng() % 8));
        } else {
            continue;  // 空密文时 mode 0/1 无字节可改，跳过本轮篡改。
        }
        // 多字节随机翻转可能互相抵消、退化成原始密文；本轮跳过，避免把
        // “没有真正篡改”误判成认证失败漏检。
        if (tampered == ciphertext && tampered_bytes == ciphertext.size() &&
            std::memcmp(tampered_tag, tag, kAeadTagBytes) == 0) {
            continue;
        }
        std::vector<std::uint8_t> guarded(tampered_bytes, 0xA5);
        if (AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(), tampered.data(),
                     tampered_bytes, tampered_tag, guarded.data())) {
            ++accepted;
        }
        bool untouched = true;
        for (std::uint8_t byte : guarded) {
            untouched = untouched && byte == 0xA5;
        }
        BENTO_CHECK(untouched);
    }
    BENTO_CHECK_EQ(accepted, 0);
}

/// 帧级随机篡改：状态必须落在已知失败集合，且不残留内层明文。
void TestFrameRandomTamper() {
    std::mt19937 rng(0x9E3779B9U);
    int accepted = 0;
    for (int round = 0; round < 200; ++round) {
        const std::vector<std::uint8_t> key = RandomBytes(rng, kChaCha20KeyBytes);
        const std::vector<std::uint8_t> payload = RandomBytes(rng, 1 + rng() % 300);
        const std::vector<std::uint8_t> inner =
            BuildFrame(SampleHeader(), nullptr, payload.data(), payload.size());
        BENTO_CHECK(!inner.empty());
        const std::uint32_t seq = static_cast<std::uint32_t>(rng() | 1U);
        std::vector<std::uint8_t> sealed;
        BENTO_CHECK(SealFrame(key.data(), SecureDirection::kClientToServer, seq, inner.data(),
                              inner.size(), &sealed));
        ReplayWindow window;
        std::vector<std::uint8_t> opened;
        OpenStatus status = OpenStatus::kMalformed;
        BENTO_CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed.data(),
                              sealed.size(), &opened, &status));
        BENTO_CHECK(status == OpenStatus::kOk);
        BENTO_CHECK(opened == inner);

        std::vector<std::uint8_t> tampered = sealed;
        std::size_t tampered_bytes = tampered.size();
        const unsigned int mode = rng() % 4;
        if (mode == 0) {
            tampered[rng() % tampered.size()] ^= 0x01;
        } else if (mode == 1) {
            for (int change = 0; change < 3; ++change) {
                tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(rng() & 0xFFU);
            }
        } else if (mode == 2) {
            tampered_bytes = tampered.size() > 16 ? tampered.size() - 1 - (rng() % 16) : 0;
        } else {
            tampered[3] = static_cast<std::uint8_t>(tampered[3] & ~kFlagEncrypted);
        }
        if (tampered == sealed && tampered_bytes == sealed.size()) {
            continue;  // 随机翻转互相抵消，本轮没有有效篡改。
        }
        ReplayWindow fresh;
        std::vector<std::uint8_t> output;
        OpenStatus tamper_status = OpenStatus::kMalformed;
        if (OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, tampered.data(),
                      tampered_bytes, &output, &tamper_status)) {
            ++accepted;
        }
        BENTO_CHECK(tamper_status == OpenStatus::kAuthFailed ||
                    tamper_status == OpenStatus::kMalformed ||
                    tamper_status == OpenStatus::kNotEncrypted);
        // OpenFrame 失败时 inner 已被 resize 成零向量；断言它不含任何明文。
        bool output_zero = true;
        for (std::uint8_t byte : output) {
            output_zero = output_zero && byte == 0;
        }
        BENTO_CHECK(output_zero);
    }
    BENTO_CHECK_EQ(accepted, 0);
}

/// OpenFrame 参数与结构矩阵：null/0 长度/未加密/seq=0/超长。
void TestFrameArgumentBounds() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0x61);
    const std::vector<std::uint8_t> payload = Bytes("bounds");
    const std::vector<std::uint8_t> inner =
        BuildFrame(SampleHeader(), nullptr, payload.data(), payload.size());
    std::vector<std::uint8_t> out;

    BENTO_CHECK(!SealFrame(nullptr, SecureDirection::kClientToServer, 1, inner.data(),
                           inner.size(), &out));
    BENTO_CHECK(!SealFrame(key.data(), SecureDirection::kClientToServer, 0, inner.data(),
                           inner.size(), &out));
    BENTO_CHECK(!SealFrame(key.data(), SecureDirection::kClientToServer, 1, nullptr,
                           inner.size(), &out));
    BENTO_CHECK(!SealFrame(key.data(), SecureDirection::kClientToServer, 1, inner.data(), 0,
                           &out));
    BENTO_CHECK(!SealFrame(key.data(), SecureDirection::kClientToServer, 1, inner.data(),
                           kMaxProbeFrameBytes + 1, &out));
    BENTO_CHECK(!SealFrame(key.data(), SecureDirection::kClientToServer, 1, inner.data(),
                           inner.size(), nullptr));

    std::uint8_t zero_key[kChaCha20KeyBytes] = {};
    BENTO_CHECK(SealFrame(zero_key, SecureDirection::kClientToServer, 1, inner.data(),
                          inner.size(), &out));
    ReplayWindow window;
    std::vector<std::uint8_t> opened;
    OpenStatus status = OpenStatus::kOk;
    BENTO_CHECK(!OpenFrame(nullptr, SecureDirection::kClientToServer, &window, out.data(),
                           out.size(), &opened, &status));
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, nullptr,
                           out.size(), &opened, &status));
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, out.data(), 0,
                           &opened, &status));
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, out.data(),
                           kFrameHeaderBytes, &opened, &status));
    BENTO_CHECK(status == OpenStatus::kMalformed);
    // 外层 seq=0：即使长度合法也必须 kMalformed。
    std::vector<std::uint8_t> zero_seq = out;
    zero_seq[8] = 0;
    zero_seq[9] = 0;
    zero_seq[10] = 0;
    zero_seq[11] = 0;
    ReplayWindow fresh;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, zero_seq.data(),
                           zero_seq.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kMalformed);
    // 错误方向：kAuthFailed。
    ReplayWindow fresh2;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kServerToClient, &fresh2, out.data(),
                           out.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed);
}

/// 会话票据随机单字节篡改 / 错误 PSK / 长度不符。
void TestSessionTicketTamperRandom() {
    std::mt19937 rng(0xC001D00DU);
    const std::vector<std::uint8_t> psk = FillSequence(32, 0xB7);
    int accepted = 0;
    for (int round = 0; round < 200; ++round) {
        SessionTicket ticket;
        ticket.conn_id = static_cast<std::uint32_t>(rng() | 1U);
        ticket.session_id = static_cast<std::uint32_t>(rng() | 1U);
        ticket.issued_ms = rng() % 100000U;
        ticket.expires_ms = ticket.issued_ms + 1 + rng() % 10000U;
        for (std::uint8_t& byte : ticket.client_nonce) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFU);
        }
        for (std::uint8_t& byte : ticket.server_nonce) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFU);
        }
        ticket.client_next_wire_seq = static_cast<std::uint32_t>(rng() | 1U);
        ticket.server_next_wire_seq = static_cast<std::uint32_t>(rng() | 1U);
        std::vector<std::uint8_t> encoded;
        BENTO_CHECK(EncodeSessionTicket(ticket, psk.data(), &encoded));
        BENTO_CHECK_EQ(encoded.size(), kSessionTicketBytes);
        SessionTicket decoded;
        BENTO_CHECK(DecodeSessionTicket(encoded.data(), encoded.size(), psk.data(), &decoded));
        BENTO_CHECK_EQ(decoded.conn_id, ticket.conn_id);

        std::vector<std::uint8_t> tampered = encoded;
        tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
        if (DecodeSessionTicket(tampered.data(), tampered.size(), psk.data(), &decoded)) {
            ++accepted;
        }
        std::vector<std::uint8_t> wrong_psk = psk;
        wrong_psk[rng() % wrong_psk.size()] ^= 0x80;
        if (DecodeSessionTicket(encoded.data(), encoded.size(), wrong_psk.data(), &decoded)) {
            ++accepted;
        }
        if (DecodeSessionTicket(encoded.data(), encoded.size() - 1, psk.data(), &decoded)) {
            ++accepted;
        }
    }
    BENTO_CHECK_EQ(accepted, 0);
}

/// 4 线程并发 Seal/Open：模块不得有可变全局状态。
void TestConcurrentSealOpen() {
    const std::vector<std::uint8_t> key = FillSequence(32, 0xC1);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([thread, &key, &failures]() {
            std::vector<std::uint8_t> plaintext(64 + static_cast<std::size_t>(thread) * 32, 0);
            for (std::size_t i = 0; i < plaintext.size(); ++i) {
                plaintext[i] = static_cast<std::uint8_t>(i * 7U + static_cast<std::size_t>(thread));
            }
            std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
            std::vector<std::uint8_t> decrypted(plaintext.size(), 0);
            for (int i = 0; i < 2000; ++i) {
                std::uint8_t nonce[kChaCha20NonceBytes] = {};
                // 每个线程使用独立 seq，避免测试自身构成 (key, nonce) 重用。
                MakeSecureNonce(SecureDirection::kClientToServer,
                                static_cast<std::uint32_t>(thread * 100000 + i + 1), nonce);
                std::uint8_t tag[kAeadTagBytes] = {};
                AeadSeal(key.data(), nonce, nullptr, 0, plaintext.data(), plaintext.size(),
                         ciphertext.data(), tag);
                if (!AeadOpen(key.data(), nonce, nullptr, 0, ciphertext.data(), ciphertext.size(),
                              tag, decrypted.data()) ||
                    decrypted != plaintext) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
}

/// 基础原语边界：常量时间比较、PSK 指纹、握手帧参数与「认证失败不推进窗口」。
void TestPrimitiveBoundsAndFingerprint() {
    const std::vector<std::uint8_t> psk = FillSequence(32, 0x5C);
    std::uint8_t fingerprint[8] = {};
    DerivePskFingerprint(psk.data(), fingerprint);
    std::uint8_t fingerprint_again[8] = {};
    DerivePskFingerprint(psk.data(), fingerprint_again);
    BENTO_CHECK(std::memcmp(fingerprint, fingerprint_again, sizeof(fingerprint)) == 0);
    std::vector<std::uint8_t> other_psk = psk;
    other_psk[0] ^= 0x01;
    std::uint8_t other_fingerprint[8] = {};
    DerivePskFingerprint(other_psk.data(), other_fingerprint);
    BENTO_CHECK(std::memcmp(fingerprint, other_fingerprint, sizeof(fingerprint)) != 0);
    std::uint8_t untouched[8];
    std::memset(untouched, 0xA5, sizeof(untouched));
    DerivePskFingerprint(nullptr, untouched);
    BENTO_CHECK(untouched[0] == 0xA5);

    const std::uint8_t equal_a[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::uint8_t equal_b[16];
    std::memcpy(equal_b, equal_a, sizeof(equal_b));
    BENTO_CHECK(ConstantTimeEqual16(equal_a, equal_b));
    equal_b[15] ^= 0x80;
    BENTO_CHECK(!ConstantTimeEqual16(equal_a, equal_b));
    BENTO_CHECK(!ConstantTimeEqual16(nullptr, equal_a));
    BENTO_CHECK(!ConstantTimeEqual16(equal_a, nullptr));

    // 握手帧参数边界：null/0 参数与非法帧类型必须拒绝。
    std::vector<std::uint8_t> handshake;
    HelloBody hello;
    hello.session = 0x10203040U;
    std::memcpy(hello.nonce, FillSequence(16, 0x77).data(), 16);
    hello.timestamp_ms = 42;
    std::uint8_t raw[kHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, raw, sizeof(raw)));
    BENTO_CHECK(!SealHandshakeFrame(nullptr, SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                    1, 1, raw, sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), 0, 1, 1, raw,
                                    sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                    0, 1, raw, sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                    1, 0, raw, sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kStream), hello.session,
                                    1, 1, raw, sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                    1, 1, nullptr, sizeof(raw), &handshake));
    BENTO_CHECK(!SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                    1, 1, raw, sizeof(raw), nullptr));
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                   1, 1, raw, sizeof(raw), &handshake));
    BENTO_CHECK(!OpenHandshakeFrame(nullptr, SecureDirection::kClientToServer, handshake.data(),
                                    handshake.size(), nullptr, nullptr, nullptr, nullptr,
                                    nullptr));
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::vector<std::uint8_t> body;
    OpenStatus handshake_status = OpenStatus::kOk;
    BENTO_CHECK(!OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer, nullptr,
                                    handshake.size(), &inner_type, &session, nullptr, &body,
                                    &handshake_status));
    BENTO_CHECK(!OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    handshake.data(), handshake.size(), nullptr, &session,
                                    nullptr, &body, &handshake_status));
    BENTO_CHECK(!OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    handshake.data(), handshake.size(), &inner_type, nullptr,
                                    nullptr, &body, &handshake_status));
    BENTO_CHECK(!OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                    handshake.data(), handshake.size(), &inner_type, &session,
                                    nullptr, nullptr, &handshake_status));

    // 认证失败（tag 篡改）不得推进重放窗口；随后合法包仍可接受。
    const std::vector<std::uint8_t> key = FillSequence(32, 0x33);
    const std::vector<std::uint8_t> payload = Bytes("window");
    const std::vector<std::uint8_t> inner =
        BuildFrame(SampleHeader(), nullptr, payload.data(), payload.size());
    std::vector<std::uint8_t> sealed;
    BENTO_CHECK(SealFrame(key.data(), SecureDirection::kClientToServer, 9, inner.data(),
                          inner.size(), &sealed));
    std::vector<std::uint8_t> tampered = sealed;
    tampered.back() ^= 0x01;
    ReplayWindow window;
    std::vector<std::uint8_t> opened;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, tampered.data(),
                           tampered.size(), &opened, &status));
    BENTO_CHECK(status == OpenStatus::kAuthFailed);
    BENTO_CHECK(!window.initialized());
    BENTO_CHECK_EQ(window.highest(), 0U);
    BENTO_CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed.data(),
                          sealed.size(), &opened, &status));
    BENTO_CHECK_EQ(window.highest(), 9U);
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
    TestReplayWindowInitializeBefore();
    TestAeadMessageSizes();
    TestAeadInPlace();
    TestAeadRandomTamper();
    TestFrameRandomTamper();
    TestFrameArgumentBounds();
    TestSessionTicketTamperRandom();
    TestConcurrentSealOpen();
    TestPrimitiveBoundsAndFingerprint();
    TestAeadThroughput();
    return bento_test::Finish("test_crypto");
}
