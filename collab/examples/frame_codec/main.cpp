// JinShan REQ-001 验收示例：FrameCodec 两种 spec / 半包 / 粘包 / 畸形字节流。
// 只验证行为，不做性能测量。构建运行见同目录 build_and_run.sh。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "go2cpp/frame/frame_codec.hpp"

namespace f = go2cpp::frame;
static int g_fail = 0;

static void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::vector<std::uint8_t> Payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(seed + i * 7u);
    return p;
}

int main() {
    // RDRL 上限 64 KiB，RDCH 上限 4 MiB；帧头 12B + payload + CRC32 4B。
    f::FrameCodec rdrl(f::MakeFrameSpec(f::MakeMagic("RDRL"), 1, 65536));
    f::FrameCodec rdch(f::MakeFrameSpec(f::MakeMagic("RDCH"), 1, 4 * 1024 * 1024));
    Check(rdrl.Spec().MaxFrameBytes() == 12u + 65536u + 4u &&
              rdch.Spec().MaxFrameBytes() == 12u + 4u * 1024u * 1024u + 4u,
          "两种 FrameSpec: RDRL=64KiB, RDCH=4MiB, MaxFrameBytes 含 12B 头 + 4B CRC");

    f::Error err;
    const f::Frame frame{0x31, 0x0102, Payload(300, 0x11)};
    std::vector<std::uint8_t> wire;
    Check(rdrl.Encode(frame, &wire, &err), "Encode 一帧（type=0x31, flags=0x0102, payload=300B）");

    // 半包：一帧拆成 1 字节 × N 次 Feed，每次都必须成功且 consumed==1。
    {
        std::vector<f::Frame> frames;
        bool steps_ok = true;
        for (std::size_t i = 0; i < wire.size(); ++i) {
            std::size_t consumed = 0;
            const bool ok = rdrl.Feed(&wire[i], 1, &frames, &consumed, &err);
            steps_ok = steps_ok && ok && consumed == 1;
        }
        Check(steps_ok && frames.size() == 1 && frames[0].payload == frame.payload,
              "半包: 逐字节 Feed 全部成功后拼出 1 帧，payload 逐字节一致");
    }

    // 粘包：3 帧拼成一个 buffer，一次 Feed 应解出 3 帧，consumed 等于总字节数。
    {
        std::vector<std::uint8_t> glued;
        for (int i = 0; i < 3; ++i) {
            std::vector<std::uint8_t> bytes;
            if (!rdrl.Encode(f::Frame{static_cast<std::uint8_t>(i + 1), 0,
                                      Payload(static_cast<std::size_t>(100 + i),
                                              static_cast<std::uint8_t>(i * 3))},
                             &bytes, &err)) { ++g_fail; break; }
            glued.insert(glued.end(), bytes.begin(), bytes.end());
        }
        std::vector<f::Frame> frames;
        std::size_t consumed = 0;
        const bool ok = rdrl.Feed(glued.data(), glued.size(), &frames, &consumed, &err);
        Check(ok && frames.size() == 3 && consumed == glued.size(),
              "粘包: 3 帧连发、一次 Feed 返回 3 帧且 consumed == 输入总长");
    }

    // 畸形：三种都必须返回 false，并给出各自的错误码。
    auto feed_bad = [&](const std::vector<std::uint8_t>& bytes) {
        rdrl.Reset();
        std::vector<f::Frame> frames;
        std::size_t consumed = 0;
        return !rdrl.Feed(bytes.data(), bytes.size(), &frames, &consumed, &err);
    };
    std::vector<std::uint8_t> bad_magic = wire, bad_crc = wire;
    bad_magic[0] = 'X';
    bad_crc.back() ^= 0xFFu;
    Check(feed_bad(bad_magic) && err.code == f::ErrorCode::kBadMagic,
          "畸形: 坏 magic -> kBadMagic(5)");
    Check(feed_bad(bad_crc) && err.code == f::ErrorCode::kChecksumMismatch,
          "畸形: 坏 CRC -> kChecksumMismatch(7)");
    std::vector<std::uint8_t> bad_len = {'R', 'D', 'R', 'L', 1, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; ++i)  // length = 65537 > max_payload(65536)，小端写入偏移 8
        bad_len[8 + i] = static_cast<std::uint8_t>((65537u >> (8 * i)) & 0xFFu);
    Check(feed_bad(bad_len) && err.code == f::ErrorCode::kLengthExceeded,
          "畸形: length 65537 越界 -> kLengthExceeded(4)");

    std::printf("frame_codec: %s (%d 个断言失败)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
