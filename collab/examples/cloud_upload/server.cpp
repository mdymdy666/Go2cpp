/**
 * @file server.cpp
 * @brief REQ-011 测试用模拟云端：接收分片、返回已收清单、校验分片与整体 SHA-256。
 *
 * 仅用于本机验收，**不是**生产服务端：
 * - 分片落在 `<root>/<sha256>/<index>.part`，先写 `.tmp` 再 rename，避免半截文
 *   件被误认为已收分片；
 * - `GET /manifest` 扫目录返回已收索引，是断点续传的唯一依据；
 * - `POST /finalize` 按索引顺序流式重算整文件 SHA-256 并与客户端声明比对；
 * - `--fail-once N`：前 N 个分片 PUT 注入 HTTP 500，用于验证客户端重试。
 *
 * 数据面用 `go2cpp::net::ListenTCP` + `go2cpp::http::Server`（accept 循环 +
 * `serve_connection`），不引入第三方库。
 */

#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace http = go2cpp::http;
namespace hash = go2cpp::hash;
namespace net = go2cpp::net;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) { g_stop = 1; }

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{29300};
    fs::path root{"/tmp/upload_store"};
    int fail_once{0};
    /// 每个分片 PUT 前的人为延迟毫秒数；用于演示「上传中途被 kill」。
    int delay_ms{0};
};

struct Context {
    ServerOptions options;
    std::atomic<int> fail_budget{0};
    std::atomic<std::uint64_t> put_count{0};
    std::atomic<std::uint64_t> bytes_received{0};
};

struct Params {
    std::string sha256;
    std::uint64_t size{0};
    std::uint64_t chunk_size{0};
    std::uint32_t chunks{0};
    std::uint32_t index{0};
};

std::string Hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0f];
    }
    return out;
}

bool IsSha256Hex(const std::string& text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

std::string Field(const std::map<std::string, std::string>& fields, const std::string& name) {
    const auto it = fields.find(name);
    return it == fields.end() ? std::string() : it->second;
}

std::map<std::string, std::string> ParseQuery(const std::string& query) {
    std::map<std::string, std::string> fields;
    std::size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const std::string pair =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = pair.find('=');
        if (eq != std::string::npos) fields[pair.substr(0, eq)] = pair.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return fields;
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

/// 解析并校验公共查询参数；失败时写 error 供 400 响应。
bool ParseParams(const http::Request& request, Params* out, std::string* error) {
    const auto fields = ParseQuery(request.query());
    if (!IsSha256Hex(Field(fields, "sha256"))) {
        *error = "sha256 必须是 64 位小写 hex";
        return false;
    }
    out->sha256 = Field(fields, "sha256");
    std::uint64_t chunks = 0;
    if (!ParseU64(Field(fields, "size"), &out->size) ||
        !ParseU64(Field(fields, "chunk_size"), &out->chunk_size) ||
        !ParseU64(Field(fields, "chunks"), &chunks) || chunks > UINT32_MAX) {
        *error = "size/chunk_size/chunks 缺失或非法";
        return false;
    }
    out->chunks = static_cast<std::uint32_t>(chunks);
    if (out->chunk_size == 0 || out->chunk_size > (1ULL << 30) || out->size > (1ULL << 40)) {
        *error = "chunk_size 或 size 超出范围";
        return false;
    }
    const std::uint64_t expected_chunks =
        out->size == 0 ? 0 : (out->size + out->chunk_size - 1) / out->chunk_size;
    if (expected_chunks != out->chunks) {
        *error = "chunks 与 size/chunk_size 不一致";
        return false;
    }
    return true;
}

std::uint64_t ChunkBytes(std::uint64_t size, std::uint64_t chunk_size, std::uint32_t index) {
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * chunk_size;
    if (offset >= size) return 0;
    return std::min(chunk_size, size - offset);
}

http::Response TextResponse(http::Status status, std::string body) {
    http::Response response;
    response.status = status;
    response.header.set("Content-Type", "text/plain; charset=utf-8");
    response.body = std::move(body);
    return response;
}

http::Response BadRequest(const std::string& message) {
    return TextResponse(http::Status::BadRequest, "ERR " + message + "\n");
}

// ---------------------------------------------------------------------------
// 各路由实现（handler 在 http::Server 的 serve_connection 线程内执行）。
// ---------------------------------------------------------------------------

http::Response HandleManifest(const Context& ctx, const http::Request& request) {
    Params params;
    std::string error;
    if (!ParseParams(request, &params, &error)) return BadRequest(error);

    std::vector<std::uint32_t> received;
    const fs::path directory = ctx.options.root / params.sha256;
    std::error_code ec;
    if (fs::is_directory(directory, ec)) {
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            constexpr std::size_t kSuffixSize = 5;  // ".part"
            if (name.size() <= kSuffixSize ||
                name.compare(name.size() - kSuffixSize, kSuffixSize, ".part") != 0) {
                continue;
            }
            const std::string digits = name.substr(0, name.size() - kSuffixSize);
            std::uint64_t index = 0;
            if (!ParseU64(digits, &index) || index >= params.chunks) continue;
            const auto expected = ChunkBytes(params.size, params.chunk_size,
                                             static_cast<std::uint32_t>(index));
            if (entry.file_size(ec) == expected) {
                received.push_back(static_cast<std::uint32_t>(index));
            }
        }
    }
    std::sort(received.begin(), received.end());

    std::string body = "chunk_size=" + std::to_string(params.chunk_size) +
                       "\nsize=" + std::to_string(params.size) +
                       "\nchunks=" + std::to_string(params.chunks) + "\nreceived=";
    for (std::size_t i = 0; i < received.size(); ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(received[i]);
    }
    body.push_back('\n');
    std::printf("[server] GET /manifest sha=%s chunks=%u received=%zu -> 200\n",
                params.sha256.c_str(), params.chunks, received.size());
    std::fflush(stdout);
    return TextResponse(http::Status::OK, std::move(body));
}

http::Response HandleUpload(Context& ctx, const http::Request& request) {
    Params params;
    std::string error;
    if (!ParseParams(request, &params, &error)) return BadRequest(error);
    if (ctx.options.delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ctx.options.delay_ms));
    }
    std::uint64_t index = 0;
    const auto fields = ParseQuery(request.query());
    if (!ParseU64(Field(fields, "index"), &index) || index >= params.chunks) {
        return BadRequest("index 缺失或越界");
    }
    params.index = static_cast<std::uint32_t>(index);
    const std::uint64_t expected = ChunkBytes(params.size, params.chunk_size, params.index);
    if (request.body.size() != expected) {
        return BadRequest("分片长度不符：期望 " + std::to_string(expected) + "，实际 " +
                          std::to_string(request.body.size()));
    }
    if (ctx.fail_budget.load() > 0 && ctx.fail_budget.fetch_sub(1) > 0) {        std::printf("[server] PUT /upload index=%u 注入 500（剩余注入次数 %d）\n", params.index,
                    ctx.fail_budget.load());
        std::fflush(stdout);
        return TextResponse(http::Status::InternalError, "ERR injected failure\n");
    }
    const std::string chunk_sha = request.header.get("X-Chunk-SHA256");
    if (!chunk_sha.empty()) {
        hash::Sha256 hasher;
        hasher.Write(request.body.data(), request.body.size());
        const auto digest = hasher.Sum();
        const std::string actual = Hex(digest.data(), digest.size());
        if (actual != chunk_sha) {
            return BadRequest("分片 SHA-256 不符：声明 " + chunk_sha + "，实际 " + actual);
        }
    }
    const fs::path directory = ctx.options.root / params.sha256;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return TextResponse(http::Status::InternalError, "ERR 创建目录失败\n");
    const fs::path target = directory / (std::to_string(params.index) + ".part");
    const fs::path temporary = directory / (std::to_string(params.index) + ".part.tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
        if (!output) return TextResponse(http::Status::InternalError, "ERR 写分片失败\n");
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(target, ignored);
        fs::rename(temporary, target, ec);
        if (ec) return TextResponse(http::Status::InternalError, "ERR 提交分片失败\n");
    }
    ctx.put_count.fetch_add(1);
    ctx.bytes_received.fetch_add(expected);
    std::printf("[server] PUT /upload index=%u bytes=%llu sha=%s -> 200\n", params.index,
                static_cast<unsigned long long>(expected), chunk_sha.c_str());
    std::fflush(stdout);
    return TextResponse(http::Status::OK, "OK\n");
}

http::Response HandleFinalize(const Context& ctx, const http::Request& request) {
    Params params;
    std::string error;
    if (!ParseParams(request, &params, &error)) return BadRequest(error);
    const fs::path directory = ctx.options.root / params.sha256;
    hash::Sha256 whole;
    std::vector<char> buffer(1U << 20);
    std::uint64_t total = 0;
    for (std::uint32_t index = 0; index < params.chunks; ++index) {
        const fs::path file = directory / (std::to_string(index) + ".part");
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            return BadRequest("缺少分片 " + std::to_string(index));
        }
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = input.gcount();
            if (got <= 0) break;
            whole.Write(buffer.data(), static_cast<std::size_t>(got));
            total += static_cast<std::uint64_t>(got);
        }
    }
    if (total != params.size) {
        return BadRequest("分片总长 " + std::to_string(total) + " 与声明 " +
                          std::to_string(params.size) + " 不符");
    }
    const auto digest = whole.Sum();
    const std::string actual = Hex(digest.data(), digest.size());
    std::printf("[server] POST /finalize sha=%s computed=%s size=%llu chunks=%u -> %s\n",
                params.sha256.c_str(), actual.c_str(),
                static_cast<unsigned long long>(params.size), params.chunks,
                actual == params.sha256 ? "200" : "400");
    std::fflush(stdout);
    if (actual != params.sha256) {
        return BadRequest("整体 SHA-256 不符：声明 " + params.sha256 + "，实际 " + actual);
    }
    return TextResponse(http::Status::OK,
                       "OK " + actual + " " + std::to_string(params.size) + " " +
                           std::to_string(params.chunks) + "\n");
}

void PrintUsage(const char* program) {
    std::printf(
        "用法：%s [--port 29300] [--host 127.0.0.1] [--root /tmp/upload_store] "
        "[--fail-once N]\n"
        "  --port <n>       监听端口，默认 29300\n"
        "  --host <addr>    监听地址，默认 127.0.0.1\n"
        "  --root <dir>     分片存储目录，默认 /tmp/upload_store\n"
        "  --fail-once <n>  前 n 个分片 PUT 返回 500（验证客户端重试）\n"
        "  --delay-ms <n>   每个分片 PUT 前延迟 n 毫秒（演示中途 kill 后续传）\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        const auto need_value = [&](const char* name) -> const char* {
            if (std::strcmp(argv[i], name) != 0) return nullptr;
            if (i + 1 >= argc) return nullptr;
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (const char* port = need_value("--port")) {
            std::uint64_t value = 0;
            if (!ParseU64(port, &value) || value == 0 || value > 65535) {
                std::fprintf(stderr, "非法 --port：%s\n", port);
                return 1;
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (const char* host = need_value("--host")) {
            options.host = host;
        } else if (const char* root = need_value("--root")) {
            options.root = root;
        } else if (const char* fail = need_value("--fail-once")) {
            options.fail_once = std::atoi(fail);
            if (options.fail_once < 0) {
                std::fprintf(stderr, "非法 --fail-once：%s\n", fail);
                return 1;
            }
        } else if (const char* delay = need_value("--delay-ms")) {
            options.delay_ms = std::atoi(delay);
            if (options.delay_ms < 0) {
                std::fprintf(stderr, "非法 --delay-ms：%s\n", delay);
                return 1;
            }
        } else {
            std::fprintf(stderr, "未知参数：%s（--help 查看用法）\n", argv[i]);
            return 1;
        }
    }

    std::error_code ec;
    fs::create_directories(options.root, ec);
    if (ec) {
        std::fprintf(stderr, "无法创建存储目录 %s：%s\n", options.root.c_str(), ec.message().c_str());
        return 1;
    }

    auto listener = net::ListenTCP(options.host, options.port);
    if (!listener) {
        std::fprintf(stderr, "监听 %s:%u 失败（端口被占用？）\n", options.host.c_str(),
                     static_cast<unsigned>(options.port));
        return 1;
    }

    auto context = std::make_shared<Context>();
    context->options = options;
    context->fail_budget.store(options.fail_once);

    http::Server server(listener);
    server.route(http::Method::Get, "/health",
                 [](const http::Request&) { return TextResponse(http::Status::OK, "OK\n"); });
    server.route(http::Method::Get, "/manifest", [context](const http::Request& request) {
        return HandleManifest(*context, request);
    });
    server.route(http::Method::Put, "/upload", [context](const http::Request& request) {
        return HandleUpload(*context, request);
    });
    server.route(http::Method::Post, "/finalize", [context](const http::Request& request) {
        return HandleFinalize(*context, request);
    });
    server.set_fallback([](const http::Request&) {
        return TextResponse(http::Status::NotFound, "ERR not found\n");
    });

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::printf("[server] listening http://%s:%u root=%s fail_once=%d delay_ms=%d\n",
                options.host.c_str(), static_cast<unsigned>(options.port), options.root.c_str(),
                options.fail_once, options.delay_ms);
    std::fflush(stdout);

    while (g_stop == 0) {
        auto connection = listener->accept(std::chrono::milliseconds(100));
        if (!connection) continue;
        server.serve_connection(connection, 1000);
        (void)connection->close();
    }
    (void)listener->close();
    std::printf("[server] stopped; puts=%llu bytes=%llu\n",
                static_cast<unsigned long long>(context->put_count.load()),
                static_cast<unsigned long long>(context->bytes_received.load()));
    std::fflush(stdout);
    return 0;
}
