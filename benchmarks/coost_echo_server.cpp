/**
 * @file coost_echo_server.cpp
 * @brief coost 版的 echo server：与 GMP/epoll 压测程序使用完全相同的协议与业务。
 *
 * 协议：[u32 length][u8 opcode][body] -> [u32 length][body]
 * 业务：thread_local seed 递推 + 排序（与 gmp_vs_epoll_fair_benchmark 逐行一致）。
 *
 * 用法：coost_echo_server --port 0 --workers 12
 */
#include "co/co.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kMaxFrame = 8U << 20;

std::string process_body(std::uint8_t opcode, const char* body, std::size_t length) {
    static thread_local std::uint32_t seed = 0x9e3779b9u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const std::uint32_t count = seed % 4097u;
    if (count > 1) {
        static thread_local std::vector<std::int32_t> scratch;
        if (scratch.size() < count) {
            scratch.resize(count);
        }
        std::uint32_t r = seed | 1u;
        for (std::uint32_t i = 0; i < count; ++i) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            scratch[i] = static_cast<std::int32_t>(r);
        }
        std::sort(scratch.begin(), scratch.begin() + count);
        if (scratch[0] == std::numeric_limits<std::int32_t>::min()) {
            return {};
        }
    }
    switch (opcode) {
        case 1: return std::string(body, length);
        case 2: return std::string(1, 'A');
        case 3: return std::string(4096, 'W');
        case 5: return std::string(1, 'P');
        default: return {};
    }
}

bool send_all(sock_t conn, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int n = co::send(conn, data + sent, static_cast<int>(length - sent), -1);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void handle_connection(sock_t conn) {
    char buffer[64 * 1024];
    std::string in;
    std::string out;
    for (;;) {
        const int n = co::recv(conn, buffer, sizeof(buffer), -1);
        if (n <= 0) {
            break;
        }
        in.append(buffer, static_cast<std::size_t>(n));
        std::size_t off = 0;
        bool closed = false;
        for (;;) {
            const std::size_t available = in.size() - off;
            if (available < 4) {
                break;
            }
            std::uint32_t length = 0;
            std::memcpy(&length, in.data() + off, 4);
            if (length == 0 || length > kMaxFrame) {
                closed = true;
                break;
            }
            if (available < static_cast<std::size_t>(length) + 4) {
                break;
            }
            const char* body = in.data() + off + 4;
            const std::string response = process_body(
                static_cast<std::uint8_t>(body[0]), body + 1,
                static_cast<std::size_t>(length) - 1);
            out.clear();
            out.reserve(4 + response.size());
            const std::uint32_t response_length =
                static_cast<std::uint32_t>(response.size());
            out.append(reinterpret_cast<const char*>(&response_length), 4);
            out.append(response);
            if (!send_all(conn, out.data(), out.size())) {
                closed = true;
                break;
            }
            off += static_cast<std::size_t>(length) + 4;
        }
        if (closed) {
            break;
        }
        if (off == in.size()) {
            in.clear();
        } else if (off > (1U << 20)) {
            in.erase(0, off);
        } else {
            in.erase(0, off);
        }
    }
    co::close(conn);
}

int run_server(std::uint16_t port, std::size_t workers) {
    flag::set_value("co_sched_num", std::to_string(workers).c_str());
    const sock_t listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        std::fputs("[coost] socket 失败\n", stderr);
        return 1;
    }
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        co::listen(listener, 8192) != 0) {
        std::fputs("[coost] bind/listen 失败\n", stderr);
        return 1;
    }
    socklen_t length = sizeof(address);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(ntohs(address.sin_port)));
    std::fflush(stdout);
    for (;;) {
        const sock_t conn = co::accept(listener, nullptr, nullptr);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }
        go(handle_connection, conn);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    std::size_t workers = 12;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            port = static_cast<std::uint16_t>(std::stoul(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--workers") == 0) {
            workers = static_cast<std::size_t>(std::stoul(argv[i + 1]));
        }
    }
    flag::set_value("co_sched_num", std::to_string(workers).c_str());
    co::wait_group wg(1);
    go([&] {
        const int result = run_server(port, workers);
        if (result != 0) {
            std::exit(result);
        }
        wg.done();
    });
    wg.wait();
    return 0;
}
