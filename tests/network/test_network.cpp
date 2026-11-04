#include "go2cpp/network/address.hpp"
#include "go2cpp/network/byte_array.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/framing.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/go.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

class BlockingSocketWaiter final : public go2cpp::net::SocketWaiter {
public:
    int wait(int, go2cpp::io::IOEvent, std::chrono::milliseconds) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        (void)m_condition.wait_for(lock, std::chrono::seconds(3),
                                   [this] { return m_released; });
        return ECANCELED;
    }
    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, timeout,
                                    [this] { return m_entered; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_condition.notify_all();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered{false};
    bool m_released{false};
};

/**
 * @brief 验证网络基础层在普通线程中的地址、编解码、分帧和 TCP/UDP 行为。
 * @details 测试故意把 HTTP 输入拆成两段，覆盖半包；同时验证错误长度、
 *          chunked、长度前缀、socketpair、回环 TCP、无连接 UDP 和超时路径。
 */
void run_network_tests() {
    go2cpp_tests::announce("network address bytearray socket http");
    // 地址对象只保存值，不拥有任何文件描述符。
    auto address = go2cpp::net::IPv4Address::Create("127.0.0.1", 8080);
    GO2CPP_CHECK(address && address->port() == 8080);
    GO2CPP_CHECK(address->network_address(24));
    GO2CPP_CHECK(!address->network_address(33));
    // IPv6 zone 必须是完整的十进制 scope id，不能静默接受尾随垃圾或溢出。
    auto scoped = go2cpp::net::IPv6Address::Create("fe80::1%3", 80);
    GO2CPP_CHECK(scoped && scoped->to_string() == "[fe80::1%3]:80");
    GO2CPP_CHECK(!go2cpp::net::IPv6Address::Create("fe80::1%"));
    GO2CPP_CHECK(!go2cpp::net::IPv6Address::Create("fe80::1%3x"));
    GO2CPP_CHECK(!go2cpp::net::IPv6Address::Create("fe80::1%4294967296"));
    // Unix 地址同时覆盖文件系统路径、抽象命名空间和短 sockaddr 缓冲区。
    const std::size_t unix_capacity = sizeof(sockaddr_un{}.sun_path);
    std::string abstract_max(unix_capacity, 'x');
    abstract_max.front() = '\0';
    go2cpp::net::UnixAddress abstract_address(abstract_max);
    GO2CPP_CHECK(abstract_address.addr_len() == sizeof(sockaddr_un));
    bool abstract_overflow_rejected = false;
    try {
        std::string too_long(unix_capacity + 1, 'x');
        too_long.front() = '\0';
        go2cpp::net::UnixAddress invalid_abstract(too_long);
    } catch (const std::length_error&) { abstract_overflow_rejected = true; }
    GO2CPP_CHECK(abstract_overflow_rejected);
    bool embedded_nul_rejected = false;
    try {
        go2cpp::net::UnixAddress invalid_path(std::string("file\0name", 9));
    } catch (const std::invalid_argument&) { embedded_nul_rejected = true; }
    GO2CPP_CHECK(embedded_nul_rejected);
    sockaddr_un short_unix{};
    short_unix.sun_family = AF_UNIX;
    short_unix.sun_path[0] = 'x';
    GO2CPP_CHECK(go2cpp::net::Address::Create(
                     reinterpret_cast<const sockaddr*>(&short_unix),
                     static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1)) != nullptr);
    GO2CPP_CHECK(go2cpp::net::Address::Create(
                     reinterpret_cast<const sockaddr*>(&short_unix),
                     static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) - 1)) == nullptr);
    sockaddr_un padded_unix{};
    padded_unix.sun_family = AF_UNIX;
    padded_unix.sun_path[0] = 'x';
    padded_unix.sun_path[2] = 'y';
    bool invalid_unix_padding_rejected = false;
    try {
        go2cpp::net::UnixAddress invalid_padding(
            padded_unix, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 3));
    } catch (const std::invalid_argument&) { invalid_unix_padding_rejected = true; }
    GO2CPP_CHECK(invalid_unix_padding_rejected);
    padded_unix.sun_family = AF_INET;
    bool invalid_unix_family_rejected = false;
    try {
        go2cpp::net::UnixAddress invalid_family(
            padded_unix, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1));
    } catch (const std::invalid_argument&) { invalid_unix_family_rejected = true; }
    GO2CPP_CHECK(invalid_unix_family_rejected);
    // 网络协议显式选择大端，避免主机字节序在跨机器时产生歧义。
    go2cpp::net::ByteArray bytes;
    bytes.set_little_endian(false);
    bytes.write_fuint32(0x12345678U);
    bytes.write_varuint64(300);
    bytes.write_string_vint("hello");
    bytes.set_position(0);
    GO2CPP_CHECK(bytes.read_fuint32() == 0x12345678U);
    GO2CPP_CHECK(bytes.read_varuint64() == 300);
    GO2CPP_CHECK(bytes.read_string_vint() == "hello");
    auto framer = go2cpp::net::Framer::delimiter("|"); framer.feed("a|b"); auto frames = framer.take_frames(); GO2CPP_CHECK(frames.size() == 1 && frames[0] == "a");
    // 新接口把超长输入转为状态码；固定帧一次输入多帧时不错误地按单帧上限拒绝。
    auto safe_framer = go2cpp::net::Framer::delimiter("|", 3);
    GO2CPP_CHECK(safe_framer.try_feed("a|") == go2cpp::net::Framer::FeedError::None);
    GO2CPP_CHECK(safe_framer.try_feed("bb|") == go2cpp::net::Framer::FeedError::None);
    const auto safe_frames = safe_framer.take_frames();
    GO2CPP_CHECK(safe_frames.size() == 2 && safe_frames[0] == "a" && safe_frames[1] == "bb");
    GO2CPP_CHECK(safe_framer.try_feed("toolong") == go2cpp::net::Framer::FeedError::FrameTooLarge);
    GO2CPP_CHECK(safe_framer.try_feed("x|") == go2cpp::net::Framer::FeedError::Failed);
    safe_framer.reset();
    GO2CPP_CHECK(safe_framer.try_feed("ok|") == go2cpp::net::Framer::FeedError::None);
    auto fixed_framer = go2cpp::net::Framer::fixed(2);
    GO2CPP_CHECK(fixed_framer.try_feed("aabb") == go2cpp::net::Framer::FeedError::None);
    const auto fixed_frames = fixed_framer.take_frames();
    GO2CPP_CHECK(fixed_frames.size() == 2 && fixed_frames[0] == "aa" && fixed_frames[1] == "bb");
    // HTTP 是字节流协议，不能把一次 recv 当作完整请求。
    go2cpp::http::Parser parser;
    go2cpp::http::Request request;
    const std::string message = "POST /x?a=1 HTTP/1.1\r\nHost: test\r\nContent-Length: 5\r\n\r\nhello";
    auto first = parser.feed(std::string_view(message.data(), 11), request);
    GO2CPP_CHECK(first.state == go2cpp::http::ParseState::NeedMore);
    auto second = parser.feed(std::string_view(message.data() + 11, message.size() - 11), request);
    GO2CPP_CHECK(second.state == go2cpp::http::ParseState::Complete);
    GO2CPP_CHECK(request.body == "hello" && request.path() == "/x");
    // 没有 Content-Length/Transfer-Encoding 的响应以 EOF 结束 body；EOF
    // 前只能 NeedMore，finish() 后才交付完整响应。
    go2cpp::http::Parser close_delimited_parser;
    go2cpp::http::Response close_delimited_response;
    auto close_header = close_delimited_parser.feed(
        "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npar", close_delimited_response);
    GO2CPP_CHECK(close_header.state == go2cpp::http::ParseState::NeedMore &&
                 close_delimited_response.body.empty());
    close_delimited_parser.feed("tial", close_delimited_response);
    auto close_finished = close_delimited_parser.finish(close_delimited_response);
    GO2CPP_CHECK(close_finished.state == go2cpp::http::ParseState::Complete &&
                 close_delimited_response.body == "partial" && close_delimited_response.close);
    go2cpp::http::Parser truncated_parser;
    go2cpp::http::Response truncated_response;
    truncated_parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nxy", truncated_response);
    GO2CPP_CHECK(truncated_parser.finish(truncated_response).state == go2cpp::http::ParseState::Error);
    go2cpp::http::Parser chunk_parser;
    go2cpp::http::Request chunk_request;
    const std::string chunked = "POST /chunk HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    auto chunk_result = chunk_parser.feed(chunked, chunk_request);
    GO2CPP_CHECK(chunk_result.state == go2cpp::http::ParseState::Complete && chunk_request.body == "Wikipedia");
    go2cpp::http::Parser invalid_parser;
    go2cpp::http::Request invalid_request;
    const std::string invalid = "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\na";
    GO2CPP_CHECK(invalid_parser.feed(invalid, invalid_request).state == go2cpp::http::ParseState::Error);
    // Transfer-Encoding 必须是本实现明确支持的单一 chunked，不能用
    // 子串匹配把 notchunked 当成合法编码；chunk trailer 也必须独立保存。
    go2cpp::http::Parser strict_chunk_parser;
    go2cpp::http::Request strict_chunk_request;
    const std::string strict_chunk =
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;tag=x\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n";
    const auto strict_result = strict_chunk_parser.feed(strict_chunk, strict_chunk_request);
    GO2CPP_CHECK(strict_result.state == go2cpp::http::ParseState::Complete &&
                 strict_chunk_request.body == "abc" &&
                 strict_chunk_request.trailer.get("X-Checksum") == "ok");
    go2cpp::http::Parser bad_encoding_parser;
    go2cpp::http::Request bad_encoding_request;
    GO2CPP_CHECK(bad_encoding_parser.feed(
        "POST / HTTP/1.1\r\nTransfer-Encoding: notchunked\r\n\r\n",
        bad_encoding_request).state == go2cpp::http::ParseState::Error);
    // 204/304/1xx 和 HEAD 响应不能被 Content-Length 诱导去等待不存在的 body。
    go2cpp::http::Parser no_body_parser;
    go2cpp::http::Response no_body_response;
    GO2CPP_CHECK(no_body_parser.feed("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
                                     no_body_response).state == go2cpp::http::ParseState::Complete);
    go2cpp::http::Response invalid_body_response;
    invalid_body_response.status = go2cpp::http::Status::NoContent;
    invalid_body_response.body = "must not be sent";
    invalid_body_response.header.set("Content-Length", "99");
    const auto invalid_body_wire = invalid_body_response.to_string();
    GO2CPP_CHECK(invalid_body_wire.find("Content-Length") == std::string::npos &&
                 invalid_body_wire.rfind("must not be sent") == std::string::npos);
    go2cpp::http::Headers safe_headers;
    safe_headers.set("X-Test", "ok\r\nInjected: yes");
    GO2CPP_CHECK(!safe_headers.has("X-Test"));
    go2cpp::http::Parser token_parser;
    go2cpp::http::Response token_result;
    GO2CPP_CHECK(token_parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: disclose, keep-alive\r\n\r\n",
                                   token_result).state == go2cpp::http::ParseState::Complete &&
                 !token_result.close);
    go2cpp::http::Parser close_token_parser;
    go2cpp::http::Response close_token_result;
    GO2CPP_CHECK(close_token_parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive, CLOSE\r\n\r\n",
                                         close_token_result).state == go2cpp::http::ParseState::Complete &&
                 close_token_result.close);
    go2cpp::http::Request injected_request;
    injected_request.target = "/ok\r\nX-Injected: yes";
    GO2CPP_CHECK(injected_request.to_string().empty());
    go2cpp::http::Response injected_response;
    injected_response.version = "HTTP/1.1\r\nX-Injected: yes";
    GO2CPP_CHECK(injected_response.to_string().empty());
    go2cpp::http::Parser bad_target_parser;
    go2cpp::http::Request bad_target_request;
    GO2CPP_CHECK(bad_target_parser.feed("GET /bad target HTTP/1.1\r\nHost: x\r\n\r\n",
                                        bad_target_request).state == go2cpp::http::ParseState::Error);
    GO2CPP_CHECK(go2cpp::http::Client{}.get("http://127.0.0.1:65537/").error ==
                 go2cpp::http::Result::Error::InvalidUrl);
    int closed_fd = -1;
    {
        int closed_pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, closed_pair) == 0);
        closed_fd = closed_pair[0];
        ::close(closed_pair[0]);
        ::close(closed_pair[1]);
    }
    GO2CPP_CHECK(!go2cpp::net::Socket::FromFd(closed_fd, AF_UNIX, SOCK_STREAM));
    go2cpp::http::Parser head_parser;
    head_parser.set_response_to_head(true);
    go2cpp::http::Response head_response;
    GO2CPP_CHECK(head_parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n",
                                  head_response).state == go2cpp::http::ParseState::Complete &&
                 head_response.body.empty());
    // 回归 9f7ab5c：Parser 增量 feed 时 header_block 每次都是同一段
    // （m_buffer 保留未消费字节），若不先 clear() 就会把同名头重复追加。
    // 6 MiB 是抖音上传触发全 502 的真实量级：原实现把 User-Agent /
    // Authorization / Content-Type 重复上千份，wire 头膨胀到 ~147 KB。
    {
        go2cpp::http::Parser large_parser;
        go2cpp::http::Request large_request;
        constexpr std::size_t kBodySize = 6U * 1024U * 1024U;  // 6291456
        constexpr std::size_t kChunkSize = 1024U;              // 每次 feed 1 KiB
        constexpr std::size_t kChunkCount = kBodySize / kChunkSize;
        static_assert(kChunkCount == 6144, "6 MiB / 1 KiB 必须是整数块");
        const std::string large_head =
            "POST /api/douyin/video-write HTTP/1.1\r\n"
            "Host: 127.0.0.1:18080\r\n"
            "User-Agent: go2cpp-test/1.0\r\n"
            "Authorization: Bearer header-dup-regression\r\n"
            "Content-Type: video/mp4\r\n"
            "Content-Length: 6291456\r\n\r\n";
        // 每个字节由偏移决定：既能校验首尾抽样，也能发现错位或丢字节。
        const auto sample_byte = [](std::size_t offset) {
            return static_cast<char>((offset * 31U + (offset >> 8U) + 0x5AU) & 0xFFU);
        };
        // 头部单独一次喂入（真实 recv 边界），body 严格按 1 KiB 逐块喂。
        auto large_result = large_parser.feed(large_head, large_request);
        GO2CPP_CHECK(large_result.state == go2cpp::http::ParseState::NeedMore);
        GO2CPP_CHECK(large_request.header.entries().size() == 5);
        std::string block(kChunkSize, '\0');
        std::size_t fed_blocks = 0;
        for (std::size_t offset = 0; offset < kBodySize; offset += kChunkSize) {
            const std::size_t size = std::min(kChunkSize, kBodySize - offset);
            for (std::size_t i = 0; i < size; ++i) block[i] = sample_byte(offset + i);
            large_result = large_parser.feed(std::string_view(block.data(), size), large_request);
            ++fed_blocks;
            // 第 100 块时头仍必须恰好一份——这正是修复前最先膨胀的位置。
            if (fed_blocks == 100) {
                GO2CPP_CHECK(large_request.header.entries().size() == 5);
                GO2CPP_CHECK(large_request.header.values("User-Agent").size() == 1);
            }
            if (large_result.state != go2cpp::http::ParseState::NeedMore) break;
        }
        GO2CPP_CHECK(large_result.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(fed_blocks == kChunkCount);
        GO2CPP_CHECK(large_request.header.entries().size() == 5);
        GO2CPP_CHECK(large_request.header.values("User-Agent").size() == 1);
        GO2CPP_CHECK(large_request.header.values("Authorization").size() == 1);
        GO2CPP_CHECK(large_request.header.values("Content-Type").size() == 1);
        GO2CPP_CHECK(large_request.header.values("Content-Length").size() == 1);
        GO2CPP_CHECK(large_request.header.get("Content-Type") == "video/mp4");
        GO2CPP_CHECK(large_request.body.size() == 6291456);
        GO2CPP_CHECK(large_request.body.front() == sample_byte(0));
        GO2CPP_CHECK(large_request.body[1] == sample_byte(1));
        GO2CPP_CHECK(large_request.body[kBodySize - 2] == sample_byte(kBodySize - 2));
        GO2CPP_CHECK(large_request.body.back() == sample_byte(kBodySize - 1));
        GO2CPP_CHECK(large_request.body[kBodySize / 2] == sample_byte(kBodySize / 2));
    }
    // 回归：chunked 增量解析必须跨 feed 复用游标。修复前每次 NeedMore 都
    // 丢弃局部 body/cursor，下一次从 body_start 全量重扫，6 MiB body 的总
    // 扫描量是 O(n²)：close-delimited 19 ms vs chunked 4017 ms（211 倍）。
    // 这里用与真实 recv 一致的 1 KiB 任意边界切片喂完整 chunked wire，
    // 并同时跑一遍 close-delimited 作为相对基线。
    {
        constexpr std::size_t kChunkedBody = 6U * 1024U * 1024U;
        constexpr std::size_t kChunkUnit = 1024U;  // 每个 chunk 0x400 字节
        constexpr std::size_t kChunkCount = kChunkedBody / kChunkUnit;
        constexpr std::size_t kFeedSize = 1024U;   // 每次 feed 1 KiB wire
        static_assert(kChunkCount == 6144, "6 MiB / 1 KiB 必须是整数块");
        // 每个字节由偏移决定：既能校验首尾抽样，也能发现错位或丢字节。
        const auto sample_byte = [](std::size_t offset) {
            return static_cast<char>((offset * 31U + (offset >> 8U) + 0x5AU) & 0xFFU);
        };
        std::string chunked_wire =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Content-Type: application/octet-stream\r\n\r\n";
        const std::size_t chunked_head_size = chunked_wire.size();
        chunked_wire.reserve(chunked_head_size + kChunkCount * (kChunkUnit + 8) + 8);
        for (std::size_t offset = 0; offset < kChunkedBody; offset += kChunkUnit) {
            chunked_wire += "400\r\n";
            for (std::size_t i = 0; i < kChunkUnit; ++i)
                chunked_wire += sample_byte(offset + i);
            chunked_wire += "\r\n";
        }
        chunked_wire += "0\r\n\r\n";
        // 同样的 6 MiB body 走 close-delimited，仅作性能基线。
        std::string close_wire = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
        const std::size_t close_head_size = close_wire.size();
        close_wire.reserve(close_head_size + kChunkedBody);
        for (std::size_t offset = 0; offset < kChunkedBody; ++offset)
            close_wire += sample_byte(offset);

        go2cpp::http::Parser close_parser;
        go2cpp::http::Response close_response;
        const auto close_begin = std::chrono::steady_clock::now();
        GO2CPP_CHECK(close_parser.feed(
                         std::string_view(close_wire.data(), close_head_size), close_response)
                         .state == go2cpp::http::ParseState::NeedMore);
        for (std::size_t offset = 0; offset < kChunkedBody; offset += kFeedSize) {
            const std::size_t size = std::min(kFeedSize, kChunkedBody - offset);
            close_parser.feed(std::string_view(close_wire.data() + close_head_size + offset, size),
                              close_response);
        }
        const auto close_finished = close_parser.finish(close_response);
        const auto close_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - close_begin);
        GO2CPP_CHECK(close_finished.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(close_response.body.size() == kChunkedBody);

        go2cpp::http::Parser chunked_parser;
        go2cpp::http::Response chunked_response;
        const auto chunked_begin = std::chrono::steady_clock::now();
        auto chunked_result = chunked_parser.feed(
            std::string_view(chunked_wire.data(), chunked_head_size), chunked_response);
        GO2CPP_CHECK(chunked_result.state == go2cpp::http::ParseState::NeedMore);
        const std::size_t chunked_body_wire = chunked_wire.size() - chunked_head_size;
        for (std::size_t offset = 0; offset < chunked_body_wire; offset += kFeedSize) {
            const std::size_t size = std::min(kFeedSize, chunked_body_wire - offset);
            chunked_result = chunked_parser.feed(
                std::string_view(chunked_wire.data() + chunked_head_size + offset, size),
                chunked_response);
            if (chunked_result.state != go2cpp::http::ParseState::NeedMore) break;
        }
        const auto chunked_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - chunked_begin);
        GO2CPP_CHECK(chunked_result.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(chunked_response.status == go2cpp::http::Status::OK);
        GO2CPP_CHECK(chunked_response.body.size() == kChunkedBody);
        bool chunked_body_ok = chunked_response.body.size() == kChunkedBody;
        for (std::size_t offset = 0; chunked_body_ok && offset < kChunkedBody; ++offset)
            chunked_body_ok = chunked_response.body[offset] == sample_byte(offset);
        GO2CPP_CHECK(chunked_body_ok);
        // 修复前的 e2e 复现数据是 4017 ms；修复后应在个位数毫秒。
        // 同时用 close-delimited 基线兜底，避免 CI 慢机把绝对阈值变脆。
        GO2CPP_CHECK(chunked_elapsed.count() < 100);
        GO2CPP_CHECK(chunked_elapsed.count() <= close_elapsed.count() * 4 + 50);
        go2cpp_tests::announce("chunked 6MiB/1KiB feed: " +
                               std::to_string(chunked_elapsed.count()) + " ms vs close-delimited " +
                               std::to_string(close_elapsed.count()) + " ms");
    }
    // chunked 跨 feed 状态必须随 reset() 清理：复用同一 Parser 解析下一条
    // 消息时，不能把上一条已解码 body 拼进来（m_chunked_body/m_chunk_scan_pos）。
    {
        go2cpp::http::Parser reuse_parser;
        go2cpp::http::Response reuse_response;
        GO2CPP_CHECK(reuse_parser
                         .feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "5\r\nfirst\r\n",
                               reuse_response)
                         .state == go2cpp::http::ParseState::NeedMore);
        GO2CPP_CHECK(reuse_response.body.empty());
        reuse_parser.reset();
        reuse_response = go2cpp::http::Response{};
        GO2CPP_CHECK(reuse_parser
                         .feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "6\r\nsecond\r\n0\r\n\r\n",
                               reuse_response)
                         .state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(reuse_response.body == "second");
        // 小限额：chunk 声明超过 max_body_bytes 时必须在读数据前报错，
        // 不能先把数据攒进 m_chunked_body 再在完成时判超限。
        go2cpp::http::ParserLimits tight_limits;
        tight_limits.max_body_bytes = 16;
        go2cpp::http::Parser tight_parser(tight_limits);
        go2cpp::http::Response tight_response;
        GO2CPP_CHECK(tight_parser
                         .feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n20\r\n",
                               tight_response)
                         .state == go2cpp::http::ParseState::Error);
    }
    // 快速路径：16 KiB body 分 16 次喂，确认短消息同样逐次重建头而不是累加。
    {
        go2cpp::http::Parser small_parser;
        go2cpp::http::Request small_request;
        constexpr std::size_t kSmallBody = 16U * 1024U;
        constexpr std::size_t kSmallChunk = 1024U;
        const std::string small_head =
            "POST /upload HTTP/1.1\r\n"
            "Host: 127.0.0.1:18080\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: 16384\r\n\r\n";
        go2cpp::http::ParseResult small_result;
        GO2CPP_CHECK(small_parser.feed(small_head, small_request).state ==
                     go2cpp::http::ParseState::NeedMore);
        GO2CPP_CHECK(small_request.header.entries().size() == 3);
        std::string small_block(kSmallChunk, '\0');
        std::size_t small_feeds = 0;
        for (std::size_t offset = 0; offset < kSmallBody; offset += kSmallChunk) {
            // 第 n 块全填 'a'+n，便于定位错位。
            std::fill(small_block.begin(), small_block.end(),
                      static_cast<char>('a' + (offset / kSmallChunk)));
            small_result = small_parser.feed(small_block, small_request);
            ++small_feeds;
            if (small_result.state != go2cpp::http::ParseState::NeedMore) break;
        }
        GO2CPP_CHECK(small_result.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(small_feeds == 16);
        GO2CPP_CHECK(small_request.header.entries().size() == 3);
        GO2CPP_CHECK(small_request.header.values("Content-Type").size() == 1);
        GO2CPP_CHECK(small_request.header.values("Content-Length").size() == 1);
        GO2CPP_CHECK(small_request.body.size() == 16384);
        GO2CPP_CHECK(small_request.body.front() == 'a');
        GO2CPP_CHECK(small_request.body[1024] == 'b');
        GO2CPP_CHECK(small_request.body.back() == 'p');
    }
    // 响应侧同样按「每次 feed 重建一份」：9f7ab5c 同时覆盖 Request/Response。
    {
        go2cpp::http::Parser response_parser;
        go2cpp::http::Response dup_response;
        const std::string response_wire =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 8\r\n"
            "\r\n"
            "{\"ok\":1}";
        go2cpp::http::ParseResult response_result;
        for (std::size_t offset = 0; offset < response_wire.size(); offset += 8) {
            const std::size_t size = std::min<std::size_t>(8, response_wire.size() - offset);
            response_result = response_parser.feed(
                std::string_view(response_wire.data() + offset, size), dup_response);
        }
        GO2CPP_CHECK(response_result.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(dup_response.header.entries().size() == 2);
        GO2CPP_CHECK(dup_response.header.values("Content-Type").size() == 1);
        GO2CPP_CHECK(dup_response.body == "{\"ok\":1}");
    }

    // 回归（BUG-1）：只有起始行、没有头字段的 informational 响应
    // （`HTTP/1.1 100 Continue\r\n\r\n`）必须解析成完整消息。修复前
    // header_block 内没有 CRLF，find("\r\n") 返回 npos，被误判为
    // invalid start line（Client 0ms 返回 Parse(7)）。
    {
        go2cpp::http::Parser continue_parser;
        go2cpp::http::Response continue_response;
        const auto continue_result =
            continue_parser.feed("HTTP/1.1 100 Continue\r\n\r\n", continue_response);
        GO2CPP_CHECK(continue_result.state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(continue_response.status == go2cpp::http::Status::Continue);
        GO2CPP_CHECK(continue_response.body.empty());
        GO2CPP_CHECK(continue_parser.take_remaining().empty());
    }
    // 同一 recv 内 100 与 200 粘连：100 完成后剩余字节必须原样交给下一次
    // reset+feed（Client 的 pending/take_remaining 路径）。
    {
        go2cpp::http::Parser continued_parser;
        go2cpp::http::Response continued_response;
        GO2CPP_CHECK(continued_parser
                         .feed("HTTP/1.1 100 Continue\r\n\r\n"
                               "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                               continued_response)
                         .state == go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(continued_response.status == go2cpp::http::Status::Continue);
        const std::string buffered = continued_parser.take_remaining();
        GO2CPP_CHECK(buffered == "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        continued_parser.reset();
        continued_response = go2cpp::http::Response{};
        GO2CPP_CHECK(continued_parser.feed(buffered, continued_response).state ==
                     go2cpp::http::ParseState::Complete);
        GO2CPP_CHECK(continued_response.status == go2cpp::http::Status::OK &&
                     continued_response.body == "ok");
    }
    // Client 层：上游先回空头 100、再回 200。修复前 Client 在 100 上直接
    // 返回 Parse，gateway 把上游 100 变成 502；修复后必须跳过 100 拿到 200。
    auto continue_listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(continue_listener &&
                   continue_listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) &&
                   continue_listener->listen(4));
    auto continue_address =
        std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(continue_listener->local_address());
    GO2CPP_REQUIRE(continue_address);
    std::thread continue_server([continue_listener] {
        auto accepted = continue_listener->accept(std::chrono::seconds(2));
        if (!accepted) return;
        std::string request_bytes;
        std::array<char, 512> request_buffer{};
        while (request_bytes.find("\r\n\r\n") == std::string::npos) {
            const auto read = accepted->recv(request_buffer.data(), request_buffer.size(), 0,
                                              std::chrono::seconds(2));
            if (!read.ok() || read.transferred == 0) return;
            request_bytes.append(request_buffer.data(), static_cast<std::size_t>(read.transferred));
        }
        static constexpr char responses[] =
            "HTTP/1.1 100 Continue\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        const auto sent =
            accepted->send(responses, sizeof(responses) - 1, 0, std::chrono::seconds(2));
        GO2CPP_CHECK(sent.ok() && sent.transferred == static_cast<ssize_t>(sizeof(responses) - 1));
        accepted->close();
    });
    const auto continue_client_result = go2cpp::http::Client(std::chrono::seconds(2)).get(
        "http://127.0.0.1:" + std::to_string(continue_address->port()) + "/continue");
    GO2CPP_CHECK(continue_client_result.ok());
    if (continue_client_result.response) {
        GO2CPP_CHECK(continue_client_result.response->status == go2cpp::http::Status::OK);
        GO2CPP_CHECK(continue_client_result.response->body == "ok");
    }
    GO2CPP_JOIN_WITH_WATCHDOG(continue_server, std::chrono::seconds(3));
    continue_listener->close();

    // C6：对端只回 1xx。超过 10 次 informational 响应必须返回 Parse，
    // 而不是无限消费 pending 假死。
    auto informational_listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(informational_listener &&
                   informational_listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) &&
                   informational_listener->listen(4));
    auto informational_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(
        informational_listener->local_address());
    GO2CPP_REQUIRE(informational_address);
    std::thread informational_server([informational_listener] {
        auto accepted = informational_listener->accept(std::chrono::seconds(2));
        if (!accepted) return;
        std::string request_bytes;
        std::array<char, 512> request_buffer{};
        while (request_bytes.find("\r\n\r\n") == std::string::npos) {
            const auto read = accepted->recv(request_buffer.data(), request_buffer.size(), 0,
                                              std::chrono::seconds(2));
            if (!read.ok() || read.transferred == 0) return;
            request_bytes.append(request_buffer.data(), static_cast<std::size_t>(read.transferred));
        }
        std::string informational;
        for (int i = 0; i < 11; ++i) informational += "HTTP/1.1 100 Continue\r\n\r\n";
        const auto sent = accepted->send(informational.data(), informational.size(), 0,
                                         std::chrono::seconds(2));
        GO2CPP_CHECK(sent.ok() &&
                     sent.transferred == static_cast<ssize_t>(informational.size()));
        accepted->close();
    });
    const auto informational_result = go2cpp::http::Client(std::chrono::seconds(2)).get(
        "http://127.0.0.1:" + std::to_string(informational_address->port()) + "/informational");
    GO2CPP_CHECK(informational_result.error == go2cpp::http::Result::Error::Parse);
    GO2CPP_CHECK(!informational_result.ok());
    GO2CPP_JOIN_WITH_WATCHDOG(informational_server, std::chrono::seconds(3));
    informational_listener->close();

    // 流式响应只允许 Idle -> HeadersCommitted -> Closed，重复 close/write 必须失败。
    int stream_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair) == 0);
    auto stream_socket = go2cpp::net::Socket::FromFd(stream_pair[0], AF_UNIX, SOCK_STREAM);
    auto stream_peer = go2cpp::net::Socket::FromFd(stream_pair[1], AF_UNIX, SOCK_STREAM);
    go2cpp::http::ResponseWriter writer(stream_socket, std::chrono::seconds(1));
    GO2CPP_CHECK(!writer.write("before"));
    GO2CPP_CHECK(writer.start() && writer.state() == go2cpp::http::StreamState::HeadersCommitted);
    GO2CPP_CHECK(writer.write("hello"));
    GO2CPP_CHECK(writer.close() && writer.state() == go2cpp::http::StreamState::Closed);
    GO2CPP_CHECK(!writer.write("after") && !writer.close());
    stream_socket->close();
    stream_peer->close();
    int major = 0, minor = 0;
    GO2CPP_CHECK(go2cpp::http::parse_http_version("HTTP/1.1", major, minor) && major == 1 && minor == 1);
    go2cpp::http::QueryValues query;
    GO2CPP_CHECK(go2cpp::http::decode_query("a=1&a=2&empty", query) && query["a"].size() == 2 && query["empty"][0].empty());
    GO2CPP_CHECK(go2cpp::http::Client{}.get("bad-url").error == go2cpp::http::Result::Error::InvalidUrl);
    GO2CPP_CHECK(go2cpp::http::Client(std::chrono::milliseconds(1)).get("http://[::1]/").error !=
                 go2cpp::http::Result::Error::InvalidUrl);
    GO2CPP_CHECK(go2cpp::http::Client{}.get("http://::1/").error ==
                 go2cpp::http::Result::Error::InvalidUrl);

    // C3：POST/PUT/PATCH 的空 body 也必须显式补 Content-Length: 0，否则服务端
    // 会按未定长度等待请求体；已显式设置时不重复追加，GET 不引入多余头。
    {
        go2cpp::http::Request empty_post;
        empty_post.method = go2cpp::http::Method::Post;
        empty_post.target = "/upload";
        const std::string empty_post_wire = empty_post.to_string();
        GO2CPP_CHECK(empty_post_wire.find("Content-Length: 0\r\n") != std::string::npos);
        empty_post.header.set("Content-Length", "0");
        const std::string explicit_wire = empty_post.to_string();
        GO2CPP_CHECK(explicit_wire.find("Content-Length") == explicit_wire.rfind("Content-Length"));
        go2cpp::http::Request empty_get;
        empty_get.method = go2cpp::http::Method::Get;
        GO2CPP_CHECK(empty_get.to_string().find("Content-Length") == std::string::npos);
        go2cpp::http::Request body_post;
        body_post.method = go2cpp::http::Method::Post;
        body_post.body = "abc";
        GO2CPP_CHECK(body_post.to_string().find("Content-Length: 3\r\n") != std::string::npos);
    }
    // C4：对端 RST（ECONNRESET）与写已关闭套接字（EPIPE）归为 Closed；
    // 超时/取消保持独立语义，其余 errno 回落阶段错误。
    GO2CPP_CHECK(go2cpp::http::classify_io_error(go2cpp::http::Result::Error::Receive, ECONNRESET) ==
                 go2cpp::http::Result::Error::Closed);
    GO2CPP_CHECK(go2cpp::http::classify_io_error(go2cpp::http::Result::Error::Send, EPIPE) ==
                 go2cpp::http::Result::Error::Closed);
    GO2CPP_CHECK(go2cpp::http::classify_io_error(go2cpp::http::Result::Error::Connect, ETIMEDOUT) ==
                 go2cpp::http::Result::Error::Timeout);
    GO2CPP_CHECK(go2cpp::http::classify_io_error(go2cpp::http::Result::Error::Receive, ECANCELED) ==
                 go2cpp::http::Result::Error::Cancelled);
    GO2CPP_CHECK(go2cpp::http::classify_io_error(go2cpp::http::Result::Error::Send, EIO) ==
                 go2cpp::http::Result::Error::Send);
    // C5：connect 失败取 socket 记录的 error（errno 兜底）。绑定了但已关闭的
    // 端口必然返回 ECONNREFUSED，分类为 Connect 且诊断文本来自真实 errno。
    std::uint16_t refused_port = 0;
    {
        auto probe = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(probe && probe->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)));
        auto probe_address =
            std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(probe->local_address());
        GO2CPP_REQUIRE(probe_address);
        refused_port = probe_address->port();
        probe->close();
    }
    const auto refused_result = go2cpp::http::Client(std::chrono::seconds(2)).get(
        "http://127.0.0.1:" + std::to_string(refused_port) + "/refused");
    GO2CPP_CHECK(refused_result.error == go2cpp::http::Result::Error::Connect);
    GO2CPP_CHECK(refused_result.message.find("Connection refused") != std::string::npos);
    // 客户端收到 EOF 才能完成无长度响应；不能把连接关闭误判为缺失 body。
    auto close_listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(close_listener && close_listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) &&
                   close_listener->listen(4));
    auto close_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(close_listener->local_address());
    GO2CPP_REQUIRE(close_address);
    std::thread close_server([close_listener] {
        auto accepted = close_listener->accept(std::chrono::seconds(2));
        if (accepted) {
            std::string request_bytes;
            std::array<char, 512> request_buffer{};
            while (request_bytes.find("\r\n\r\n") == std::string::npos) {
                const auto read = accepted->recv(request_buffer.data(), request_buffer.size(), 0,
                                                  std::chrono::seconds(2));
                if (!read.ok() || read.transferred == 0) return;
                request_bytes.append(request_buffer.data(), static_cast<std::size_t>(read.transferred));
            }
            static constexpr char response[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello";
            const auto sent = accepted->send(response, sizeof(response) - 1, 0, std::chrono::seconds(2));
            GO2CPP_CHECK(sent.ok() && sent.transferred == static_cast<ssize_t>(sizeof(response) - 1));
            accepted->close();
        }
    });
    const auto close_result = go2cpp::http::Client(std::chrono::seconds(2)).get(
        "http://127.0.0.1:" + std::to_string(close_address->port()) + "/eof");
    GO2CPP_CHECK(close_result.ok() && close_result.response->body == "hello");
    GO2CPP_JOIN_WITH_WATCHDOG(close_server, std::chrono::seconds(3));
    close_listener->close();

    // 每次只发送一个 body 字节且间隔大于剩余请求预算。整体请求必须在
    // 一份绝对 deadline 到期，而不是每次 recv 都重新获得完整 timeout。
    auto slow_listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(slow_listener && slow_listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) &&
                   slow_listener->listen(4));
    auto slow_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(slow_listener->local_address());
    GO2CPP_REQUIRE(slow_address);
    std::thread slow_server([slow_listener] {
        auto accepted = slow_listener->accept(std::chrono::seconds(2));
        if (!accepted) return;
        std::string request_bytes;
        std::array<char, 512> request_buffer{};
        while (request_bytes.find("\r\n\r\n") == std::string::npos) {
            const auto read = accepted->recv(request_buffer.data(), request_buffer.size(), 0,
                                              std::chrono::seconds(2));
            if (!read.ok() || read.transferred == 0) return;
            request_bytes.append(request_buffer.data(), static_cast<std::size_t>(read.transferred));
        }
        static constexpr char header[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n";
        const auto header_sent = accepted->send(header, sizeof(header) - 1, 0, std::chrono::seconds(2));
        if (!header_sent.ok()) return;
        for (const char byte : std::string("slow")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(130));
            const auto sent = accepted->send(&byte, 1, 0, std::chrono::seconds(1));
            if (!sent.ok()) break;
        }
        accepted->close();
    });
    const auto slow_result = go2cpp::http::Client(std::chrono::milliseconds(230)).get(
        "http://127.0.0.1:" + std::to_string(slow_address->port()) + "/slow");
    GO2CPP_CHECK(slow_result.error == go2cpp::http::Result::Error::Timeout);
    GO2CPP_JOIN_WITH_WATCHDOG(slow_server, std::chrono::seconds(4));
    slow_listener->close();
    auto length_framer = go2cpp::net::Framer::big_endian32();
    const char framed[] = {0, 0, 0, 3, 'a', 'b', 'c', 0, 0, 0, 1, 'z'};
    length_framer.feed(std::string(framed, sizeof(framed)));
    auto length_frames = length_framer.take_frames();
    GO2CPP_CHECK(length_frames.size() == 2 && length_frames[0] == "abc" && length_frames[1] == "z");
    auto listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(listener && listener->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) && listener->listen(8));
    auto listener_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(listener->local_address());
    GO2CPP_REQUIRE(listener_address && listener_address->port() != 0);
    std::thread acceptor([listener] { auto accepted = listener->accept(std::chrono::seconds(2)); GO2CPP_CHECK(accepted && accepted->valid()); if (accepted) { char data[4]{}; auto result = accepted->recv(data, 3, 0, std::chrono::seconds(2)); GO2CPP_CHECK(result.ok() && result.transferred == 3 && std::string(data, 3) == "tcp"); } });
    auto client = go2cpp::net::Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(client && client->connect(*listener_address, std::chrono::seconds(2)));
    GO2CPP_CHECK(client->send("tcp", 3).ok());
    // H-06：SendAll 全量语义 vs 单次 send 短写。
    // 8 MiB 远大于回环缓冲区，单次 send 必然只写进一部分；SendAll 在同一个
    // socket 上循环到全部写完，慢读端逐字节校验内容后再断言 transferred。
    {
        constexpr std::size_t kTotal = 8U * 1024U * 1024U;
        std::vector<std::uint8_t> payload(kTotal);
        for (std::size_t index = 0; index < kTotal; ++index) {
            payload[index] = static_cast<std::uint8_t>((index * 131U + 7U) & 0xffU);
        }
        const auto make_listener = [] {
            auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
            if (!socket) return socket;
            const int buffer = 64 * 1024;
            socket->set_option(SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
            if (!socket->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)) ||
                !socket->listen(8)) {
                return go2cpp::net::Socket::Ptr{};
            }
            return socket;
        };
        // 1) 对照：单次 send 对 8 MiB 只写入缓冲区容量，ok() 仍为 true——
        //    这正是 H-06 说的「部分成功即成功」，按 Go Write 用会静默丢数据。
        {
            auto control_listener = make_listener();
            GO2CPP_REQUIRE(control_listener);
            auto control_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(
                control_listener->local_address());
            GO2CPP_REQUIRE(control_address);
            std::thread idle_reader([control_listener] {
                auto accepted = control_listener->accept(std::chrono::seconds(3));
                GO2CPP_CHECK(accepted && accepted->valid());
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            });
            auto control_sender = go2cpp::net::Socket::CreateTCP(AF_INET);
            GO2CPP_REQUIRE(control_sender);
            const int send_buffer = 64 * 1024;
            control_sender->set_option(SOL_SOCKET, SO_SNDBUF, &send_buffer,
                                       sizeof(send_buffer));
            GO2CPP_REQUIRE(control_sender->connect(*control_address,
                                                  std::chrono::seconds(3)));
            const auto single = control_sender->send(payload.data(), payload.size(), 0,
                                                     std::chrono::seconds(5));
            GO2CPP_CHECK(single.ok());
            GO2CPP_CHECK(single.transferred > 0);
            GO2CPP_CHECK(static_cast<std::size_t>(single.transferred) < kTotal);
            go2cpp_tests::announce(
                "SendAll baseline: single send wrote " +
                std::to_string(single.transferred) + " of " +
                std::to_string(kTotal) + " bytes (ok=true, short write)");
            control_sender->close();
            GO2CPP_JOIN_WITH_WATCHDOG(idle_reader, std::chrono::seconds(5));
            control_listener->close();
        }
        // 2) SendAll：慢读端每 64 KiB 停 1 ms，逼发送侧跨多轮 EAGAIN 等待；
        //    全部 8 MiB 必须逐字节按序到达。
        {
            auto all_listener = make_listener();
            GO2CPP_REQUIRE(all_listener);
            auto all_address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(
                all_listener->local_address());
            GO2CPP_REQUIRE(all_address);
            std::atomic<std::size_t> received_total{0};
            std::atomic<bool> content_ok{true};
            std::atomic<bool> reader_done{false};
            std::thread slow_reader([&, all_listener] {
                auto accepted = all_listener->accept(std::chrono::seconds(3));
                if (!accepted) {
                    reader_done.store(true, std::memory_order_release);
                    return;
                }
                std::vector<std::uint8_t> chunk(64U * 1024U);
                std::size_t offset = 0;
                while (offset < kTotal) {
                    const auto read = accepted->recv(chunk.data(), chunk.size(), 0,
                                                     std::chrono::seconds(5));
                    if (!read.ok() || read.transferred == 0) break;
                    const auto count = static_cast<std::size_t>(read.transferred);
                    for (std::size_t index = 0; index < count; ++index) {
                        if (chunk[index] != payload[(offset + index) % kTotal]) {
                            content_ok.store(false, std::memory_order_release);
                        }
                    }
                    offset += count;
                    received_total.store(offset, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                reader_done.store(true, std::memory_order_release);
            });
            auto all_sender = go2cpp::net::Socket::CreateTCP(AF_INET);
            GO2CPP_REQUIRE(all_sender);
            const int all_buffer = 64 * 1024;
            all_sender->set_option(SOL_SOCKET, SO_SNDBUF, &all_buffer,
                                   sizeof(all_buffer));
            GO2CPP_REQUIRE(all_sender->connect(*all_address,
                                              std::chrono::seconds(3)));
            const auto send_started = std::chrono::steady_clock::now();
            const auto all_sent = all_sender->SendAll(payload.data(), payload.size(), 0,
                                                      std::chrono::seconds(10));
            const auto send_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - send_started);
            GO2CPP_CHECK(all_sent.ok());
            GO2CPP_CHECK(all_sent.transferred == static_cast<ssize_t>(kTotal));
            GO2CPP_REQUIRE_EVENTUALLY(reader_done.load(std::memory_order_acquire),
                                      std::chrono::seconds(10));
            GO2CPP_JOIN_WITH_WATCHDOG(slow_reader, std::chrono::seconds(10));
            GO2CPP_CHECK(received_total.load(std::memory_order_acquire) == kTotal);
            GO2CPP_CHECK(content_ok.load(std::memory_order_acquire));
            go2cpp_tests::announce(
                "SendAll 8MiB slow reader: sent=" +
                std::to_string(all_sent.transferred) + " received=" +
                std::to_string(received_total.load(std::memory_order_acquire)) +
                " in " + std::to_string(send_elapsed.count()) + " ms");
            all_sender->close();
            all_listener->close();
        }
        // 3) 边界：空写成功；非空 + nullptr 拒绝；对端不读时总超时生效，
        //    且失败返回保留已发送进度（不会把前缀重发算进去）。
        {
            auto empty_socket = go2cpp::net::Socket::CreateTCP(AF_INET);
            GO2CPP_REQUIRE(empty_socket);
            const auto empty = empty_socket->SendAll(nullptr, 0);
            GO2CPP_CHECK(empty.ok() && empty.transferred == 0);
            const auto null_data = empty_socket->SendAll(nullptr, 1);
            GO2CPP_CHECK(!null_data.ok() && null_data.error == EINVAL &&
                         null_data.transferred == -1);
            empty_socket->close();
            int stalled_pair[2]{};
            GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stalled_pair) == 0);
            auto stalled_sender = go2cpp::net::Socket::FromFd(stalled_pair[0], AF_UNIX,
                                                              SOCK_STREAM);
            auto stalled_peer = go2cpp::net::Socket::FromFd(stalled_pair[1], AF_UNIX,
                                                            SOCK_STREAM);
            GO2CPP_REQUIRE(stalled_sender && stalled_peer);
            const int stalled_buffer = 64 * 1024;
            stalled_sender->set_option(SOL_SOCKET, SO_SNDBUF, &stalled_buffer,
                                       sizeof(stalled_buffer));
            const auto stalled = stalled_sender->SendAll(payload.data(), payload.size(), 0,
                                                         std::chrono::milliseconds(100));
            GO2CPP_CHECK(!stalled.ok());
            GO2CPP_CHECK(stalled.error == ETIMEDOUT);
            GO2CPP_CHECK(stalled.transferred > 0 &&
                         static_cast<std::size_t>(stalled.transferred) < kTotal);
            go2cpp_tests::announce(
                "SendAll timeout: transferred=" +
                std::to_string(stalled.transferred) + " error=" +
                std::to_string(stalled.error));
            stalled_sender->close();
            stalled_peer->close();
        }
    }

    // H-04：DNS 查询在受管 Fiber 里包 BlockingRegion——结果正确、不挂死，
    // 查询后调度器（sysmon）仍在推进；普通线程路径与受管路径结果一致。
    {
        const auto plain = go2cpp::net::Address::LookupAnyIPAddress("localhost:80");
        GO2CPP_REQUIRE(plain);
        GO2CPP_CHECK(plain->port() == 80);
        go2cpp::SchedulerConfig dns_config;
        dns_config.processor_count = 1;
        dns_config.max_workers = 2;
        dns_config.enable_sysmon = true;
        dns_config.sysmon_interval = std::chrono::milliseconds(1);
        dns_config.long_syscall_threshold = std::chrono::milliseconds(1);
        go2cpp::Scheduler dns_scheduler(dns_config);
        dns_scheduler.start();
        std::atomic<bool> dns_done{false};
        std::atomic<bool> dns_ok{false};
        std::atomic<bool> dns_managed{false};
        dns_scheduler.Go([&] {
            // 受管 G 里 current_task() 非空，Address::Lookup 会走 BlockingRegion
            // 分支（sysmon 可见的前提条件）；普通线程上构造则是 no-op。
            dns_managed.store(go2cpp::Scheduler::current_task() != nullptr,
                              std::memory_order_release);
            const auto resolved =
                go2cpp::net::Address::LookupAnyIPAddress("localhost:80");
            dns_ok.store(resolved && resolved->port() == 80,
                         std::memory_order_release);
            dns_done.store(true, std::memory_order_release);
        });
        GO2CPP_REQUIRE_EVENTUALLY(dns_done.load(std::memory_order_acquire),
                                  std::chrono::seconds(5));
        GO2CPP_CHECK(dns_managed.load(std::memory_order_acquire));
        GO2CPP_CHECK(dns_ok.load(std::memory_order_acquire));
        // sysmon 活过查询：即使 getaddrinfo 较慢，阻塞区也能让替代 M 接管 P。
        // sysmon 是独立线程，查询本身可能几微秒就结束；这里等它至少跑完一轮
        // 周期再断言计数，避免把「还没来得及采样」误判成「sysmon 挂了」。
        GO2CPP_CHECK(dns_scheduler.sysmon_running());
        const auto sysmon_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (dns_scheduler.sysmon_pass_count() == 0 &&
               std::chrono::steady_clock::now() < sysmon_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        GO2CPP_CHECK(dns_scheduler.sysmon_pass_count() > 0);
        dns_scheduler.shutdown();
    }

    GO2CPP_JOIN_WITH_WATCHDOG(acceptor, std::chrono::seconds(3));
    auto udp_server = go2cpp::net::Socket::CreateUDP(AF_INET);
    GO2CPP_REQUIRE(udp_server && udp_server->bind(*go2cpp::net::IPv4Address::Create("127.0.0.1", 0)));
    auto udp_address = udp_server->local_address(); auto udp_client = go2cpp::net::Socket::CreateUDP(AF_INET);
    GO2CPP_REQUIRE(udp_address && udp_client);
    GO2CPP_CHECK(udp_client->send_to("udp", 3, *udp_address).ok()); char udp_data[8]{}; go2cpp::net::AddressPtr udp_from;
    auto udp_result = udp_server->recv_from(udp_data, 8, udp_from, 0, std::chrono::seconds(2));
    GO2CPP_CHECK(udp_result.ok() && udp_result.transferred == 3 && std::string(udp_data, 3) == "udp" && udp_from);
    // socketpair 用于验证 Socket 的 partial I/O 和线程安全唤醒。
    int pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    auto left = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    auto right = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    std::thread sender([right] { const char value[] = "ping"; auto result = right->send(value, 4); GO2CPP_CHECK(result.ok() && result.transferred == 4); });
    char output[8]{}; auto received = left->recv(output, 4, 0, std::chrono::seconds(2));
    GO2CPP_CHECK(received.ok() && std::string(output, 4) == "ping");
    GO2CPP_JOIN_WITH_WATCHDOG(sender, std::chrono::seconds(2));
    // 受管 Fiber 走 IOManager wait_for，发送线程不依赖 Fiber 锁即可唤醒它。
    go2cpp::SchedulerConfig config; config.processor_count = 1; config.max_workers = 1;
    go2cpp::IOManager manager(config);
    GO2CPP_REQUIRE(manager.Start());
    int managed_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, managed_pair) == 0);
    auto managed_reader = go2cpp::net::Socket::FromFd(managed_pair[0], AF_UNIX, SOCK_STREAM);
    auto managed_writer = go2cpp::net::Socket::FromFd(managed_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> managed_done{false};
    manager.Go([managed_reader, &managed_done] {
        char value[2]{};
        const auto result = managed_reader->recv(value, 1, 0, std::chrono::seconds(2));
        GO2CPP_CHECK(result.ok() && result.transferred == 1 && value[0] == 'f');
        managed_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    GO2CPP_CHECK(managed_writer->send("f", 1).ok());
    GO2CPP_REQUIRE_EVENTUALLY(managed_done.load(std::memory_order_acquire), std::chrono::seconds(3));
    int managed_close_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, managed_close_pair) == 0);
    auto managed_close_reader = go2cpp::net::Socket::FromFd(managed_close_pair[0], AF_UNIX, SOCK_STREAM);
    auto managed_close_peer = go2cpp::net::Socket::FromFd(managed_close_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> managed_close_done{false};
    std::atomic<int> managed_close_error{0};
    manager.Go([managed_close_reader, &managed_close_done, &managed_close_error] {
        char value{};
        const auto result = managed_close_reader->recv(&value, 1);
        managed_close_error.store(result.error, std::memory_order_release);
        managed_close_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    GO2CPP_CHECK(managed_close_reader->close());
    GO2CPP_REQUIRE_EVENTUALLY(managed_close_done.load(std::memory_order_acquire), std::chrono::seconds(3));
    GO2CPP_CHECK(managed_close_error.load(std::memory_order_acquire) == ECANCELED ||
                 managed_close_error.load(std::memory_order_acquire) == EBADF);
    managed_close_peer->close();

    // 关闭方本身也可能是 Fiber。单 worker 时，关闭 Fiber 必须让出 M，
    // 让被 NotifyCloseAll 唤醒的 recv Fiber 先退出并递减 in-flight；如果
    // 这里退回原生 condition_variable，两个 Fiber 会互相等待而永久挂起。
    int fiber_close_pair[2]{-1, -1};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fiber_close_pair) == 0);
    auto fiber_close_reader =
        go2cpp::net::Socket::FromFd(fiber_close_pair[0], AF_UNIX, SOCK_STREAM);
    auto fiber_close_peer =
        go2cpp::net::Socket::FromFd(fiber_close_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> fiber_reader_started{false};
    std::atomic<bool> fiber_reader_done{false};
    std::atomic<bool> fiber_closer_done{false};
    auto fiber_reader = manager.Go([fiber_close_reader, &fiber_reader_started,
                                    &fiber_reader_done] {
        fiber_reader_started.store(true, std::memory_order_release);
        char value{};
        const auto result = fiber_close_reader->recv(&value, 1);
        GO2CPP_CHECK(result.error == ECANCELED || result.error == EBADF);
        fiber_reader_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(fiber_reader_started.load(std::memory_order_acquire),
                              std::chrono::seconds(2));
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_reader->state() == go2cpp::GState::kWaiting,
        std::chrono::seconds(2));
    auto fiber_closer = manager.Go([fiber_close_reader, &fiber_closer_done] {
        GO2CPP_CHECK(fiber_close_reader->close());
        fiber_closer_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(fiber_closer_done.load(std::memory_order_acquire),
                              std::chrono::seconds(3));
    GO2CPP_REQUIRE_EVENTUALLY(fiber_reader_done.load(std::memory_order_acquire),
                              std::chrono::seconds(3));
    GO2CPP_CHECK(fiber_reader->wait_for(std::chrono::seconds(2)));
    GO2CPP_CHECK(fiber_closer->wait_for(std::chrono::seconds(2)));
    GO2CPP_CHECK(fiber_close_peer->close());
    manager.Shutdown();

    // Fiber 内 recv 超时必须返回 ETIMEDOUT，而不是 0 或 EAGAIN。
    // 回归点（P1-1）：等待在 IOManager::wait_for 里 park，Fiber 可能被迁移到
    // 另一个 OS 线程；错误码若靠 errno 跨 park 传递，会读到 park 前线程的
    // TLS 旧值（0/EAGAIN）。这里用默认多 worker 的 IOManager 强制覆盖迁移，
    // 并连续多轮降低偶发通过概率。
    {
        go2cpp::IOManager timeout_manager;
        GO2CPP_REQUIRE(timeout_manager.Start());
        int timeout_pair[2]{-1, -1};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, timeout_pair) == 0);
        auto timeout_socket =
            go2cpp::net::Socket::FromFd(timeout_pair[0], AF_UNIX, SOCK_STREAM);
        auto timeout_peer =
            go2cpp::net::Socket::FromFd(timeout_pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(timeout_socket && timeout_peer);
        for (int round = 0; round < 8; ++round) {
            std::atomic<bool> timeout_done{false};
            std::atomic<int> timeout_error{-1};
            timeout_manager.Go([timeout_socket, &timeout_done, &timeout_error] {
                char value{};
                const auto result = timeout_socket->recv(
                    &value, 1, 0, std::chrono::milliseconds(40));
                timeout_error.store(result.error, std::memory_order_release);
                timeout_done.store(true, std::memory_order_release);
            });
            GO2CPP_REQUIRE_EVENTUALLY(
                timeout_done.load(std::memory_order_acquire),
                std::chrono::seconds(3));
            GO2CPP_CHECK(timeout_error.load(std::memory_order_acquire) ==
                         ETIMEDOUT);
        }
        // 取消同样必须返回 ECANCELED。取消动作放在 Fiber 里执行，才能拿到
        // 当前 IOManager 的唤醒通知（普通线程没有可通知的 manager）。
        std::atomic<bool> cancel_started{false};
        std::atomic<bool> cancel_done{false};
        std::atomic<int> cancel_error{0};
        auto cancel_reader = timeout_manager.Go(
            [timeout_socket, &cancel_started, &cancel_done, &cancel_error] {
                cancel_started.store(true, std::memory_order_release);
                char value{};
                const auto result = timeout_socket->recv(&value, 1);
                cancel_error.store(result.error, std::memory_order_release);
                cancel_done.store(true, std::memory_order_release);
            });
        GO2CPP_REQUIRE_EVENTUALLY(
            cancel_started.load(std::memory_order_acquire),
            std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto cancel_fiber = timeout_manager.Go([timeout_socket] {
            GO2CPP_CHECK(timeout_socket->cancel_read());
        });
        GO2CPP_REQUIRE_EVENTUALLY(cancel_done.load(std::memory_order_acquire),
                                  std::chrono::seconds(3));
        GO2CPP_CHECK(cancel_error.load(std::memory_order_acquire) == ECANCELED);
        GO2CPP_CHECK(cancel_reader->wait_for(std::chrono::seconds(2)));
        GO2CPP_CHECK(cancel_fiber->wait_for(std::chrono::seconds(2)));
        GO2CPP_CHECK(timeout_peer->close());
        timeout_manager.Shutdown();
    }

    // close 必须唤醒普通线程中的无限期 recv，并等待 in-flight 操作退出；
    // 这条测试专门覆盖 close 与阻塞系统调用的生命周期边界，防止 fd
    // 已被复用后旧操作继续使用同一个整数 fd。
    int close_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, close_pair) == 0);
    auto blocked_reader = go2cpp::net::Socket::FromFd(close_pair[0], AF_UNIX, SOCK_STREAM);
    auto blocked_peer = go2cpp::net::Socket::FromFd(close_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> blocked_done{false};
    std::atomic<int> blocked_error{0};
    std::thread blocked([blocked_reader, &blocked_done, &blocked_error] {
        char value{};
        const auto result = blocked_reader->recv(&value, 1);
        blocked_error.store(result.error, std::memory_order_release);
        blocked_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    GO2CPP_CHECK(blocked_reader->close());
    GO2CPP_REQUIRE_EVENTUALLY(blocked_done.load(std::memory_order_acquire), std::chrono::seconds(2));
    GO2CPP_JOIN_WITH_WATCHDOG(blocked, std::chrono::seconds(2));
    GO2CPP_CHECK(blocked_error.load(std::memory_order_acquire) == ECANCELED ||
                 blocked_error.load(std::memory_order_acquire) == EBADF);
    GO2CPP_CHECK(blocked_peer->close());

    // 无 Scheduler 的手动 Fiber 无法 park；close/release 失败时也必须由
    // 最后一个在途操作关闭 fd，不能把描述符留在不可复用状态。
    const auto verify_deferred_socket_close = [](bool request_release) {
        int deferred_pair[2]{-1, -1};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, deferred_pair) == 0);
        auto deferred_socket = go2cpp::net::Socket::FromFd(
            deferred_pair[0], AF_UNIX, SOCK_STREAM);
        auto deferred_peer = go2cpp::net::Socket::FromFd(
            deferred_pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(deferred_socket && deferred_peer);
        auto waiter = std::make_shared<BlockingSocketWaiter>();
        deferred_socket->set_waiter(waiter);
        std::atomic<int> read_error{0};
        auto* const raw_socket = deferred_socket.get();
        std::thread operation([raw_socket, &read_error] {
            char value{};
            const auto result = raw_socket->recv(
                &value, 1, 0, go2cpp::net::Socket::Duration::max());
            read_error.store(result.error, std::memory_order_release);
        });
        const bool waiter_entered =
            waiter->wait_until_entered(std::chrono::seconds(2));
        GO2CPP_CHECK(waiter_entered);
        if (!waiter_entered) {
            waiter->release();
            GO2CPP_JOIN_WITH_WATCHDOG(operation, std::chrono::seconds(3));
            GO2CPP_CHECK(deferred_socket->close());
            GO2CPP_CHECK(deferred_peer->close());
            return;
        }

        bool close_result = true;
        int release_result = -2;
        go2cpp::Fiber closer([&] {
            if (request_release) release_result = deferred_socket->release();
            else close_result = deferred_socket->close();
        });
        GO2CPP_CHECK(closer.resume());
        if (waiter_entered) {
            GO2CPP_CHECK(request_release ? release_result == -1 : !close_result);
            GO2CPP_CHECK(deferred_socket->fd() == deferred_pair[0]);
            GO2CPP_CHECK(::fcntl(deferred_pair[0], F_GETFD) >= 0);
        }
        // 释放最后一个外部所有者；在途操作的自持有引用必须延长 Socket
        // 生命周期，直到 end_operation 完成延迟关闭。
        deferred_socket.reset();
        waiter->release();
        GO2CPP_JOIN_WITH_WATCHDOG(operation, std::chrono::seconds(3));
        GO2CPP_CHECK(read_error.load(std::memory_order_acquire) == ECANCELED);
        errno = 0;
        GO2CPP_CHECK(::fcntl(deferred_pair[0], F_GETFD) == -1 && errno == EBADF);
        GO2CPP_CHECK(deferred_peer->close());
    };
    verify_deferred_socket_close(false);
    verify_deferred_socket_close(true);

    // 固定 seed 重复 close/wake 交错，覆盖多个 native waiter 与多个关闭方
    // 同时访问同一个 Socket 的生命周期边界。每轮 close/recv 都有 watchdog，
    // 防止通知丢失时把测试进程永久挂起。
    std::mt19937 close_random(0x51C0DEU);
    constexpr int kCloseRaceIterations = 96;
    for (int iteration = 0; iteration < kCloseRaceIterations; ++iteration) {
        int race_pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, race_pair) == 0);
        auto race_socket = go2cpp::net::Socket::FromFd(
            race_pair[0], AF_UNIX, SOCK_STREAM);
        auto race_peer = go2cpp::net::Socket::FromFd(
            race_pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(race_socket && race_peer);
        std::atomic<int> readers_done{0};
        std::atomic<int> unexpected_errors{0};
        std::vector<std::thread> readers;
        for (int index = 0; index < 2; ++index) {
            readers.emplace_back([race_socket, &readers_done,
                                  &unexpected_errors] {
                char value{};
                const auto result = race_socket->recv(
                    &value, 1, 0, std::chrono::seconds(2));
                // 收紧：失败结果必须带明确 errno（ECANCELED/EBADF/ETIMEDOUT）。
                // error==0 表示失败原因被吞掉（历史 P1：Fiber park 后 errno
                // 读到 park 前线程的 TLS 旧值），必须立即失败而不是放行。
                if (result.error == 0 ||
                    (result.error != ECANCELED && result.error != EBADF &&
                     result.error != ETIMEDOUT)) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                }
                readers_done.fetch_add(1, std::memory_order_release);
            });
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds(close_random() % 2000U));
        std::vector<std::thread> closers;
        for (int index = 0; index < 3; ++index) {
            closers.emplace_back([race_socket, &unexpected_errors] {
                if (!race_socket->close()) {
                    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& closer : closers) {
            GO2CPP_JOIN_WITH_WATCHDOG(closer, std::chrono::seconds(3));
        }
        for (auto& reader : readers) {
            GO2CPP_JOIN_WITH_WATCHDOG(reader, std::chrono::seconds(3));
        }
        GO2CPP_CHECK(readers_done.load(std::memory_order_acquire) == 2);
        GO2CPP_CHECK(unexpected_errors.load(std::memory_order_acquire) == 0);
        GO2CPP_CHECK(race_peer->close());
    }

    // WebSocket 握手必须严格验证 Upgrade 头和 RFC 6455 的 SHA-1 派生值。
    go2cpp::http::Request upgrade_request;
    upgrade_request.method = go2cpp::http::Method::Get;
    upgrade_request.version = "HTTP/1.1";
    upgrade_request.target = "/chat";
    upgrade_request.header.set("Host", "localhost");
    upgrade_request.header.set("Upgrade", "WebSocket");
    upgrade_request.header.set("Connection", "keep-alive, Upgrade");
    upgrade_request.header.set("Sec-WebSocket-Version", "13");
    upgrade_request.header.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    std::string upgrade_key;
    GO2CPP_CHECK(go2cpp::WebSocket::ValidateUpgradeRequest(upgrade_request, upgrade_key));
    const auto upgrade_response = go2cpp::WebSocket::BuildUpgradeResponse(upgrade_request);
    GO2CPP_CHECK(static_cast<int>(upgrade_response.status) == 101 &&
                 upgrade_response.header.get("Sec-WebSocket-Accept") ==
                     "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    auto bad_upgrade = upgrade_request;
    bad_upgrade.header.set("Sec-WebSocket-Key", "not-base64");
    GO2CPP_CHECK(!go2cpp::WebSocket::ValidateUpgradeRequest(bad_upgrade, upgrade_key));

    // socketpair 同时验证客户端掩码、服务端掩码校验、126 字节扩展长度、
    // 关闭握手以及普通线程双向消息顺序。
    int websocket_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, websocket_pair) == 0);
    auto websocket_server_socket = go2cpp::net::Socket::FromFd(websocket_pair[0], AF_UNIX, SOCK_STREAM);
    auto websocket_client_socket = go2cpp::net::Socket::FromFd(websocket_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> websocket_server_ok{false};
    std::atomic<bool> websocket_server_done{false};
    std::thread websocket_server([websocket_server_socket, &websocket_server_ok, &websocket_server_done] {
        go2cpp::http::Parser request_parser;
        go2cpp::http::Request request;
        std::array<char, 256> input{};
        for (;;) {
            const auto received = websocket_server_socket->recv(input.data(), input.size(), 0,
                                                                std::chrono::seconds(3));
            if (!received.ok() || received.transferred == 0) { websocket_server_done.store(true); return; }
            const auto parsed = request_parser.feed(
                std::string_view(input.data(), static_cast<std::size_t>(received.transferred)), request);
            if (parsed.state == go2cpp::http::ParseState::Error) { websocket_server_done.store(true); return; }
            if (parsed.state == go2cpp::http::ParseState::Complete) break;
        }
        go2cpp::WebSocketResult handshake_result;
        auto websocket = go2cpp::WebSocket::Accept(websocket_server_socket, request, {}, &handshake_result);
        if (!websocket || !handshake_result.ok()) { websocket_server_done.store(true); return; }
        go2cpp::WebSocketMessage first;
        if (!websocket->read_message(first).ok() || !first.text() || first.payload != "hello") {
            websocket_server_done.store(true); return;
        }
        if (!websocket->send_text("world").ok()) { websocket_server_done.store(true); return; }
        go2cpp::WebSocketMessage large;
        if (!websocket->read_message(large).ok() || large.opcode != go2cpp::WebSocketOpcode::Binary ||
            large.payload.size() != 126) { websocket_server_done.store(true); return; }
        go2cpp::WebSocketMessage closing;
        const auto close_result = websocket->read_message(closing);
        websocket_server_ok.store(close_result.ok() && closing.opcode == go2cpp::WebSocketOpcode::Close,
                                  std::memory_order_release);
        websocket_server_done.store(true, std::memory_order_release);
    });
    go2cpp::WebSocketResult client_handshake;
    auto websocket_client = go2cpp::WebSocket::Connect(websocket_client_socket, "localhost", "/chat", {}, &client_handshake);
    GO2CPP_REQUIRE(websocket_client && client_handshake.ok());
    GO2CPP_CHECK(websocket_client->send_text("hello").ok());
    go2cpp::WebSocketMessage response_message;
    GO2CPP_CHECK(websocket_client->read_message(response_message).ok() &&
                 response_message.text() && response_message.payload == "world");
    GO2CPP_CHECK(websocket_client->send_binary(std::string(126, 'x')).ok());
    GO2CPP_CHECK(websocket_client->close(1000, "done").ok());
    GO2CPP_REQUIRE_EVENTUALLY(websocket_server_done.load(std::memory_order_acquire), std::chrono::seconds(3));
    GO2CPP_JOIN_WITH_WATCHDOG(websocket_server, std::chrono::seconds(3));
    GO2CPP_CHECK(websocket_server_ok.load(std::memory_order_acquire));

    // 原始帧边界：HTTP 请求和首个 masked 分片帧可以在同一次 recv 到达，
    // AcceptWithPending 必须保留 Parser 的剩余字节；服务端还必须拒绝未掩码帧。
    int pending_pair[2]{};
    GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pending_pair) == 0);
    auto pending_server_socket = go2cpp::net::Socket::FromFd(pending_pair[0], AF_UNIX, SOCK_STREAM);
    auto pending_client_socket = go2cpp::net::Socket::FromFd(pending_pair[1], AF_UNIX, SOCK_STREAM);
    std::atomic<bool> pending_ok{false};
    std::thread pending_server([pending_server_socket, &pending_ok] {
        go2cpp::http::Parser parser;
        go2cpp::http::Request request;
        std::array<char, 512> input{};
        for (;;) {
            const auto received = pending_server_socket->recv(input.data(), input.size(), 0,
                                                              std::chrono::seconds(3));
            if (!received.ok() || received.transferred == 0) return;
            const auto parsed = parser.feed(
                std::string_view(input.data(), static_cast<std::size_t>(received.transferred)), request);
            if (parsed.state == go2cpp::http::ParseState::Error) return;
            if (parsed.state == go2cpp::http::ParseState::Complete) break;
        }
        go2cpp::WebSocketResult handshake;
        auto ws = go2cpp::WebSocket::AcceptWithPending(
            pending_server_socket, request, parser.take_remaining(), {}, &handshake);
        if (!ws || !handshake.ok()) return;
        go2cpp::WebSocketMessage message;
        const auto received = ws->read_message(message);
        pending_ok.store(received.ok() && message.text() && message.payload == "abcd",
                         std::memory_order_release);
    });
    const std::string raw_header =
        "GET /raw HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const std::array<std::uint8_t, 4> mask{{1, 2, 3, 4}};
    const auto make_masked = [&mask](std::uint8_t first, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(first));
        frame.push_back(static_cast<char>(0x80U | payload.size()));
        frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
        for (std::size_t i = 0; i < payload.size(); ++i)
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % mask.size()]));
        return frame;
    };
    const auto first_fragment = make_masked(0x01U, "ab");
    const auto second_fragment = make_masked(0x80U, "cd");
    const std::string raw_wire = raw_header + first_fragment + second_fragment;
    go2cpp::SocketStream pending_stream(pending_client_socket, false);
    GO2CPP_CHECK(pending_stream.write_all(raw_wire.data(), raw_wire.size()));
    std::string raw_response;
    for (;;) {
        char byte{};
        const auto received = pending_client_socket->recv(&byte, 1, 0, std::chrono::seconds(2));
        GO2CPP_REQUIRE(received.ok() && received.transferred == 1);
        raw_response.push_back(byte);
        if (raw_response.size() >= 4 && raw_response.compare(raw_response.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    GO2CPP_REQUIRE_EVENTUALLY(pending_ok.load(std::memory_order_acquire), std::chrono::seconds(3));
    GO2CPP_JOIN_WITH_WATCHDOG(pending_server, std::chrono::seconds(3));
    pending_client_socket->close();
    // H-01：裸 Scheduler（不挂 IOManager）中的阻塞 socket 不能独占 P。
    // 三条断言：① 100ms 超时的 recv 不会无限挂死；② 长阻塞期间 sysmon 能
    // 看见（long_syscall_count > 0）并补出替代 M；③ 阻塞未结束时提交的
    // 第二个 Fiber 仍能运行，证明确实发生了 M 的补位而不是 P 被独占。
    go2cpp_tests::announce("bare scheduler blocking socket recv");
    const auto bare_scheduler_config = [] {
        go2cpp::SchedulerConfig config;
        config.processor_count = 1;
        config.min_workers = 1;
        config.max_workers = 2;  // 只允许一台补位 M，避免普通多 worker 掩盖问题
        config.allow_worker_oversubscription = true;
        config.enable_sysmon = true;
        config.sysmon_interval = std::chrono::milliseconds(10);
        config.long_syscall_threshold = std::chrono::milliseconds(50);
        return config;
    };
    {
        int pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        auto reader = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
        auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(reader && peer);
        go2cpp::Scheduler scheduler(bare_scheduler_config());
        scheduler.start();
        std::atomic<bool> done{false};
        std::atomic<int> error{0};
        std::atomic<long long> elapsed_ms{0};
        auto task = go2cpp::go(scheduler, [&] {
            const auto began = std::chrono::steady_clock::now();
            char byte{};
            const auto result = reader->recv(&byte, 1, 0,
                                             std::chrono::milliseconds(100));
            elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - began).count(),
                             std::memory_order_release);
            error.store(result.error, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
        GO2CPP_REQUIRE(task != nullptr);
        GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire),
                                  std::chrono::seconds(2));
        GO2CPP_CHECK(error.load(std::memory_order_acquire) == ETIMEDOUT);
        GO2CPP_CHECK(elapsed_ms.load(std::memory_order_acquire) >= 90);
        GO2CPP_CHECK(elapsed_ms.load(std::memory_order_acquire) < 1000);
        (void)task->wait_for(std::chrono::seconds(1));
        reader->close();
        peer->close();
        scheduler.shutdown();
    }
    {
        int pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        auto reader = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
        auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(reader && peer);
        go2cpp::Scheduler scheduler(bare_scheduler_config());
        scheduler.start();
        const std::size_t baseline_workers = scheduler.worker_count();
        std::atomic<bool> blocked_started{false};
        std::atomic<int> blocked_error{0};
        auto blocked = go2cpp::go(scheduler, [&] {
            blocked_started.store(true, std::memory_order_release);
            char byte{};
            const auto result = reader->recv(&byte, 1, 0,
                                             std::chrono::milliseconds(900));
            blocked_error.store(result.error, std::memory_order_release);
        });
        GO2CPP_REQUIRE(blocked != nullptr);
        GO2CPP_REQUIRE_EVENTUALLY(blocked_started.load(std::memory_order_acquire),
                                  std::chrono::seconds(1));

        bool sysmon_detached = false;
        bool worker_grew = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(400);
        do {
            for (const auto& machine : scheduler.machines()) {
                if (machine.long_syscall_count > 0) sysmon_detached = true;
            }
            if (scheduler.worker_count() > baseline_workers) worker_grew = true;
            if (sysmon_detached && worker_grew) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        GO2CPP_CHECK(sysmon_detached);
        GO2CPP_CHECK(worker_grew);

        std::atomic<bool> other_done{false};
        std::atomic<long long> other_elapsed_ms{-1};
        const auto other_began = std::chrono::steady_clock::now();
        auto other = go2cpp::go(scheduler, [&] {
            other_elapsed_ms.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - other_began).count(),
                std::memory_order_release);
            other_done.store(true, std::memory_order_release);
        });
        GO2CPP_REQUIRE(other != nullptr);
        GO2CPP_REQUIRE_EVENTUALLY(other_done.load(std::memory_order_acquire),
                                  std::chrono::milliseconds(700));
        // 第二个 Fiber 必须在 900ms 的 recv 结束前完成，否则说明 P 被独占。
        GO2CPP_CHECK(other_elapsed_ms.load(std::memory_order_acquire) < 850);
        (void)blocked->wait_for(std::chrono::seconds(2));
        GO2CPP_CHECK(blocked_error.load(std::memory_order_acquire) == ETIMEDOUT);
        reader->close();
        peer->close();
        scheduler.shutdown();
    }

    // P1 H-02/H-03：超时三态必须是「0/负数非法、正数有效、max 才是无限」，
    // 且 0 不再被静默接受成「默认值」。单次 recv(0) 仍是合法的「不等待」。
    {
        int pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        auto reader = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
        auto writer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(reader && writer);
        using Duration = go2cpp::net::Socket::Duration;
        GO2CPP_CHECK(!reader->set_recv_timeout(Duration(0)));
        GO2CPP_CHECK(!reader->set_recv_timeout(Duration(-5)));
        GO2CPP_CHECK(!reader->set_send_timeout(Duration(0)));
        GO2CPP_CHECK(reader->set_recv_timeout(Duration(1)));
        GO2CPP_CHECK(reader->recv_timeout() == Duration(1));
        GO2CPP_CHECK(reader->set_recv_timeout(Duration::max()));
        GO2CPP_CHECK(reader->recv_timeout() == Duration::max());
        GO2CPP_CHECK(reader->set_recv_timeout(Duration(50)));
        GO2CPP_CHECK(reader->clear_recv_timeout());
        GO2CPP_CHECK(reader->recv_timeout() == Duration::max());
        // 单次 recv 传 0：立即 ETIMEDOUT（不是「无限等」）。
        char byte{};
        const auto started = std::chrono::steady_clock::now();
        const auto result = reader->recv(&byte, 1, 0, Duration(0));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        GO2CPP_CHECK(result.transferred == -1);
        GO2CPP_CHECK(result.error == ETIMEDOUT);
        GO2CPP_CHECK(elapsed < std::chrono::milliseconds(200));
        reader->close();
        writer->close();
    }

    // 审查回归：三处改动叠加后的行为。
    // ① recv(Duration::max()) 无数据时不能变成「100ms 返回」——100ms 分片只
    //    用于让 sysmon/close 可见，语义仍是无限阻塞；有数据必须立刻唤醒。
    // ② 负数超时是非法参数（文档承诺 EINVAL），数据已就绪时也不能算成功。
    // ③ SendAll 对端关闭：必须报错，不能死循环或谎报全量。
    {
        int pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        auto reader = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
        auto writer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(reader && writer);
        using Duration = go2cpp::net::Socket::Duration;

        std::atomic<bool> done{false};
        std::atomic<int> blocked_error{0};
        std::atomic<ssize_t> blocked_transferred{0};
        std::thread blocked([&] {
            char byte{};
            const auto result = reader->recv(&byte, 1, 0, Duration::max());
            blocked_transferred.store(result.transferred, std::memory_order_release);
            blocked_error.store(result.error, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        GO2CPP_CHECK(!done.load(std::memory_order_acquire));
        GO2CPP_REQUIRE(::write(pair[1], "x", 1) == 1);
        GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire),
                                  std::chrono::seconds(2));
        blocked.join();
        GO2CPP_CHECK(blocked_error.load() == 0);
        GO2CPP_CHECK(blocked_transferred.load() == 1);

        char byte{};
        const auto negative = reader->recv(&byte, 1, 0, Duration(-1));
        GO2CPP_CHECK(negative.transferred == -1 && negative.error == EINVAL);
        GO2CPP_REQUIRE(::write(pair[1], "y", 1) == 1);
        const auto negative_ready = reader->recv(&byte, 1, 0, Duration(-1));
        GO2CPP_CHECK(negative_ready.transferred == -1 &&
                     negative_ready.error == EINVAL);
        const auto normal = reader->recv(&byte, 1, 0, Duration(100));
        GO2CPP_CHECK(normal.ok() && normal.transferred == 1 && byte == 'y');

        GO2CPP_REQUIRE(writer->close());
        std::vector<char> payload(64 * 1024, 'z');
        const auto broken = reader->SendAll(payload.data(), payload.size(), 0,
                                            Duration(500));
        GO2CPP_CHECK(!broken.ok());
        GO2CPP_CHECK(broken.error == EPIPE || broken.error == ECONNRESET);
        reader->close();
    }

    // P1 H-05：Server 默认读/写各 10s（旧实现无限），且非法配置被拒绝。
    {
        go2cpp::http::ServerOptions defaults;
        GO2CPP_CHECK(defaults.read_timeout == std::chrono::seconds(10));
        GO2CPP_CHECK(defaults.write_timeout == std::chrono::seconds(10));
        auto listener = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(listener != nullptr);
        go2cpp::http::Server server(listener);
        GO2CPP_CHECK(server.options().read_timeout == std::chrono::seconds(10));
        GO2CPP_CHECK(!server.set_options({std::chrono::milliseconds(0),
                                          std::chrono::milliseconds(1000)}));
        GO2CPP_CHECK(!server.set_options({std::chrono::milliseconds(-1),
                                          std::chrono::milliseconds(1000)}));
        GO2CPP_CHECK(server.options().read_timeout == std::chrono::seconds(10));

        int pair[2]{};
        GO2CPP_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        auto connection = go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
        auto peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
        GO2CPP_REQUIRE(connection && peer);
        GO2CPP_CHECK(server.set_options({std::chrono::milliseconds(100),
                                         std::chrono::milliseconds(100)}));
        // 只发半个请求头：旧实现会永久阻塞在 recv，新实现 100ms 后返回。
        const std::string half = "GET / HTTP/1.1\r\nHost: example";
        const auto sent = peer->SendAll(half.data(), half.size());
        GO2CPP_CHECK(sent.ok());
        const auto began = std::chrono::steady_clock::now();
        std::thread serving([&] { server.serve_connection(connection); });
        serving.join();
        const auto spent = std::chrono::steady_clock::now() - began;
        GO2CPP_CHECK(spent < std::chrono::seconds(3));
        GO2CPP_CHECK(spent >= std::chrono::milliseconds(50));
        connection->close();
        peer->close();
    }
}

// ---------------------------------------------------------------------------
// Socket 生命周期观察者（`src/network/socket.cpp` 的可选 C 钩子）。
// ---------------------------------------------------------------------------
namespace {

constexpr int kObserverCreated = 1;
constexpr int kObserverClosed = 2;
constexpr int kObserverDetached = 3;
using SocketObserverCallback = void (*)(int fd, int event);

// 基座导出的 C 链接安装函数；测试手工声明，验证不依赖 MCP 也能安装。
extern "C" void go2cpp_network_set_socket_observer(
    SocketObserverCallback observer);

struct ObserverLog {
    std::mutex mutex;
    std::vector<std::pair<int, int>> events;
    std::size_t valid_at_close{0};
    std::size_t invalid_at_close{0};
    std::size_t valid_at_detach{0};

    void Record(int fd, int event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.emplace_back(fd, event);
        const bool valid = ::fcntl(fd, F_GETFD) != -1;
        if (event == kObserverClosed) {
            if (valid) {
                ++valid_at_close;
            } else {
                ++invalid_at_close;
            }
        } else if (event == kObserverDetached && valid) {
            ++valid_at_detach;
        }
    }
    std::size_t Count(int event) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (const auto& item : events) {
            if (item.second == event) {
                ++count;
            }
        }
        return count;
    }
    bool SawFd(int fd, int event) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& item : events) {
            if (item.first == fd && item.second == event) {
                return true;
            }
        }
        return false;
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
        valid_at_close = 0;
        invalid_at_close = 0;
        valid_at_detach = 0;
    }
};

ObserverLog* g_observer_log = nullptr;

void RecordObserverEvent(int fd, int event) {
    if (g_observer_log != nullptr) {
        g_observer_log->Record(fd, event);
    }
}

void ThrowingObserver(int, int) { throw std::runtime_error("observer failure"); }

}  // namespace

/**
 * @brief 验证 socket.cpp 的生命周期钩子：安装/卸载、创建/accept/close/
 *        release、fd 有效性与并发计数守恒。
 * @details 不链接 MCP：这里只验证基座 C ABI，自动登记语义由 mcp 测试覆盖。
 */
void run_socket_observer_tests() {
    go2cpp_tests::announce("network socket lifecycle observer");
    ObserverLog log;
    g_observer_log = &log;

    // 1) 未安装：创建/关闭不产生任何回调。
    {
        auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(socket != nullptr);
        GO2CPP_CHECK(log.Count(kObserverCreated) == 0);
        GO2CPP_CHECK(socket->close());
        GO2CPP_CHECK(log.Count(kObserverClosed) == 0);
    }

    go2cpp_network_set_socket_observer(&RecordObserverEvent);

    // 2) 创建 + 关闭：created/closed 各一次；closed 回调时 fd 仍然有效。
    {
        log.Clear();
        auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(socket != nullptr);
        GO2CPP_CHECK(log.Count(kObserverCreated) == 1);
        GO2CPP_CHECK(log.SawFd(socket->fd(), kObserverCreated));
        const int fd = socket->fd();
        GO2CPP_CHECK(socket->close());
        GO2CPP_CHECK(log.Count(kObserverClosed) == 1);
        GO2CPP_CHECK(log.SawFd(fd, kObserverClosed));
        GO2CPP_CHECK(log.valid_at_close == std::size_t{1});
        GO2CPP_CHECK(log.invalid_at_close == std::size_t{0});
    }

    // 3) accept：监听 fd 与接入连接都触发 created。
    {
        log.Clear();
        auto listener = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(listener &&
                       listener->bind(*go2cpp::net::IPv4Address::Create(
                           "127.0.0.1", 0)) &&
                       listener->listen(4));
        auto address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(
            listener->local_address());
        GO2CPP_REQUIRE(address != nullptr);
        auto client = go2cpp::net::DialTCP("127.0.0.1", address->port(),
                                           std::chrono::seconds(2));
        GO2CPP_REQUIRE(client != nullptr);
        auto server = listener->accept(std::chrono::seconds(2));
        GO2CPP_REQUIRE(server != nullptr);
        GO2CPP_CHECK(log.Count(kObserverCreated) == 3);
        GO2CPP_CHECK(log.SawFd(listener->fd(), kObserverCreated));
        GO2CPP_CHECK(log.SawFd(client->fd(), kObserverCreated));
        GO2CPP_CHECK(log.SawFd(server->fd(), kObserverCreated));
        server->close();
        client->close();
        listener->close();
        GO2CPP_CHECK(log.Count(kObserverClosed) == 3);
    }

    // 4) release：detached 且 fd 仍然有效；显式关闭移交的 fd。
    {
        log.Clear();
        auto socket = go2cpp::net::Socket::CreateUDP(AF_INET);
        GO2CPP_REQUIRE(socket != nullptr);
        const int fd = socket->release();
        GO2CPP_CHECK(fd >= 0);
        GO2CPP_CHECK(log.Count(kObserverDetached) == 1);
        GO2CPP_CHECK(log.valid_at_detach == std::size_t{1});
        GO2CPP_CHECK(::fcntl(fd, F_GETFD) != -1);
        GO2CPP_CHECK(::close(fd) == 0);
    }

    // 5) 观察者抛异常：被基座吞掉，socket 生命周期不受影响。
    {
        go2cpp_network_set_socket_observer(&ThrowingObserver);
        auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(socket != nullptr);
        GO2CPP_CHECK(socket->valid());
        GO2CPP_CHECK(socket->close());
    }

    // 6) 并发创建/关闭：created == closed，无崩溃/丢事件。
    {
        go2cpp_network_set_socket_observer(&RecordObserverEvent);
        log.Clear();
        constexpr int kThreads = 8;
        constexpr int kIterations = 25;
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(kThreads));
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([] {
                for (int iteration = 0; iteration < kIterations; ++iteration) {
                    auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
                    if (!socket) {
                        continue;
                    }
                    socket->close();
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const std::size_t expected =
            static_cast<std::size_t>(kThreads) * static_cast<std::size_t>(kIterations);
        GO2CPP_CHECK(log.Count(kObserverCreated) == expected);
        GO2CPP_CHECK(log.Count(kObserverClosed) == expected);
    }

    // 7) 卸载：不再回调。
    go2cpp_network_set_socket_observer(nullptr);
    {
        log.Clear();
        auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(socket != nullptr);
        GO2CPP_CHECK(socket->close());
        GO2CPP_CHECK(log.Count(kObserverCreated) == 0);
        GO2CPP_CHECK(log.Count(kObserverClosed) == 0);
    }
    g_observer_log = nullptr;
}
