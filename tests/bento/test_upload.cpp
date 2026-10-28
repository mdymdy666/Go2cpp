/**
 * @file test_upload.cpp
 * @brief MOD-31 Upload：进程内 HTTP test server 验证 PUT/POST、token/header、
 *        重试后成功、重试耗尽保留本地包、连接失败、参数校验。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/app/backup.hpp"
#include "go2cpp/bento/app/zip.hpp"
#include "go2cpp/hash/sha256.hpp"
#include "test_backup_support.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using go2cpp::bento::app::HttpMethod;
using go2cpp::bento::app::RemoteVerifyMode;
using go2cpp::bento::app::Upload;
using go2cpp::bento::app::UploadOptions;
using go2cpp::bento::app::UploadRequest;
using go2cpp::bento::app::UploadStatus;
using go2cpp::bento::app::ZipStatus;
using go2cpp::bento::app::ZipWriter;

using namespace std::chrono_literals;

/// 大小写不敏感比较（测试内部使用）。
bool EqualIgnoreCase(const std::string& first, const std::string& second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (std::size_t i = 0; i < first.size(); ++i) {
        const auto a = static_cast<unsigned char>(first[i]);
        const auto b = static_cast<unsigned char>(second[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::string LowerCopy(const std::string& text) {
    std::string copy = text;
    for (char& ch : copy) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return copy;
}

std::string TrimCopy(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

struct HttpRequest final {
    std::string method;
    std::string path;
    std::string version;
    std::vector<std::pair<std::string, std::string>> header_list;
    std::map<std::string, std::string> headers; // 小写 key
    std::string body;
};

struct HttpResponse final {
    int status{200};
    std::string body;
    /// 额外响应头；未显式给 Content-Length 时按 body 自动补。
    std::vector<std::pair<std::string, std::string>> headers;

    HttpResponse() = default;
    HttpResponse(int status_code, std::string response_body)
        : status(status_code), body(std::move(response_body)) {}
    HttpResponse(int status_code, std::string response_body,
                 std::vector<std::pair<std::string, std::string>> response_headers)
        : status(status_code),
          body(std::move(response_body)),
          headers(std::move(response_headers)) {}
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;

std::string HeaderValue(const HttpRequest& request, const std::string& name) {
    for (const auto& entry : request.header_list) {
        if (EqualIgnoreCase(entry.first, name)) {
            return entry.second;
        }
    }
    return {};
}

std::size_t HeaderCount(const HttpRequest& request, const std::string& name) {
    std::size_t count = 0;
    for (const auto& entry : request.header_list) {
        if (EqualIgnoreCase(entry.first, name)) {
            ++count;
        }
    }
    return count;
}

bool SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 极简 HTTP/1.1 服务端：一次连接一个请求，支持 Expect: 100-continue。
class TestHttpServer final {
public:
    explicit TestHttpServer(Handler handler) : m_handler(std::move(handler)) {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        if (::listen(m_listen_fd, 16) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { Serve(); });
    }

    ~TestHttpServer() {
        m_stop = true;
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_listen_fd >= 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
    }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    bool valid() const noexcept { return m_port > 0; }
    int port() const noexcept { return m_port; }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(m_port) + path;
    }
    std::vector<HttpRequest> requests() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests;
    }

private:
    void Serve() {
        while (!m_stop) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (fd < 0) {
                if (m_stop) {
                    break;
                }
                continue;
            }
            Handle(fd);
            ::close(fd);
        }
    }

    void Handle(int fd) {
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string buffer;
        char chunk[4096];
        std::size_t marker = std::string::npos;
        while (buffer.size() < 64 * 1024) {
            marker = buffer.find("\r\n\r\n");
            if (marker != std::string::npos) {
                break;
            }
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
        }
        if (marker == std::string::npos) {
            return;
        }
        const std::string header_block = buffer.substr(0, marker);
        HttpRequest request;
        const std::size_t line_end = header_block.find("\r\n");
        std::istringstream request_line(header_block.substr(0, line_end));
        request_line >> request.method >> request.path >> request.version;
        std::size_t position = line_end == std::string::npos ? header_block.size() : line_end + 2;
        while (position < header_block.size()) {
            const std::size_t end = header_block.find("\r\n", position);
            const std::string line = header_block.substr(
                position, end == std::string::npos ? std::string::npos : end - position);
            if (end == std::string::npos) {
                position = header_block.size();
            } else {
                position = end + 2;
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string name = TrimCopy(line.substr(0, colon));
            const std::string value = TrimCopy(line.substr(colon + 1));
            request.header_list.emplace_back(name, value);
            request.headers[LowerCopy(name)] = value;
        }
        const auto expect = request.headers.find("expect");
        if (expect != request.headers.end() && EqualIgnoreCase(expect->second, "100-continue")) {
            if (!SendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n")) {
                return;
            }
        }
        std::size_t content_length = 0;
        const auto length = request.headers.find("content-length");
        if (length != request.headers.end()) {
            try {
                content_length = static_cast<std::size_t>(std::stoul(length->second));
            } catch (...) {
                content_length = 0;
            }
        }
        request.body = buffer.substr(marker + 4);
        while (request.body.size() < content_length) {
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                break;
            }
            request.body.append(chunk, static_cast<std::size_t>(got));
        }
        if (request.body.size() > content_length) {
            request.body.resize(content_length);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back(request);
        }
        const HttpResponse response = m_handler ? m_handler(request) : HttpResponse{200, {}};
        const std::string body = response.body;
        std::string reply = "HTTP/1.1 " + std::to_string(response.status) + " X\r\n";
        bool has_content_length = false;
        for (const auto& header : response.headers) {
            if (EqualIgnoreCase(header.first, "Content-Length")) {
                has_content_length = true;
            }
            reply += header.first + ": " + header.second + "\r\n";
        }
        if (!has_content_length) {
            reply += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        reply += "Connection: close\r\n\r\n";
        // HEAD 只回头部：Content-Length 仍表示资源的完整大小。
        if (!EqualIgnoreCase(request.method, "HEAD")) {
            reply += body;
        }
        (void)SendAll(fd, reply);
    }

    Handler m_handler;
    int m_listen_fd{-1};
    int m_port{0};
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::vector<HttpRequest> m_requests;
};

/// 生成一个 store-only zip（含一个随机文件）。
fs::path MakeZip(const fs::path& tmp, std::size_t payload_bytes) {
    const auto zip = tmp / "payload.zip";
    const auto data = bento_test::RandomBytes(payload_bytes, 0x51505150);
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddBytes("payload.bin", data.data(), data.size()) == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    return zip;
}

/// 生成一个合法的空 zip（仅 EOCD）。
fs::path MakeEmptyZip(const fs::path& tmp) {
    const auto zip = tmp / "empty.zip";
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    return zip;
}

/// PUT 成功：方法/路径/header/token/body 全部核对。
void TestUploadPutSuccess() {
    bento_test::TempDir tmp("upload_put");
    const auto zip = MakeZip(tmp.path(), 512 * 1024);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{201, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/put");
    request.method = HttpMethod::kPut;
    request.headers = {"X-Backup-Id: 42"};
    request.token = "secret-token";
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.status, UploadStatus::kOk);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK_EQ(result.http_status, 201);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(requests[0].method, std::string("PUT"));
        BENTO_CHECK_EQ(requests[0].path, std::string("/backup/put"));
        BENTO_CHECK_EQ(HeaderValue(requests[0], "Authorization"),
                       std::string("Bearer secret-token"));
        BENTO_CHECK_EQ(HeaderValue(requests[0], "X-Backup-Id"), std::string("42"));
        BENTO_CHECK_EQ(HeaderValue(requests[0], "Content-Type"),
                       std::string("application/octet-stream"));
        BENTO_CHECK_EQ(requests[0].body.size(), fs::file_size(zip));
        BENTO_CHECK(std::vector<std::uint8_t>(requests[0].body.begin(), requests[0].body.end()) ==
                    bento_test::ReadFile(zip));
    }
}

/// POST 成功。
void TestUploadPostSuccess() {
    bento_test::TempDir tmp("upload_post");
    const auto zip = MakeZip(tmp.path(), 64 * 1024);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{200, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/upload");
    request.method = HttpMethod::kPost;
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.http_status, 200);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(requests[0].method, std::string("POST"));
        BENTO_CHECK_EQ(requests[0].body.size(), fs::file_size(zip));
    }
}

/// 前两次 503、第三次 201：指数退避重试成功。
void TestUploadRetryThenSuccess() {
    bento_test::TempDir tmp("upload_retry_ok");
    const auto zip = MakeZip(tmp.path(), 32 * 1024);
    std::atomic<int> calls{0};
    TestHttpServer server([&calls](const HttpRequest&) {
        const int attempt = ++calls;
        if (attempt <= 2) {
            return HttpResponse{503, {}};
        }
        return HttpResponse{201, {}};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/retry");
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 3;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.attempts, 3);
    BENTO_CHECK_EQ(result.http_status, 201);
    BENTO_CHECK_EQ(server.requests().size(), 3U);
    BENTO_CHECK_EQ(calls.load(), 3);
}

/// 始终 500：4 次尝试后失败，本地包必须保留。
void TestUploadRetryExhaustedKeepsZip() {
    bento_test::TempDir tmp("upload_retry_fail");
    const auto zip = MakeZip(tmp.path(), 16 * 1024);
    const auto before = bento_test::ReadFile(zip);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{500, "error"}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/fail");
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 3;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(!result.ok());
    BENTO_CHECK_EQ(result.status, UploadStatus::kHttpRejected);
    BENTO_CHECK_EQ(result.attempts, 4);
    BENTO_CHECK_EQ(result.http_status, 500);
    BENTO_CHECK(!result.error.empty());
    BENTO_CHECK_EQ(server.requests().size(), 4U);
    BENTO_CHECK(fs::exists(zip));
    BENTO_CHECK(bento_test::ReadFile(zip) == before);
}

/// 连接被拒绝：重试后失败，本地包保留。
void TestUploadConnectionRefusedKeepsZip() {
    bento_test::TempDir tmp("upload_refused");
    const auto zip = MakeZip(tmp.path(), 8 * 1024);
    const auto before = bento_test::ReadFile(zip);
    UploadRequest request;
    request.endpoint = "http://127.0.0.1:1/upload";
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.max_retries = 2;
    options.retry_delay = 1ms;
    options.connect_timeout = 2s;
    options.request_timeout = 3s;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(!result.ok());
    BENTO_CHECK_EQ(result.status, UploadStatus::kNetworkError);
    BENTO_CHECK_EQ(result.attempts, 3);
    BENTO_CHECK(bento_test::ReadFile(zip) == before);
}

/// 本地 zip 不存在：不尝试请求，不删除任何东西。
void TestUploadMissingZip() {
    bento_test::TempDir tmp("upload_missing");
    UploadRequest request;
    request.endpoint = "http://127.0.0.1:1/upload";
    const auto result = Upload((tmp / "nope.zip").string(), request);
    BENTO_CHECK_EQ(result.status, UploadStatus::kZipMissing);
    BENTO_CHECK_EQ(result.attempts, 0);
}

/// 参数校验：非法 endpoint/header/token/重试配置。
void TestUploadInvalidArguments() {
    bento_test::TempDir tmp("upload_invalid");
    const auto zip = MakeZip(tmp.path(), 1024);
    const auto check_invalid = [&zip](UploadRequest request, UploadOptions options) {
        const auto result = Upload(zip.string(), request, options);
        BENTO_CHECK_EQ(result.status, UploadStatus::kInvalidArgument);
        BENTO_CHECK_EQ(result.attempts, 0);
    };
    UploadRequest request;
    request.endpoint = "ftp://example.com/upload";
    check_invalid(request, UploadOptions{});
    request.endpoint = "http://example.com/a b";
    check_invalid(request, UploadOptions{});
    request.endpoint = "http://example.com/upload";
    request.headers = {"Bad\nHeader: value"};
    check_invalid(request, UploadOptions{});
    request.headers = {"NoColon"};
    check_invalid(request, UploadOptions{});
    request.headers.clear();
    request.token = "a\r\nb";
    check_invalid(request, UploadOptions{});
    request.token.clear();
    UploadOptions retries;
    retries.max_retries = 17;
    check_invalid(request, retries);
    UploadOptions delay;
    delay.retry_delay = -1ms;
    check_invalid(request, delay);
    UploadOptions timeout;
    timeout.request_timeout = 0ms;
    check_invalid(request, timeout);
}

/// 已有 Authorization header 时 token 不重复添加。
void TestUploadTokenNotDuplicated() {
    bento_test::TempDir tmp("upload_token");
    const auto zip = MakeZip(tmp.path(), 1024);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{201, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/token");
    request.method = HttpMethod::kPut;
    request.headers = {"Authorization: Token abc"};
    request.token = "Bearer xyz";
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(HeaderCount(requests[0], "Authorization"), 1U);
        BENTO_CHECK_EQ(HeaderValue(requests[0], "Authorization"), std::string("Token abc"));
    }
}

/// 自定义 Content-Type 优先于默认值。
void TestUploadContentTypeOverride() {
    bento_test::TempDir tmp("upload_content_type");
    const auto zip = MakeZip(tmp.path(), 1024);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{204, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/content-type");
    request.method = HttpMethod::kPost;
    request.headers = {"Content-Type: application/x-backup"};
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.http_status, 204);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(HeaderCount(requests[0], "Content-Type"), 1U);
        BENTO_CHECK_EQ(HeaderValue(requests[0], "Content-Type"),
                       std::string("application/x-backup"));
    }
}

/// 空归档（仅 EOCD）也能上传。
void TestUploadEmptyArchive() {
    bento_test::TempDir tmp("upload_empty");
    const auto zip = MakeEmptyZip(tmp.path());
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{200, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/empty");
    request.method = HttpMethod::kPut;
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(requests[0].body.size(), fs::file_size(zip));
    }
}

/// SHA-256 小写十六进制（测试内比对响应头/响应体）。
std::string Sha256Hex(const std::vector<std::uint8_t>& data) {
    const std::array<std::uint8_t, go2cpp::hash::Sha256::kSize> digest =
        go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(data));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0fu]);
    }
    return text;
}

/// FR-18：上传成功后 GET 回读，响应体与 SHA-256 头一致 -> kOk。
void TestUploadRemoteVerifyGetSuccess() {
    bento_test::TempDir tmp("upload_verify_get_ok");
    const auto zip = MakeZip(tmp.path(), 128 * 1024);
    const auto zip_bytes = bento_test::ReadFile(zip);
    const std::string zip_sha = Sha256Hex(zip_bytes);
    TestHttpServer server([&zip_bytes, &zip_sha](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        if (request.method == "GET") {
            return HttpResponse{200, std::string(zip_bytes.begin(), zip_bytes.end()),
                                {{"X-Content-SHA256", zip_sha}}};
        }
        return HttpResponse{405, {}};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/verify");
    request.method = HttpMethod::kPut;
    request.token = "verify-token";
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.status, UploadStatus::kOk);
    BENTO_CHECK(result.remote_verified);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK_EQ(result.http_status, 201);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 2U);
    if (requests.size() == 2) {
        BENTO_CHECK_EQ(requests[0].method, std::string("PUT"));
        BENTO_CHECK_EQ(requests[1].method, std::string("GET"));
        BENTO_CHECK_EQ(HeaderValue(requests[1], "Authorization"),
                       std::string("Bearer verify-token"));
    }
}

/// FR-18：verify_remote 默认 false，不产生额外回读请求（保持旧行为）。
void TestUploadRemoteVerifyDisabledByDefault() {
    bento_test::TempDir tmp("upload_verify_off");
    const auto zip = MakeZip(tmp.path(), 4096);
    TestHttpServer server([](const HttpRequest&) { return HttpResponse{201, {}}; });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/upload/off");
    UploadOptions options;
    options.retry_delay = 1ms;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK(!result.remote_verified);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 1U);
    if (requests.size() == 1) {
        BENTO_CHECK_EQ(requests[0].method, std::string("PUT"));
    }
}

/// FR-18：GET 响应体大小相同但内容不同（无哈希头）-> kRemoteVerifyFailed。
void TestUploadRemoteVerifyBodyHashMismatch() {
    bento_test::TempDir tmp("upload_verify_hash");
    const auto zip = MakeZip(tmp.path(), 64 * 1024);
    const auto zip_bytes = bento_test::ReadFile(zip);
    std::vector<std::uint8_t> corrupted = zip_bytes;
    corrupted[corrupted.size() / 2] ^= 0xFFu;
    const auto before = bento_test::ReadFile(zip);
    TestHttpServer server([&corrupted](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        return HttpResponse{200, std::string(corrupted.begin(), corrupted.end())};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/hash-mismatch");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(!result.ok());
    BENTO_CHECK_EQ(result.status, UploadStatus::kRemoteVerifyFailed);
    BENTO_CHECK(!result.remote_verified);
    BENTO_CHECK(!result.error.empty());
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK(bento_test::ReadFile(zip) == before);
}

/// FR-18：响应头 SHA-256 与本地不一致 -> kRemoteVerifyFailed（头优先）。
void TestUploadRemoteVerifyChecksumHeaderMismatch() {
    bento_test::TempDir tmp("upload_verify_header");
    const auto zip = MakeZip(tmp.path(), 32 * 1024);
    const auto zip_bytes = bento_test::ReadFile(zip);
    const std::string wrong_sha(64, '0');
    TestHttpServer server([&zip_bytes, &wrong_sha](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        // 头名用小写，验证实现是大小写不敏感的。
        return HttpResponse{200, std::string(zip_bytes.begin(), zip_bytes.end()),
                            {{"x-content-sha256", wrong_sha}}};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/header-mismatch");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK_EQ(result.status, UploadStatus::kRemoteVerifyFailed);
    BENTO_CHECK(!result.remote_verified);
}

/// FR-18：HEAD 的 Content-Length 与本地 zip 大小一致 -> kOk。
void TestUploadRemoteVerifyHeadSuccess() {
    bento_test::TempDir tmp("upload_verify_head_ok");
    const auto zip = MakeZip(tmp.path(), 16 * 1024);
    const std::uintmax_t zip_size = fs::file_size(zip);
    TestHttpServer server([&zip_size](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        if (request.method == "HEAD") {
            return HttpResponse{200, {}, {{"Content-Length", std::to_string(zip_size)}}};
        }
        return HttpResponse{405, {}};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/head-ok");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kHead;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK(result.ok());
    BENTO_CHECK(result.remote_verified);
    const auto requests = server.requests();
    BENTO_CHECK_EQ(requests.size(), 2U);
    if (requests.size() == 2) {
        BENTO_CHECK_EQ(requests[1].method, std::string("HEAD"));
    }
}

/// FR-18：HEAD 的 Content-Length 与本地不一致 -> kRemoteVerifyFailed。
void TestUploadRemoteVerifyHeadSizeMismatch() {
    bento_test::TempDir tmp("upload_verify_head_bad");
    const auto zip = MakeZip(tmp.path(), 8 * 1024);
    const std::uintmax_t wrong_size = fs::file_size(zip) + 1;
    TestHttpServer server([&wrong_size](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        if (request.method == "HEAD") {
            return HttpResponse{200, {}, {{"Content-Length", std::to_string(wrong_size)}}};
        }
        return HttpResponse{405, {}};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/head-bad");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kHead;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK_EQ(result.status, UploadStatus::kRemoteVerifyFailed);
    BENTO_CHECK(!result.remote_verified);
}

/// FR-18：GET 回读的响应体被截断 -> kRemoteVerifyFailed（大小校验）。
void TestUploadRemoteVerifyBodySizeMismatch() {
    bento_test::TempDir tmp("upload_verify_size");
    const auto zip = MakeZip(tmp.path(), 48 * 1024);
    const auto zip_bytes = bento_test::ReadFile(zip);
    std::vector<std::uint8_t> truncated(zip_bytes.begin(),
                                        zip_bytes.begin() + zip_bytes.size() / 2);
    TestHttpServer server([&truncated](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        return HttpResponse{200, std::string(truncated.begin(), truncated.end())};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/size-mismatch");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK_EQ(result.status, UploadStatus::kRemoteVerifyFailed);
    BENTO_CHECK(!result.remote_verified);
}

/// FR-18：回读请求返回 404 -> kRemoteVerifyFailed（上传本身已成功）。
void TestUploadRemoteVerifyHttpError() {
    bento_test::TempDir tmp("upload_verify_404");
    const auto zip = MakeZip(tmp.path(), 2048);
    TestHttpServer server([](const HttpRequest& request) {
        if (request.method == "PUT") {
            return HttpResponse{201, {}};
        }
        return HttpResponse{404, "not found"};
    });
    BENTO_CHECK(server.valid());
    UploadRequest request;
    request.endpoint = server.url("/backup/404");
    UploadOptions options;
    options.retry_delay = 1ms;
    options.verify_remote = true;
    options.verify_mode = RemoteVerifyMode::kGet;
    const auto result = Upload(zip.string(), request, options);
    BENTO_CHECK_EQ(result.status, UploadStatus::kRemoteVerifyFailed);
    BENTO_CHECK(!result.remote_verified);
    BENTO_CHECK_EQ(result.attempts, 1);
}

} // namespace

int main() {
    TestUploadPutSuccess();
    TestUploadPostSuccess();
    TestUploadRetryThenSuccess();
    TestUploadRetryExhaustedKeepsZip();
    TestUploadConnectionRefusedKeepsZip();
    TestUploadMissingZip();
    TestUploadInvalidArguments();
    TestUploadTokenNotDuplicated();
    TestUploadContentTypeOverride();
    TestUploadEmptyArchive();
    TestUploadRemoteVerifyGetSuccess();
    TestUploadRemoteVerifyDisabledByDefault();
    TestUploadRemoteVerifyBodyHashMismatch();
    TestUploadRemoteVerifyChecksumHeaderMismatch();
    TestUploadRemoteVerifyHeadSuccess();
    TestUploadRemoteVerifyHeadSizeMismatch();
    TestUploadRemoteVerifyBodySizeMismatch();
    TestUploadRemoteVerifyHttpError();
    return bento_test::Finish("test_upload");
}
