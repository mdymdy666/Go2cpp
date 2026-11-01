/**
 * @file test_avi_mjpeg.cpp
 * @brief MJPEG AVI 写入器验证：把 JPEG 序列封成 AVI，交给 ffprobe/ffmpeg 检查。
 *
 * ## 用法
 *
 * ```
 * test_avi_mjpeg --frames <JPEG 目录> --out <输出.avi> [--fps 30]
 * ```
 *
 * 目录里的 JPEG 按文件名字典序读取（`frame_001.jpg` ...）。退出码 0 表示
 * 写入成功；容器层面的断言（codec/尺寸/帧数/可解码性）由
 * `build_and_run_avi.sh` 里的 ffprobe/ffmpeg 完成。
 */
#include "go2cpp/media/avi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool IsJpeg(const std::vector<std::uint8_t>& data) {
    return data.size() >= 4 && data[0] == 0xff && data[1] == 0xd8 && data[data.size() - 2] == 0xff &&
           data[data.size() - 1] == 0xd9;
}

}  // namespace

int main(int argc, char** argv) {
    std::string frames_dir;
    std::string out_path;
    std::uint32_t fps = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            frames_dir = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else {
            std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
            return 2;
        }
    }
    if (frames_dir.empty() || out_path.empty()) {
        std::fprintf(stderr, "用法：%s --frames DIR --out FILE [--fps N]\n", argv[0]);
        return 2;
    }

    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(frames_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "目录里没有 .jpg：%s\n", frames_dir.c_str());
        return 3;
    }

    // 用第一帧的 JPEG 尺寸推断宽高（测试帧由 ffmpeg 生成，尺寸固定）。
    const std::vector<std::uint8_t> first = ReadFile(files.front());
    if (!IsJpeg(first)) {
        std::fprintf(stderr, "第一帧不是完整 JPEG：%s\n", files.front().c_str());
        return 3;
    }
    // JPEG 的 SOF0/SOF2 段里取宽高；不依赖外部库。
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::size_t i = 2; i + 9 < first.size();) {
        if (first[i] != 0xff) {
            ++i;
            continue;
        }
        const std::uint8_t marker = first[i + 1];
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) {
            i += 2;
            continue;
        }
        const std::uint16_t length = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(first[i + 2]) << 8) | first[i + 3]);
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            height = static_cast<std::uint32_t>(
                (static_cast<std::uint32_t>(first[i + 5]) << 8) | first[i + 6]);
            width = static_cast<std::uint32_t>(
                (static_cast<std::uint32_t>(first[i + 7]) << 8) | first[i + 8]);
            break;
        }
        i += 2 + length;
    }
    if (width == 0 || height == 0) {
        std::fprintf(stderr, "无法从 JPEG 解析宽高\n");
        return 3;
    }

    go2cpp::media::MjpegAviWriter writer;
    std::string error;
    if (!writer.Open(out_path, {width, height, fps}, &error)) {
        std::fprintf(stderr, "Open 失败：%s\n", error.c_str());
        return 4;
    }
    std::uint64_t total_bytes = 0;
    for (const std::string& path : files) {
        const std::vector<std::uint8_t> jpeg = ReadFile(path);
        if (!IsJpeg(jpeg)) {
            std::fprintf(stderr, "不是完整 JPEG：%s\n", path.c_str());
            return 4;
        }
        if (!writer.WriteFrame(jpeg.data(), jpeg.size(), &error)) {
            std::fprintf(stderr, "WriteFrame 失败：%s\n", error.c_str());
            return 4;
        }
        total_bytes += jpeg.size();
    }
    if (!writer.Finish(&error)) {
        std::fprintf(stderr, "Finish 失败：%s\n", error.c_str());
        return 4;
    }
    std::printf("[avi] 写入 %s：%u 帧 %ux%u@%u，JPEG 合计 %llu B，平均 %.1f KiB/帧\n",
                out_path.c_str(), writer.frame_count(), width, height, fps,
                static_cast<unsigned long long>(total_bytes),
                static_cast<double>(total_bytes) / static_cast<double>(writer.frame_count()) /
                    1024.0);
    return 0;
}
