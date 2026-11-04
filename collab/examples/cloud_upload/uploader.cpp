/**
 * @file uploader.cpp
 * @brief REQ-011 云端上传器的实现：流式分片、重试、断点续传、整体校验。
 *
 * 只依赖仓库内组件：`go2cpp::http::Client` 与 `go2cpp::hash::Sha256`
 * （以及标准库文件 IO），不引入第三方库。
 */

#include "uploader.hpp"

#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/network/http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cloud {

namespace http = go2cpp::http;
namespace hash = go2cpp::hash;

namespace {

using SteadyClock = std::chrono::steady_clock;
using HeaderList = std::vector<std::pair<std::string, std::string>>;

constexpr std::uint64_t kMinChunkSize = 1U << 10;   // 1 KiB
constexpr std::uint64_t kMaxChunkSize = 1U << 30;   // 1 GiB
constexpr int kMaxBackoffMs = 5000;

/// 服务基地址的解析结果（只支持 http://host[:port][/base]）。
struct Endpoint {
    std::string host;
    std::uint16_t port{80};
    std::string base;  ///< 规范化后的路径前缀；空或以 '/' 开头、无尾 '/'
};

/// 单次 HTTP 请求结果；transport_ok=false 表示连接/发送/接收失败。
struct Reply {
    bool transport_ok{false};
    int status{0};
    std::string body;
    std::string error;

    bool ok() const noexcept { return transport_ok && status >= 200 && status < 300; }
    /// 5xx/408/429 与传输错误可重试；其他 4xx 是确定性错误。
    bool retryable() const noexcept {
        if (!transport_ok) return true;
        return status >= 500 || status == 408 || status == 429;
    }
};

void Log(const Config& config, const char* fmt, ...) {
    if (!config.verbose) return;
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[uploader] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

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

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\r' || text[begin] == '\n')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return std::string(text.substr(begin, end - begin));
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
    if (text.empty()) return false;
    try {
        std::size_t used = 0;
        const auto parsed = std::stoull(text, &used, 10);
        if (used != text.size()) return false;
        *value = static_cast<std::uint64_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// 指数退避：base, 2*base, 4*base ... 封顶 kMaxBackoffMs。
int BackoffMs(int base_ms, int attempt) {
    int wait = base_ms > 0 ? base_ms : 1;
    for (int i = 0; i < attempt && wait < kMaxBackoffMs; ++i) wait *= 2;
    return std::min(wait, kMaxBackoffMs);
}

bool ParseEndpoint(const std::string& url, Endpoint* out, std::string* error) {
    constexpr std::string_view kScheme = "http://";
    if (url.size() <= kScheme.size() || url.compare(0, kScheme.size(), kScheme) != 0) {
        *error = "endpoint 必须是 http://host[:port][/base] 形式：" + url;
        return false;
    }
    std::string rest = url.substr(kScheme.size());
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) {
        *error = "endpoint 缺少主机名：" + url;
        return false;
    }
    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    out->base = slash == std::string::npos ? std::string() : rest.substr(slash);
    if (authority.empty()) {
        *error = "endpoint 缺少主机名：" + url;
        return false;
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && colon > 0) {
        out->host = authority.substr(0, colon);
        std::uint64_t port = 0;
        if (!ParseU64(authority.substr(colon + 1), &port) || port == 0 || port > 65535) {
            *error = "endpoint 端口非法：" + url;
            return false;
        }
        out->port = static_cast<std::uint16_t>(port);
    } else {
        out->host = authority;
        out->port = 80;
    }
    return true;
}

Reply Perform(const Endpoint& endpoint, http::Method method, const std::string& target,
              const std::string& body, const HeaderList& headers, int timeout_ms) {
    http::Client client{std::chrono::milliseconds(timeout_ms)};
    http::Request request = http::NewRequest(method, target, body);
    request.header.set("Content-Type", "application/octet-stream");
    for (const auto& header : headers) request.header.set(header.first, header.second);
    const auto result = client.do_request(request, endpoint.host, endpoint.port);
    Reply reply;
    if (!result.ok()) {
        reply.error = result.message.empty() ? "HTTP 请求失败" : result.message;
        return reply;
    }
    reply.transport_ok = true;
    reply.status = static_cast<int>(result.response->status);
    reply.body = result.response->body;
    return reply;
}

/// 已收分片清单（服务端返回的文本协议）。
struct Manifest {
    std::uint64_t chunk_size{0};
    std::uint64_t size{0};
    std::uint32_t chunks{0};
    std::vector<std::uint32_t> received;
};

bool ParseManifest(const std::string& body, Manifest* out, std::string* error) {
    std::map<std::string, std::string> fields;
    std::size_t begin = 0;
    while (begin < body.size()) {
        auto end = body.find('\n', begin);
        if (end == std::string::npos) end = body.size();
        const std::string line = Trim(std::string_view(body).substr(begin, end - begin));
        begin = end + 1;
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        fields[line.substr(0, eq)] = line.substr(eq + 1);
    }
    std::uint64_t chunks = 0;
    if (!ParseU64(fields["chunk_size"], &out->chunk_size) ||
        !ParseU64(fields["size"], &out->size) || !ParseU64(fields["chunks"], &chunks) ||
        chunks > UINT32_MAX) {
        *error = "服务端清单缺少/非法 chunk_size、size、chunks 字段";
        return false;
    }
    out->chunks = static_cast<std::uint32_t>(chunks);
    std::string_view list = fields["received"];
    std::size_t pos = 0;
    while (pos < list.size()) {
        const auto comma = list.find(',', pos);
        const auto length = comma == std::string_view::npos ? std::string_view::npos : comma - pos;
        const std::string token = Trim(list.substr(pos, length));
        if (!token.empty()) {
            std::uint64_t index = 0;
            if (!ParseU64(token, &index) || index >= out->chunks) {
                *error = "服务端清单包含非法分片索引：" + token;
                return false;
            }
            out->received.push_back(static_cast<std::uint32_t>(index));
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return true;
}

/// 第 index 片应有的字节数（最后一片可能更短）。
std::uint64_t ChunkBytes(std::uint64_t size, std::uint64_t chunk_size, std::uint32_t index) {
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * chunk_size;
    return std::min(chunk_size, size - offset);
}

std::string ManifestTarget(const Endpoint& endpoint, const std::string& sha, std::uint64_t size,
                           std::uint64_t chunk_size, std::uint32_t chunks) {
    return endpoint.base + "/manifest?sha256=" + sha + "&size=" + std::to_string(size) +
           "&chunk_size=" + std::to_string(chunk_size) + "&chunks=" + std::to_string(chunks);
}

std::string UploadTarget(const Endpoint& endpoint, const std::string& sha, std::uint64_t size,
                         std::uint64_t chunk_size, std::uint32_t chunks, std::uint32_t index) {
    return endpoint.base + "/upload?sha256=" + sha + "&index=" + std::to_string(index) +
           "&size=" + std::to_string(size) + "&chunk_size=" + std::to_string(chunk_size) +
           "&chunks=" + std::to_string(chunks);
}

std::string FinalizeTarget(const Endpoint& endpoint, const std::string& sha, std::uint64_t size,
                           std::uint64_t chunk_size, std::uint32_t chunks) {
    return endpoint.base + "/finalize?sha256=" + sha + "&size=" + std::to_string(size) +
           "&chunk_size=" + std::to_string(chunk_size) + "&chunks=" + std::to_string(chunks);
}

/// 带指数退避的执行器：把「重试同一请求」收在一处，避免三处循环重复。
bool RunWithRetry(const Config& config, const Endpoint& endpoint, http::Method method,
                  const std::string& target, const std::string& body, const HeaderList& headers,
                  const char* what, std::string* response_body, int* retries_used,
                  std::string* error) {
    int attempt = 0;
    for (;;) {
        const Reply reply = Perform(endpoint, method, target, body, headers, config.timeout_ms);
        if (reply.ok()) {
            if (response_body != nullptr) *response_body = reply.body;
            return true;
        }
        const std::string reason =
            reply.transport_ok ? ("HTTP " + std::to_string(reply.status) + " " + Trim(reply.body))
                               : reply.error;
        if (!reply.retryable() || attempt >= config.retries) {
            *error = std::string(what) + "失败：" + reason;
            return false;
        }
        const int wait_ms = BackoffMs(config.retry_backoff_ms, attempt);
        ++attempt;
        if (retries_used != nullptr) *retries_used += 1;
        Log(config, "%s 第 %d/%d 次重试：%s（等待 %d ms）", what, attempt, config.retries,
            reason.c_str(), wait_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

}  // namespace

Uploader::Uploader(Config config) : m_config(std::move(config)) {}

Result Uploader::UploadFile(const std::string& path, const ProgressFn& progress) {
    Result result;
    const auto started = SteadyClock::now();
    const auto finish = [&result, started] {
        result.seconds = std::chrono::duration<double>(SteadyClock::now() - started).count();
        return result;
    };

    // step1: 校验配置并打开文件；文件大小是后续分片表的唯一依据。
    if (m_config.chunk_size < kMinChunkSize || m_config.chunk_size > kMaxChunkSize) {
        result.error = "chunk_size 必须在 1 KiB..1 GiB 之间";
        return finish();
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "无法打开文件：" + path;
        return finish();
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        result.error = "无法读取文件大小：" + path;
        return finish();
    }
    input.seekg(0, std::ios::beg);
    result.size = static_cast<std::uint64_t>(end);
    result.chunks = result.size == 0
                        ? 0
                        : static_cast<std::uint32_t>((result.size + m_config.chunk_size - 1) /
                                                     m_config.chunk_size);

    // step2: 流式算全文件 SHA-256（内容身份，也是断点续传的寻址键）。
    {
        hash::Sha256 whole;
        std::vector<char> buffer(1U << 20);
        std::uint64_t remaining = result.size;
        while (remaining > 0) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(want));
            const auto got = input.gcount();
            if (got <= 0) {
                result.error = "读取文件失败（扫描阶段）";
                return finish();
            }
            whole.Write(buffer.data(), static_cast<std::size_t>(got));
            remaining -= static_cast<std::uint64_t>(got);
        }
        const auto digest = whole.Sum();
        result.sha256 = Hex(digest.data(), digest.size());
    }
    input.clear();  // 清掉 EOF 标志，后续 seekg 才能工作。

    // step3: 解析 endpoint 并取服务端已收分片清单。
    Endpoint endpoint;
    std::string error;
    if (!ParseEndpoint(m_config.endpoint, &endpoint, &error)) {
        result.error = error;
        return finish();
    }
    Manifest manifest;
    manifest.chunk_size = m_config.chunk_size;
    manifest.size = result.size;
    manifest.chunks = result.chunks;
    {
        std::string body;
        const std::string target = ManifestTarget(endpoint, result.sha256, result.size,
                                                  m_config.chunk_size, result.chunks);
        int retries = 0;
        if (!RunWithRetry(m_config, endpoint, http::Method::Get, target, {}, {}, "获取清单", &body,
                          &retries, &error)) {
            result.error = error;
            result.retries_used += retries;
            return finish();
        }
        result.retries_used += retries;
        if (!ParseManifest(body, &manifest, &error)) {
            result.error = error;
            return finish();
        }
    }
    if (manifest.chunk_size != m_config.chunk_size || manifest.size != result.size ||
        manifest.chunks != result.chunks) {
        result.error = "服务端清单与本地不一致（分片大小/文件大小/分片数）；请清空服务端存储后重试";
        return finish();
    }

    std::vector<bool> received(result.chunks, false);
    std::uint64_t done = 0;
    for (const auto index : manifest.received) {
        if (!received[index]) {
            received[index] = true;
            ++result.resumed;
            done += ChunkBytes(result.size, m_config.chunk_size, index);
        }
    }
    if (result.resumed > 0) {
        Log(m_config, "服务端已收 %u/%u 分片（%.2f MiB 已就绪），跳过这些分片继续",
            static_cast<unsigned>(result.resumed), static_cast<unsigned>(result.chunks),
            static_cast<double>(done) / (1U << 20));
    }
    if (progress) progress(done, result.size);

    // step4: 逐片流式读取 + 分片 SHA-256 + PUT；每片独立重试。
    std::vector<char> chunk(static_cast<std::size_t>(m_config.chunk_size));
    for (std::uint32_t index = 0; index < result.chunks; ++index) {
        if (received[index]) continue;
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * m_config.chunk_size;
        const std::uint64_t bytes = ChunkBytes(result.size, m_config.chunk_size, index);
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input.read(chunk.data(), static_cast<std::streamsize>(bytes));
        if (static_cast<std::uint64_t>(input.gcount()) != bytes) {
            result.error = "读取分片失败：index=" + std::to_string(index);
            return finish();
        }
        HeaderList headers;
        if (m_config.sha256) {
            hash::Sha256 chunk_hash;
            chunk_hash.Write(chunk.data(), static_cast<std::size_t>(bytes));
            const auto digest = chunk_hash.Sum();
            headers.emplace_back("X-Chunk-SHA256", Hex(digest.data(), digest.size()));
        }
        const std::string target = UploadTarget(endpoint, result.sha256, result.size,
                                                m_config.chunk_size, result.chunks, index);
        const std::string body(chunk.data(), static_cast<std::size_t>(bytes));
        std::string ignored;
        int retries = 0;
        if (!RunWithRetry(m_config, endpoint, http::Method::Put, target, body, headers, "上传分片",
                          &ignored, &retries, &error)) {
            result.error = error + "（index=" + std::to_string(index) + "，已上传 " +
                           std::to_string(result.uploaded) + " 片，可重跑续传）";
            result.retries_used += retries;
            return finish();
        }
        result.retries_used += retries;
        ++result.uploaded;
        done += bytes;
        if (progress) progress(done, result.size);
    }

    // step5: 整体清单校验：服务端按收到的分片重算整文件 SHA-256，与本地比对。
    {
        const std::string target = FinalizeTarget(endpoint, result.sha256, result.size,
                                                  m_config.chunk_size, result.chunks);
        std::string body;
        int retries = 0;
        if (!RunWithRetry(m_config, endpoint, http::Method::Post, target, {}, {}, "整体校验", &body,
                          &retries, &error)) {
            result.error = error;
            result.retries_used += retries;
            return finish();
        }
        result.retries_used += retries;
        if (m_config.sha256) {
            const std::string reply = Trim(body);
            const auto space = reply.find(' ');
            std::string server_sha =
                space == std::string::npos ? std::string() : Trim(reply.substr(space + 1));
            const auto second = server_sha.find(' ');
            if (second != std::string::npos) server_sha.resize(second);
            if (reply.compare(0, 2, "OK") != 0 || server_sha != result.sha256) {
                result.error = "整体 SHA-256 校验失败：本地 " + result.sha256 + "，服务端 " + reply;
                return finish();
            }
        }
    }
    result.ok = true;
    if (progress) progress(result.size, result.size);
    return finish();
}

bool ParseSize(const std::string& text, std::uint64_t* bytes) {
    if (text.empty() || bytes == nullptr) return false;
    std::string digits;
    std::uint64_t multiplier = 1;
    std::size_t pos = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        digits.push_back(text[pos]);
        ++pos;
    }
    if (digits.empty()) return false;
    std::string suffix;
    while (pos < text.size()) {
        suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos]))));
        ++pos;
    }
    if (!suffix.empty() && suffix != "b") {
        if (suffix == "k" || suffix == "kb" || suffix == "kib") {
            multiplier = 1ULL << 10;
        } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
            multiplier = 1ULL << 20;
        } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
            multiplier = 1ULL << 30;
        } else {
            return false;
        }
    }
    std::uint64_t base = 0;
    if (!ParseU64(digits, &base)) return false;
    if (base == 0 || base > (UINT64_MAX / multiplier)) return false;
    *bytes = base * multiplier;
    return true;
}

}  // namespace cloud
