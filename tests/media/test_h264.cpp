/**
 * @file test_h264.cpp
 * @brief H.264 编码后端探测：ffmpeg / NVENC 可用性 + 真实编码-解码往返 PSNR。
 * @details M1 的主路径是自研 kDeltaI420（无损、可逐帧严格校验），H.264 在这里
 *          作为**独立验证**：确认 WSL2 的 NVENC（`h264_nvenc`）与 `libx264` 真能
 *          编出可解码的码流，并量出有损编码的 PSNR。之所以走 ffmpeg CLI 而不是
 *          `go2cpp_media` 的 `CodecKind::kH264`：WSL2 没有 `libavcodec-dev`
 *          头文件，子进程后端无法做到「一帧进一帧出」的边界对齐（见
 *          `src/media/ffmpeg_codec.cpp` 的说明），而 M1 的逐帧校验不需要有损编码。
 *
 *          验证流程：
 *            1. 生成 30 帧 320x240 的确定性 BGRA 原始帧（含运动方块）；
 *            2. ffmpeg 编码为 H.264 Annex-B（优先 h264_nvenc，失败退 libx264）；
 *            3. ffmpeg 解码回 BGRA；
 *            4. 逐帧 PSNR，要求 >= 30 dB（有损但结构正确），帧数必须相等。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <sys/wait.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFrames = 30;

/// @brief 执行命令（命令串是编译期常量 + 固定路径，无注入面）。
int Run(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/// @brief 跑一条命令并返回是否成功。
bool RunOk(const std::string& command) {
    return Run(command) == 0;
}

/// @brief 生成确定性测试帧序列（BGRA）。
std::vector<std::uint8_t> MakeSequence() {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(kWidth) * kHeight * 4 * kFrames, 0);
    for (int f = 0; f < kFrames; ++f) {
        std::uint8_t* frame = data.data() + static_cast<std::size_t>(f) * kWidth * kHeight * 4;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                std::uint8_t* pixel = frame + (static_cast<std::size_t>(y) * kWidth + x) * 4;
                pixel[0] = static_cast<std::uint8_t>((x * 255 / kWidth + f * 4) & 0xFF);
                pixel[1] = static_cast<std::uint8_t>((y * 255 / kHeight) & 0xFF);
                pixel[2] = static_cast<std::uint8_t>(((x ^ y) & 32) != 0 ? 0xC0 : 0x30);
                pixel[3] = 255;
                const int square_x = (f * 8) % (kWidth - 40);
                const int square_y = (f * 5) % (kHeight - 40);
                if (x >= square_x && x < square_x + 40 && y >= square_y && y < square_y + 40) {
                    pixel[0] = 255;
                    pixel[1] = 0;
                    pixel[2] = 255;
                }
            }
        }
    }
    return data;
}

/// @brief 读整个文件。
std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::vector<std::uint8_t> data;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return data;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        const std::size_t read = std::fread(data.data(), 1, data.size(), file);
        data.resize(read);
    }
    std::fclose(file);
    return data;
}

/// @brief 写文件。
bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return written == data.size();
}

/// @brief 检查 ffmpeg 的编码器列表是否包含某个名字。
bool HasEncoder(const char* name) {
    FILE* pipe = ::popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[512];
    bool found = false;
    while (!found && std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (std::strstr(buffer, name) != nullptr) {
            found = true;
        }
    }
    ::pclose(pipe);
    return found;
}

/// @brief 评估一次有损往返，返回平均 PSNR 与解码帧数。
struct H264Result {
    int frames = 0;
    double mean_psnr = 0.0;
    double min_psnr = 0.0;
    std::size_t bitstream_bytes = 0;
    bool ok = false;
};

H264Result TryEncoder(const std::string& workdir, const std::vector<std::uint8_t>& source,
                      const char* encoder, bool nvenc) {
    H264Result result;
    std::string opts;
    if (nvenc) {
        opts = "-preset p1 -tune ll -rc cbr -b:v 4M";
    } else {
        opts = "-preset ultrafast -tune zerolatency -b:v 4M";
    }
    const std::string encode_command =
        "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra -s " +
        std::to_string(kWidth) + "x" + std::to_string(kHeight) + " -r 30 -i " + workdir +
        "/in.raw -c:v " + encoder + " " + opts + " -pix_fmt yuv420p -f h264 " + workdir +
        "/out.h264";
    if (!RunOk(encode_command)) {
        return result;
    }
    const std::string decode_command = "ffmpeg -hide_banner -loglevel error -y -f h264 -i " +
                                       workdir + "/out.h264 -f rawvideo -pix_fmt bgra " + workdir +
                                       "/decoded.raw";
    if (!RunOk(decode_command)) {
        return result;
    }
    const std::vector<std::uint8_t> bitstream = ReadFile(workdir + "/out.h264");
    const std::vector<std::uint8_t> decoded = ReadFile(workdir + "/decoded.raw");
    result.bitstream_bytes = bitstream.size();
    if (decoded.empty() || bitstream.empty()) {
        return result;
    }
    const std::size_t frame_bytes = static_cast<std::size_t>(kWidth) * kHeight * 4;
    const int frames = static_cast<int>(decoded.size() / frame_bytes);
    if (frames <= 0) {
        return result;
    }
    double total = 0.0;
    double worst = 99.0;
    for (int f = 0; f < frames; ++f) {
        VideoFrame reference;
        reference.width = kWidth;
        reference.height = kHeight;
        reference.format = PixelFormat::kBGRA;
        reference.stride = kWidth * 4;
        reference.data.assign(source.begin() + static_cast<std::ptrdiff_t>(f) * frame_bytes,
                              source.begin() + static_cast<std::ptrdiff_t>(f + 1) * frame_bytes);
        VideoFrame test;
        test.width = kWidth;
        test.height = kHeight;
        test.format = PixelFormat::kBGRA;
        test.stride = kWidth * 4;
        test.data.assign(decoded.begin() + static_cast<std::ptrdiff_t>(f) * frame_bytes,
                         decoded.begin() + static_cast<std::ptrdiff_t>(f + 1) * frame_bytes);
        const double psnr = FramePsnr(reference, test);
        total += psnr;
        if (psnr < worst) {
            worst = psnr;
        }
    }
    result.frames = frames;
    result.mean_psnr = total / frames;
    result.min_psnr = worst;
    result.ok = true;
    return result;
}

}  // namespace

int main() {
    const CodecAvailability availability = ProbeAvailability();
    std::printf("[INFO] availability: ffmpeg=%d nvenc=%d x11=%d sdl=%d\n",
                availability.h264 ? 1 : 0, availability.h264_nvenc ? 1 : 0,
                availability.x11_capture ? 1 : 0, availability.sdl_render ? 1 : 0);
    if (!availability.h264) {
        media_test::Skip("ffmpeg not available - H.264 backend probe skipped");
        return media_test::Finish("test_h264");
    }
    MEDIA_CHECK(HasEncoder("h264_nvenc"));
    MEDIA_CHECK(HasEncoder("libx264"));

    const std::string workdir = "/tmp/go2cpp_media_h264";
    MEDIA_CHECK(RunOk("rm -rf " + workdir + " && mkdir -p " + workdir));
    const std::vector<std::uint8_t> source = MakeSequence();
    MEDIA_CHECK(WriteFile(workdir + "/in.raw", source));
    MEDIA_CHECK_EQ(ReadFile(workdir + "/in.raw").size(), source.size());

    bool any_ok = false;
    if (availability.h264_nvenc) {
        const H264Result nvenc = TryEncoder(workdir, source, "h264_nvenc", true);
        std::printf("[INFO] h264_nvenc: ok=%d frames=%d psnr_mean=%.2f psnr_min=%.2f bytes=%zu\n",
                    nvenc.ok ? 1 : 0, nvenc.frames, nvenc.mean_psnr, nvenc.min_psnr,
                    nvenc.bitstream_bytes);
        if (nvenc.ok) {
            any_ok = true;
            MEDIA_CHECK_EQ(nvenc.frames, kFrames);
            MEDIA_CHECK_GE(nvenc.mean_psnr, 30.0);
            MEDIA_CHECK_GE(nvenc.min_psnr, 24.0);
            // 4Mbps @30fps 的 30 帧 ≈ 500KB；给出宽松上界防止「编码器空转」。
            MEDIA_CHECK(nvenc.bitstream_bytes > 1024);
        } else {
            media_test::Skip("h264_nvenc encode/decode failed - falling back to libx264");
        }
    } else {
        MEDIA_CHECK(HasEncoder("libx264"));
    }

    // libx264 作为对照（也是 NVENC 不可用时的默认后端）。
    const H264Result x264 = TryEncoder(workdir, source, "libx264", false);
    std::printf("[INFO] libx264: ok=%d frames=%d psnr_mean=%.2f psnr_min=%.2f bytes=%zu\n",
                x264.ok ? 1 : 0, x264.frames, x264.mean_psnr, x264.min_psnr, x264.bitstream_bytes);
    if (x264.ok) {
        any_ok = true;
        MEDIA_CHECK_EQ(x264.frames, kFrames);
        MEDIA_CHECK_GE(x264.mean_psnr, 30.0);
    }
    MEDIA_CHECK(any_ok);

    // Annex-B 起始码健全性：码流里必须出现 00 00 00 01。
    const std::vector<std::uint8_t> bitstream = ReadFile(workdir + "/out.h264");
    bool found_start_code = false;
    for (std::size_t i = 0; i + 3 < bitstream.size(); ++i) {
        if (bitstream[i] == 0 && bitstream[i + 1] == 0 && bitstream[i + 2] == 0 &&
            bitstream[i + 3] == 1) {
            found_start_code = true;
            break;
        }
    }
    MEDIA_CHECK(found_start_code);
    std::printf("[INFO] annex-b start code found=%d, bitstream=%zu bytes\n",
                found_start_code ? 1 : 0, bitstream.size());
    return media_test::Finish("test_h264");
}
