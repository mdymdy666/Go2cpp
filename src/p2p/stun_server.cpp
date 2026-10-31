/**
 * @file stun_server.cpp
 * @brief STUN echo 服务器实现（4 socket / CHANGE-REQUEST / 单线程 poll）。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/stun_server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include "go2cpp/p2p/stun_message.hpp"

namespace go2cpp::p2p {
namespace {

constexpr int kPollTimeoutMs = 100;

struct BoundSocket {
    int fd = -1;
    Endpoint endpoint;

    bool valid() const noexcept { return fd >= 0; }
};

std::string ErrorText(const std::string& what, int error_number) {
    return what + " 失败: " + std::strerror(error_number);
}

bool BindSocket(const Endpoint& bind_endpoint, std::size_t receive_buffer_bytes,
                BoundSocket* socket, std::string* error) {
    if (!bind_endpoint.valid()) {
        if (error != nullptr) {
            *error = "绑定端点为无效地址";
        }
        return false;
    }
    const int fd = ::socket(bind_endpoint.family(), SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) {
            *error = ErrorText("socket", errno);
        }
        return false;
    }
    if (receive_buffer_bytes > 0) {
        const int bytes = static_cast<int>(std::min<std::size_t>(
            receive_buffer_bytes, static_cast<std::size_t>(1U << 30U)));
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    }
    if (::bind(fd, bind_endpoint.sockaddr_ptr(), bind_endpoint.sockaddr_length()) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = ErrorText("bind " + bind_endpoint.ToString(), saved);
        }
        return false;
    }
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = ErrorText("getsockname", saved);
        }
        return false;
    }
    socket->fd = fd;
    socket->endpoint = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&local), local_length);
    return true;
}

}  // namespace

class StunServer::Impl {
public:
    explicit Impl(StunServerConfig config) : m_config(std::move(config)) {}

    ~Impl() { Stop(); }

    bool Start(std::string* error) {
        if (m_started) {
            return true;
        }
        if (!m_config.primary.valid()) {
            if (error != nullptr) {
                *error = "未配置有效的 primary 监听地址";
            }
            return false;
        }
        m_sockets.fill(BoundSocket{});
        m_socket_count = 0;

        BoundSocket primary;
        if (!BindSocket(m_config.primary, m_config.receive_buffer_bytes, &primary, error)) {
            return false;
        }
        m_sockets[0] = primary;
        m_socket_count = 1;
        m_has_alternate = m_config.alternate.valid();
        m_has_alternate_ip =
            m_has_alternate &&
            m_config.alternate.AddressString() != primary.endpoint.AddressString();

        if (m_has_alternate) {
            const std::uint16_t primary_port = primary.endpoint.port();
            const std::uint16_t alternate_port =
                m_config.alternate.port() != 0 ? m_config.alternate.port() : 0;
            // socket 1：primary IP + alternate port（端口 0 时由内核分配）。
            BoundSocket primary_alt_port;
            if (!BindSocket(primary.endpoint.WithPort(alternate_port),
                            m_config.receive_buffer_bytes, &primary_alt_port, error)) {
                CleanupSockets();
                return false;
            }
            m_sockets[1] = primary_alt_port;
            m_socket_count = 2;
            const std::uint16_t resolved_alternate_port = primary_alt_port.endpoint.port();

            if (m_has_alternate_ip) {
                // socket 2：alternate IP + primary port。
                BoundSocket alternate_primary_port;
                if (!BindSocket(m_config.alternate.WithPort(primary_port),
                                m_config.receive_buffer_bytes, &alternate_primary_port, error)) {
                    CleanupSockets();
                    return false;
                }
                m_sockets[2] = alternate_primary_port;
                m_socket_count = 3;
                // socket 3：alternate IP + alternate port。
                BoundSocket alternate_alt_port;
                if (!BindSocket(m_config.alternate.WithPort(resolved_alternate_port),
                                m_config.receive_buffer_bytes, &alternate_alt_port, error)) {
                    CleanupSockets();
                    return false;
                }
                m_sockets[3] = alternate_alt_port;
                m_socket_count = 4;
            }
        }
        m_stop.store(false);
        m_started = true;
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    void Stop() {
        if (!m_started) {
            return;
        }
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        CleanupSockets();
        m_started = false;
    }

    bool started() const noexcept { return m_started; }

    Endpoint primary_endpoint() const {
        return m_socket_count > 0 ? m_sockets[0].endpoint : Endpoint();
    }

    Endpoint alternate_endpoint() const {
        if (m_socket_count >= 4) {
            return m_sockets[3].endpoint;
        }
        if (m_socket_count >= 2 && !m_has_alternate_ip) {
            return m_sockets[1].endpoint;
        }
        if (m_socket_count >= 3) {
            return m_sockets[3].endpoint;
        }
        return Endpoint();
    }

    std::uint64_t requests_served() const noexcept { return m_requests.load(); }
    std::uint64_t responses_sent() const noexcept { return m_responses.load(); }

private:
    void CleanupSockets() {
        for (BoundSocket& socket : m_sockets) {
            if (socket.valid()) {
                ::close(socket.fd);
                socket.fd = -1;
            }
        }
        m_socket_count = 0;
    }

    void Run() {
        std::array<pollfd, 4> descriptors{};
        for (;;) {
            if (m_stop.load()) {
                return;
            }
            std::size_t count = 0;
            for (std::size_t index = 0; index < m_socket_count; ++index) {
                descriptors[count].fd = m_sockets[index].fd;
                descriptors[count].events = POLLIN;
                descriptors[count].revents = 0;
                ++count;
            }
            const int ready = ::poll(descriptors.data(), static_cast<nfds_t>(count),
                                     kPollTimeoutMs);
            if (ready <= 0) {
                continue;
            }
            for (std::size_t index = 0; index < count; ++index) {
                if ((descriptors[index].revents & POLLIN) == 0) {
                    continue;
                }
                HandleReadable(index);
            }
        }
    }

    void HandleReadable(std::size_t socket_index) {
        std::uint8_t buffer[2048];
        sockaddr_storage client_address{};
        socklen_t client_length = sizeof(client_address);
        const ssize_t received =
            ::recvfrom(m_sockets[socket_index].fd, buffer, sizeof(buffer), 0,
                       reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (received <= 0) {
            return;
        }
        const Endpoint client =
            Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&client_address), client_length);
        if (!client.valid()) {
            return;
        }
        // 零拷贝解析：属性直接指进接收栈缓冲，不再为每个属性分配 vector。
        const auto request = ParseStunMessageView(buffer, static_cast<std::size_t>(received));
        if (!request.has_value() || request->klass() != StunClass::kRequest ||
            request->method() != kStunMethodBinding) {
            return;
        }
        ++m_requests;

        std::size_t response_index = 0;
        const std::uint32_t change =
            request->GetU32(kStunAttributeChangeRequest).value_or(0U);
        if (m_config.support_change_request && change != 0) {
            const bool want_ip = (change & kStunChangeIp) != 0;
            const bool want_port = (change & kStunChangePort) != 0;
            if (want_ip) {
                if (!m_has_alternate_ip || m_socket_count < 4) {
                    // 无法改变源 IP：保持沉默，否则客户端会误判过滤行为。
                    return;
                }
                response_index = want_port ? 3 : 2;
            } else if (want_port) {
                if (!m_has_alternate || m_socket_count < 2) {
                    return;
                }
                response_index = 1;
            }
        }

        // 直接写线格式：XOR-MAPPED/MAPPED/[RESPONSE-ORIGIN]/[OTHER-ADDRESS]/
        // [SOFTWARE] 全部就地编码，每请求 0 次堆分配、0 次中间拷贝。
        const std::size_t software_padded =
            (m_config.software.size() + 3U) & ~std::size_t{3};
        const std::size_t needed =
            kStunHeaderSize + 5U * (4U + 4U + 16U) + 4U + software_padded;
        if (m_response_buffer.size() < needed) {
            m_response_buffer.resize(needed);
        }
        std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
        std::memcpy(transaction_id.data(), request->transaction_id(), transaction_id.size());
        const Endpoint origin = m_sockets[response_index].endpoint;
        const Endpoint other = alternate_endpoint();
        const std::size_t wire_size = BuildBindingSuccessWire(
            m_response_buffer.data(), m_response_buffer.size(), transaction_id, client, &origin,
            other.valid() ? &other : nullptr, m_config.software);
        if (wire_size == 0) {
            return;
        }
        const ssize_t sent =
            ::sendto(m_sockets[response_index].fd, m_response_buffer.data(), wire_size, 0,
                     client.sockaddr_ptr(), client.sockaddr_length());
        if (sent == static_cast<ssize_t>(wire_size)) {
            ++m_responses;
        }
    }

    StunServerConfig m_config;
    std::array<BoundSocket, 4> m_sockets{};
    std::size_t m_socket_count = 0;
    bool m_has_alternate = false;
    bool m_has_alternate_ip = false;
    bool m_started = false;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::atomic<std::uint64_t> m_requests{0};
    std::atomic<std::uint64_t> m_responses{0};
    /// 响应线格式 scratch（单线程 poll 线程独占；只增不减，稳态零分配）。
    std::vector<std::uint8_t> m_response_buffer;
};

StunServer::StunServer(StunServerConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

StunServer::~StunServer() = default;

bool StunServer::Start(std::string* error) {
    return m_impl->Start(error);
}

void StunServer::Stop() {
    m_impl->Stop();
}

bool StunServer::started() const noexcept {
    return m_impl->started();
}

Endpoint StunServer::primary_endpoint() const {
    return m_impl->primary_endpoint();
}

Endpoint StunServer::alternate_endpoint() const {
    return m_impl->alternate_endpoint();
}

std::uint64_t StunServer::requests_served() const noexcept {
    return m_impl->requests_served();
}

std::uint64_t StunServer::responses_sent() const noexcept {
    return m_impl->responses_sent();
}

}  // namespace go2cpp::p2p
