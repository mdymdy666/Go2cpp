/**
 * @file test_fetch.cpp
 * @brief MOD-27 下载模块测试：本地 HTTP server（测试内 socket 实现）驱动
 *        1MB 下载、SHA-256、404、超时、重试、大小上限、路径逃逸、重定向、
 *        chunked、截断与 https/curl 分支。
 * @details 覆盖 16 个场景：
 *          1. 参数与摘要格式校验；
 *          2. 目标路径沙箱（拒绝 `..`、绝对路径、分隔符、超长）；
 *          3. 1MB 下载 + SHA-256 一致 + 内容逐字节比对；
 *          4. 404 不重试；
 *          5. 读超时（max_retries=0）；
 *          6. 503 两次后成功（attempts=3）；
 *          7. 持续 503 用尽重试（kRetryExhausted）；
 *          8. 文件名逃逸拒绝且不留文件；
 *          9. 大小上限（Content-Length 预检）；
 *          10. chunked 下载；
 *          11. Content-Length 大于实际内容 → kTruncated；
 *          12. 302 重定向跟随；
 *          13. 重定向次数上限；
 *          14. SHA-256 不符 → kChecksumMismatch；
 *          15. 目标已存在：默认拒绝、overwrite 覆盖（且不重复发请求）；
 *          16. https 走系统 curl：对普通 HTTP server 握手失败；
 *          17. 无 Content-Length（EOF 模式）与不支持的 Content-Encoding。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/bento/app/fetch.hpp"
#include "go2cpp/hash/sha256.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;

using go2cpp::bento::app::Fetch;
using go2cpp::bento::app::FetchOptions;
using go2cpp::bento::app::FetchResult;
using go2cpp::bento::app::FetchStatus;
using go2cpp::bento::app::ParseSha256Hex;
using go2cpp::bento::app::ResolveFetchPath;

// ---------------------------------------------------------------------------
// 测试数据与文件辅助
// ---------------------------------------------------------------------------

/// 生成确定性的伪随机数据（同一 seed 结果恒定）。
std::vector<std::uint8_t> MakeData(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = seed | 1u;
    for (std::uint8_t& byte : data) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

/// 摘要转 64 位小写十六进制。
std::string Hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

/// 计算一段数据的 SHA-256 十六进制。
std::string Sha256Hex(const std::vector<std::uint8_t>& data) {
    return Hex(go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(data)));
}

/// 创建唯一临时目录；失败返回空路径。
fs::path MakeTempDir(const char* tag) {
    const fs::path base = fs::temp_directory_path();
    for (int index = 0; index < 100; ++index) {
        const fs::path dir =
            base / (std::string("bento_fetch_") + tag + "_" + std::to_string(::getpid()) + "_" +
                    std::to_string(index));
        std::error_code ec;
        if (fs::create_directories(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

/// 递归删除。
void RemoveTree(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

/// 读整个文件；失败返回 false。
bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>* data) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

/// 统计目录内的条目数（含隐藏文件，不含 . 与 ..）。
std::size_t CountEntries(const fs::path& dir) {
    std::error_code ec;
    std::size_t count = 0;
    fs::directory_iterator iterator(dir, ec);
    const fs::directory_iterator end;
    while (iterator != end && !ec) {
        ++count;
        iterator.increment(ec);
    }
    return count;
}

// ---------------------------------------------------------------------------
// 测试用 HTTP 服务器
// ---------------------------------------------------------------------------

/**
 * @brief 单线程 HTTP/1.1 测试服务器。
 *
 * @details 路由固定，覆盖正常/错误/慢响应/chunked/截断/重定向等路径。
 *          为避免测试进程被 SIGPIPE 杀死，所有 send 都带 MSG_NOSIGNAL。
 *          收到非 HTTP 首字节（例如 https 的 TLS ClientHello）时立即回 400，
 *          让 curl 尽快以 TLS 错误退出，避免双方互等。
 */
class TestHttpServer final {
public:
    TestHttpServer() = default;
    ~TestHttpServer() { Stop(); }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    /// 绑定 127.0.0.1:0 并启动接受线程。
    bool Start() {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listen_fd, 16) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        m_port = ntohs(address.sin_port);
        m_running.store(true);
        m_thread = std::thread([this]() { Serve(); });
        return true;
    }

    /// 停止接受新连接并等待线程退出。
    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    /// `http://127.0.0.1:<port>`。
    std::string BaseUrl() const {
        return "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(m_port));
    }

    /// 已处理的请求总数。
    int requests() const { return m_requests.load(); }

    /// `/flaky` 被请求的次数。
    int flaky_requests() const { return m_flaky.load(); }

private:
    void Serve() {
        while (m_running.load()) {
            const int client = ::accept(m_listen_fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            HandleClient(client);
            ::close(client);
        }
    }

    static void SendAll(int fd, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t result =
                ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (result > 0) {
                sent += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }

    static void SendHead(int fd, int status, std::string_view reason, std::uint64_t length,
                         std::string_view extra_headers = {}) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                           "\r\nContent-Length: " + std::to_string(length) + "\r\n";
        head += extra_headers;
        head += "Connection: close\r\n\r\n";
        SendAll(fd, head);
    }

    /// 故意保持连接不关闭，模拟 keep-alive：无响应体的状态码不应等 EOF。
    void HoldConnection() const {
        for (int waited = 0; waited < 600 && m_running.load(); waited += 25) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    static void SendBody(int fd, const std::vector<std::uint8_t>& body) {
        if (!body.empty()) {
            SendAll(fd, std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
        }
    }

    void HandleClient(int fd) {
        m_requests.fetch_add(1);
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(got));
            // TLS ClientHello 以 0x16 开头；直接回 400，避免和 curl 互等。
            if (request.front() != 'G') {
                SendHead(fd, 400, "Bad Request", 0);
                return;
            }
        }
        const std::size_t first_space = request.find(' ');
        if (first_space == std::string::npos) {
            SendHead(fd, 400, "Bad Request", 0);
            return;
        }
        const std::size_t second_space = request.find(' ', first_space + 1);
        if (second_space == std::string::npos) {
            SendHead(fd, 400, "Bad Request", 0);
            return;
        }
        std::string path = request.substr(first_space + 1, second_space - first_space - 1);
        if (const std::size_t query = path.find('?'); query != std::string::npos) {
            path.resize(query);
        }

        if (path == "/small") {
            const std::vector<std::uint8_t> body = {'h', 'e', 'l', 'l', 'o', '-',
                                                    'f', 'e', 't', 'c', 'h'};
            SendHead(fd, 200, "OK", body.size());
            SendBody(fd, body);
        } else if (path == "/no-content") {
            SendAll(fd, "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n");
            HoldConnection();
        } else if (path == "/reset-content") {
            SendAll(fd, "HTTP/1.1 205 Reset Content\r\nConnection: keep-alive\r\n\r\n");
            HoldConnection();
        } else if (path == "/not-modified") {
            SendAll(fd, "HTTP/1.1 304 Not Modified\r\nConnection: keep-alive\r\n\r\n");
            HoldConnection();
        } else if (path == "/empty") {
            SendHead(fd, 200, "OK", 0);
        } else if (path == "/file-1mb") {
            SendHead(fd, 200, "OK", m_one_megabyte.size());
            SendBody(fd, m_one_megabyte);
        } else if (path == "/big") {
            SendHead(fd, 200, "OK", m_two_megabytes.size());
            SendBody(fd, m_two_megabytes);
        } else if (path == "/chunked-big") {
            SendAll(fd,
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
            constexpr std::size_t kChunkBytes = 64 * 1024;
            std::size_t offset = 0;
            while (offset < m_two_megabytes.size()) {
                const std::size_t size =
                    std::min(kChunkBytes, m_two_megabytes.size() - offset);
                char size_line[32] = {};
                std::snprintf(size_line, sizeof(size_line), "%zx\r\n", size);
                SendAll(fd, size_line);
                SendAll(fd, std::string_view(
                                reinterpret_cast<const char*>(m_two_megabytes.data() + offset),
                                size));
                SendAll(fd, "\r\n");
                offset += size;
            }
            SendAll(fd, "0\r\n\r\n");
        } else if (path == "/chunked") {
            const std::string body = "chunked-body-0123456789";
            SendAll(fd,
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
            std::size_t offset = 0;
            const std::size_t chunks[] = {6, 7, body.size() - 13};
            for (const std::size_t size : chunks) {
                char size_line[32] = {};
                std::snprintf(size_line, sizeof(size_line), "%zx\r\n", size);
                SendAll(fd, size_line);
                SendAll(fd, std::string_view(body).substr(offset, size));
                SendAll(fd, "\r\n");
                offset += size;
            }
            SendAll(fd, "0\r\n\r\n");
        } else if (path == "/no-length") {
            SendAll(fd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
            SendAll(fd, "eof-body");
        } else if (path == "/no-length-big") {
            SendAll(fd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
            SendBody(fd, m_two_megabytes);
        } else if (path == "/truncated") {
            SendHead(fd, 200, "OK", 1000);
            SendAll(fd, "0123456789");
        } else if (path == "/missing") {
            const std::vector<std::uint8_t> body = {'n', 'o', 't', '-', 'f', 'o', 'u', 'n', 'd'};
            SendHead(fd, 404, "Not Found", body.size());
            SendBody(fd, body);
        } else if (path == "/slow") {
            for (int waited = 0; waited < 800 && m_running.load(); waited += 25) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (m_running.load()) {
                SendHead(fd, 200, "OK", 4);
                SendAll(fd, "slow");
            }
        } else if (path == "/flaky") {
            const int seen = m_flaky.fetch_add(1) + 1;
            if (seen < 3) {
                SendHead(fd, 503, "Service Unavailable", 0);
            } else {
                SendHead(fd, 200, "OK", 8);
                SendAll(fd, "flaky-ok");
            }
        } else if (path == "/always-503") {
            SendHead(fd, 503, "Service Unavailable", 0);
        } else if (path == "/redirect") {
            SendHead(fd, 302, "Found", 0, "Location: /small\r\n");
        } else if (path == "/redirect-no-location") {
            SendHead(fd, 302, "Found", 0);
        } else if (path == "/redirect-loop") {
            SendHead(fd, 302, "Found", 0, "Location: /redirect-loop\r\n");
        } else if (path == "/gzip") {
            const std::vector<std::uint8_t> body = {1, 2, 3, 4, 5};
            SendHead(fd, 200, "OK", body.size(), "Content-Encoding: gzip\r\n");
            SendBody(fd, body);
        } else if (path == "/huge-headers") {
            std::string head = "HTTP/1.1 200 OK\r\n";
            for (int index = 0; index < 100; ++index) {
                head += "X-Filler: " + std::string(1024, 'a') + "\r\n";
            }
            head += "Content-Length: 0\r\nConnection: close\r\n\r\n";
            SendAll(fd, head);
        } else {
            SendHead(fd, 404, "Not Found", 0);
        }
    }

    int m_listen_fd{-1};
    std::uint16_t m_port{0};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_requests{0};
    std::atomic<int> m_flaky{0};
    std::vector<std::uint8_t> m_one_megabyte = MakeData(1024 * 1024, 0x5eed1234u);
    std::vector<std::uint8_t> m_two_megabytes = MakeData(2 * 1024 * 1024, 0x0badf00du);
};

/// 启动服务器；失败时记录断言并返回 false。
bool StartServer(TestHttpServer* server) {
    const bool started = server->Start();
    BENTO_CHECK(started);
    return started;
}

/// 写文本文件。
bool WriteTextFile(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

/// 读整个文本文件；失败返回空串。
std::string ReadTextFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// 假 curl 脚本：记录 argv/stdin，并按 capture/mode 模拟输出形态。
/// - symlink：把 output 换成指向 capture/target 的符号链接（模拟链接攻击）；
/// - oversize：写 4 KiB（配合很小的 max_bytes 验证大小校验）；
/// - 其它：写固定的 fake-https-body。
constexpr const char* kFakeCurlScript = R"SH(#!/bin/sh
set -u
cap="${BENTO_FAKE_CURL_CAPTURE:?}"
: > "$cap/argv"
for arg in "$@"; do
  printf '%s
' "$arg" >> "$cap/argv"
done
cat > "$cap/stdin"
out=$(sed -n 's/^output = "\(.*\)"$/\1/p' "$cap/stdin")
[ -n "$out" ] || exit 9
mode=$(cat "$cap/mode")
case "$mode" in
  symlink)
    ln -sf "$(cat "$cap/target")" "$out"
    [ -L "$out" ] || exit 8
    ;;
  oversize)
    head -c 4096 /dev/zero > "$out"
    [ -f "$out" ] || exit 8
    ;;
  status302)
    printf '302'
    exit 0
    ;;
  status304)
    printf '304'
    exit 0
    ;;
  *)
    printf 'fake-https-body' > "$out"
    ;;
esac
printf '200'
exit 0
)SH";

/// 安装假 curl 并临时改写 PATH；析构恢复环境。
class FakeCurl final {
   public:
    FakeCurl() = default;
    ~FakeCurl() { Restore(); }

    FakeCurl(const FakeCurl&) = delete;
    FakeCurl& operator=(const FakeCurl&) = delete;

    bool Install(const fs::path& root) {
        m_bin = root / "bin";
        m_capture = root / "capture";
        std::error_code ec;
        fs::create_directories(m_bin, ec);
        fs::create_directories(m_capture, ec);
        if (ec) {
            return false;
        }
        const fs::path script = m_bin / "curl";
        if (!WriteTextFile(script, kFakeCurlScript)) {
            return false;
        }
        if (::chmod(script.c_str(), 0755) != 0) {
            return false;
        }
        const char* old_path = std::getenv("PATH");
        m_old_path = old_path != nullptr ? old_path : "";
        if (::setenv("BENTO_FAKE_CURL_CAPTURE", m_capture.c_str(), 1) != 0) {
            return false;
        }
        const std::string path = m_bin.string() + ":" + m_old_path;
        if (::setenv("PATH", path.c_str(), 1) != 0) {
            return false;
        }
        m_installed = true;
        return true;
    }

    void SetMode(std::string_view mode) const { WriteTextFile(m_capture / "mode", mode); }
    void SetTarget(const std::string& target) const { WriteTextFile(m_capture / "target", target); }
    std::string Argv() const { return ReadTextFile(m_capture / "argv"); }
    std::string Stdin() const { return ReadTextFile(m_capture / "stdin"); }

   private:
    void Restore() {
        if (!m_installed) {
            return;
        }
        ::setenv("PATH", m_old_path.c_str(), 1);
        ::unsetenv("BENTO_FAKE_CURL_CAPTURE");
        m_installed = false;
    }

    fs::path m_bin;
    fs::path m_capture;
    std::string m_old_path;
    bool m_installed = false;
};

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

/// 场景 1/2：参数、摘要格式与路径沙箱的纯函数行为。
void TestArgumentValidation() {
    const fs::path dir = MakeTempDir("args");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }

    std::string resolved;
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "a.bin", 200, &resolved), FetchStatus::kOk);
    BENTO_CHECK(!resolved.empty());
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "../escape", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "/abs", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "a/b", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "a\\b", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "..", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "", 200, &resolved),
                   FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string(), "a.bin", 0, &resolved),
                   FetchStatus::kInvalidArgument);
    BENTO_CHECK_EQ(ResolveFetchPath(dir.string() + "/does-not-exist", "a.bin", 200, &resolved),
                   FetchStatus::kPathRejected);

    std::string canonical;
    BENTO_CHECK(ParseSha256Hex(std::string(64, 'A'), &canonical));
    BENTO_CHECK_EQ(canonical, std::string(64, 'a'));
    BENTO_CHECK(!ParseSha256Hex("xyz", &canonical));
    BENTO_CHECK(!ParseSha256Hex(std::string(63, '0'), &canonical));

    const FetchResult scheme = Fetch("ftp://example.com/a", dir.string());
    BENTO_CHECK_EQ(scheme.status, FetchStatus::kUnsupportedScheme);
    const FetchResult empty = Fetch("", dir.string());
    BENTO_CHECK_EQ(empty.status, FetchStatus::kInvalidArgument);
    const FetchResult bad_url = Fetch("http://", dir.string());
    BENTO_CHECK_EQ(bad_url.status, FetchStatus::kInvalidArgument);

    FetchOptions options;
    options.max_bytes = 0;
    BENTO_CHECK_EQ(Fetch("http://127.0.0.1:1/a", dir.string(), options).status,
                   FetchStatus::kInvalidArgument);
    options = FetchOptions{};
    options.read_timeout_ms = 0;
    BENTO_CHECK_EQ(Fetch("http://127.0.0.1:1/a", dir.string(), options).status,
                   FetchStatus::kInvalidArgument);
    options = FetchOptions{};
    options.max_retries = 99;
    BENTO_CHECK_EQ(Fetch("http://127.0.0.1:1/a", dir.string(), options).status,
                   FetchStatus::kInvalidArgument);
    options = FetchOptions{};
    options.expected_sha256 = "not-a-hash";
    BENTO_CHECK_EQ(Fetch("http://127.0.0.1:1/a", dir.string(), options).status,
                   FetchStatus::kInvalidArgument);

    RemoveTree(dir);
}

/// 场景 3：1MB 下载 + SHA-256 + 内容逐字节一致。
void TestDownloadOneMegabyte() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("1mb");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::vector<std::uint8_t> expected = MakeData(1024 * 1024, 0x5eed1234u);
    FetchOptions options;
    options.expected_sha256 = Sha256Hex(expected);
    options.max_bytes = 2 * 1024 * 1024;
    const FetchResult result = Fetch(server.BaseUrl() + "/file-1mb", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(result.bytes, static_cast<std::uint64_t>(expected.size()));
    BENTO_CHECK_EQ(result.sha256, Sha256Hex(expected));
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK_EQ(result.http_status, 200);
    BENTO_CHECK_EQ(fs::file_size(result.path), static_cast<std::uint64_t>(expected.size()));
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(result.path, &actual));
    BENTO_CHECK(actual == expected);
    BENTO_CHECK_EQ(server.requests(), 1);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(1));
    RemoveTree(dir);
}

/// 场景 4：404 不重试、不落盘。
void TestHttpNotFound() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("404");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_retries = 3;
    options.retry_backoff_ms = 1;
    const FetchResult result = Fetch(server.BaseUrl() + "/missing", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kHttpError);
    BENTO_CHECK_EQ(result.http_status, 404);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK(result.path.empty());
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 5：读超时（不重试）。
void TestReadTimeout() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("timeout");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.read_timeout_ms = 200;
    options.total_timeout_ms = 3000;
    options.max_retries = 0;
    const FetchResult result = Fetch(server.BaseUrl() + "/slow", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kTimeout);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 6：两次 503 后成功，attempts=3。
void TestRetryThenSuccess() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("retry");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_retries = 3;
    options.retry_backoff_ms = 5;
    const FetchResult result = Fetch(server.BaseUrl() + "/flaky", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(result.attempts, 3);
    BENTO_CHECK_EQ(result.http_status, 200);
    BENTO_CHECK_EQ(server.flaky_requests(), 3);
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(result.path, &actual));
    BENTO_CHECK_EQ(std::string(actual.begin(), actual.end()), std::string("flaky-ok"));
    RemoveTree(dir);
}

/// 场景 7：持续 503，重试用尽。
void TestRetryExhausted() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("retry_exhausted");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_retries = 1;
    options.retry_backoff_ms = 5;
    const FetchResult result = Fetch(server.BaseUrl() + "/always-503", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kRetryExhausted);
    BENTO_CHECK_EQ(result.attempts, 2);
    BENTO_CHECK_EQ(result.http_status, 503);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 8：文件名逃逸拒绝。
void TestFilenameEscapeRejected() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("escape");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::vector<std::string> names = {
        "../escape.bin", "/etc/passwd", "a/b.bin", "..", "a\\b.bin", "C:evil.bin",
        std::string(300, 'x')};
    for (const std::string& name : names) {
        FetchOptions options;
        options.filename = name;
        options.max_retries = 0;
        const FetchResult result = Fetch(server.BaseUrl() + "/small", dir.string(), options);
        BENTO_CHECK_EQ(result.status, FetchStatus::kPathRejected);
    }
    // URL 最后一段解码后是 .. 时也拒绝。
    FetchOptions url_options;
    url_options.max_retries = 0;
    const FetchResult url_result =
        Fetch(server.BaseUrl() + "/%2e%2e", dir.string(), url_options);
    BENTO_CHECK_EQ(url_result.status, FetchStatus::kPathRejected);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 9：Content-Length 超过 max_bytes。
void TestSizeLimit() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("toolarge");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_bytes = 1024 * 1024;
    options.max_retries = 3;
    options.retry_backoff_ms = 1;
    const FetchResult result = Fetch(server.BaseUrl() + "/big", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kTooLarge);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// T-04：流式（chunked / 无 Content-Length）累计超过 max_bytes 必须中止。
void TestStreamingSizeLimit() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("stream_toolarge");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    for (const char* route : {"/chunked-big", "/no-length-big"}) {
        FetchOptions options;
        options.filename = std::string(route + 1) + ".bin";
        options.max_bytes = 1024 * 1024;
        options.max_retries = 3;
        options.retry_backoff_ms = 1;
        const FetchResult result = Fetch(server.BaseUrl() + route, dir.string(), options);
        BENTO_CHECK_EQ(result.status, FetchStatus::kTooLarge);
        BENTO_CHECK_EQ(result.attempts, 1);
        BENTO_CHECK(!result.message.empty());
        BENTO_CHECK(result.path.empty());
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    }
    BENTO_CHECK_EQ(server.requests(), 2);
    RemoveTree(dir);
}

/// T-05：/empty 返回 0 字节，SHA-256 为空串摘要。
void TestEmptyResponse() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("empty");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const FetchResult result = Fetch(server.BaseUrl() + "/empty", dir.string());
    const std::string empty_sha256 =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(result.bytes, std::uint64_t{0});
    BENTO_CHECK_EQ(result.sha256, empty_sha256);
    BENTO_CHECK_EQ(result.http_status, 200);
    BENTO_CHECK_EQ(result.attempts, 1);
    BENTO_CHECK(result.message.empty());
    BENTO_CHECK_EQ(fs::file_size(result.path), std::uint64_t{0});
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(1));
    RemoveTree(dir);
}

/// 场景 10：chunked 下载。
void TestChunked() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("chunked");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const FetchResult result = Fetch(server.BaseUrl() + "/chunked", dir.string());
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(result.path, &actual));
    BENTO_CHECK_EQ(std::string(actual.begin(), actual.end()),
                   std::string("chunked-body-0123456789"));
    BENTO_CHECK_EQ(result.sha256, Sha256Hex(actual));
    RemoveTree(dir);
}

/// 场景 11：Content-Length 大于实际内容 → 截断。
void TestTruncated() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("truncated");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_retries = 0;
    const FetchResult result = Fetch(server.BaseUrl() + "/truncated", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kTruncated);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 12：302 跟随。
void TestRedirect() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("redirect");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const FetchResult result = Fetch(server.BaseUrl() + "/redirect", dir.string());
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(result.path, &actual));
    BENTO_CHECK_EQ(std::string(actual.begin(), actual.end()), std::string("hello-fetch"));
    BENTO_CHECK(fs::path(result.path).filename() == "redirect");
    RemoveTree(dir);
}

/// 场景 13：重定向次数上限。
void TestRedirectLimit() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("redirect_limit");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_redirects = 2;
    options.max_retries = 0;
    const FetchResult result = Fetch(server.BaseUrl() + "/redirect-loop", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kHttpError);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// T-05：禁止跟随重定向、3xx 缺 Location、连接拒绝。
void TestRedirectPolicyAndConnectFailure() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("redirect_policy");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    {
        FetchOptions options;
        options.allow_redirects = false;
        options.max_retries = 0;
        const FetchResult result = Fetch(server.BaseUrl() + "/redirect", dir.string(), options);
        BENTO_CHECK_EQ(result.status, FetchStatus::kHttpError);
        BENTO_CHECK_EQ(result.http_status, 302);
        BENTO_CHECK(!result.message.empty());
        BENTO_CHECK(result.path.empty());
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    }
    {
        FetchOptions options;
        options.max_retries = 0;
        const FetchResult result =
            Fetch(server.BaseUrl() + "/redirect-no-location", dir.string(), options);
        // 头文件承诺 3xx 无 Location 归 kHttpError（实现已对齐）。
        BENTO_CHECK_EQ(result.status, FetchStatus::kHttpError);
        BENTO_CHECK_EQ(result.http_status, 302);
        BENTO_CHECK(result.message.find("Location") != std::string::npos);
        BENTO_CHECK(result.path.empty());
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    }
    {
        FetchOptions options;
        options.max_retries = 0;
        options.connect_timeout_ms = 500;
        const FetchResult result =
            Fetch("http://127.0.0.1:1/fetch.bin", dir.string(), options);
        BENTO_CHECK_EQ(result.status, FetchStatus::kConnectFailed);
        BENTO_CHECK_EQ(result.attempts, 1);
        BENTO_CHECK(!result.message.empty());
        BENTO_CHECK(result.path.empty());
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    }
    RemoveTree(dir);
}

/// 场景 14：摘要不符。
void TestChecksumMismatch() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("checksum");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.expected_sha256 = std::string(64, '0');
    options.max_retries = 0;
    const FetchResult result = Fetch(server.BaseUrl() + "/small", dir.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kChecksumMismatch);
    BENTO_CHECK_EQ(result.bytes, static_cast<std::uint64_t>(11));
    BENTO_CHECK(result.sha256 != std::string(64, '0'));
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    RemoveTree(dir);
}

/// 场景 15：目标已存在时默认拒绝；overwrite 覆盖且重发请求。
void TestOverwriteSemantics() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("overwrite");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const FetchResult first = Fetch(server.BaseUrl() + "/small", dir.string());
    BENTO_CHECK_EQ(first.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(server.requests(), 1);

    const FetchResult blocked = Fetch(server.BaseUrl() + "/small", dir.string());
    BENTO_CHECK_EQ(blocked.status, FetchStatus::kTargetExists);
    BENTO_CHECK_EQ(server.requests(), 1);

    FetchOptions options;
    options.overwrite = true;
    const FetchResult replaced = Fetch(server.BaseUrl() + "/small", dir.string(), options);
    BENTO_CHECK_EQ(replaced.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(server.requests(), 2);
    RemoveTree(dir);
}

/// 场景 16：https 走 curl；对普通 HTTP server 握手失败。
void TestHttpsViaCurl() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("https");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    FetchOptions options;
    options.max_retries = 0;
    options.connect_timeout_ms = 1000;
    options.total_timeout_ms = 5000;
    std::string https_url = server.BaseUrl();
    https_url.replace(0, 4, "https");
    https_url += "/small";
    const FetchResult result = Fetch(https_url, dir.string(), options);
    if (result.status == FetchStatus::kBackendUnavailable) {
        bento_test::Skip("系统没有 curl，跳过 https 分支");
    } else {
        BENTO_CHECK(result.status == FetchStatus::kTlsError ||
                    result.status == FetchStatus::kConnectFailed ||
                    result.status == FetchStatus::kTimeout);
        BENTO_CHECK(!result.ok());
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(0));
    }
    RemoveTree(dir);
}

/// 场景 19：204/205/304 无响应体：立即成功，不等待 EOF/读超时（FETCH-02）。
void TestNoBodyStatusCodes() {
    struct Case {
        const char* path;
        int status;
        const char* filename;
    };
    const Case cases[] = {
        {"/no-content", 204, "no-content.bin"},
        {"/reset-content", 205, "reset-content.bin"},
        {"/not-modified", 304, "not-modified.bin"},
    };
    const std::string empty_sha = Sha256Hex(std::vector<std::uint8_t>{});
    for (const Case& item : cases) {
        TestHttpServer server;
        if (!StartServer(&server)) {
            return;
        }
        const fs::path dir = MakeTempDir("nobody");
        BENTO_CHECK(!dir.empty());
        if (dir.empty()) {
            return;
        }
        FetchOptions options;
        options.filename = item.filename;
        options.max_retries = 0;
        options.read_timeout_ms = 300;
        options.total_timeout_ms = 2000;
        const auto started = std::chrono::steady_clock::now();
        const FetchResult result = Fetch(server.BaseUrl() + item.path, dir.string(), options);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
        BENTO_CHECK_EQ(result.http_status, item.status);
        BENTO_CHECK_EQ(result.bytes, std::uint64_t{0});
        BENTO_CHECK_EQ(result.sha256, empty_sha);
        if (result.ok()) {
            BENTO_CHECK_EQ(fs::file_size(result.path), std::uint64_t{0});
        }
        // 关键：不能等到 read_timeout（300ms）才结束。
        BENTO_CHECK(elapsed < std::chrono::milliseconds(250));
        BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(1));
        server.Stop();
        RemoveTree(dir);
    }
}

/// 场景 20：HTTPS 的 URL/输出路径只经 stdin 配置传给 curl，不进 argv（D-04）。
void TestHttpsUsesStdinConfig() {
    const fs::path root = MakeTempDir("https_stdin");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path dest = root / "dest";
    std::error_code ec;
    fs::create_directories(dest, ec);
    BENTO_CHECK(!ec);
    FakeCurl fake;
    BENTO_CHECK(fake.Install(root));
    fake.SetMode("normal");

    const std::string url =
        "https://example.invalid/file.bin?X-Amz-Signature=SECRET-TOKEN";
    FetchOptions options;
    options.filename = "file.bin";
    options.max_retries = 0;
    options.total_timeout_ms = 5000;
    const FetchResult result = Fetch(url, dest.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(result.http_status, 200);

    const std::string expected_body = "fake-https-body";
    const std::vector<std::uint8_t> expected(expected_body.begin(), expected_body.end());
    BENTO_CHECK_EQ(result.bytes, static_cast<std::uint64_t>(expected.size()));
    BENTO_CHECK_EQ(result.sha256, Sha256Hex(expected));
    BENTO_CHECK_EQ(ReadTextFile(result.path), expected_body);
    // 私有临时目录已被清理，只留下最终文件。
    BENTO_CHECK_EQ(CountEntries(dest), static_cast<std::size_t>(1));

    // argv 里只有 --config -，URL/token/输出路径都不出现。
    const std::string argv = fake.Argv();
    BENTO_CHECK(argv.find("--config") != std::string::npos);
    BENTO_CHECK(argv.find("\n-\n") != std::string::npos);
    BENTO_CHECK(argv.find("SECRET-TOKEN") == std::string::npos);
    BENTO_CHECK(argv.find("https://") == std::string::npos);
    BENTO_CHECK(argv.find(dest.string()) == std::string::npos);

    // stdin 配置里带完整 URL 与输出路径。
    const std::string config = fake.Stdin();
    BENTO_CHECK(config.find("url = \"" + url + "\"") != std::string::npos);
    BENTO_CHECK(config.find("output = \"") != std::string::npos);
    BENTO_CHECK(config.find("proto = \"=https\"") != std::string::npos);
    BENTO_CHECK(config.find("max-filesize = \"") != std::string::npos);
    RemoveTree(root);
}

/// 场景 21：HTTPS 输出被替换成符号链接时拒绝落地，且不写穿到链接目标（D-03）。
void TestHttpsRejectsSymlinkOutput() {
    const fs::path root = MakeTempDir("https_symlink");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path dest = root / "dest";
    std::error_code ec;
    fs::create_directories(dest, ec);
    BENTO_CHECK(!ec);
    const fs::path victim = root / "victim.txt";
    BENTO_CHECK(WriteTextFile(victim, "ORIGINAL"));

    FakeCurl fake;
    BENTO_CHECK(fake.Install(root));
    fake.SetMode("symlink");
    fake.SetTarget(victim.string());

    FetchOptions options;
    options.filename = "file.bin";
    options.max_retries = 0;
    options.total_timeout_ms = 5000;
    const FetchResult result =
        Fetch("https://example.invalid/file.bin", dest.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kIoError);
    BENTO_CHECK(!fs::exists(dest / "file.bin"));
    BENTO_CHECK_EQ(ReadTextFile(victim), std::string("ORIGINAL"));
    // 私有临时目录（含其中的链接）已被清理。
    BENTO_CHECK_EQ(CountEntries(dest), static_cast<std::size_t>(0));
    RemoveTree(root);
}

/// 场景 22：curl 产出的文件超过 max_bytes 时拒绝（HTTPS 也有大小校验）。
void TestHttpsRejectsOversizeOutput() {
    const fs::path root = MakeTempDir("https_oversize");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path dest = root / "dest";
    std::error_code ec;
    fs::create_directories(dest, ec);
    BENTO_CHECK(!ec);
    FakeCurl fake;
    BENTO_CHECK(fake.Install(root));
    fake.SetMode("oversize");

    FetchOptions options;
    options.filename = "too-big.bin";
    options.max_bytes = 1024;
    options.max_retries = 0;
    options.total_timeout_ms = 5000;
    const FetchResult result =
        Fetch("https://example.invalid/big.bin", dest.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kTooLarge);
    BENTO_CHECK(!fs::exists(dest / "too-big.bin"));
    BENTO_CHECK_EQ(CountEntries(dest), static_cast<std::size_t>(0));
    RemoveTree(root);
}

/// 场景 23：HTTPS 返回非 2xx（3xx 缺 Location/未跟随）时按 kHttpError 失败。
void TestHttpsRejectsUnexpectedStatus() {
    const fs::path root = MakeTempDir("https_status");
    BENTO_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path dest = root / "dest";
    std::error_code ec;
    fs::create_directories(dest, ec);
    BENTO_CHECK(!ec);
    FakeCurl fake;
    BENTO_CHECK(fake.Install(root));
    fake.SetMode("status302");

    FetchOptions options;
    options.filename = "redirected.bin";
    options.max_retries = 0;
    options.total_timeout_ms = 5000;
    const FetchResult result =
        Fetch("https://example.invalid/moved.bin", dest.string(), options);
    BENTO_CHECK_EQ(result.status, FetchStatus::kHttpError);
    BENTO_CHECK_EQ(result.http_status, 302);
    BENTO_CHECK(!fs::exists(dest / "redirected.bin"));
    BENTO_CHECK_EQ(CountEntries(dest), static_cast<std::size_t>(0));

    // 304 无响应体：即使 curl 没创建输出文件，也应按「成功且 0 字节」完成。
    fake.SetMode("status304");
    FetchOptions not_modified_options;
    not_modified_options.filename = "cached.bin";
    not_modified_options.max_retries = 0;
    not_modified_options.total_timeout_ms = 5000;
    const FetchResult not_modified =
        Fetch("https://example.invalid/cached.bin", dest.string(), not_modified_options);
    BENTO_CHECK_EQ(not_modified.status, FetchStatus::kOk);
    BENTO_CHECK_EQ(not_modified.http_status, 304);
    BENTO_CHECK_EQ(not_modified.bytes, std::uint64_t{0});
    if (not_modified.ok()) {
        BENTO_CHECK_EQ(fs::file_size(not_modified.path), std::uint64_t{0});
    }
    BENTO_CHECK_EQ(CountEntries(dest), static_cast<std::size_t>(1));
    RemoveTree(root);
}

/// 场景 17：无 Content-Length（EOF 模式）与不支持的编码。
void TestEofBodyAndUnsupportedEncoding() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("eof");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const FetchResult eof = Fetch(server.BaseUrl() + "/no-length", dir.string());
    BENTO_CHECK_EQ(eof.status, FetchStatus::kOk);
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(eof.path, &actual));
    BENTO_CHECK_EQ(std::string(actual.begin(), actual.end()), std::string("eof-body"));

    FetchOptions options;
    options.filename = "gzip.bin";
    options.max_retries = 0;
    const FetchResult gzip = Fetch(server.BaseUrl() + "/gzip", dir.string(), options);
    BENTO_CHECK_EQ(gzip.status, FetchStatus::kProtocolError);

    FetchOptions header_options;
    header_options.filename = "headers.bin";
    header_options.max_retries = 0;
    const FetchResult huge =
        Fetch(server.BaseUrl() + "/huge-headers", dir.string(), header_options);
    BENTO_CHECK_EQ(huge.status, FetchStatus::kProtocolError);
    RemoveTree(dir);
}

/// 场景 18：4 个线程并发下载 1MB（线程安全 + 服务端串行排队）。
void TestConcurrentDownloads() {
    TestHttpServer server;
    if (!StartServer(&server)) {
        return;
    }
    const fs::path dir = MakeTempDir("concurrent");
    BENTO_CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::vector<std::uint8_t> expected = MakeData(1024 * 1024, 0x5eed1234u);
    const std::string sha = Sha256Hex(expected);
    constexpr int kThreads = 4;
    std::vector<FetchResult> results(static_cast<std::size_t>(kThreads));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kThreads));
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&server, &dir, &results, &sha, index]() {
            FetchOptions options;
            options.filename = "part-" + std::to_string(index) + ".bin";
            options.expected_sha256 = sha;
            options.max_bytes = 2 * 1024 * 1024;
            results[static_cast<std::size_t>(index)] =
                Fetch(server.BaseUrl() + "/file-1mb", dir.string(), options);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int index = 0; index < kThreads; ++index) {
        const FetchResult& result = results[static_cast<std::size_t>(index)];
        BENTO_CHECK_EQ(result.status, FetchStatus::kOk);
        BENTO_CHECK_EQ(result.sha256, sha);
        BENTO_CHECK_EQ(result.bytes, static_cast<std::uint64_t>(expected.size()));
    }
    BENTO_CHECK_EQ(server.requests(), kThreads);
    BENTO_CHECK_EQ(CountEntries(dir), static_cast<std::size_t>(kThreads));
    RemoveTree(dir);
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    TestArgumentValidation();
    TestDownloadOneMegabyte();
    TestHttpNotFound();
    TestReadTimeout();
    TestRetryThenSuccess();
    TestRetryExhausted();
    TestFilenameEscapeRejected();
    TestSizeLimit();
    TestStreamingSizeLimit();
    TestEmptyResponse();
    TestChunked();
    TestTruncated();
    TestRedirect();
    TestRedirectLimit();
    TestRedirectPolicyAndConnectFailure();
    TestChecksumMismatch();
    TestOverwriteSemantics();
    TestHttpsViaCurl();
    TestEofBodyAndUnsupportedEncoding();
    TestConcurrentDownloads();
    TestNoBodyStatusCodes();
    TestHttpsUsesStdinConfig();
    TestHttpsRejectsSymlinkOutput();
    TestHttpsRejectsOversizeOutput();
    TestHttpsRejectsUnexpectedStatus();
    return bento_test::Finish("test_fetch");
}
