/**
 * @file main.cpp
 * @brief 流表框架演示：一条连接上传一个文件 + 一张 k=v 表，走完整帧/流控/调度。
 * @details 用 `MemoryTransport` 把两个 `Connection` 背靠背连起来，因此不需要
 *          端口、信号与第三方客户端，但帧编解码、握手、流状态机、连接/流两级
 *          窗口、WFQ 调度、资源分块与 SHA-256 校验全部是**真实路径**：
 *
 *          - 资源：`FileChunkSource` 按 128 KiB 分块，`ResourceProducer` 生成
 *            HEADERS + DATA 帧；服务端 `FileResourceSink` 落盘并校验整资源
 *            SHA-256 与每块 CRC-32；
 *          - KV：一张 64 KiB 的大表（`payload` 值超过 4 KiB 阈值）自动走
 *            「HEADERS + DATA」流式模式；一张 5 对的小表走单 HEADERS 模式；
 *          - 两条流共享连接窗口，初始流窗口 65535 小于一个资源块，因此能看到
 *            真实的 WINDOW_UPDATE 与 `flow_control_blocks` 计数。
 *
 *          运行：./go2cpp_stream_demo [--file <输入文件>] [--workdir <目录>]
 *                                    [--kv-bytes 65536] [--chunk-size 131072]
 *
 *          不传 `--file` 时自动生成一个 1 MiB 的确定性文件，便于自检。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/app/connection.hpp"
#include "go2cpp/app/kv.hpp"
#include "go2cpp/app/kv_stream.hpp"
#include "go2cpp/app/resource.hpp"
#include "go2cpp/app/transport.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::app::BuildResourceMeta;
using go2cpp::app::Connection;
using go2cpp::app::ConnectionConfig;
using go2cpp::app::ErrorCode;
using go2cpp::app::Event;
using go2cpp::app::EventType;
using go2cpp::app::FileChunkSource;
using go2cpp::app::FileResourceSink;
using go2cpp::app::KvTable;
using go2cpp::app::MemoryTransport;
using go2cpp::app::ResourceMeta;
using go2cpp::app::Role;
using go2cpp::app::Sha256Hex;
using go2cpp::app::StreamKindName;

/// @brief 演示参数。
struct Options {
    std::string file;                              ///< 输入文件；空=自动生成。
    std::string workdir = "/tmp/go2cpp_stream_demo";  ///< 工作目录。
    std::size_t kv_bytes = 64 * 1024;              ///< 大 KV 值的字节数。
    std::uint32_t chunk_size = 128 * 1024;         ///< 资源块大小。
};

/// @brief 接收侧状态（事件回调在 Step 线程同步执行，单线程无锁）。
struct ReceiverState {
    bool resource_complete = false;  ///< 是否收到资源完成事件。
    bool resource_ok = false;        ///< 资源校验是否通过。
    std::uint64_t resource_bytes = 0;///< 收到的资源字节。
    std::uint32_t resource_chunks = 0;  ///< 完成的块数。
    std::string resource_sha256;     ///< 实际 SHA-256。
    double resource_throughput = 0.0;///< 吞吐 MiB/s。
    int kv_complete = 0;             ///< 完成的 KV 流数量。
    std::size_t big_kv_value = 0;    ///< 大表 payload 长度。
    std::string big_kv_name;         ///< 大表 name。
    std::size_t small_kv_keys = 0;   ///< 小表键数。
    bool error = false;              ///< 是否出现错误事件。
};

/// @brief 单调时钟毫秒数。
std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// @brief 生成确定性字节并写入文件。
bool WritePatternFile(const std::filesystem::path& path, std::uint64_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    std::uint32_t state = 0x12345678U;
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));
        for (std::size_t i = 0; i < chunk; ++i) {
            state = state * 1664525U + 1013904223U;
            buffer[i] = static_cast<std::uint8_t>(state >> 24);
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk));
        if (!out) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

/// @brief 读取整个文件（校验用）。
bool ReadAllBytes(const std::filesystem::path& path, std::vector<std::uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// @brief 解析无符号整数。
bool ParseUint(const char* text, unsigned long long* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/// @brief 打印用法。
void PrintUsage(const char* program) {
    std::printf(
        "用法: %s [--file <输入文件>] [--workdir <目录>] [--kv-bytes N]\n"
        "         [--chunk-size N]\n"
        "  --file       要传的文件；不传则自动生成 1 MiB 确定性文件\n"
        "  --workdir    输入/输出文件目录（默认 /tmp/go2cpp_stream_demo）\n"
        "  --kv-bytes   大 KV 值的字节数（默认 65536，>4096 即触发流式模式）\n"
        "  --chunk-size 资源块大小（默认 131072，范围 64KiB..4MiB）\n",
        program);
}

/// @brief 处理服务端事件（资源完成 / KV 完成 / 错误）。
void OnServerEvent(const Event& event, ReceiverState* state) {
    switch (event.type) {
        case EventType::kResourceComplete:
            state->resource_complete = true;
            state->resource_ok = event.ok;
            state->resource_bytes = event.resource_stats.bytes;
            state->resource_chunks = event.resource_stats.chunks;
            state->resource_sha256 = event.resource_stats.sha256_hex;
            state->resource_throughput = event.resource_stats.throughput_mbps;
            std::printf(
                "[server] 资源流 #%u 完成：ok=%d bytes=%llu chunks=%u sha256=%.16s... "
                "吞吐=%.1f MiB/s\n",
                event.stream_id, static_cast<int>(event.ok),
                static_cast<unsigned long long>(event.resource_stats.bytes),
                event.resource_stats.chunks, event.resource_stats.sha256_hex.c_str(),
                event.resource_stats.throughput_mbps);
            break;
        case EventType::kKvComplete: {
            ++state->kv_complete;
            const std::string payload = event.headers.Get("payload");
            if (!payload.empty()) {
                state->big_kv_value = payload.size();
                state->big_kv_name = event.headers.Get("name");
                std::printf("[server] KV 流 #%u 完成：keys=%zu name=%s payload=%zu 字节\n",
                            event.stream_id, event.headers.size(),
                            state->big_kv_name.c_str(), payload.size());
            } else {
                state->small_kv_keys = event.headers.size();
                std::printf("[server] KV 流 #%u 完成：keys=%zu（小表，单 HEADERS 帧）\n",
                            event.stream_id, event.headers.size());
            }
            break;
        }
        case EventType::kError:
            state->error = true;
            std::printf("[server] 错误事件：stream=%u %s\n", event.stream_id,
                        event.message.c_str());
            break;
        case EventType::kClosed:
            std::printf("[server] 连接关闭\n");
            break;
        default:
            break;
    }
}

/// @brief 打印客户端的流表与连接统计。
void PrintClientStats(const Connection& client) {
    const auto stats = client.stats();
    std::printf(
        "[client] 统计：帧=%llu/%llu 载荷字节=%llu/%llu flush=%llu 流控阻塞=%llu "
        "WINDOW_UPDATE=%llu 资源流=%llu KV流=%llu\n",
        static_cast<unsigned long long>(stats.frames_sent),
        static_cast<unsigned long long>(stats.frames_received),
        static_cast<unsigned long long>(stats.bytes_sent),
        static_cast<unsigned long long>(stats.bytes_received),
        static_cast<unsigned long long>(stats.flush_rounds),
        static_cast<unsigned long long>(stats.flow_control_blocks),
        static_cast<unsigned long long>(stats.window_updates_sent),
        static_cast<unsigned long long>(stats.resource_streams),
        static_cast<unsigned long long>(stats.kv_streams));
    std::printf("[client] 流表：\n");
    for (const auto& entry : client.table().streams()) {
        const auto& stream = *entry.second;
        std::printf("  #%-3u %-9s 状态=%-14s 权重=%u 发送=%llu 字节/%llu 帧\n", stream.id,
                    StreamKindName(stream.kind),
                    go2cpp::app::StreamStateName(stream.state), stream.weight,
                    static_cast<unsigned long long>(stream.bytes_sent),
                    static_cast<unsigned long long>(stream.frames_sent));
    }
}

/// @brief 主流程。
int Run(const Options& options) {
    std::error_code ec;
    std::filesystem::create_directories(options.workdir, ec);

    const std::filesystem::path input =
        options.file.empty() ? std::filesystem::path(options.workdir) / "input.bin"
                             : std::filesystem::path(options.file);
    const std::filesystem::path output =
        std::filesystem::path(options.workdir) / "output.bin";

    if (options.file.empty()) {
        if (!WritePatternFile(input, 1024 * 1024)) {
            std::fprintf(stderr, "[stream_demo] 生成输入文件失败: %s\n",
                         input.string().c_str());
            return 1;
        }
        std::printf("[stream_demo] 已生成输入文件 %s（1 MiB）\n", input.string().c_str());
    }
    const auto input_size = std::filesystem::file_size(input, ec);
    if (ec || input_size == 0) {
        std::fprintf(stderr, "[stream_demo] 输入文件不可读或为空: %s\n",
                     input.string().c_str());
        return 1;
    }
    std::printf("[stream_demo] 工作目录 %s\n", options.workdir.c_str());
    std::printf("[stream_demo] 输入 %s（%llu 字节）\n", input.string().c_str(),
                static_cast<unsigned long long>(input_size));

    // ---- 1. 扫描文件，生成资源元数据（SHA-256 / CRC / 块数）----
    auto source = std::make_shared<FileChunkSource>(input.string());
    std::string error;
    if (!source->Open(&error)) {
        std::fprintf(stderr, "[stream_demo] 打开输入失败: %s\n", error.c_str());
        return 1;
    }
    KvTable extra;
    extra.Set("x.asset_type", "demo");
    ResourceMeta meta = BuildResourceMeta("demo/asset.bin", *source, options.chunk_size,
                                          extra, &error);
    if (!meta.Validate(&error)) {
        std::fprintf(stderr, "[stream_demo] 元数据非法: %s\n", error.c_str());
        return 1;
    }
    std::printf("[stream_demo] 资源元数据：chunk_size=%u chunks=%u sha256=%.16s...\n",
                meta.chunk_size, meta.chunk_count, meta.sha256_hex.c_str());

    // ---- 2. 建立内存传输与两端连接（真实帧流，无端口）----
    auto pair = MemoryTransport::CreatePair();
    ConnectionConfig config;
    ReceiverState state;
    auto server = std::make_unique<Connection>(std::move(pair.second), Role::kServer, config);
    auto client = std::make_unique<Connection>(std::move(pair.first), Role::kClient, config);

    server->SetResourceSinkFactory(
        [output](const ResourceMeta& /*meta*/) -> std::unique_ptr<go2cpp::app::ResourceSink> {
            return std::make_unique<FileResourceSink>(output.string(), true);
        });
    server->SetEventHandler([&state](const Event& event) { OnServerEvent(event, &state); });

    if (!server->Start(&error) || !client->Start(&error)) {
        std::fprintf(stderr, "[stream_demo] 握手启动失败: %s\n", error.c_str());
        return 1;
    }

    // ---- 3. 并发发起 3 条流：资源（高权重）+ 大 KV + 小 KV ----
    const auto resource_result = client->SendResource(meta, source, 0, 32);
    if (!resource_result.ok()) {
        std::fprintf(stderr, "[stream_demo] SendResource 失败: %s\n",
                     resource_result.message.c_str());
        return 1;
    }
    KvTable big;
    big.Set("name", "stream_demo");
    big.Set("version", "1");
    big.Set("payload", std::string(options.kv_bytes, 'x'));
    big.Set("note", "大值走 DATA 帧");
    const auto big_result = client->SendKv(big, 16);
    if (!big_result.ok()) {
        std::fprintf(stderr, "[stream_demo] 大 KV 发送失败: %s\n",
                     big_result.message.c_str());
        return 1;
    }
    KvTable small;
    small.Set("k1", "v1");
    small.Set("k2", "v2");
    small.Set("k3", "v3");
    small.Set("k4", "v4");
    small.Set("k5", "v5");
    const auto small_result = client->SendKv(small, 8);
    if (!small_result.ok()) {
        std::fprintf(stderr, "[stream_demo] 小 KV 发送失败: %s\n",
                     small_result.message.c_str());
        return 1;
    }
    std::printf("[stream_demo] 已发起：资源流 #%u、大 KV 流 #%u、小 KV 流 #%u\n",
                resource_result.stream_id, big_result.stream_id, small_result.stream_id);

    // ---- 4. 单线程泵：交替驱动两端，直到 3 条流全部完成或超时 ----
    const std::uint64_t started = NowMs();
    const std::uint64_t deadline = started + 20000;
    while (NowMs() < deadline) {
        client->Step(0);
        server->Step(0);
        if (state.error) {
            break;
        }
        if (state.resource_complete && state.kv_complete >= 2) {
            break;
        }
        if (client->closed() && server->closed()) {
            break;
        }
        std::this_thread::sleep_for(200us);
    }
    const std::uint64_t elapsed = NowMs() - started;

    PrintClientStats(*client);
    client->Close();
    server->Close();

    if (state.error) {
        std::fprintf(stderr, "[stream_demo] 传输失败（错误事件）\n");
        return 1;
    }
    if (!state.resource_complete || state.kv_complete < 2) {
        std::fprintf(stderr,
                     "[stream_demo] 超时：资源完成=%d KV完成=%d（期望 1/2）耗时=%llu ms\n",
                     static_cast<int>(state.resource_complete), state.kv_complete,
                     static_cast<unsigned long long>(elapsed));
        return 1;
    }

    // ---- 5. 校验落盘文件 ----
    std::vector<std::uint8_t> received;
    if (!ReadAllBytes(output, &received)) {
        std::fprintf(stderr, "[stream_demo] 读取输出文件失败: %s\n",
                     output.string().c_str());
        return 1;
    }
    const std::string output_sha256 = Sha256Hex(received.data(), received.size());
    const bool hash_ok = (output_sha256 == meta.sha256_hex);
    std::printf("[stream_demo] 输出 %s（%zu 字节）sha256=%.16s...\n",
                output.string().c_str(), received.size(), output_sha256.c_str());
    if (!hash_ok) {
        std::fprintf(stderr, "[stream_demo] SHA-256 不一致：期望 %s\n",
                     meta.sha256_hex.c_str());
        return 1;
    }
    if (!state.resource_ok) {
        std::fprintf(stderr, "[stream_demo] 服务端资源校验失败\n");
        return 1;
    }
    if (state.big_kv_value != options.kv_bytes ||
        state.big_kv_name != "stream_demo" || state.small_kv_keys != 5) {
        std::fprintf(stderr,
                     "[stream_demo] KV 校验失败：payload=%zu name=%s small_keys=%zu\n",
                     state.big_kv_value, state.big_kv_name.c_str(), state.small_kv_keys);
        return 1;
    }

    std::printf(
        "[stream_demo] 摘要：资源 %llu 字节 / %u 块，KV 大表 %zu 字节 + 小表 %zu 键，"
        "总耗时 %llu ms，接收吞吐 %.1f MiB/s\n",
        static_cast<unsigned long long>(state.resource_bytes), state.resource_chunks,
        state.big_kv_value, state.small_kv_keys,
        static_cast<unsigned long long>(elapsed), state.resource_throughput);
    std::printf("STREAM_OK\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        unsigned long long value = 0;
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--file" && i + 1 < argc) {
            options.file = argv[++i];
        } else if (arg == "--workdir" && i + 1 < argc) {
            options.workdir = argv[++i];
        } else if (arg == "--kv-bytes" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value) || value == 0) {
                std::fprintf(stderr, "--kv-bytes 非法\n");
                return 2;
            }
            options.kv_bytes = static_cast<std::size_t>(value);
        } else if (arg == "--chunk-size" && i + 1 < argc) {
            if (!ParseUint(argv[++i], &value)) {
                std::fprintf(stderr, "--chunk-size 非法\n");
                return 2;
            }
            options.chunk_size = static_cast<std::uint32_t>(value);
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }
    return Run(options);
}
