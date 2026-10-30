#include "go2cpp/network/address.hpp"
#include "go2cpp/network/byte_array.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/framing.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"
#include "go2cpp/fiber.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <array>
#include <atomic>
#include <chrono>
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
}
