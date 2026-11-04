/**
 * @file main.cpp
 * @brief REQ-011 云上传命令行入口：`--file <path> --endpoint <url> --chunk-size 8M`。
 *
 * 退出码：0 上传并校验成功；1 参数/上传失败。
 */

#include "uploader.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void PrintUsage(const char* program) {
    std::printf(
        "用法：%s --file <path> [--endpoint <url>] [--chunk-size 8M] [--retries 5] [--quiet]\n"
        "  --file <path>        要上传的本地文件（必填）\n"
        "  --endpoint <url>     服务基地址，默认 http://127.0.0.1:29300\n"
        "  --chunk-size <size>  分片大小，支持 8M / 1MiB / 1048576，默认 8M\n"
        "  --retries <n>        单请求最大重试次数，默认 5\n"
        "  --timeout-ms <n>     单请求超时毫秒，默认 30000\n"
        "  --quiet              不打印续传/重试细节\n",
        program);
}

const char* FindValue(int argc, char** argv, int* index, const char* name) {
    if (std::strcmp(argv[*index], name) != 0) return nullptr;
    if (*index + 1 >= argc) return nullptr;
    *index += 1;
    return argv[*index];
}

}  // namespace

int main(int argc, char** argv) {
    cloud::Config config;
    std::string file;
    config.chunk_size = 8U << 20;  // CLI 默认 8 MiB；库默认 1 MiB。

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--quiet") == 0) {
            config.verbose = false;
            continue;
        }
        if (const char* value = FindValue(argc, argv, &i, "--file")) {
            file = value;
        } else if (const char* endpoint = FindValue(argc, argv, &i, "--endpoint")) {
            config.endpoint = endpoint;
        } else if (const char* size = FindValue(argc, argv, &i, "--chunk-size")) {
            if (!cloud::ParseSize(size, &config.chunk_size)) {
                std::fprintf(stderr, "非法 --chunk-size：%s\n", size);
                return 1;
            }
        } else if (const char* retries = FindValue(argc, argv, &i, "--retries")) {
            config.retries = std::atoi(retries);
            if (config.retries < 0 || config.retries > 100) {
                std::fprintf(stderr, "非法 --retries：%s（0..100）\n", retries);
                return 1;
            }
        } else if (const char* timeout = FindValue(argc, argv, &i, "--timeout-ms")) {
            config.timeout_ms = std::atoi(timeout);
            if (config.timeout_ms <= 0) {
                std::fprintf(stderr, "非法 --timeout-ms：%s\n", timeout);
                return 1;
            }
        } else {
            std::fprintf(stderr, "未知参数：%s（--help 查看用法）\n", argv[i]);
            return 1;
        }
    }

    if (file.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::printf("[main] file=%s\n", file.c_str());
    std::printf("[main] endpoint=%s chunk_size=%llu retries=%d\n", config.endpoint.c_str(),
                static_cast<unsigned long long>(config.chunk_size), config.retries);

    // 进度：每 5% 打一行，最后一行 100% 必打；stdout 逐行刷新便于管道观察。
    int last_step = -1;
    const auto progress = [&last_step](std::uint64_t done, std::uint64_t total) {
        const int percent =
            total == 0 ? 100 : static_cast<int>(done * 100 / total);
        const int step = percent / 5;
        if (step == last_step) return;
        last_step = step;
        std::printf("[progress] %llu/%llu bytes (%d%%)\n",
                    static_cast<unsigned long long>(done),
                    static_cast<unsigned long long>(total), percent);
        std::fflush(stdout);
    };

    cloud::Uploader uploader(config);
    const cloud::Result result = uploader.UploadFile(file, progress);

    std::printf("[result] %s\n", result.ok ? "OK" : "FAIL");
    if (!result.ok) std::printf("[result] error=%s\n", result.error.c_str());
    std::printf("[result] size=%llu chunks=%u uploaded=%u resumed=%u retries=%d sha256=%s\n",
                static_cast<unsigned long long>(result.size), result.chunks, result.uploaded,
                result.resumed, result.retries_used, result.sha256.c_str());
    std::printf("[result] seconds=%.3f throughput=%.2f MiB/s\n", result.seconds,
                result.seconds > 0.0
                    ? static_cast<double>(result.size) / (1U << 20) / result.seconds
                    : 0.0);
    return result.ok ? 0 : 1;
}
